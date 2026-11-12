// SPDX-License-Identifier: MIT

// The views of a sequence that replaces a sound's sequence or comes in as a new item, on the 3DS, the Wii U, the
// Switch and the Wii: its starts, programs, drums and variables. replace_midi_maps.cpp contains the MIDI maps.

#include "app/replace_sequence_views.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/formats.h"
#include "xsflib/files.h"

#include "app/export_warnings.h"
#include "app/replace_common.h"
#include "app/replace_view.h"
#include "edit/bytecode.h"
#include "edit/converter.h"
#include "edit/groups.h"
#include "edit/new_items.h"
#include "edit/other_console.h"
#include "edit/replace.h"
#include "edit/timelines.h"
#include "edit/variables.h"
#include "format/archive.h"
#include "format/file.h"
#include "format/kinds.h"
#include "song/archive.h"
#include "song/items.h"
#include "song/sequence.h"

namespace hokuto::app
{
namespace
{

using replace_common::AddProgramWarnings;
using replace_common::AddWiiGroupWarning;
using replace_common::BankNames;
using replace_common::FillDrumRows;
using replace_common::FillGroups;
using replace_common::FillImportModels;
using replace_common::FillWiiGroups;
using replace_common::HoldsNames;
using replace_common::kSlots;
using replace_common::kWiiLastGroup;
using replace_common::MidiSource;
using replace_common::NameRefusal;
using replace_common::NameTaken;
using replace_common::NumberList;
using replace_common::Placed;
using replace_common::ProgramsOfBank;
using replace_common::Refused;
using replace_common::SizeGroups;
using replace_common::SoundBanks;
using replace_common::TakenNames;
using replace_common::TrackList;
using replace_common::WiiFilesOfSound;
using replace_common::WiiTakenNames;

// The name of sound `sound` of `source`'s archive.
std::string SoundName(const ReplaceSource& source, uint32_t sound)
{
    return source.played.Wii() ? source.played.Wii()->Sounds()[sound].name : source.played.Sar()->Sounds()[sound].name;
}

// The bank slots of a sound of `source`'s archive.
std::size_t SlotsOf(const ReplaceSource& source)
{
    return source.played.Wii() ? 1 : kSlots;
}

// FindGroups on the Wii. One build of the archive that an export writes finds the groups, the refusal and the size of
// each file. The archive that an export writes differs from the one that the edits read only in the bytes of the
// timelines' sequence files. Its groups and its layout are the same. For a file whose timelines don't fold, or whose
// folded archive doesn't build, the groups and the refusal come from the archive that the edits read, and there are no
// sizes.
std::optional<std::string> FindWiiGroups(ReplaceSource& source)
{
    source.file_sizes.clear();
    source.group_sizes.clear();
    std::vector<uint8_t> bytes;
    try
    {
        bytes = format::ExportArchive(edit::FoldTimelines(*source.file));
    }
    catch (const std::exception&)
    {
        edit::WiiAdditions additions = edit::WiiAdditionsOf(*source.file);
        source.groups = std::move(additions.groups);
        return additions.refused;
    }

    edit::WiiAdditions additions = edit::WiiAdditionsOf(*source.file, bytes);
    source.groups = std::move(additions.groups);
    try
    {
        const std::shared_ptr<const ntdlib::rsar::SoundArchive> wii =
            song::LoadWiiArchive(std::move(bytes), source.file_name, ntdlib::rvl::snd::kRsarOptions);
        for (uint32_t id = 0; id < wii->FileCount(); ++id)
        {
            source.file_sizes.push_back(wii->FileData(id).size() + wii->WaveData(id).size());
        }
    }
    catch (const std::exception&)
    {
        source.file_sizes.clear();
    }

    return additions.refused;
}

} // namespace

std::vector<uint32_t> ReplacedSequences(const ReplaceSource& source)
{
    if (source.sound >= source.places.size() || !source.places[source.sound] ||
        !edit::SharingFile(source.places, source.sound).empty())
    {
        return {};
    }

    return {source.places[source.sound]->sequence};
}

void FindReaders(ReplaceSource& source)
{
    source.readers = {};
    if (!source.places.empty())
    {
        source.readers = edit::VariableReaders(*source.file, source.places);
    }
}

std::optional<std::string> FindGroups(ReplaceSource& source)
{
    if (source.played.Wii() && source.file)
    {
        return FindWiiGroups(source);
    }
    if (!source.file || !source.file->native || !source.played.Sar())
    {
        return std::nullopt;
    }

    source.groups = edit::GroupsOf(*source.file, *source.played.Sar());

    // The archive that an export writes. Without it, the window measures the archive that the program plays.
    source.file_sizes.clear();
    source.group_sizes.clear();
    try
    {
        const format::File folded = edit::FoldTimelines(*source.file);
        std::vector<uint8_t> bytes = format::ExportArchive(folded);
        source.group_sizes = format::SizesOf(bytes, FlavorOf(*folded.native), format::GroupFilesBeside(folded)).groups;
        const std::shared_ptr<const ntdlib::sar::SoundArchive> archive =
            song::LoadArchive(std::move(bytes), source.file_name, format::ExportGroupFiles(folded));
        for (uint32_t id = 0; id < archive->FileCount(); ++id)
        {
            source.file_sizes.push_back(archive->FileData(id).size());
        }
    }
    catch (const std::exception&)
    {
        source.file_sizes.clear();
        source.group_sizes.clear();
    }

    return std::nullopt;
}

void FindSoundStarts(ReplaceSource& source)
{
    source.sound_starts.clear();
    try
    {
        if (!source.places.empty())
        {
            source.sound_starts = edit::SoundStarts(*source.file, source.places, source.incoming.sequence);
        }
    }
    catch (const std::invalid_argument&)
    {
        // The archive's sequences can't be edited. Replace… isn't offered for them.
    }
}

namespace
{

// The starts of `source.incoming`: its labels in the order of their items, after the first command when no label names
// it. When the file holds the commands of a sequence of the archive, the places where the sequence's sounds start come
// in too, among the labels. Each start then names the sounds that start there.
std::vector<ReplaceStart> StartsOf(const ReplaceSource& source)
{
    const ntdlib::commands::Sequence& incoming = source.incoming.sequence;
    std::vector<ReplaceStart> starts;
    for (const ntdlib::commands::Label& label : incoming.labels)
    {
        starts.push_back({label.name, label.item, true, {}});
    }
    std::ranges::stable_sort(starts, {}, &ReplaceStart::item);
    if (!incoming.items.empty() && (starts.empty() || starts.front().item != 0))
    {
        starts.insert(starts.begin(), {"First command in file", 0, false, {}});
    }

    // The sounds that play the same commands, at their starts.
    for (const edit::StartingSound& sound : source.sound_starts)
    {
        if (sound.item >= incoming.items.size())
        {
            continue;
        }

        auto start = std::ranges::find(starts, sound.item, &ReplaceStart::item);
        if (start == starts.end())
        {
            start = starts.insert(std::ranges::upper_bound(starts, sound.item, {}, &ReplaceStart::item),
                                  {{}, sound.item, false, {}});
        }
        start->sounds.push_back(SoundName(source, sound.sound));
    }

    // A start without a label past the first command names its sounds.
    for (ReplaceStart& start : starts)
    {
        if (!start.label && start.item != 0)
        {
            start.name = "Start of " + NameList(start.sounds);
        }
    }

    return starts;
}

// The start that the window offers first: where the sound named after the file starts, or else the label named after
// the file, or else the first label, or else the first command. Hokuto's Export… names a sequence file after its sound.
std::size_t FirstStart(const std::vector<ReplaceStart>& starts, const std::string& file_name)
{
    const std::string stem = xsflib::Utf8(xsflib::PathFromUtf8(file_name).stem());
    const auto of_sound = [&stem](const ReplaceStart& start)
    {
        return std::ranges::find(start.sounds, stem) != start.sounds.end();
    };
    const auto sound = std::ranges::find_if(starts, of_sound);
    const auto named =
        std::ranges::find_if(starts, [&stem](const ReplaceStart& start) { return start.label && start.name == stem; });
    const auto label = std::ranges::find_if(starts, &ReplaceStart::label);
    const auto first = sound != starts.end() ? sound : named != starts.end() ? named : label;
    return first != starts.end() ? static_cast<std::size_t>(first - starts.begin()) : 0;
}

// The start that a window of a sequence offers first. The song of a MIDI file and an empty song start after the
// allocation of their tracks.
std::size_t FirstStart(const std::vector<ReplaceStart>& starts, const ReplaceSource& source)
{
    const auto song_start = std::ranges::find(starts, edit::kEmptySongStart, &ReplaceStart::item);
    return (source.new_song || source.incoming.midi) && song_start != starts.end()
               ? static_cast<std::size_t>(song_start - starts.begin())
               : FirstStart(starts, source.file_name);
}

// The name of a new label at the commands from `start`: the label's name, the first sound's name where sounds start
// without a label, or else the file's name.
std::string NewLabel(const ReplaceStart& start, const std::string& file_name)
{
    if (start.label)
    {
        return start.name;
    }

    return start.item != 0 ? start.sounds.front() : xsflib::Utf8(xsflib::PathFromUtf8(file_name).stem());
}

// The rows of the programs that the commands from `start` select, with the banks in the sound's slots, or in the slots
// chosen for an import. A row takes its program from `programs`, or keeps its number.
std::vector<ProgramRow> RowsOf(const ReplaceSource& source, uint32_t start, const edit::ProgramMap& programs,
                               std::size_t& undecided)
{
    const edit::Selections selections = edit::ProgramsSelected(source.incoming.sequence, start);
    undecided = selections.undecided;
    const std::vector<uint32_t> banks = source.banks ? *source.banks : SoundBanks(source, source.sound);
    const std::vector<std::string> bank_names = BankNames(source);
    std::vector<ProgramRow> rows;
    for (const edit::Program& program : selections.programs)
    {
        ProgramRow& row = rows.emplace_back();
        row.program = program;
        const auto found = programs.find(program);
        row.target = found != programs.end() ? found->second : program.number;
        const auto slot = static_cast<std::size_t>(program.slot);
        const uint32_t id = program.slot >= 0 && slot < banks.size() ? banks[slot] : ntdlib::sar::kNoItem;
        if (id == ntdlib::sar::kNoItem || ntdlib::sar::ItemNumber(id) >= bank_names.size())
        {
            continue;
        }

        row.bank = bank_names[ntdlib::sar::ItemNumber(id)];
        try
        {
            row.choices = ProgramsOfBank(source, ntdlib::sar::ItemNumber(id));
        }
        catch (const std::exception&)
        {
            // The bank offers no programs.
        }
    }

    return rows;
}

// The rows of the variables that the commands from `start` read before they set them, and of the global ones that they
// set. A row takes its choice from `variables`, or keeps its number. A move starts at the lowest number that neither
// the new commands nor the archive's sounds use, and a fixed value at the first choice past -1, or else at 0.
std::vector<VariableRow> VariableRowsOf(const ReplaceSource& source, uint32_t start, const edit::VariableMap& variables)
{
    const ntdlib::commands::Sequence& incoming = source.incoming.sequence;
    const edit::VariableUses uses = edit::UsesOf(incoming, start);
    std::map<int, VariableRow> rows;
    for (const song::Variable& variable : song::VariablesOf(incoming, start).starting)
    {
        rows[variable.number] = VariableRow{.number = variable.number, .choices = variable.choices};
    }
    for (int number = edit::kSoundVariables; number < song::kGameVariables; number++)
    {
        if ((uses.sets >> number & 1u) != 0 && !rows.contains(number))
        {
            rows[number] = VariableRow{.number = number, .read = false};
        }
    }

    const uint64_t used = uses.reads | uses.sets;
    int unused = 0;
    while (unused < song::kGameVariables && ((used >> unused & 1u) != 0 || !source.readers[unused].empty()))
    {
        unused++;
    }

    std::vector<VariableRow> result;
    for (auto& [number, row] : rows)
    {
        const auto past = std::ranges::find_if(row.choices, [](int choice) { return choice != -1; });
        row.value = past != row.choices.end() ? *past : 0;
        row.to = unused < song::kGameVariables ? unused : number;
        if (const auto moved = variables.moves.find(number); moved != variables.moves.end())
        {
            row.choice = VariableChoice::kMove;
            row.to = moved->second;
        }
        else if (const auto fixed = variables.fixed.find(number); fixed != variables.fixed.end() && row.read)
        {
            row.choice = VariableChoice::kFix;
            row.value = fixed->second;
        }
        result.push_back(std::move(row));
    }

    return result;
}

// Whether `sound` reads variable `number` from its start.
bool Reads(const ReplaceSource& source, int number, uint32_t sound)
{
    return std::ranges::find(source.readers[static_cast<std::size_t>(number)], sound) !=
           source.readers[static_cast<std::size_t>(number)].end();
}

// The names of the sounds of the archive that read variable `number`, besides a replaced sound.
std::vector<std::string> OtherReaders(const ReplaceSource& source, int number, bool import)
{
    std::vector<std::string> names;
    for (const uint32_t sound : source.readers[static_cast<std::size_t>(number)])
    {
        if (import || sound != source.sound)
        {
            names.push_back(SoundName(source, sound));
        }
    }

    return names;
}

// The variables 0 to 31 for the menus of the moves, each with what the new commands and the archive's sounds do with
// it. `used` holds the variables that the new commands use, a bit each. A replaced sound's commands read the sound's
// variables that the game's code sets. A new sound's commands read none.
std::vector<std::string> NumberNames(const ReplaceSource& source, uint64_t used, bool import)
{
    std::vector<std::string> names;
    for (int number = 0; number < song::kGameVariables; number++)
    {
        const bool global = number >= edit::kSoundVariables;
        std::vector<std::string> notes;
        if (global)
        {
            notes.emplace_back("global");
        }
        if ((used >> number & 1u) != 0)
        {
            notes.emplace_back("used by new commands");
        }
        if (!global && !import && Reads(source, number, source.sound))
        {
            notes.emplace_back("read by this sound");
        }
        const std::size_t others = global ? OtherReaders(source, number, import).size() : 0;
        if (others > 0)
        {
            notes.push_back("read by " + std::to_string(others) + (import ? "" : " other") +
                            (others == 1 ? " sound" : " sounds"));
        }
        std::string name = "Variable " + std::to_string(number);
        for (std::size_t i = 0; i < notes.size(); i++)
        {
            name += (i == 0 ? " (" : ", ") + notes[i] + (i + 1 == notes.size() ? ")" : "");
        }
        names.push_back(std::move(name));
    }

    return names;
}

// The sound's variables that the game's code sets, a bit each: those that a replaced sound's commands read now. A new
// sound has none.
uint32_t TakenVariables(const ReplaceSource& source, bool import)
{
    uint32_t taken = 0;
    for (int number = 0; number < edit::kSoundVariables && !import; number++)
    {
        taken |= Reads(source, number, source.sound) ? 1u << number : 0u;
    }

    return taken;
}

// The warnings about the variables: those that the game's code may never set, the global ones that other sounds read,
// and the variables that hold the fixed values.
void AddVariableWarnings(const ReplaceSource& source, const std::vector<VariableRow>& rows,
                         const std::map<int, int>& holders, bool import, std::vector<std::string>& warnings)
{
    // The variables that keep their numbers or move, by their numbers in the new commands.
    std::vector<uint32_t> unset;
    for (const VariableRow& row : rows)
    {
        const int number = row.choice == VariableChoice::kMove ? row.to : row.number;
        if (row.choice == VariableChoice::kFix)
        {
            continue;
        }
        if (row.read && number < edit::kSoundVariables && (import || !Reads(source, number, source.sound)))
        {
            unset.push_back(static_cast<uint32_t>(number));
        }
        if (number < edit::kSoundVariables)
        {
            continue;
        }

        // NW4R SND starts the global variables at 0. A sequence that comes in converted for the Wii sets those that it
        // reads before it sets them to -1 at its start (edit::WithLaterGlobals).
        const std::vector<std::string> others = OtherReaders(source, number, import);
        const std::string global = "global variable " + std::to_string(number);
        const char* const stays = source.played.Wii() && !source.incoming.converted ? "0" : "-1";
        if (others.empty() && row.read)
        {
            warnings.push_back("No other sound reads " + global + ". If the game never sets it, it stays at " + stays +
                               ".");
        }
        else if (!others.empty())
        {
            warnings.push_back(std::string("The new commands ") + (row.read ? "read " : "set ") + global + ". " +
                               NameList(others) + (others.size() == 1 ? " also reads" : " also read") +
                               " it. Global variables are shared by all sounds.");
        }
    }
    if (!unset.empty())
    {
        const bool one = unset.size() == 1;
        const std::string list = (one ? "variable " : "variables ") + NumberList(unset);
        warnings.push_back(import ? "The game doesn't set variables for a new sound. The new commands read " + list +
                                        ". " + (one ? "It stays" : "They stay") +
                                        " at -1 unless the game code is changed to set " + (one ? "it." : "them.")
                                  : "The current commands don't read " + list + ". If the game never sets " +
                                        (one ? "it, it stays" : "them, they stay") + " at -1.");
    }

    // A fixed value takes a command at the start.
    for (const auto& [number, holder] : holders)
    {
        const auto row = std::ranges::find(rows, number, &VariableRow::number);
        const std::string value = row != rows.end() ? std::to_string(row->value) : "the fixed value";
        if (holder == number)
        {
            warnings.push_back("A command added at the start sets variable " + std::to_string(number) + " to " + value +
                               ".");
        }
        else
        {
            warnings.push_back("A command added at the start sets variable " + std::to_string(holder) +
                               " (in place of variable " + std::to_string(number) + ") to " + value + ". " +
                               (number >= edit::kSoundVariables
                                    ? "Global variables are shared by all sounds."
                                    : "The game sets variable " + std::to_string(number) + " for this sound."));
        }
    }
}

// Gives `view` the banks of `source`'s archive, and a menu for each bank slot with the bank of `banks` in it. A sound
// of the Wii has one slot.
void FillSlots(ReplaceView& view, const ReplaceSource& source, const std::vector<uint32_t>& banks)
{
    view.banks = BankNames(source);
    const std::size_t slots = SlotsOf(source);
    view.slots.assign(slots, -1);
    for (std::size_t slot = 0; slot < std::min(banks.size(), slots); slot++)
    {
        const uint32_t index = ntdlib::sar::ItemNumber(banks[slot]);
        if (banks[slot] != ntdlib::sar::kNoItem && index < view.banks.size())
        {
            view.slots[slot] = static_cast<int>(index);
        }
    }
}

// The files that a new sequence sound of `source`'s archive with the bank slots `slots`, bank item IDs, requires: its
// new file, under an ID past the archive's files, and the files of its banks (edit::FilesOfItem). On the Wii, a bank
// requires its file alone.
std::vector<uint32_t> NewSoundFiles(const ReplaceSource& source, std::span<const uint32_t> slots)
{
    std::vector<uint32_t> files;
    const auto add = [&files](uint32_t id)
    {
        if (std::ranges::find(files, id) == files.end())
        {
            files.push_back(id);
        }
    };
    if (source.played.Wii())
    {
        add(source.played.Wii()->FileCount());
        for (const uint32_t slot : slots)
        {
            if (slot != ntdlib::sar::kNoItem && ntdlib::sar::ItemNumber(slot) < source.played.Wii()->Banks().size())
            {
                add(source.played.Wii()->Banks()[ntdlib::sar::ItemNumber(slot)].file_id);
            }
        }
    }
    else if (source.played.Sar())
    {
        add(static_cast<uint32_t>(source.played.Sar()->FileCount()));
        for (const uint32_t slot : slots)
        {
            std::ranges::for_each(edit::FilesOfItem(*source.played.Sar(), slot), add);
        }
    }

    return files;
}

// The warning about a sequence file of another console whose commands go into an archive of the Wii, and what the
// conversion changed (edit::WiiCommandsOf).
void AddWiiConversionWarnings(const edit::Incoming& incoming, std::vector<std::string>& warnings)
{
    if (!incoming.converted)
    {
        return;
    }

    if (!incoming.midi)
    {
        warnings.push_back("This is a " + song::ConsoleName(incoming.flavor) +
                           " file. Its commands are converted for the NW4R SND build of this archive.");
    }
    warnings.insert(warnings.end(), incoming.warnings.begin(), incoming.warnings.end());
}

// Fills the groups of a replace view: those that load the sound, proposed where they are missing a file of its new
// bank slots.
void FillReplaceGroups(ReplaceView& view, const ReplaceSource& source, const std::string& sound_name)
{
    if (source.played.Sar())
    {
        const ntdlib::sar::SoundArchive& archive = *source.played.Sar();
        const uint32_t item = ntdlib::sar::ItemId(ntdlib::sar::kSoundItem, source.sound);
        const std::vector<uint32_t> files = edit::FilesOfItem(archive, item);
        FillGroups(view, source, files,
                   source.banks ? edit::FilesOfItem(archive, item, {.banks = source.banks}) : files,
                   "Proposed: groups that load " + sound_name + " but are missing one of its banks or wave archives.",
                   "No group loads " + sound_name +
                       ". If the game loads sound files through groups, it won't load this sound.");
    }
}

// Fills the starts of `view` and the rows of the chosen start: its programs, drums, variables and numbers. `undecided`
// counts the programs with no proposal. Returns nullptr, with the error set, when the file contains no commands.
const ReplaceStart* FillStartRows(ReplaceView& view, const ReplaceSource& source, std::optional<std::size_t> chosen,
                                  const edit::ProgramMap& programs, const edit::VariableMap& variables, bool import,
                                  std::size_t& undecided)
{
    view.starts = StartsOf(source);
    if (view.starts.empty())
    {
        view.error = "The file contains no commands.";
        return nullptr;
    }
    view.chosen = std::min(chosen.value_or(FirstStart(view.starts, source)), view.starts.size() - 1);
    const ReplaceStart& start = view.starts[view.chosen];
    view.programs = RowsOf(source, start.item, programs, undecided);
    FillDrumRows(view, source);
    view.variables = VariableRowsOf(source, start.item, variables);
    const edit::VariableUses uses = edit::UsesOf(source.incoming.sequence, start.item);
    view.numbers = NumberNames(source, uses.reads | uses.sets, import);

    return &start;
}

// Warns of the other sounds that start at the same point as the sound, and of those whose sequences share its file.
void WarnSharingSounds(ReplaceView& view, const ReplaceSource& source, const format::File& file, uint32_t sequence)
{
    std::vector<std::string> sharing_start;
    for (const uint32_t other : edit::SharingStart(source.places, source.sound))
    {
        sharing_start.push_back(SoundName(source, other));
    }
    std::vector<std::string> sharing_file;
    for (const uint32_t other : edit::SharingFile(source.places, source.sound))
    {
        const std::string name = SoundName(source, other);
        if (std::ranges::find(sharing_start, name) == sharing_start.end())
        {
            sharing_file.push_back(name);
        }
    }
    if (!sharing_start.empty())
    {
        const bool one = sharing_start.size() == 1;
        view.warnings.push_back(NameList(sharing_start) + (one ? " starts" : " start") +
                                " at the same point as this sound. The new sequence plays in " + (one ? "it" : "them") +
                                " too." +
                                (!source.banks ? ""
                                 : one         ? " It also gets the same banks."
                                               : " They also get the same banks."));
    }
    const bool changed = *view.result->sequences[sequence] != *file.sequences[sequence];
    if (!sharing_file.empty())
    {
        view.warnings.push_back("The sequence file also contains the sequences of " + NameList(sharing_file) + ". " +
                                (changed
                                     ? "The new commands are added after the existing ones."
                                     : "The file already contains the new commands. The sound now starts at them.") +
                                " The other sounds keep their commands.");
    }
}

// Adds the warnings of a MIDI file, or of a sequence file of another console than `flavor`.
void AddConsoleWarnings(const ReplaceSource& source, ntdlib::sar::Flavor flavor, ReplaceView& view)
{
    if (source.incoming.midi)
    {
        view.warnings.insert(view.warnings.end(), source.incoming.warnings.begin(), source.incoming.warnings.end());
    }
    else if (source.incoming.flavor != flavor)
    {
        view.warnings.push_back("This is a " + song::ConsoleName(source.incoming.flavor) +
                                " file. Its commands are converted for the " + song::ConsoleName(flavor) + ".");
        view.warnings.insert(view.warnings.end(), source.incoming.warnings.begin(), source.incoming.warnings.end());
    }
}

// Sizes the groups that contain the sequence file for the replacement, from the saved commands before and after it.
void SizeReplaceGroups(ReplaceView& view, const ReplaceSource& source, const format::File& file,
                       const ntdlib::commands::Sequence& saved_before, const ntdlib::commands::Sequence& saved_after)
{
    if (view.offers_groups && source.played.Sar() && source.sound < source.played.Sar()->Sounds().size())
    {
        try
        {
            const uint32_t id = source.played.Sar()->Sounds()[source.sound].file_id;
            const std::size_t present =
                id < source.file_sizes.size() ? source.file_sizes[id] : source.played.Sar()->FileData(id).size();
            const std::size_t new_size =
                present + edit::NewSequenceFileSize(file, saved_after) - edit::NewSequenceFileSize(file, saved_before);
            SizeGroups(view, source, {{id, new_size}});
        }
        catch (const std::exception&)
        {
            // The lines keep the sequence file at its present size.
        }
    }
}

// Sizes the new commands for `console` and the groups that take the new file, unless the view has an error already.
// Returns the size of the new commands, or 0 with the error set.
std::size_t SizeNewSequence(ReplaceView& view, const ReplaceSource& source, const format::File& file,
                            const std::vector<uint32_t>& new_files, const edit::MappedVariables& mapped,
                            const std::string& console)
{
    std::size_t size = 0;
    if (view.error.empty())
    {
        try
        {
            size = edit::Offsets(mapped.sequence, edit::ConsoleOf(file)).back();
        }
        catch (const std::exception& error)
        {
            view.error =
                "The new commands can't be written for the " + console + ". " + format::AsSentence(error.what());
        }
    }
    if (view.error.empty())
    {
        // The groups take the new file at its size.
        try
        {
            SizeGroups(view, source, {{new_files.front(), edit::NewSequenceFileSize(file, mapped.sequence)}});
        }
        catch (const std::invalid_argument& error)
        {
            view.error = error.what();
        }
    }

    return size;
}

// Warns that the new sound allocates more than track 0 when the new commands open other tracks.
void AddNewSoundTrackWarning(const edit::MappedVariables& mapped, ReplaceView& view)
{
    const uint32_t tracks = edit::TracksOpened(mapped.sequence, mapped.start);
    if (tracks != 1)
    {
        view.warnings.push_back("The new sound allocates " + TrackList(tracks) + " because the new commands open " +
                                (std::popcount(tracks) == 1 ? "it." : "them."));
    }
}

// The groups of a new sound of `source`: those that load the model, or none without one. `new_files` are the files
// that the new sound brings in.
void FillNewSoundGroups(ReplaceView& view, const ReplaceSource& source, const std::vector<uint32_t>& new_files)
{
    const ntdlib::sar::SoundArchive& archive = *source.played.Sar();
    constexpr char kNoGroup[] =
        "No group loads the new sound. If the game loads sound files through groups, it won't load this sound.";
    if (source.models.empty())
    {
        FillGroups(view, source, {}, new_files, "", kNoGroup);
    }
    else
    {
        FillGroups(
            view, source, edit::FilesOfItem(archive, ntdlib::sar::ItemId(ntdlib::sar::kSoundItem, source.sound)),
            new_files,
            "Proposed: groups that load " + archive.Sounds()[source.sound].name + ". The new sound uses its settings.",
            kNoGroup);
    }
}

// The same on the Wii: the groups with names that load the model.
void FillNewWiiSoundGroups(ReplaceView& view, const ReplaceSource& source, const std::vector<uint32_t>& new_files)
{
    const ntdlib::rsar::SoundArchive& archive = *source.played.Wii();
    const std::string none = std::string(
                                 "No named group loads the new sound. If the game loads sound files through "
                                 "named groups, it won't load this sound. ") +
                             kWiiLastGroup;
    if (source.models.empty())
    {
        FillWiiGroups(view, source, {}, new_files, "", none);
    }
    else
    {
        FillWiiGroups(
            view, source, WiiFilesOfSound(archive, source.sound), new_files,
            "Proposed: groups that load " + archive.Sounds()[source.sound].name + ". The new sound uses its settings.",
            none);
    }
}

// The new commands of an import from `start` under `name`, with the variables and the programs that `view` chooses.
// A name that the archive refuses, or commands that can't map, set the view's error.
edit::MappedVariables MapImportedSequence(ReplaceView& view, const ReplaceSource& source, const ReplaceStart& start,
                                          const std::string& name)
{
    const ntdlib::sar::SoundArchive& archive = *source.played.Sar();
    const format::NativeTables& tables = *source.file->native->tables;
    edit::MappedVariables mapped;
    const auto taken = [&tables, &archive](const std::string& each)
    {
        return NameTaken(TakenNames(tables, &archive), each);
    };
    if (!HoldsNames(source))
    {
        view.error = edit::NoNamesReason("sound");
    }
    else if (const std::optional<std::string> refusal = NameRefusal(name, "sound", taken))
    {
        view.error = *refusal;
    }
    else
    {
        try
        {
            mapped = edit::WithVariables(source.incoming.sequence, start.item, ChosenVariables(view));
            mapped.sequence = edit::WithPrograms(mapped.sequence, mapped.start, ChosenPrograms(view));
        }
        catch (const std::invalid_argument& error)
        {
            view.error = error.what();
        }
    }

    return mapped;
}

// The same on the Wii, where the sound also requires a bank. A converted sequence gives the global variables the later
// consoles' starting value, and `globals` lists them.
edit::MappedVariables MapWiiImportedSequence(ReplaceView& view, const ReplaceSource& source, const ReplaceStart& start,
                                             const std::string& name, std::vector<int>& globals)
{
    const ntdlib::rsar::SoundArchive& archive = *source.played.Wii();
    edit::MappedVariables mapped;
    const auto taken = [&source](const std::string& each)
    {
        return NameTaken(WiiTakenNames(source), each);
    };
    if (const std::optional<std::string> refusal = NameRefusal(name, "sound", taken))
    {
        view.error = *refusal;
    }
    else if (archive.Banks().empty())
    {
        view.error = "The archive has no banks. A Wii sequence sound requires a bank. Add one with Project > Add Bank…";
    }
    else if (view.slots.empty() || view.slots.front() < 0)
    {
        view.error = "A Wii sequence sound requires a bank. Choose one for the bank slot.";
    }
    else
    {
        try
        {
            mapped = edit::WithVariables(source.incoming.sequence, start.item, ChosenVariables(view));
            if (source.incoming.converted)
            {
                std::tie(mapped.start, globals) = edit::WithLaterGlobals(mapped.sequence, mapped.start);
            }
            mapped.sequence = edit::WithPrograms(mapped.sequence, mapped.start, ChosenPrograms(view));
        }
        catch (const std::invalid_argument& error)
        {
            view.error = error.what();
        }
    }

    return mapped;
}

// The warnings about the tracks of a replaced sequence. `added` contains the tracks that the sound now allocates.
// `view.left_out` contains the tracks that the new commands open and the sound doesn't allocate. With `all_tracks`, the
// sound allocates those too.
void AddReplaceTrackWarnings(ReplaceView& view, uint32_t added, bool all_tracks)
{
    if (added != 0)
    {
        view.warnings.push_back("The sound now also allocates " + TrackList(added) + " because the new commands open " +
                                (std::popcount(added) == 1 ? "it." : "them."));
    }
    if (view.left_out != 0 && !all_tracks)
    {
        const bool one = std::popcount(view.left_out) == 1;
        const std::string them = one ? "it" : "them";
        view.warnings.push_back("The sound doesn't allocate " + TrackList(view.left_out) + ". The game keeps " + them +
                                " silent even though the current commands open " + them + ". New notes on " + them +
                                " will be silent too.");
    }
}

// The change in the size of the replaced `sequence`, and the sizes of the groups that load it. An edit that the
// program can't settle sets the error and drops the result.
void AddReplacedSize(ReplaceView& view, const ReplaceSource& source, const format::File& file, uint32_t sequence)
{
    // A timeline takes the edit as the program settles it. A saved file holds a timeline with its subroutines.
    std::size_t before = 0;
    std::size_t after = 0;
    ntdlib::commands::Sequence saved_before;
    ntdlib::commands::Sequence saved_after;
    try
    {
        const edit::Settled settled = edit::SettleTimelines(file, *view.result, ReplacedSequences(source));
        saved_before = edit::SavedSequence(file, sequence);
        saved_after = edit::SavedSequence(settled.file, sequence);
        before = edit::Offsets(saved_before, edit::ConsoleOf(file)).back();
        after = edit::Offsets(saved_after, edit::ConsoleOf(file)).back();
    }
    catch (const std::invalid_argument& error)
    {
        view.error = error.what();
        view.result.reset();
        return;
    }
    view.warnings.push_back("The sequence data " + SizeChangeText(before, after, false) + ".");

    // The groups that hold the sequence file take it at its new size: its present size, and the change that the saved
    // commands make to a file that holds them (edit::NewSequenceFileSize).
    SizeReplaceGroups(view, source, file, saved_before, saved_after);
}

} // namespace

ReplaceView MakeReplaceView(const ReplaceSource& source, std::optional<std::size_t> chosen,
                            const edit::ProgramMap& programs, const edit::VariableMap& variables, bool all_tracks)
{
    // The file and its starts. On the Wii, the file keeps the sound's places in the groups, and Play waits for a
    // preview of the Wii's sequence sounds. A sequence file of another console goes into an archive of the Wii
    // converted.
    const std::string sound_name = SoundName(source, source.sound);
    const bool wii_file = source.incoming.wii && !source.incoming.converted;
    ReplaceView view;
    view.title = "Replace " + sound_name;
    view.source = source.incoming.midi
                      ? MidiSource(source)
                      : source.file_name + ": " +
                            (wii_file ? std::string("Wii") : song::ConsoleName(source.incoming.flavor)) +
                            " sequence file, " + SizeText(source.incoming.size);
    view.playable = !source.played.Wii();
    FillSlots(view, source, source.banks ? *source.banks : SoundBanks(source, source.sound));

    // The groups that load the sound and lack a file of its new bank slots.
    FillReplaceGroups(view, source, sound_name);
    std::size_t undecided = 0;
    const ReplaceStart* chosen_start = FillStartRows(view, source, chosen, programs, variables, false, undecided);
    if (chosen_start == nullptr)
    {
        return view;
    }
    const ReplaceStart& start = *chosen_start;

    // The programs, the variables, the tracks, and the replacement. A new label takes the start's name, the name of the
    // sound that starts there, or the file's for its first command.
    const format::File& file = *source.file;
    view.all_tracks = all_tracks;
    const std::string label = NewLabel(start, source.file_name);
    const uint32_t tracks = file.native->tables && source.sound < file.native->tables->sounds.size()
                                ? file.native->tables->sounds[source.sound].tracks
                            : source.played.Wii() ? source.played.Wii()->Sounds()[source.sound].allocate_track_flags
                                                  : source.played.Sar()->Sounds()[source.sound].allocate_track_flags;
    uint32_t added = 0;
    edit::MappedVariables mapped;
    std::vector<int> globals;
    try
    {
        mapped = edit::WithVariables(source.incoming.sequence, start.item, ChosenVariables(view),
                                     TakenVariables(source, false));
        if (source.incoming.converted)
        {
            std::tie(mapped.start, globals) = edit::WithLaterGlobals(mapped.sequence, mapped.start);
        }
        added = edit::TracksToAdd(file, source.places, source.sound, mapped.sequence, mapped.start);
        view.left_out = edit::TracksOpened(mapped.sequence, mapped.start) & ~(tracks | added);
        added |= all_tracks ? view.left_out : 0;
        view.result = edit::ReplaceSequence(file, source.places, source.sound, mapped.sequence, mapped.start,
                                            ChosenPrograms(view), label, added, source.banks);
    }
    catch (const std::invalid_argument& error)
    {
        view.error = error.what();
        return view;
    }

    // The other sounds that the replacement changes, and those that it leaves.
    const uint32_t sequence = source.places[source.sound]->sequence;
    WarnSharingSounds(view, source, file, sequence);

    // The console, the programs, the tracks and the size.
    const ntdlib::sar::Flavor flavor = FlavorOf(*file.native);
    if (!source.played.Wii())
    {
        AddConsoleWarnings(source, flavor, view);
    }
    AddWiiConversionWarnings(source.incoming, view.warnings);
    if (!globals.empty())
    {
        view.warnings.push_back(edit::LaterGlobalsWarning(globals));
    }
    AddProgramWarnings(view.programs, undecided, view.warnings);
    AddVariableWarnings(source, view.variables, mapped.holders, false, view.warnings);
    AddReplaceTrackWarnings(view, added, all_tracks);
    AddReplacedSize(view, source, file, sequence);

    return view;
}

ReplaceView MakeImportView(const ReplaceSource& source, std::optional<std::size_t> chosen,
                           const edit::ProgramMap& programs, const edit::VariableMap& variables,
                           const std::string& name)
{
    // The file, the name, the sounds to copy and the starts.
    const ntdlib::sar::SoundArchive& archive = *source.played.Sar();
    const format::File& file = *source.file;
    ReplaceView view;
    view.title = source.new_song ? "Add Sequence Sound" : "Import Sequence";
    view.action = source.new_song ? "Add" : "Import";
    view.import = true;
    view.playable = !source.new_song;
    if (source.new_song)
    {
        view.heading = "Sequence";
        view.source = "An empty song to write in the editors. It opens as a timeline.";
    }
    else
    {
        view.source = source.incoming.midi ? MidiSource(source)
                                           : source.file_name + ": " + song::ConsoleName(source.incoming.flavor) +
                                                 " sequence file, " + SizeText(source.incoming.size);
    }
    FillImportModels(view, source, archive, name);

    // The bank slots: the model's, or those that the user chose. The groups that load the model. Without a model, the
    // window set the slots when it opened.
    const std::vector<uint32_t> slots = source.banks            ? *source.banks
                                        : source.models.empty() ? std::vector<uint32_t>{ntdlib::sar::kNoItem}
                                                                : archive.Sounds()[source.sound].banks;
    FillSlots(view, source, slots);
    const std::vector<uint32_t> new_files = NewSoundFiles(source, slots);
    FillNewSoundGroups(view, source, new_files);
    std::size_t undecided = 0;
    const ReplaceStart* chosen_start = FillStartRows(view, source, chosen, programs, variables, true, undecided);
    if (chosen_start == nullptr)
    {
        return view;
    }
    const ReplaceStart& start = *chosen_start;

    // The name, and the new commands as the archive's console writes them. The import makes the same checks.
    const format::NativeTables& tables = *file.native->tables;
    const ntdlib::sar::Flavor flavor = FlavorOf(*file.native);
    const edit::MappedVariables mapped = MapImportedSequence(view, source, start, name);
    std::size_t size = 0;
    if (view.error.empty())
    {
        size = SizeNewSequence(view, source, file, new_files, mapped, song::ConsoleName(flavor));
    }
    view.ready = view.error.empty();
    if (!view.ready)
    {
        return view;
    }

    // The console, the programs, the tracks, the sound's number and the size.
    AddConsoleWarnings(source, flavor, view);
    AddProgramWarnings(view.programs, undecided, view.warnings);
    AddVariableWarnings(source, view.variables, mapped.holders, true, view.warnings);
    AddNewSoundTrackWarning(mapped, view);
    if (source.models.empty())
    {
        view.warnings.push_back(
            "The archive has no sequence sound to copy settings from. The new sound uses the first player, volume "
            "127, pan centre and priority 64.");
    }
    if (std::ranges::all_of(slots, [](uint32_t bank) { return bank == ntdlib::sar::kNoItem; }))
    {
        view.warnings.push_back(
            "The new sound has no banks. Notes stay silent until a bank slot has a bank with an instrument for their "
            "program. The editors show only notes that sound.");
    }
    view.warnings.push_back("The new sound is number " + std::to_string(tables.sounds.size()) +
                            Placed(tables.sounds.size(), "sounds") +
                            ". The game plays it only if the game code requests that number or name.");
    view.warnings.push_back("The new commands take up " + SizeText(size) + ".");

    return view;
}

std::string FirstName(const format::File& file, const ntdlib::sar::SoundArchive& archive, const std::string& file_name,
                      std::size_t count)
{
    const std::string stem = xsflib::Utf8(xsflib::PathFromUtf8(file_name).stem()).substr(0, 200);
    if (!file.native || !file.native->tables)
    {
        return stem;
    }
    if (!edit::HoldsNames(archive.Bytes(), archive.ReadOptions().flavor))
    {
        return "";
    }

    const std::vector<std::string> taken_names = TakenNames(*file.native->tables, &archive);
    const auto taken = [&taken_names](const std::string& each)
    {
        return NameTaken(taken_names, each);
    };
    std::string name = stem;
    for (int number = 2; std::ranges::any_of(edit::WaveSoundNames(name, count), taken); number++)
    {
        name = stem + "_" + std::to_string(number);
    }

    return name;
}

std::string FirstWiiName(const ReplaceSource& source, const std::string& file_name, std::size_t count)
{
    const std::string stem = xsflib::Utf8(xsflib::PathFromUtf8(file_name).stem()).substr(0, 200);
    const std::vector<std::string> taken_names = WiiTakenNames(source);
    const auto taken = [&taken_names](const std::string& each)
    {
        return NameTaken(taken_names, each);
    };
    std::string name = stem;
    for (int number = 2; std::ranges::any_of(edit::WaveSoundNames(name, count), taken); number++)
    {
        name = stem + "_" + std::to_string(number);
    }

    return name;
}

ReplaceView MakeWiiImportView(const ReplaceSource& source, std::optional<std::size_t> chosen,
                              const edit::ProgramMap& programs, const edit::VariableMap& variables,
                              const std::string& name)
{
    // The file, the name, the sounds to copy and the starts. A sequence file of another console goes in converted.
    const ntdlib::rsar::SoundArchive& archive = *source.played.Wii();
    const format::File& file = *source.file;
    ReplaceView view;
    view.title = source.new_song ? "Add Sequence Sound" : "Import Sequence";
    view.action = source.new_song ? "Add" : "Import";
    view.import = true;
    if (source.new_song)
    {
        view.heading = "Sequence";
        view.source = "An empty song to write in the editors. It opens as a timeline.";
    }
    else
    {
        view.source = source.incoming.midi ? MidiSource(source)
                                           : source.file_name + ": " +
                                                 (source.incoming.converted ? song::ConsoleName(source.incoming.flavor)
                                                                            : std::string("Wii")) +
                                                 " sequence file, " + SizeText(source.incoming.size);
    }
    FillImportModels(view, source, archive, name);

    // The bank slot: the model's, or the one that the user chose. Without a model, the window chose it when it opened.
    // The groups with names that load the model.
    const std::vector<uint32_t> slots = source.banks            ? *source.banks
                                        : source.models.empty() ? std::vector<uint32_t>{ntdlib::sar::kNoItem}
                                                                : SoundBanks(source, source.sound);
    FillSlots(view, source, slots);
    const std::vector<uint32_t> new_files = NewSoundFiles(source, slots);
    FillNewWiiSoundGroups(view, source, new_files);
    std::size_t undecided = 0;
    const ReplaceStart* chosen_start = FillStartRows(view, source, chosen, programs, variables, true, undecided);
    if (chosen_start == nullptr)
    {
        return Refused(source, std::move(view));
    }
    const ReplaceStart& start = *chosen_start;

    // The name, the bank, and the new commands as the archive's build of NW4R SND writes them. The import makes the
    // same checks. A converted sequence gives the global variables the later consoles' starting value.
    std::vector<int> globals;
    const edit::MappedVariables mapped = MapWiiImportedSequence(view, source, start, name, globals);
    std::size_t size = 0;
    if (view.error.empty())
    {
        size = SizeNewSequence(view, source, file, new_files, mapped, "Wii");
    }
    view.ready = view.error.empty();
    if (!view.ready)
    {
        return Refused(source, std::move(view));
    }

    // The console, the programs, the tracks, the sound's number and the size.
    AddWiiConversionWarnings(source.incoming, view.warnings);
    if (!globals.empty())
    {
        view.warnings.push_back(edit::LaterGlobalsWarning(globals));
    }
    AddProgramWarnings(view.programs, undecided, view.warnings);
    AddVariableWarnings(source, view.variables, mapped.holders, true, view.warnings);
    AddNewSoundTrackWarning(mapped, view);
    if (source.models.empty())
    {
        view.warnings.push_back(
            "The archive has no sequence sound to copy settings from. The new sound uses the first player, volume "
            "127 and priority 64.");
    }
    view.warnings.push_back("The new sound is number " + std::to_string(archive.Sounds().size()) +
                            Placed(archive.Sounds().size(), "sounds") +
                            ". The game plays it only if the game code requests that number or name.");
    view.warnings.push_back("The new commands take up " + SizeText(size) + ".");
    AddWiiGroupWarning(view);

    return Refused(source, std::move(view));
}

} // namespace hokuto::app
