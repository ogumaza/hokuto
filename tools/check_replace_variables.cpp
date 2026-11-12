// SPDX-License-Identifier: MIT

// The mode --variables of hokuto_check_replace: the values of variables.

#include "check_replace.h"

namespace hokuto::tools::replace
{

namespace
{

// The values that the variables' check gives a variable: its choices, or else 0 and 1.
std::vector<int> CheckedValues(const song::Variable& variable)
{
    return variable.choices.empty() ? std::vector<int>{0, 1} : variable.choices;
}

// A sound whose commands read variables before they set them: the sound, its commands from its file and its start, the
// variables, and the variables that the commands use, a bit each.
struct VariableSound
{
    uint32_t sound = 0;
    Source source;
    std::vector<song::Variable> variables;
    edit::VariableUses uses;
};

// The rounds of the variables' check: a move of each variable, and fixed values from each variable's second value and
// from its last. Only the second round leaves out the variables that the sound's commands read now. Replace… leaves
// them out too.
enum class VariableRound
{
    kMove,
    kSecond,
    kLast,
};

// The map that round `round` gives the variables of `entry`. A move goes to the lowest number of the variable's kind
// that the commands don't use and no other variable takes. A variable without a free number keeps its number.
edit::VariableMap RoundMap(const VariableSound& entry, VariableRound round)
{
    edit::VariableMap map;
    uint64_t taken = entry.uses.reads | entry.uses.sets;
    for (const song::Variable& variable : entry.variables)
    {
        const std::vector<int> values = CheckedValues(variable);
        if (round == VariableRound::kSecond || round == VariableRound::kLast)
        {
            map.fixed[variable.number] = round == VariableRound::kLast ? values.back() : values[values.size() > 1];
            continue;
        }

        const bool global = variable.number >= edit::kSoundVariables;
        for (int to = global ? edit::kSoundVariables : 0; to < (global ? song::kGameVariables : edit::kSoundVariables);
             to++)
        {
            if ((taken >> to & 1u) == 0)
            {
                map.moves[variable.number] = to;
                taken |= uint64_t{1} << to;
                break;
            }
        }
    }

    return map;
}

// What the performer plays of a sound, without the variables that it reads and the values that they start with: an edit
// of the variables changes their numbers.
song::Sequence PlayedNotes(const sar::SoundArchive& archive, uint32_t sound, const song::StartValues& values)
{
    song::Sequence played = Played(archive, sound, values);
    played.variables.clear();
    played.read_variables.clear();
    played.start_values.clear();

    return played;
}

// Whether a track of sound `sound` of `archive` runs without waits at the core's limit of commands in a tick, with the
// variables' starting values `values`. The performer then warns that the track stopped.
bool RunsAway(const sar::SoundArchive& archive, uint32_t sound, const song::StartValues& values)
{
    namespace export_ = ntdlib::sequence_export;
    const export_::Settings settings = song::SettingsOf(archive);
    export_::BankSet banks(archive, archive.Sounds()[sound], settings);
    const export_::Performance performance = export_::Perform(
        archive, sound, banks, song::PerformOptionsOf(values, {}, archive.ReadOptions().flavor), settings);
    const auto runaway = [](const std::string& warning)
    {
        return warning.find("running without waits") != std::string::npos;
    };

    return std::ranges::any_of(performance.warnings, runaway);
}

// What is wrong when a moved variable of sound `entry.sound` plays one of its values in `after`, at its new number of
// `map`, otherwise than `played_before` says it played that value at its old number, or nothing.
std::string MovedValuesProblem(const sar::SoundArchive& after, const VariableSound& entry, const edit::VariableMap& map,
                               const std::function<const song::Sequence&(const song::StartValues&)>& played_before,
                               Totals& totals)
{
    for (const song::Variable& variable : entry.variables)
    {
        const auto moved = map.moves.find(variable.number);
        const int number = moved != map.moves.end() ? moved->second : variable.number;
        for (const int value : CheckedValues(variable))
        {
            totals.va_runs++;
            if (PlayedNotes(after, entry.sound, {{number, static_cast<int16_t>(value)}}) !=
                played_before({{variable.number, static_cast<int16_t>(value)}}))
            {
                return "plays otherwise with variable " + std::to_string(number) + " at " + std::to_string(value) +
                       " than with variable " + std::to_string(variable.number) + " at that value before";
            }
        }
    }

    return {};
}

// What is wrong with sound `entry.sound` of `after`, the archive after the sound took `map`, or nothing. `before` holds
// what the performer played of each sound of `built` with each set of starting values.
std::string VariablesProblem(const sar::SoundArchive& built, const sar::SoundArchive& after, const VariableSound& entry,
                             const edit::VariableMap& map,
                             std::map<std::pair<uint32_t, song::StartValues>, song::Sequence>& before, Totals& totals)
{
    const auto played_before = [&](const song::StartValues& values) -> const song::Sequence&
    {
        auto found = before.find({entry.sound, values});
        if (found == before.end())
        {
            found = before.emplace(std::pair(entry.sound, values), PlayedNotes(built, entry.sound, values)).first;
        }

        return found->second;
    };

    // A moved variable plays each of its values at its new number as it played them at its old one.
    if (map.fixed.empty())
    {
        return MovedValuesProblem(after, entry, map, played_before, totals);
    }

    // Fixed variables play as their values did, whatever values they start with.
    song::StartValues fixed;
    song::StartValues other;
    for (const auto& [number, value] : map.fixed)
    {
        fixed[number] = static_cast<int16_t>(value);
        other[number] = static_cast<int16_t>(value == 0 ? 1 : 0);
    }

    // A track that runs without waits at the core's limit of commands in a tick reads on in the next tick. The command
    // at the start moves where the limit cuts each tick. A sound that draws random numbers then draws them in other
    // ticks.
    totals.va_runs += 2;
    if (PlayedNotes(after, entry.sound, {}) != played_before(fixed))
    {
        if (RunsAway(built, entry.sound, fixed))
        {
            totals.va_runaway++;
            return {};
        }

        return "plays otherwise with its variables fixed than with those values before";
    }
    if (PlayedNotes(after, entry.sound, other) != played_before(fixed))
    {
        return "plays otherwise with other starting values after its variables took fixed values";
    }

    return {};
}

// The most sounds of each sequence that the check edits, one in each pass.
constexpr std::size_t kVariablePasses = 4;

// The sounds of `built` whose commands read variables before they set them, by their sequences, in the order of the
// sound table: up to kVariablePasses of each sequence. Counts the sounds without a start and those that read variables.
std::map<uint32_t, std::vector<VariableSound>> VariableSoundsBySequence(
    const sar::SoundArchive& built, std::span<const std::optional<edit::SoundPlace>> places, Totals& totals)
{
    std::map<uint32_t, std::vector<VariableSound>> by_sequence;
    for (std::size_t i = 0; i < built.Sounds().size(); i++)
    {
        const sar::SoundInfo& info = built.Sounds()[i];
        if (info.type != sar::SoundType::kSequence || !places[i] || built.FileData(info.file_id).empty())
        {
            continue;
        }

        Source source = SourceOf(built.FileData(info.file_id), info.start_offset);
        if (!source.start)
        {
            totals.va_no_start++;
            continue;
        }
        std::vector<song::Variable> variables = song::VariablesOf(source.incoming.sequence, *source.start).starting;
        if (variables.empty())
        {
            continue;
        }
        totals.va_reading++;
        std::vector<VariableSound>& sounds = by_sequence[places[i]->sequence];
        if (sounds.size() < kVariablePasses)
        {
            const edit::VariableUses uses = edit::UsesOf(source.incoming.sequence, *source.start);
            sounds.push_back({static_cast<uint32_t>(i), std::move(source), std::move(variables), uses});
        }
    }

    return by_sequence;
}

// A round's version of a file, and the edits that it took: each with its sound and the sound's map.
struct RoundEdits
{
    format::File edited;
    std::vector<std::pair<const VariableSound*, edit::VariableMap>> edits;
};

// The version of `file` in which sound `pass` of each sequence of `by_sequence` uses the map of `round`. Counts the
// moved, fixed and held variables, and the refused edits with their reasons.
RoundEdits EditRound(const format::File& file, std::span<const std::optional<edit::SoundPlace>> places,
                     const std::map<uint32_t, std::vector<VariableSound>>& by_sequence, std::size_t pass,
                     VariableRound round, Totals& totals)
{
    format::File edited = file;
    std::vector<std::pair<const VariableSound*, edit::VariableMap>> edits;
    for (const auto& [sequence, sounds] : by_sequence)
    {
        if (pass >= sounds.size())
        {
            continue;
        }

        const VariableSound& entry = sounds[pass];
        const edit::VariableMap map = RoundMap(entry, round);
        const uint32_t taken = round == VariableRound::kSecond ? static_cast<uint32_t>(entry.uses.reads & 0xFFFF) : 0;
        try
        {
            const edit::MappedVariables mapped =
                edit::WithVariables(entry.source.incoming.sequence, *entry.source.start, map, taken);
            const uint32_t tracks = edit::TracksToAdd(edited, places, entry.sound, mapped.sequence, mapped.start);
            edited =
                edit::ReplaceSequence(edited, places, entry.sound, mapped.sequence, mapped.start, {}, "Check", tracks);
            edits.emplace_back(&entry, map);
            totals.va_moved += map.moves.size();
            totals.va_fixed += mapped.holders.size();
            for (const auto& [number, holder] : mapped.holders)
            {
                totals.va_held += holder != number ? 1 : 0;
            }
        }
        catch (const std::invalid_argument& error)
        {
            totals.va_refused++;
            totals.va_reasons.insert(error.what());
        }
    }

    return {std::move(edited), std::move(edits)};
}

// Checks each edit of a round in `edited`, the round's version of the file. Lists the sounds that it checks in
// `checked`. `before` holds what the performer played of each sound of `built` with each set of starting values.
void CheckRoundEdits(const sar::SoundArchive& built, const format::File& edited,
                     std::span<const std::pair<const VariableSound*, edit::VariableMap>> edits,
                     std::map<std::pair<uint32_t, song::StartValues>, song::Sequence>& before,
                     std::set<uint32_t>& checked, const std::string& name, Totals& totals)
{
    // The round's version of the file saves and exports. Each of its edits fails when it doesn't.
    Archive after;
    std::string failure;
    try
    {
        hokuto::tools::CheckSaved(edited, true);
        after = song::LoadArchive(format::ExportArchive(edited), "after.bfsar", format::ExportGroupFiles(edited));
    }
    catch (const std::exception& error)
    {
        failure = std::string("fails: ") + error.what();
    }

    for (const auto& [entry, map] : edits)
    {
        std::string problem;
        try
        {
            problem = failure.empty() ? VariablesProblem(built, *after, *entry, map, before, totals) : failure;
        }
        catch (const std::exception& error)
        {
            problem = std::string("fails: ") + error.what();
        }
        if (!problem.empty())
        {
            std::printf("%s: %s %s\n", name.c_str(), built.Sounds()[entry->sound].name.c_str(), problem.c_str());
            totals.va_failed++;
        }
        checked.insert(entry->sound);
    }
}

} // namespace

void CheckVariables(const format::File& file, const sar::SoundArchive& built,
                    std::span<const std::optional<edit::SoundPlace>> places, const std::string& name, Totals& totals)
{
    // The sounds whose commands read variables before they set them, by their sequences, in the order of the sound
    // table.
    const std::map<uint32_t, std::vector<VariableSound>> by_sequence = VariableSoundsBySequence(built, places, totals);

    std::map<std::pair<uint32_t, song::StartValues>, song::Sequence> before;
    std::set<uint32_t> checked;
    for (std::size_t pass = 0; pass < kVariablePasses; pass++)
    {
        for (const VariableRound round : {VariableRound::kMove, VariableRound::kSecond, VariableRound::kLast})
        {
            // One sound of each sequence takes the round's map, in one version of the file.
            const RoundEdits round_edits = EditRound(file, places, by_sequence, pass, round, totals);
            if (round_edits.edits.empty())
            {
                continue;
            }

            CheckRoundEdits(built, round_edits.edited, round_edits.edits, before, checked, name, totals);
        }
    }
    totals.va_checked = checked.size();
}

} // namespace hokuto::tools::replace
