// SPDX-License-Identifier: MIT

// The mode --new of hokuto_check_replace: new sequence sounds.

#include "check_replace.h"

namespace hokuto::tools::replace
{

namespace
{

// The most sequence sounds that --new copies in an archive.
constexpr std::size_t kNewModels = 32;

} // namespace

std::vector<uint32_t> NewModels(std::span<const std::optional<edit::SoundPlace>> places)
{
    std::vector<uint32_t> candidates;
    for (std::size_t i = 0; i < places.size(); i++)
    {
        if (places[i])
        {
            candidates.push_back(static_cast<uint32_t>(i));
        }
    }

    std::vector<uint32_t> models;
    for (const std::size_t index : SpreadIndices(candidates.size(), kNewModels))
    {
        models.push_back(candidates[index]);
    }

    return models;
}

namespace
{

// A name that no item of the archive in `bytes` holds: HOKUTO_NEW_SOUND, with underscores after it while an item holds
// the name.
std::string FreeName(std::span<const uint8_t> bytes, sar::Flavor flavor)
{
    std::string name = "HOKUTO_NEW_SOUND";
    const std::optional<std::span<const uint8_t>> strings = sar::TablesOf(bytes, flavor).strings;
    while (strings && sar::FindItem(*strings, flavor, name))
    {
        name += "_";
    }

    return name;
}

} // namespace

bool SameValues(const sar::SoundInfo& a, const sar::SoundInfo& b)
{
    return a.type == b.type && a.volume == b.volume && a.pan_mode == b.pan_mode && a.pan_curve == b.pan_curve &&
           a.player_id == b.player_id && a.player_priority == b.player_priority &&
           a.actor_player_id == b.actor_player_id && a.banks == b.banks && a.channel_priority == b.channel_priority &&
           a.release_priority_fix == b.release_priority_fix;
}

namespace
{

// What is wrong with `added`, the version of `base` whose archive gains `name`, a copy of sound `model` that plays
// `source` from its start, or nothing. `built` is the archive that `base` builds. The new sound has to hold the model's
// values and play the model's notes, unless `untracked` says that the model leaves out tracks that its commands open.
// Every other item has to keep its values, and every file its bytes. The program has to find the new sound's sequence,
// and the new version has to save and open again in both forms.
std::string NewProblem(const format::File& base, const sar::SoundArchive& built, const format::File& added,
                       uint32_t model, const std::string& name, const Source& source, bool untracked)
{
    const std::vector<uint8_t> bytes = format::ExportArchive(added);
    const Archive after = song::LoadArchive(bytes, "after.bfsar", format::ExportGroupFiles(added));
    const auto sound = static_cast<uint32_t>(built.Sounds().size());
    if (after->Sounds().size() != sound + 1 || after->FileCount() != built.FileCount() + 1 ||
        added.sequences.size() != base.sequences.size() + 1)
    {
        return "doesn't give the archive one sound, one file and one sequence more";
    }

    // The new sound.
    const sar::SoundInfo& info = after->Sounds()[sound];
    if (info.name != name || info.file_id != built.FileCount() || !SameValues(info, built.Sounds()[model]))
    {
        return "gives the new sound other values than the model's";
    }
    if (info.allocate_track_flags != edit::TracksOpened(source.incoming.sequence, *source.start))
    {
        return "gives the new sound other tracks than its commands open";
    }
    if (!untracked && Played(*after, sound) != Played(built, model))
    {
        return "doesn't play the model's notes";
    }

    // The other items and files.
    for (uint32_t i = 0; i < sound; i++)
    {
        const sar::SoundInfo& old = built.Sounds()[i];
        const sar::SoundInfo& now = after->Sounds()[i];
        if (now.name != old.name || now.file_id != old.file_id || now.start_offset != old.start_offset ||
            now.allocate_track_flags != old.allocate_track_flags || !SameValues(now, old))
        {
            return "changes " + old.name;
        }
    }
    if (Played(*after, model) != Played(built, model))
    {
        return "changes how the model plays";
    }
    for (uint32_t id = 0; id < built.FileCount(); id++)
    {
        if (!std::ranges::equal(built.FileData(id), after->FileData(id)))
        {
            return "changes file " + std::to_string(id);
        }
    }
    if (NamesOf(after->Banks()) != NamesOf(built.Banks()) ||
        NamesOf(after->WaveArchives()) != NamesOf(built.WaveArchives()) ||
        NamesOf(after->Players()) != NamesOf(built.Players()) || NamesOf(after->Groups()) != NamesOf(built.Groups()))
    {
        return "changes the names of other items";
    }

    // The program's view of the new version.
    const std::vector<std::optional<edit::SoundPlace>> places = edit::SoundPlaces(
        *after, format::SequencesOfFiles(added, bytes, *after), format::StartFieldsOfSounds(added, bytes));
    if (!places[sound] || places[sound]->sequence != base.sequences.size())
    {
        return "leaves the new sound without its sequence";
    }
    if (!hokuto::tools::SavesAndOpens(added, true))
    {
        return "doesn't save and open again";
    }

    return {};
}

// What is wrong when the file of sound `model` for another console builds another archive than `added`, or nothing.
// `added` is `file` with the copy `new_name` that plays `source`, and `built` is the archive that `file` builds. Counts
// the files that the 3DS can't hold, the files whose commands differ only in biquad filters, and those without the
// start.
std::string OtherConsolesProblem(const format::File& file, const sar::SoundArchive& built, uint32_t model,
                                 const std::string& new_name, const Source& source, const format::File& added,
                                 Totals& totals)
{
    const sar::Flavor flavor = built.ReadOptions().flavor;
    const sar::SoundInfo& info = built.Sounds()[model];
    for (const sar::Flavor console : song::OtherConsoles(flavor))
    {
        std::vector<uint8_t> other_file;
        try
        {
            other_file = song::OtherConsoleSequenceFile(built, model, console).file;
        }
        catch (const std::runtime_error&)
        {
            if (console != sar::Flavor::kCtr)
            {
                throw;
            }
            totals.new_ctr_refused++;
            continue;
        }
        const Source other =
            SourceOf(other_file, info.start_offset, song::Console{.engine = hokuto::format::EngineOf(flavor)});
        if (other.start && other.incoming.sequence != source.incoming.sequence &&
            song::SameButBiquads(other.incoming.sequence, source.incoming.sequence))
        {
            totals.new_other_biquads++;
        }
        else if (!other.start || other.incoming.sequence != source.incoming.sequence)
        {
            totals.new_other_unreached++;
        }
        else if (format::ExportArchive(edit::AddSequenceSound(file, model, new_name, other.incoming.sequence,
                                                              *other.start, {})) != format::ExportArchive(added))
        {
            return "gives another archive from the file for the " + song::ConsoleName(console);
        }
        else
        {
            totals.new_crossed++;
        }
    }

    return {};
}

} // namespace

void CheckNewSounds(const format::File& file, const sar::SoundArchive& built, std::span<const uint8_t> bytes,
                    std::span<const edit::Group> edit_groups, std::span<const std::optional<edit::SoundPlace>> places,
                    const std::string& name, Totals& totals)
{
    const sar::Flavor flavor = built.ReadOptions().flavor;
    const std::vector<uint32_t> models = NewModels(places);
    std::optional<format::File> first;
    Joins joins;
    for (std::size_t k = 0; k < models.size(); k++)
    {
        const uint32_t model = models[k];
        const sar::SoundInfo& info = built.Sounds()[model];
        std::string problem;
        try
        {
            const Source source = SourceOf(built.FileData(info.file_id), info.start_offset);
            if (!source.start)
            {
                totals.new_no_start++;
                continue;
            }
            const bool untracked = Untracked(source, info);
            totals.new_untracked += untracked ? 1 : 0;

            // The copy with the model's file. The Import window makes the same edit.
            const std::string new_name = FreeName(bytes, flavor);
            const auto start = std::chrono::steady_clock::now();
            const format::File added =
                edit::AddSequenceSound(file, model, new_name, source.incoming.sequence, *source.start, {});
            const std::chrono::duration<double> took = std::chrono::steady_clock::now() - start;
            totals.new_slowest = std::max(totals.new_slowest, took.count());
            problem = NewProblem(file, built, added, model, new_name, source, untracked);

            // The same commands from the file for each other console give the same archive. A file that the 3DS can't
            // hold counts apart.
            if (problem.empty())
            {
                problem = OtherConsolesProblem(file, built, model, new_name, source, added, totals);
            }

            // The copy joins the groups that load the model. The first copy also joins a new group.
            if (problem.empty() && JoinsGroups(joins, edit_groups))
            {
                const uint32_t id = sar::ItemId(sar::kSoundItem, static_cast<uint32_t>(built.Sounds().size()));
                const std::vector<uint32_t> files_before =
                    edit::FilesOfItem(built, sar::ItemId(sar::kSoundItem, model));
                const auto with_copy = [&]() -> const format::File&
                {
                    return added;
                };
                problem = JoinAndNewGroupCheck(file, built, edit_groups, std::span(&id, 1), files_before, {}, with_copy,
                                               joins, totals);
            }

            // The second model's copy after the first model's.
            if (problem.empty() && k == 0)
            {
                first = added;
            }
            else if (problem.empty() && k == 1 && first)
            {
                const std::vector<uint8_t> first_bytes = format::ExportArchive(*first);
                const Archive first_built =
                    song::LoadArchive(first_bytes, "first.bfsar", format::ExportGroupFiles(*first));
                const std::string second_name = FreeName(first_bytes, flavor);
                const format::File twice =
                    edit::AddSequenceSound(*first, model, second_name, source.incoming.sequence, *source.start, {});
                problem = NewProblem(*first, *first_built, twice, model, second_name, source, untracked);
                totals.new_pairs++;
            }
        }
        catch (const std::exception& error)
        {
            problem = std::string("fails: ") + error.what();
        }

        if (!problem.empty())
        {
            std::printf("%s: %s %s\n", name.c_str(), info.name.c_str(), problem.c_str());
            totals.new_failed++;
        }
        totals.new_sounds++;
    }
}

} // namespace hokuto::tools::replace
