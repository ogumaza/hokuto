// SPDX-License-Identifier: MIT

#include "app/replace_window.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/rsar/bank_file.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/stream_file.h"
#include "ntdlib/rsar/wave_file.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/rvl/snd/stream_sound.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/wave_file.h"
#include "xsflib/files.h"

#include "app/export_window.h"
#include "app/item_views.h"
#include "app/theme.h"
#include "app/widgets.h"
#include "audio/resampler.h"
#include "edit/banks.h"
#include "edit/bytecode.h"
#include "edit/converter.h"
#include "edit/other_console.h"
#include "edit/timelines.h"
#include "format/archive.h"
#include "format/kinds.h"
#include "song/aac.h"
#include "song/archive.h"
#include "song/items.h"
#include "song/streams.h"

namespace hokuto::app
{
namespace
{

using theme::Px;

// The ID of the Replace window. Its title names the sound.
constexpr char kReplaceWindow[] = "###Replace";

// The Replace window's margin from the edges of the main window, in points: the Export window's.
constexpr float kMargin = 32.0f;

// A sound's bank slot without a bank, the bits of an item's ID that give its index, and a sound's bank slots.
constexpr uint32_t kNoBank = 0xFFFFFFFF;
constexpr uint32_t kItemIndex = 0x00FFFFFF;
constexpr std::size_t kSlots = 4;

// The tracks of a mask, by their numbers, in a list: "track 3", "tracks 1 and 3".
std::string TrackList(uint32_t tracks)
{
    std::vector<std::string> numbers;
    for (int track = 0; track < 32; track++)
    {
        if ((tracks >> track) & 1)
        {
            numbers.push_back(std::to_string(track));
        }
    }

    return (numbers.size() == 1 ? "track " : "tracks ") + NameList(numbers);
}

// The name of sound `sound` of `source`'s archive.
std::string SoundName(const ReplaceSource& source, uint32_t sound)
{
    return source.wii ? source.wii->Sounds()[sound].name : source.archive->Sounds()[sound].name;
}

// The bank slots of sound `sound` of `source`'s archive, as bank item IDs, or kNoBank for an empty slot. A sound of the
// Wii has one slot.
std::vector<uint32_t> SoundBanks(const ReplaceSource& source, uint32_t sound)
{
    if (!source.wii)
    {
        return source.archive->Sounds()[sound].banks;
    }

    const uint32_t bank = source.wii->Sounds()[sound].bank;
    return {bank < source.wii->Banks().size() ? ntdlib::sar::ItemId(ntdlib::sar::kBankItem, bank) : kNoBank};
}

// The names of the banks of `source`'s archive, in order.
std::vector<std::string> BankNames(const ReplaceSource& source)
{
    std::vector<std::string> names;
    if (source.wii)
    {
        for (const ntdlib::rsar::BankInfo& bank : source.wii->Banks())
        {
            names.push_back(bank.name);
        }
        return names;
    }

    for (const ntdlib::sar::BankInfo& bank : source.archive->Banks())
    {
        names.push_back(bank.name);
    }

    return names;
}

// The programs of bank `bank` of `source`'s archive. Throws as song::ProgramsOf does.
std::vector<uint32_t> ProgramsOfBank(const ReplaceSource& source, uint32_t bank)
{
    return source.wii ? song::ProgramsOf(*source.wii, bank) : song::ProgramsOf(*source.archive, bank);
}

// The bank slots of a sound of `source`'s archive.
std::size_t SlotsOf(const ReplaceSource& source)
{
    return source.wii ? 1 : kSlots;
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

void FindGroups(ReplaceSource& source)
{
    if (!source.file || !source.file->native || (!source.archive && !source.wii))
    {
        return;
    }

    source.groups = source.wii ? edit::WiiGroupsOf(*source.file) : edit::GroupsOf(*source.file, *source.archive);

    // The archive that an export writes. Without it, the window measures the archive that the program plays.
    source.file_sizes.clear();
    source.group_sizes.clear();
    try
    {
        const format::File folded = edit::FoldTimelines(*source.file);
        std::vector<uint8_t> bytes = format::ExportArchive(folded);
        if (source.wii)
        {
            const std::shared_ptr<const ntdlib::rsar::SoundArchive> wii =
                song::LoadWiiArchive(std::move(bytes), source.file_name, ntdlib::rvl::snd::kRsarOptions);
            for (uint32_t id = 0; id < wii->FileCount(); ++id)
            {
                source.file_sizes.push_back(wii->FileData(id).size() + wii->WaveData(id).size());
            }
            return;
        }

        source.group_sizes = format::SizesOf(bytes, folded.native->flavor, format::GroupFilesBeside(folded)).groups;
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
        starts.insert(starts.begin(), {"The file's first command", 0, false, {}});
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
            start.name = "Where " + NameList(start.sounds) + (start.sounds.size() == 1 ? " starts" : " start");
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
        const uint32_t id = program.slot >= 0 && slot < banks.size() ? banks[slot] : kNoBank;
        if (id == kNoBank || (id & kItemIndex) >= bank_names.size())
        {
            continue;
        }

        row.bank = bank_names[id & kItemIndex];
        try
        {
            row.choices = ProgramsOfBank(source, id & kItemIndex);
        }
        catch (const std::exception&)
        {
            // The bank offers no programs.
        }
    }

    return rows;
}

// Whether the bank of a row's slot has the program chosen in the row.
bool Holds(const ProgramRow& row)
{
    return std::ranges::find(row.choices, static_cast<uint32_t>(row.target)) != row.choices.end();
}

// The warnings about the programs: those from a variable or a random range, the slots without a bank, and the programs
// that a bank lacks.
void AddProgramWarnings(const std::vector<ProgramRow>& rows, std::size_t undecided, std::vector<std::string>& warnings)
{
    if (undecided > 0)
    {
        warnings.push_back(
            std::to_string(undecided) +
            (undecided == 1 ? " program change takes its program" : " program changes take their programs") +
            " from a variable or a random range. Hokuto can't put other programs in their place.");
    }
    for (const ProgramRow& row : rows)
    {
        const std::string program = "program " + std::to_string(row.target);
        const std::string slot = "slot " + std::to_string(row.program.slot);
        if (row.bank.empty())
        {
            warnings.push_back("The sound has no bank in " + slot + ". The notes of " + program + " there are silent.");
        }
        else if (!Holds(row))
        {
            warnings.push_back(row.bank + " has no " + program + ". Its notes in " + slot + " are silent.");
        }
    }
}

// A list of numbers: "5", "5 and 6".
std::string NumberList(const std::vector<uint32_t>& numbers)
{
    std::vector<std::string> texts;
    for (const uint32_t number : numbers)
    {
        texts.push_back(std::to_string(number));
    }

    return NameList(texts);
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
            notes.emplace_back("used by the new commands");
        }
        if (!global && !import && Reads(source, number, source.sound))
        {
            notes.emplace_back("read by the sound now");
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
        const char* const stays = source.wii && !source.incoming.converted ? "0" : "-1";
        if (others.empty() && row.read)
        {
            warnings.push_back("No other sound of the archive reads " + global +
                               ". The game's code may never set it. It then stays at " + stays + ".");
        }
        else if (!others.empty())
        {
            warnings.push_back(std::string("The new commands ") + (row.read ? "read " : "set ") + global + ". " +
                               NameList(others) + (others.size() == 1 ? " reads" : " read") +
                               " it too. A global variable holds one value for every sound that plays.");
        }
    }
    if (!unset.empty())
    {
        const bool one = unset.size() == 1;
        const std::string list = (one ? "variable " : "variables ") + NumberList(unset);
        warnings.push_back(import ? "The game's code sets no variable of a new sound. The new commands read " + list +
                                        ". " + (one ? "It stays" : "They stay") +
                                        " at -1 unless a change to the "
                                        "game's code sets " +
                                        (one ? "it." : "them.")
                                  : "The sound's commands don't read " + list + " now. The game's code may never set " +
                                        (one ? "it" : "them") + ". " + (one ? "It then stays" : "They then stay") +
                                        " at -1.");
    }

    // A fixed value takes a command at the start.
    for (const auto& [number, holder] : holders)
    {
        const auto row = std::ranges::find(rows, number, &VariableRow::number);
        const std::string value = row != rows.end() ? std::to_string(row->value) : "its value";
        if (holder == number)
        {
            warnings.push_back("A command at the sound's start sets variable " + std::to_string(number) + " to " +
                               value + ".");
        }
        else
        {
            warnings.push_back("A command at the sound's start sets variable " + std::to_string(holder) + " to " +
                               value + ", in the place of variable " + std::to_string(number) + ". " +
                               (number >= edit::kSoundVariables
                                    ? "A global variable holds one value for every sound that plays."
                                    : "The game's code sets variable " + std::to_string(number) + " for the sound."));
        }
    }
}

// The line of a bank file, a wave archive file or a wave sound file that goes in: the name of the file that was read,
// `what` it is, and its console, its version and its size. `flavor` gives the archive's console.
std::string IncomingSource(const std::string& file_name, const std::string& what, const edit::IncomingFile& incoming,
                           ntdlib::sar::Flavor flavor)
{
    const std::optional<edit::ConvertedFile>& converted = incoming.converted;
    const ntdlib::sar::Flavor console = converted ? converted->flavor : flavor;
    return file_name + ": a " + what + " of the " + song::ConsoleName(console) + ", version " +
           song::VersionText(console, converted ? converted->version : incoming.version) + ", " +
           SizeText(converted ? converted->size : incoming.bytes.size());
}

// The warning about a file that goes in converted for an archive of `flavor`, or nothing: a file of the other console,
// or one of a later version than the archive's files.
std::optional<std::string> ConversionWarning(const edit::IncomingFile& incoming, ntdlib::sar::Flavor flavor)
{
    if (!incoming.converted)
    {
        return std::nullopt;
    }

    const std::optional<uint32_t>& waves = incoming.converted->wave_version;
    if (incoming.converted->flavor == flavor)
    {
        return "Hokuto writes the file again with the versions of the archive's files: " +
               song::VersionText(flavor, incoming.version) +
               (waves ? ", and " + song::VersionText(flavor, *waves) + " for its waves" : "") +
               ". The values stay the same. A game may refuse a later version than its files hold.";
    }

    std::string warning = "The file comes from the " + song::ConsoleName(incoming.converted->flavor) +
                          ". Hokuto converts it: the same values in a file of the " + song::ConsoleName(flavor) +
                          ", with version " + song::VersionText(flavor, incoming.version) + ".";
    if (waves)
    {
        warning += " Its waves take version " + song::VersionText(flavor, *waves) + ".";
    }

    return warning;
}

// The samples of a WAV file in words: "16-bit PCM" or "32-bit floating point".
std::string SamplesText(const edit::IncomingWave& incoming)
{
    return std::to_string(incoming.bits) + (incoming.floating ? "-bit floating point" : "-bit PCM");
}

// A wave's encoding in words, such as "DSP-ADPCM" or "16-bit PCM".
const char* EncodingText(ntdlib::sar::WaveEncoding encoding)
{
    switch (encoding)
    {
    case ntdlib::sar::WaveEncoding::kPcm8:
        return "8-bit PCM";
    case ntdlib::sar::WaveEncoding::kPcm16:
        return "16-bit PCM";
    case ntdlib::sar::WaveEncoding::kDspAdpcm:
        return "DSP-ADPCM";
    case ntdlib::sar::WaveEncoding::kImaAdpcm:
        break;
    }

    return "IMA-ADPCM";
}

// The encodings that the window offers for a new wave, in its menu's order.
constexpr std::array<ntdlib::sar::WaveEncoding, 2> kEncodings = {ntdlib::sar::WaveEncoding::kDspAdpcm,
                                                                 ntdlib::sar::WaveEncoding::kPcm16};

// A wave's channels in words: "mono", "stereo" or "3 channels".
std::string ChannelsText(std::size_t channels)
{
    return channels == 1 ? "mono" : channels == 2 ? "stereo" : std::to_string(channels) + " channels";
}

// A count of samples and its length at `rate`: "52,000 samples (1.08 s)".
std::string LengthText(std::size_t frames, uint32_t rate)
{
    std::array<char, 32> seconds{};
    std::snprintf(seconds.data(), seconds.size(), "%.2f", rate == 0 ? 0.0 : static_cast<double>(frames) / rate);

    return Grouped(frames) + (frames == 1 ? " sample (" : " samples (") + seconds.data() + " s)";
}

// Whether the archive of `source` holds names (edit::HoldsNames), and true without an archive. Where the archive holds
// none, the window refuses a new item that requires a name from the start, and offers no new group. The edit refuses
// such an item too.
bool HoldsNames(const ReplaceSource& source)
{
    return source.archive == nullptr || edit::HoldsNames(source.archive->Bytes(), source.archive->ReadOptions().flavor);
}

// The place of a new item with number `number` among the archive's `items`, such as "sounds", for a warning: after the
// others, or first where the archive holds none.
std::string Placed(std::size_t number, const std::string& items)
{
    return number == 0 ? ", the first in the archive" : ", after the archive's other " + items;
}

// The names of the items of the archive whose tables are `tables`, sorted, each once: its sounds, banks, wave archives,
// players and groups. With `archive`, the archive's sound groups count too, and so do the names that the browser shows
// for the archive's items. The browser names a bank or a wave archive without a name after its number, such as WARC_3.
std::vector<std::string> TakenNames(const format::NativeTables& tables, const ntdlib::sar::SoundArchive* archive)
{
    std::vector<std::string> names(tables.banks.begin(), tables.banks.end());
    names.insert(names.end(), tables.wave_archives.begin(), tables.wave_archives.end());
    names.insert(names.end(), tables.groups.begin(), tables.groups.end());

    const auto add = [&names](const auto& items)
    {
        for (const auto& item : items)
        {
            names.push_back(item.name);
        }
    };
    add(tables.sounds);
    add(tables.players);
    if (archive != nullptr)
    {
        add(archive->Sounds());
        add(archive->SoundGroups());
        add(archive->Banks());
        add(archive->WaveArchives());
    }
    std::ranges::sort(names);
    names.erase(std::ranges::unique(names).begin(), names.end());

    return names;
}

// Whether `taken`, sorted names (TakenNames), holds `name`.
bool NameTaken(std::span<const std::string> taken, const std::string& name)
{
    return std::ranges::binary_search(taken, name);
}

// A count of waves in words: "1 wave", "12 waves".
std::string WavesText(std::size_t count)
{
    return Grouped(count) + (count == 1 ? " wave" : " waves");
}

// What can become of a variable, in the order of the menu.
constexpr std::array kVariableChoices = {VariableChoice::kKeep, VariableChoice::kMove, VariableChoice::kFix};

const char* ChoiceText(VariableChoice choice)
{
    switch (choice)
    {
    case VariableChoice::kKeep:
        return "Keep";
    case VariableChoice::kMove:
        return "Move to";
    case VariableChoice::kFix:
        return "Fixed value";
    }

    return "";
}

// A fixed value in its menu. -1 is the value that a variable starts with.
std::string ValueText(int value)
{
    return value == -1 ? "-1, the starting value" : std::to_string(value);
}

// What the new commands do with the variable of `row`.
std::string UseText(const VariableRow& row)
{
    const std::string global = row.number >= edit::kSoundVariables ? "Global. " : "";
    if (!row.read)
    {
        return global + "The new commands set it.";
    }

    return global + (row.choices.empty() ? "The new commands use its value." : "The new commands compare it.");
}

// Draws `count` rows, each with a label on its left and a menu that fills the rest. `widest` is the widest label.
// `label` gives each row's label, and `menu` draws its menu and says whether the user chose another item. A choice sets
// `choice`.
template <class Label, class Menu>
void DrawRows(const char* widest, std::size_t count, const Label& label, const Menu& menu, ReplaceChoice& choice)
{
    const float menu_x = ImGui::GetCursorPosX() + ImGui::CalcTextSize(widest).x + Px(16.0f);
    for (std::size_t i = 0; i < count; i++)
    {
        ImGui::PushID(static_cast<int>(i));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label(i).c_str());
        ImGui::SameLine(menu_x);
        if (menu(i))
        {
            choice = ReplaceChoice::kChange;
        }
        ImGui::PopID();
    }
}

// Draws an import's choices under `heading`s: the new item's name and a menu of the sounds whose settings a new sound
// can copy. A field at the top of the menu filters its items by name. A change sets `choice`.
template <class Heading>
void DrawImportChoices(ReplaceView& view, const Heading& heading, ReplaceChoice& choice)
{
    // The name takes the keys as the window opens. The whole name is selected, and typing replaces it.
    ImGui::Spacing();
    heading("Name");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::IsWindowAppearing())
    {
        ImGui::SetKeyboardFocusHere();
    }
    if (ImGui::InputTextWithHint("##name", view.name_hint.c_str(), view.name.data(), view.name.size(),
                                 ImGuiInputTextFlags_AutoSelectAll))
    {
        choice = ReplaceChoice::kChange;
    }

    // The sounds whose settings a new sound can copy.
    if (!view.models.empty())
    {
        ImGui::Spacing();
        heading("Settings from");
        int model = static_cast<int>(view.model);
        if (FilteredCombo("##model", view.models, model, nullptr, view.filter))
        {
            view.model = static_cast<std::size_t>(model);
            choice = ReplaceChoice::kChange;
        }
    }
}

// Draws a menu under a heading for each wave archive that the waves of a bank or a wave sound file name. A change sets
// `choice`.
template <class Heading>
void DrawWaveArchiveRows(ReplaceView& view, const Heading& heading, ReplaceChoice& choice)
{
    ImGui::Spacing();
    heading(view.rows_heading.c_str());
    const auto label = [&view](std::size_t row)
    {
        return view.wave_archive_rows[row].label;
    };
    const auto menu = [&view](std::size_t row)
    {
        return FilteredCombo("##wave_archive", view.wave_archives, view.wave_archive_rows[row].target, nullptr,
                             view.filter);
    };
    DrawRows("Wave archive 99 (999 waves)", view.wave_archive_rows.size(), label, menu, choice);
}

// Draws a menu under a heading for each program that the sounds of a bank select and the new bank lacks. Its first item
// leaves the notes silent. A change sets `choice`. The rows open at their top in a window that is `appearing`.
template <class Heading>
void DrawProgramFixRows(ReplaceView& view, const Heading& heading, bool appearing, ReplaceChoice& choice)
{
    ImGui::Spacing();
    heading("Instruments");
    const auto label = [&view](std::size_t row)
    {
        return "Program " + std::to_string(view.program_fixes[row].program);
    };
    const auto menu = [&view](std::size_t row)
    {
        ProgramFixRow& fix = view.program_fixes[row];
        return FilteredCombo("##fix", fix.items, fix.choice, "Silent", view.filter);
    };
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, Px(220.0f)));
    if (appearing)
    {
        ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));
    }
    ImGui::BeginChild("##fixes", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AutoResizeY);
    DrawRows("Program 65535", view.program_fixes.size(), label, menu, choice);
    ImGui::EndChild();
}

// Draws the choices of a sequence's replacement under `heading`s: a menu of the file's starts, a menu for each bank
// slot, a row for each program that the new commands select, and the tracks that the sound leaves out. A field at the
// top of each menu filters its items by name. A change sets `choice`. The programs open at their top in a window that
// is `appearing`.
template <class Heading>
void DrawSequenceChoices(ReplaceView& view, const Heading& heading, bool appearing, ReplaceChoice& choice)
{
    ImGui::Spacing();
    heading("Start");
    if (!view.starts.empty())
    {
        std::vector<std::string> names;
        for (const ReplaceStart& start : view.starts)
        {
            names.push_back(start.name);
        }
        int chosen = static_cast<int>(view.chosen);
        if (FilteredCombo("##start", names, chosen, nullptr, view.filter))
        {
            view.chosen = static_cast<std::size_t>(chosen);
            choice = ReplaceChoice::kChange;
        }
    }

    // A row for each bank slot.
    if (!view.slots.empty())
    {
        ImGui::Spacing();
        heading("Banks");
        const auto label = [](std::size_t slot)
        {
            return "Slot " + std::to_string(slot);
        };
        const auto menu = [&view](std::size_t slot)
        {
            const bool changed = FilteredCombo("##slot", view.banks, view.slots[slot], "None", view.filter);
            view.slots_changed = view.slots_changed || changed;
            return changed;
        };
        DrawRows("Slot 3, program 127", view.slots.size(), label, menu, choice);
    }

    // A row for each program that the new commands select: the program in its slot, and a menu of the programs of the
    // sound's bank in that slot.
    ImGui::Spacing();
    heading("Instruments");
    if (view.programs.empty())
    {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::kTextMuted), "The new commands select no program.");
    }
    else
    {
        ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, Px(220.0f)));
        if (appearing)
        {
            ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));
        }
        ImGui::BeginChild("##programs", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AutoResizeY);
        const float label_width = ImGui::GetCursorPosX() + ImGui::CalcTextSize("Slot 3, program 127").x + Px(16.0f);
        for (std::size_t i = 0; i < view.programs.size(); i++)
        {
            ProgramRow& row = view.programs[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::AlignTextToFramePadding();
            ImGui::Text("Slot %d, program %d", row.program.slot, row.program.number);
            ImGui::SameLine(label_width);
            ImGui::SetNextItemWidth(-FLT_MIN);
            const bool held = Holds(row);
            const std::string target = row.bank.empty() ? "No bank in this slot"
                                       : held           ? row.bank + ": program " + std::to_string(row.target)
                                                        : row.bank + " has no program " + std::to_string(row.target);
            ImGui::BeginDisabled(row.bank.empty());
            if (ImGui::BeginCombo("##target", target.c_str()))
            {
                for (const uint32_t program : row.choices)
                {
                    const std::string name = "Program " + std::to_string(program);
                    if (ImGui::Selectable(name.c_str(), static_cast<int>(program) == row.target) &&
                        static_cast<int>(program) != row.target)
                    {
                        row.target = static_cast<int>(program);
                        choice = ReplaceChoice::kChange;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndChild();
    }

    // A row for each variable: a menu of what becomes of it, then the number of a move or the value of a fixed
    // variable. The menus start after the widest label. SameLine counts from the window's edge, and the labels start at
    // the cursor.
    if (!view.variables.empty())
    {
        ImGui::Spacing();
        heading("Variables");
        const ImGuiStyle& style = ImGui::GetStyle();
        float widest = ImGui::CalcTextSize("Variable 31").x;
        for (const VariableRow& row : view.variables)
        {
            widest = std::max(widest, ImGui::CalcTextSize(("Variable " + std::to_string(row.number)).c_str()).x);
        }
        const float label_width = ImGui::GetCursorPosX() + widest + Px(16.0f);
        const float choice_width =
            ImGui::CalcTextSize("Fixed value").x + style.FramePadding.x * 2.0f + ImGui::GetFrameHeight();
        for (std::size_t i = 0; i < view.variables.size(); i++)
        {
            VariableRow& row = view.variables[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::AlignTextToFramePadding();
            ImGui::Text("Variable %d", row.number);
            ImGui::SameLine(label_width);
            ImGui::SetNextItemWidth(choice_width);
            if (ImGui::BeginCombo("##choice", ChoiceText(row.choice)))
            {
                for (const VariableChoice option : kVariableChoices)
                {
                    if ((option != VariableChoice::kFix || row.read) &&
                        ImGui::Selectable(ChoiceText(option), option == row.choice) && option != row.choice)
                    {
                        row.choice = option;
                        choice = ReplaceChoice::kChange;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (row.choice == VariableChoice::kMove)
            {
                if (FilteredCombo("##to", view.numbers, row.to, nullptr, view.filter))
                {
                    choice = ReplaceChoice::kChange;
                }
            }
            else if (row.choice == VariableChoice::kFix && !row.choices.empty())
            {
                // Each choice plays otherwise. Every other value plays as one of them.
                if (ImGui::BeginCombo("##value", ValueText(row.value).c_str()))
                {
                    for (const int value : row.choices)
                    {
                        if (ImGui::Selectable(ValueText(value).c_str(), value == row.value) && value != row.value)
                        {
                            row.value = value;
                            choice = ReplaceChoice::kChange;
                        }
                    }
                    ImGui::EndCombo();
                }
            }
            else if (row.choice == VariableChoice::kFix)
            {
                PushFigures();
                const bool typed = ImGui::InputInt("##value", &row.value, 0, 0);
                ImGui::PopFont();
                if (typed)
                {
                    row.value = std::clamp(row.value, -32768, 32767);
                    choice = ReplaceChoice::kChange;
                }
            }
            else
            {
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::kTextMuted), "%s", UseText(row).c_str());
            }
            ImGui::PopID();
        }
    }

    // The tracks that the sound leaves out, and a choice to allocate them.
    if (view.left_out != 0)
    {
        ImGui::Spacing();
        heading("Tracks");
        const std::string label = "Allocate " + TrackList(view.left_out) + " too";
        if (CheckBox(label.c_str(), &view.all_tracks))
        {
            choice = ReplaceChoice::kChange;
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
        const uint32_t index = banks[slot] & kItemIndex;
        if (banks[slot] != kNoBank && index < view.banks.size())
        {
            view.slots[slot] = static_cast<int>(index);
        }
    }
}

// The name of file `id` of `source`'s archive for a warning: the name of the wave archive or the bank that it holds, or
// "the file of" a sound that plays it. Otherwise its number.
std::string FileNameOf(const ReplaceSource& source, uint32_t id)
{
    const auto name_in = [id](const auto& items) -> std::optional<std::string>
    {
        const auto found = std::ranges::find(items, id, [](const auto& item) { return item.file_id; });
        return found != items.end() && !found->name.empty() ? std::optional<std::string>(found->name) : std::nullopt;
    };

    std::optional<std::string> name;
    std::optional<std::string> sound;
    if (source.wii)
    {
        name = name_in(source.wii->Banks());
        sound = name_in(source.wii->Sounds());
    }
    else if (source.archive)
    {
        name = name_in(source.archive->WaveArchives());
        name = name ? name : name_in(source.archive->Banks());
        sound = name_in(source.archive->Sounds());
    }
    if (name)
    {
        return *name;
    }

    return sound ? "the file of " + *sound : "file " + std::to_string(id);
}

// Gives `view` how the item changes the size of each group that it can join, and the size of the files that a new group
// takes. A group's size is the size of the files that it loads. A group's size before the edit is the Export window's
// measure (format::MeasureGrowth), from the archive that an export writes (FindGroups). `sizes` gives the size of each
// file that the edit adds or changes, by its ID. That archive gives the size of each other file. A file that a group
// holds and the edit changes changes the group's size too. The line of a group names the files that it takes besides
// the item's.
void SizeGroups(ReplaceView& view, const ReplaceSource& source, const std::map<uint32_t, std::size_t>& sizes = {})
{
    const auto present = [&source](uint32_t id) -> std::size_t
    {
        if (id < source.file_sizes.size())
        {
            return source.file_sizes[id];
        }
        if (source.wii)
        {
            return source.wii->FileData(id).size() + source.wii->WaveData(id).size();
        }

        return source.archive ? source.archive->FileData(id).size() : 0;
    };
    const auto size = [&sizes, &present](uint32_t id)
    {
        const auto found = sizes.find(id);
        return found != sizes.end() ? found->second : present(id);
    };

    view.new_group_size = 0;
    for (const uint32_t id : view.item_files)
    {
        view.new_group_size += size(id);
    }

    view.group_growth.assign(view.groups.size(), "");
    for (std::size_t i = 0; i < view.groups.size() && i < source.groups.size(); i++)
    {
        const edit::GroupOption& option = view.groups[i];
        if (option.holds || !option.unavailable.empty() || option.taken.empty())
        {
            continue;
        }

        // A file that shrinks makes the sum fall. The unsigned sum wraps around and still comes out exact.
        const edit::Group& group = source.groups[i];
        std::size_t before = 0;
        for (const uint32_t id : group.files)
        {
            before += present(id);
        }
        if (group.group < source.group_sizes.size() && !source.wii)
        {
            before = static_cast<std::size_t>(source.group_sizes[group.group]);
        }
        std::size_t after = before;
        for (const uint32_t id : group.files)
        {
            after += size(id);
            after -= present(id);
        }
        std::vector<std::string> others;
        for (const uint32_t id : option.taken)
        {
            after += size(id);
            if (id != view.item_files.front())
            {
                others.push_back(FileNameOf(source, id));
            }
        }
        view.group_growth[i] = "The files of group " + option.name + " " + SizeChangeText(before, after, true) + ".";
        if (!others.empty())
        {
            view.group_growth[i] += " The group takes " + NameList(others) + " too.";
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
    if (source.wii)
    {
        add(source.wii->FileCount());
        for (const uint32_t slot : slots)
        {
            if (slot != kNoBank && (slot & kItemIndex) < source.wii->Banks().size())
            {
                add(source.wii->Banks()[slot & kItemIndex].file_id);
            }
        }
    }
    else if (source.archive)
    {
        add(static_cast<uint32_t>(source.archive->FileCount()));
        for (const uint32_t slot : slots)
        {
            std::ranges::for_each(edit::FilesOfItem(*source.archive, slot), add);
        }
    }

    return files;
}

// Gives `view` the groups of `source`'s archive for an item that requires the files `after` in the place of an item
// that required `before`, or for a new item that copies a sound that requires `before` (edit::GroupOptions). A new
// item's file comes first in `after`, under an ID past the archive's files. A group that would take a file that Hokuto
// can't read is unavailable. The item joins the groups that the user chose, or else those that the window proposes, and
// the new groups that the user made. `note` says which groups the window proposes, if it proposes any, and `none` what
// follows when no group loads the item. `sizes` gives the sizes of the files that the edit adds or changes
// (SizeGroups). Without the archive's tables, the window offers no groups.
void FillGroups(ReplaceView& view, const ReplaceSource& source, std::span<const uint32_t> before,
                std::span<const uint32_t> after, std::string note, std::string none,
                const std::map<uint32_t, std::size_t>& sizes = {})
{
    if (!source.file || !source.file->native || !source.file->native->tables || !source.archive)
    {
        return;
    }

    view.offers_groups = true;
    view.makes_groups = HoldsNames(source);
    view.new_groups = source.new_groups;
    view.taken_names = TakenNames(*source.file->native->tables, source.archive.get());
    std::vector<uint32_t> held;
    std::ranges::copy_if(after, std::back_inserter(held),
                         [&source](uint32_t id) { return id < source.archive->FileCount(); });
    view.groups = edit::GroupOptions(source.groups, before, after, edit::UnreadableFiles(*source.archive, held));
    view.item_files.assign(after.begin(), after.end());
    for (const edit::GroupOption& option : view.groups)
    {
        const bool chosen =
            source.joined ? std::ranges::find(*source.joined, option.group) != source.joined->end() : option.proposed;
        if (chosen && !option.holds && option.unavailable.empty())
        {
            view.joined.push_back(option.group);
        }
    }
    if (std::ranges::any_of(view.groups, &edit::GroupOption::proposed))
    {
        view.groups_note = std::move(note);
    }
    view.groups_none = std::move(none);
    SizeGroups(view, source, sizes);
}

// The lines that say how the item changes the size of each group that it joins, and the size of each new group's files
// (SizeGroups).
std::vector<std::string> GroupGrowthLines(const ReplaceView& view)
{
    std::vector<std::string> lines;
    for (std::size_t i = 0; i < view.groups.size() && i < view.group_growth.size(); i++)
    {
        if (!view.group_growth[i].empty() && std::ranges::find(view.joined, view.groups[i].group) != view.joined.end())
        {
            lines.push_back(view.group_growth[i]);
        }
    }
    for (const std::string& name : view.new_groups)
    {
        lines.push_back("The files of the new group " + name + " take " + SizeText(view.new_group_size) + ".");
    }

    return lines;
}

// Draws the groups under `heading`: a menu with a row for each group of the archive, and a field at its top that
// filters the rows by name. A row's check says whether the group loads the item after the edit. The user checks the
// groups that the item joins. A group that holds the item keeps its check, and a group that the edit can't reach takes
// none. A name in the field that no group has offers a new group of that name, unless another item holds the name. A
// click on the offer or Enter in the field makes the new group. Its row follows the archive's groups, checked, and a
// click on it takes the new group out again. A choice sets `view.groups_changed`.
template <class Heading>
void DrawGroupChoices(ReplaceView& view, const Heading& heading)
{
    ImGui::Spacing();
    heading("Groups");

    const auto joined = [&view](uint32_t group)
    {
        return std::ranges::find(view.joined, group) != view.joined.end();
    };

    std::vector<std::string> loading;
    for (const edit::GroupOption& option : view.groups)
    {
        if (option.holds || joined(option.group))
        {
            loading.push_back(option.name);
        }
    }
    loading.insert(loading.end(), view.new_groups.begin(), view.new_groups.end());
    const std::string preview = loading.empty() ? "None" : NameList(loading);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##groups", preview.c_str(), ImGuiComboFlags_HeightLargest))
    {
        // The field takes the keys as the menu opens, empty.
        if (ImGui::IsWindowAppearing())
        {
            view.filter.fill('\0');
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool entered =
            ImGui::InputTextWithHint("##filter", view.makes_groups ? "Search, or name a new group" : "Search",
                                     view.filter.data(), view.filter.size(), ImGuiInputTextFlags_EnterReturnsTrue);

        // A name that no group has offers a new group, unless another item holds the name. Enter makes the new group,
        // and the field keeps the keys. Without `makes_groups`, the field only filters the rows.
        const std::string typed = view.filter.data();
        const auto named = [&typed](const edit::GroupOption& option)
        {
            return option.name == typed;
        };
        const bool offered = view.makes_groups && !typed.empty() && !std::ranges::any_of(view.groups, named) &&
                             std::ranges::find(view.new_groups, typed) == view.new_groups.end();
        const bool taken = offered && NameTaken(view.taken_names, typed);
        const auto make = [&view, &typed]
        {
            view.new_groups.push_back(typed);
            view.filter.fill('\0');
            view.groups_changed = true;
        };
        if (entered && offered && !taken)
        {
            make();
            ImGui::SetKeyboardFocusHere(-1);
        }

        std::vector<std::size_t> shown;
        for (std::size_t i = 0; i < view.groups.size(); i++)
        {
            if (Matches(view.groups[i].name, view.filter.data()))
            {
                shown.push_back(i);
            }
        }
        std::vector<std::size_t> shown_new;
        for (std::size_t k = 0; k < view.new_groups.size(); k++)
        {
            if (Matches(view.new_groups[k], view.filter.data()))
            {
                shown_new.push_back(k);
            }
        }

        // The offer's row. A click on it makes the new group, and the menu stays open.
        if (offered && view.filter[0] != '\0')
        {
            const std::string label = "New group \"" + typed + "\"";
            ImGui::PushItemFlag(ImGuiItemFlags_AutoClosePopups, false);
            if (ImGui::MenuItem(label.c_str(), taken ? "name taken" : "", false, !taken))
            {
                make();
            }
            ImGui::PopItemFlag();
            if (taken && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                WrappedTooltip("The archive holds the name " + typed + " already.");
            }
        }
        else if (shown.empty() && shown_new.empty())
        {
            const bool none = view.groups.empty() && view.new_groups.empty();
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::kTextMuted), "%s",
                               !none               ? "No matches"
                               : view.makes_groups ? "No groups. Type a name for a new group."
                                                   : "No groups");
        }

        // The rows scroll under the field. A click checks or unchecks a row, and the menu stays open. The new groups
        // follow the archive's groups. Without rows, the menu ends under the field: a child of no height would fill the
        // menu.
        const std::size_t count = shown.size() + shown_new.size();
        if (count > 0)
        {
            const float rows = ImGui::GetTextLineHeightWithSpacing() * static_cast<float>(count);
            ImGui::BeginChild("##items", ImVec2(0.0f, std::min(rows, Px(260.0f))));
            ImGui::PushItemFlag(ImGuiItemFlags_AutoClosePopups, false);
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(shown.size()));
            while (clipper.Step())
            {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
                {
                    const edit::GroupOption& option = view.groups[shown[static_cast<std::size_t>(row)]];
                    ImGui::PushID(row);
                    const bool reachable = !option.holds && option.unavailable.empty();
                    const char* note = option.holds                  ? "loads it"
                                       : !option.unavailable.empty() ? "unavailable"
                                       : option.proposed             ? "proposed"
                                                                     : "";
                    if (ImGui::MenuItem(option.name.c_str(), note, option.holds || joined(option.group), reachable))
                    {
                        if (joined(option.group))
                        {
                            std::erase(view.joined, option.group);
                        }
                        else
                        {
                            view.joined.insert(std::ranges::upper_bound(view.joined, option.group), option.group);
                        }
                        view.groups_changed = true;
                    }
                    if (!option.unavailable.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    {
                        WrappedTooltip(option.unavailable);
                    }
                    ImGui::PopID();
                }
            }
            std::optional<std::size_t> dropped;
            for (const std::size_t k : shown_new)
            {
                ImGui::PushID(view.new_groups[k].c_str());
                if (ImGui::MenuItem(view.new_groups[k].c_str(), "new", true))
                {
                    dropped = k;
                }
                ImGui::PopID();
            }
            if (dropped)
            {
                view.new_groups.erase(view.new_groups.begin() + static_cast<std::ptrdiff_t>(*dropped));
                view.groups_changed = true;
            }
            ImGui::PopItemFlag();
            ImGui::EndChild();
        }
        ImGui::EndCombo();
    }

    // Which groups the window proposes, or what follows when no group loads the item. A new group loads nothing until
    // the game's code loads it.
    const std::string& note = loading.empty() ? view.groups_none : view.groups_note;
    if (!note.empty())
    {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::kTextMuted), "%s", note.c_str());
    }
    if (!view.new_groups.empty())
    {
        ImGui::TextColored(
            ImGui::ColorConvertU32ToFloat4(theme::kTextMuted), "%s",
            "The game's code loads a group by its number. Only a change to that code loads a new group.");
    }
}

// The waves of each wave archive of `archive`, or nothing for a wave archive that Hokuto can't read. Puts the wave
// archives' names in `names` for a menu, each with its waves where they're known.
std::vector<std::optional<std::size_t>> WaveArchiveMenu(const ntdlib::sar::SoundArchive& archive,
                                                        std::vector<std::string>& names)
{
    std::vector<std::optional<std::size_t>> counts;
    for (std::size_t i = 0; i < archive.WaveArchives().size(); i++)
    {
        std::optional<std::size_t>& count = counts.emplace_back();
        try
        {
            if (!archive.FileData(archive.WaveArchives()[i].file_id).empty())
            {
                count = song::WaveCountOf(archive, i);
            }
        }
        catch (const std::exception&)
        {
            // The menu shows the wave archive without its waves.
        }
        names.push_back(archive.WaveArchives()[i].name + (count ? " (" + WavesText(*count) + ")" : ""));
    }

    return counts;
}

// The rows of a bank or a wave sound file: each wave archive that its waves name (`named`), by its number in the file's
// archive, with the wave archive of `targets`.
std::vector<WaveArchiveRow> WaveArchiveRows(const std::vector<edit::NamedWaveArchive>& named,
                                            const std::vector<int>& targets)
{
    std::vector<WaveArchiveRow> rows;
    for (std::size_t k = 0; k < named.size(); k++)
    {
        WaveArchiveRow& row = rows.emplace_back();
        row.label =
            "Wave archive " + std::to_string(named[k].id & kItemIndex) + " (" + WavesText(named[k].waves.size()) + ")";
        row.target = k < targets.size() ? targets[k] : -1;
    }

    return rows;
}

// Adds to `view` the rows and warnings of the programs that a new bank lacks, from the present bank's programs that the
// new one lacks (`changes`). `incoming` holds the instruments of `fixes`. Each program that the sounds select and the
// new bank lacks takes a row (MakeFileReplaceView).
void AddProgramFixes(const ReplaceSource& source, const edit::IncomingFile& incoming, const edit::FileChanges& changes,
                     std::span<const edit::ProgramFix> fixes, ReplaceView& view)
{
    const ntdlib::sar::SoundArchive& archive = *source.archive;

    const auto them = [](const std::vector<uint32_t>& programs)
    {
        return std::string(programs.size() == 1 ? "it" : "them");
    };

    const auto programs_text = [](const std::vector<uint32_t>& programs)
    {
        return std::string(programs.size() == 1 ? "program " : "programs ") + NumberList(programs);
    };

    // Without the sounds' places, a warning lists the present bank's programs that the new one lacks, with the sounds
    // that play the bank.
    if (source.places.empty() || !source.file)
    {
        if (!changes.programs.empty())
        {
            std::vector<std::string> players;
            for (const uint32_t sound : changes.players)
            {
                players.push_back(archive.Sounds()[sound].name);
            }
            view.warnings.push_back("The new bank lacks " + programs_text(changes.programs) + " of the present bank." +
                                    (players.empty() ? std::string(" No sound has the bank in a slot.")
                                                     : " Notes on " + them(changes.programs) + " are silent in " +
                                                           NameList(players) + "."));
        }
        return;
    }

    // The present bank and the new one. A bank that ntdlib can't read holds no programs.
    const auto read = [&archive](std::span<const uint8_t> bytes)
    {
        try
        {
            return std::optional<ntdlib::sar::Bank>(ntdlib::sar::Bank::Parse(bytes, archive.ReadOptions()));
        }
        catch (const std::exception&)
        {
            return std::optional<ntdlib::sar::Bank>();
        }
    };

    const auto holds = [](const std::optional<ntdlib::sar::Bank>& bank, uint32_t program)
    {
        return bank && program < bank->instruments.size() && bank->instruments[program].has_value();
    };

    const auto fixed = [&fixes](uint32_t program)
    {
        return std::ranges::find(fixes, program, &edit::ProgramFix::program) != fixes.end();
    };

    const std::optional<ntdlib::sar::Bank> present =
        read(archive.FileData(edit::FileIdOf(archive, edit::FileKind::kBank, source.sound)));
    const std::optional<ntdlib::sar::Bank> next = read(incoming.bytes);

    // The new bank's programs, without the copies.
    std::vector<uint32_t> held;
    for (uint32_t program = 0; next && program < next->instruments.size(); program++)
    {
        if (next->instruments[program] && !fixed(program))
        {
            held.push_back(program);
        }
    }

    // A row for each program that the sounds select and the new bank lacks, with the chosen fix.
    const edit::BankSelections selections = edit::ProgramsOfBank(*source.file, archive, source.places, source.sound);
    for (const uint32_t program : selections.programs)
    {
        if (holds(next, program) && !fixed(program))
        {
            continue;
        }

        ProgramFixRow& row = view.program_fixes.emplace_back();
        row.program = program;
        if (holds(present, program))
        {
            row.items.push_back("Present bank's instrument");
            row.fixes.push_back({program, std::nullopt});
        }
        for (const uint32_t from : held)
        {
            row.items.push_back("New bank's program " + std::to_string(from));
            row.fixes.push_back({program, from});
        }
        for (std::size_t i = 0; i < row.fixes.size(); i++)
        {
            row.choice = std::ranges::find(fixes, row.fixes[i]) != fixes.end() ? static_cast<int>(i) : row.choice;
        }
    }

    // The program changes whose program comes from a value, and the present bank's other programs that the new one
    // lacks.
    if (selections.undecided > 0)
    {
        const bool one = selections.undecided == 1;
        view.warnings.push_back(std::to_string(selections.undecided) + (one ? " program change" : " program changes") +
                                " of the sounds that play the bank " + (one ? "takes its" : "take their") +
                                " program or slot from a variable or a random number. Hokuto can't tell which " +
                                "programs they select.");
    }
    std::vector<uint32_t> unselected;
    for (const uint32_t program : changes.programs)
    {
        if (std::ranges::find(selections.programs, program) == selections.programs.end())
        {
            unselected.push_back(program);
        }
    }
    if (!unselected.empty())
    {
        view.warnings.push_back("The new bank lacks " + programs_text(unselected) +
                                " of the present bank. No program " + "change of the sounds selects " +
                                them(unselected) + ".");
    }
}

// Adds to `warnings` the other wave archives with the file of the wave archive that changes, and the copies of the file
// in groups that hold other bytes (`archive_changes`).
void CopyWarnings(const ntdlib::sar::SoundArchive& archive, const edit::FileChanges& archive_changes,
                  std::vector<std::string>& warnings)
{
    const std::vector<uint32_t>& sharing = archive_changes.sharing;
    if (!sharing.empty())
    {
        std::vector<std::string> names;
        for (const uint32_t place : sharing)
        {
            names.push_back(ItemName(archive, edit::FileKind::kWaveArchive, place));
        }
        warnings.push_back(NameList(names) +
                           (sharing.size() == 1 ? " has the same file and takes" : " have the same file and take") +
                           " the new wave too.");
    }
    if (archive_changes.other_copies > 0)
    {
        const bool one = archive_changes.other_copies == 1;
        warnings.push_back((one ? std::string("A copy") : std::to_string(archive_changes.other_copies) + " copies") +
                           " of the wave archive in " + (one ? "a group holds" : "groups hold") +
                           " other bytes. The new wave archive replaces " + (one ? "that copy" : "those copies") +
                           " too.");
    }
}

// Adds to `warnings` what a WAV file holds and its wave leaves out or changes: its loops, its samples' format, the
// detail that DSP-ADPCM loses and the bytes that hold no whole frame.
void WavWarnings(const edit::IncomingWave& incoming, std::vector<std::string>& warnings, bool stream = false)
{
    const std::string item = stream ? "stream" : "wave";
    if (incoming.loops > 0 && !incoming.loop)
    {
        warnings.push_back("The WAV file's loop lies outside its samples. The new " + item + " doesn't loop.");
    }
    if (incoming.loops > 1)
    {
        warnings.push_back("The WAV file has " + std::to_string(incoming.loops) + " loops. The " + item +
                           " takes the first.");
    }
    if (incoming.loop && !incoming.loop_forward)
    {
        warnings.push_back("The WAV file's loop plays backward, or back and forth. The " + item +
                           "'s loop plays forward.");
    }
    if (incoming.loop && !incoming.loop_endless)
    {
        warnings.push_back(
            "The WAV file's loop plays a set number of times. The " + item +
            (stream ? "'s loop repeats until the game stops the sound." : "'s loop repeats until the note ends."));
    }
    if (incoming.after_loop > 0 && incoming.loop_from_present)
    {
        warnings.push_back("The WAV file holds " + LengthText(incoming.after_loop, incoming.wav_rate) +
                           " after the end of the present " + item + "'s loop. A " + item +
                           " plays up to the end of its loop. The new " + item + " leaves them out.");
    }
    else if (incoming.after_loop > 0)
    {
        warnings.push_back("The WAV file holds " + LengthText(incoming.after_loop, incoming.wav_rate) +
                           " after its loop's end. A " + item + " plays up to the end of its loop. The new " + item +
                           " leaves them out.");
    }
    if (incoming.rate != incoming.wav_rate)
    {
        warnings.push_back("The new " + item + " holds the WAV file's samples at " + Grouped(incoming.rate) +
                           " Hz instead of " + Grouped(incoming.wav_rate) + " Hz. A filter takes out what lies above " +
                           Grouped(incoming.rate / 2) + " Hz first. Notes play at the same pitch.");
    }
    if (incoming.loop_moved > 0)
    {
        const uint32_t start = incoming.loop_start + incoming.loop_moved;
        const std::string unit = stream ? "A stream's loop starts at a block, as NintendoWare's tools place it."
                                        : "A loop in DSP-ADPCM starts at a frame of 14 samples.";
        warnings.push_back(unit + " The loop's start moves from sample " + Grouped(incoming.loop_start) + " to " +
                           Grouped(start) + ". The " + Grouped(incoming.loop_moved) +
                           (incoming.loop_moved == 1 ? " sample" : " samples") +
                           " between repeat after the loop's end. The loop plays the same samples as before.");
    }

    // The samples.
    if (incoming.bits != 16 || incoming.floating)
    {
        warnings.push_back("The WAV file holds " + SamplesText(incoming) +
                           " samples. Hokuto converts them to 16 bits.");
    }
    // Samples of more than 16 bits lose detail in 16-bit PCM too. Those of 8 bits keep theirs. DSP-ADPCM gives some WAV
    // files back exactly, such as one of 8-bit samples.
    const bool deeper = incoming.bits > 16 || incoming.floating;
    const std::string samples = deeper ? "16-bit samples" : "samples";
    if (incoming.signal_to_noise)
    {
        std::array<char, 32> ratio{};
        std::snprintf(ratio.data(), ratio.size(), "%.1f", *incoming.signal_to_noise);
        warnings.push_back("DSP-ADPCM loses some detail: the new " + item + "'s signal-to-noise ratio is " +
                           ratio.data() + " dB. 16-bit PCM keeps the " + samples + " exactly.");
    }
    else if (incoming.encoding == ntdlib::sar::WaveEncoding::kDspAdpcm)
    {
        warnings.push_back("DSP-ADPCM keeps the " + samples + " exactly.");
    }
    if (incoming.dropped > 0)
    {
        warnings.push_back("The WAV file's samples end with " + Grouped(incoming.dropped) +
                           (incoming.dropped == 1 ? " byte" : " bytes") + " that hold no whole frame. They stay out.");
    }
}

// The line that names `file_name`, a WAV file or a wave file (`wav`), and gives what its wave `incoming` holds, for an
// archive of `flavor`. A wave file of the other console gives the console and the version of the file that was read.
std::string WaveSourceLine(const std::string& file_name, const edit::IncomingWave& incoming, bool wav,
                           ntdlib::sar::Flavor flavor)
{
    const std::optional<edit::ConvertedFile>& converted = incoming.converted;
    const std::string format = wav ? SamplesText(incoming)
                                   : "a wave of the " + song::ConsoleName(converted ? converted->flavor : flavor) +
                                         ", version " +
                                         song::VersionText(converted ? converted->flavor : flavor,
                                                           converted ? converted->version : incoming.version) +
                                         ", " + EncodingText(incoming.encoding);
    const uint32_t rate = wav ? incoming.wav_rate : incoming.rate;
    const std::size_t frames = wav ? incoming.wav_frames : incoming.frames;
    std::string line = file_name + ": " + format + ", " + ChannelsText(incoming.channels) + ", " + Grouped(rate) +
                       " Hz, " + LengthText(frames + incoming.after_loop, rate);
    if (incoming.loop && !incoming.loop_from_present)
    {
        line += ", with a loop from sample " + Grouped(wav ? incoming.wav_loop_start : incoming.loop_start) + " to " +
                Grouped(frames);
    }

    return line;
}

// The rates that a WAV file's wave can take: the WAV file's `wav_rate` first, then the present wave's and the usual
// rates below it, from the highest.
std::vector<uint32_t> RatesOf(uint32_t wav_rate, std::optional<uint32_t> present = std::nullopt)
{
    std::vector<uint32_t> lower = {44100, 32000, 24000, 22050, 16000, 11025, 8000};
    if (present)
    {
        lower.push_back(*present);
    }
    std::erase_if(lower, [wav_rate](uint32_t rate) { return rate == 0 || rate >= wav_rate; });
    std::ranges::sort(lower, std::greater<>());
    lower.erase(std::ranges::unique(lower).begin(), lower.end());
    lower.insert(lower.begin(), wav_rate);

    return lower;
}

// Adds to `warnings` what a WAV file holds and its wave leaves out (`wav`), or the conversion of a wave file for an
// archive of `flavor`: a wave file of the other console, or one of a later version.
void IncomingWaveWarnings(const edit::IncomingWave& incoming, bool wav, ntdlib::sar::Flavor flavor,
                          std::vector<std::string>& warnings)
{
    if (wav)
    {
        WavWarnings(incoming, warnings);
    }
    else if (incoming.converted && incoming.converted->flavor == flavor)
    {
        warnings.push_back("Hokuto writes the wave again in version " + song::VersionText(flavor, incoming.version) +
                           ", with the same samples. A game may refuse a later version than its waves hold.");
    }
    else if (incoming.converted)
    {
        warnings.push_back("The file comes from the " + song::ConsoleName(incoming.converted->flavor) +
                           ". Hokuto converts it: the same samples in a file of the " + song::ConsoleName(flavor) +
                           ", with version " + song::VersionText(flavor, incoming.version) + ".");
    }
}

// The version of a file of the Wii in words, such as "1.2".
std::string WiiVersionText(uint32_t version)
{
    return std::to_string(version >> 8 & 0xFF) + "." + std::to_string(version & 0xFF);
}

// The files that sound `sound` of the Wii's `archive` requires in a group, by their IDs: its file, and a sequence
// sound's bank's file (edit::AddWiiSequenceSound). None for a sound past the sound table.
std::vector<uint32_t> WiiFilesOfSound(const ntdlib::rsar::SoundArchive& archive, uint32_t sound)
{
    if (sound >= archive.Sounds().size())
    {
        return {};
    }

    const ntdlib::rsar::SoundInfo& info = archive.Sounds()[sound];
    std::vector<uint32_t> files{info.file_id};
    if (info.type == ntdlib::rsar::SoundType::kSequence && info.bank < archive.Banks().size() &&
        archive.Banks()[info.bank].file_id != info.file_id)
    {
        files.push_back(archive.Banks()[info.bank].file_id);
    }

    return files;
}

// The names of the sounds, the banks and the players of `source.wii`, and of its groups with names, sorted, each once.
// No two of them can share a name (edit::AddWiiSequenceSound).
std::vector<std::string> WiiTakenNames(const ReplaceSource& source)
{
    std::vector<std::string> names;
    const auto add = [&names](const auto& items)
    {
        for (const auto& item : items)
        {
            names.push_back(item.name);
        }
    };
    add(source.wii->Sounds());
    add(source.wii->Banks());
    add(source.wii->Players());
    add(source.groups);
    std::ranges::sort(names);
    names.erase(std::ranges::unique(names).begin(), names.end());

    return names;
}

// Gives `view` the groups with names of `source.wii` (edit::WiiGroupsOf) for a new item that requires the files `after`
// and copies a sound that requires the files `before`. FillGroups does the same for the other consoles. The new item's
// file comes first in `after`, under an ID past the archive's files. A group that loads the sound takes only the files
// that the sound doesn't require. `sizes` gives the sizes of the new files (SizeGroups). The Wii's groups keep their
// numbers: the window makes no new group there.
void FillWiiGroups(ReplaceView& view, const ReplaceSource& source, std::span<const uint32_t> before,
                   std::span<const uint32_t> after, std::string note, std::string none,
                   const std::map<uint32_t, std::size_t>& sizes = {})
{
    if (source.groups.empty())
    {
        return;
    }

    view.offers_groups = true;
    view.makes_groups = false;
    view.groups = edit::GroupOptions(source.groups, before, after);
    view.item_files.assign(after.begin(), after.end());
    for (const edit::GroupOption& option : view.groups)
    {
        const bool chosen =
            source.joined ? std::ranges::find(*source.joined, option.group) != source.joined->end() : option.proposed;
        if (chosen && !option.holds && option.unavailable.empty())
        {
            view.joined.push_back(option.group);
        }
    }
    if (std::ranges::any_of(view.groups, &edit::GroupOption::proposed))
    {
        view.groups_note = std::move(note);
    }
    view.groups_none = std::move(none);
    SizeGroups(view, source, sizes);
}

// What the window says when no group with a name loads a new item of the Wii: its file goes into the archive's last
// group (edit::AddWiiSequenceSound).
constexpr char kWiiLastGroup[] =
    "The new file goes into the archive's last group, the group without a name. It holds the files that no other group "
    "holds.";

// Adds to `view` where a new item's file goes when the archive holds no group with a name.
void AddWiiGroupWarning(ReplaceView& view)
{
    if (!view.offers_groups)
    {
        view.warnings.emplace_back(kWiiLastGroup);
    }
}

// The names of the wave data of `source.wii` for the menus of the rows, each with its waves where Hokuto reads them:
// the RWAR beside the incoming file first if `beside` says so and `source.beside` holds one, then the wave data of the
// archive's files in the order of song::WaveArchivesOf. Gives the waves of each, or nothing for wave data that Hokuto
// can't read.
std::vector<std::optional<std::size_t>> WiiWaveDataMenu(const ReplaceSource& source, bool beside,
                                                        std::vector<std::string>& names)
{
    std::vector<std::optional<std::size_t>> counts;
    if (beside && !source.beside.empty())
    {
        std::optional<std::size_t>& count = counts.emplace_back();
        try
        {
            count = edit::ReadWiiWaveData(source.beside).waves.size();
        }
        catch (const std::invalid_argument&)
        {
            // The menu shows the file without its waves. The import says why it can't take them.
        }
        names.push_back(source.beside_name + ", beside the file" + (count ? " (" + WavesText(*count) + ")" : ""));
    }
    for (const song::WiiWaveArchive& wave_archive : song::WaveArchivesOf(*source.wii))
    {
        std::optional<std::size_t>& count = counts.emplace_back();
        try
        {
            count = song::WaveCountOf(*source.wii, wave_archive.file_id);
        }
        catch (const std::exception&)
        {
            // The menu shows the wave data without its waves.
        }
        names.push_back((beside ? "A copy of the waves of " : "") + wave_archive.name +
                        (count ? " (" + WavesText(*count) + ")" : ""));
    }

    return counts;
}

// The warning about a sequence file of another console whose commands go into an archive of the Wii, and what the
// conversion changed (edit::WiiCommandsOf).
void AddWiiConversionWarnings(const edit::Incoming& incoming, std::vector<std::string>& warnings)
{
    if (!incoming.converted)
    {
        return;
    }

    warnings.push_back("The file comes from the " + song::ConsoleName(incoming.flavor) +
                       ". Its commands go in as the archive's build of NW4R SND reads them.");
    warnings.insert(warnings.end(), incoming.warnings.begin(), incoming.warnings.end());
}

// Adds to `warnings` what a WAV file holds and its wave leaves out (`wav`), or the conversion of a wave file of the
// 3DS, the Wii U or the Switch for the Wii.
void WiiWaveWarnings(const edit::IncomingWave& incoming, bool wav, std::vector<std::string>& warnings)
{
    if (wav)
    {
        WavWarnings(incoming, warnings);
    }
    else if (incoming.converted)
    {
        warnings.push_back("The file comes from the " + song::ConsoleName(incoming.converted->flavor) +
                           ". Hokuto converts it: the same samples in a wave of the Wii, with the version of the wave "
                           "data's waves.");
    }
}

// The line of a wave file of the Wii that goes in, or of one of another console that goes in converted.
std::string WiiWaveSourceLine(const std::string& file_name, const edit::IncomingWave& incoming)
{
    if (incoming.converted)
    {
        return WaveSourceLine(file_name, incoming, false, incoming.converted->flavor);
    }

    std::string line = file_name + ": a wave of the Wii, version " + WiiVersionText(incoming.version) + ", " +
                       EncodingText(incoming.encoding) + ", " + ChannelsText(incoming.channels) + ", " +
                       Grouped(incoming.rate) + " Hz, " + LengthText(incoming.frames, incoming.rate);
    if (incoming.loop)
    {
        line += ", with a loop from sample " + Grouped(incoming.loop_start) + " to " + Grouped(incoming.frames);
    }

    return line;
}

// The line of a bank file, a wave archive file or a wave sound file that goes into an archive of the Wii: the name of
// the file that was read, what it is, and its console, its version and its size.
std::string WiiIncomingSource(const ReplaceSource& source, const edit::IncomingFile& incoming)
{
    const std::string kind = edit::NameOf(incoming.kind);
    const std::string file = incoming.kind == edit::FileKind::kWaveSoundFile ? kind : kind + " file";
    const std::optional<edit::ConvertedFile>& converted = incoming.converted;
    if (converted && !converted->wii)
    {
        return source.file_name + ": a " + file + " of the " + song::ConsoleName(converted->flavor) + ", version " +
               song::VersionText(converted->flavor, converted->version) + ", " + SizeText(converted->size);
    }
    if (source.later)
    {
        return source.file_name + ": a " + file + " of the " + song::ConsoleName(*source.later) + ", version " +
               song::VersionText(*source.later, incoming.version) + ", " + SizeText(incoming.bytes.size());
    }

    return source.file_name + ": a " + file + " of the Wii, version " +
           WiiVersionText(converted ? converted->version : incoming.version) + ", " +
           SizeText(converted ? converted->size : incoming.bytes.size());
}

// The warnings about the waves that the wave data of `targets` lacks for a file of another console, by the rows of
// `named` (edit::WiiFileOf). `counts` gives the waves of each wave data of the menu.
void AddLackedWiiWaves(const ReplaceSource& source, const edit::IncomingFile& incoming,
                       const std::vector<edit::NamedWaveArchive>& named, const std::vector<int>& targets,
                       const std::vector<std::optional<std::size_t>>& counts, std::vector<std::string>& warnings)
{
    const std::vector<song::WiiWaveArchive> wave_data = song::WaveArchivesOf(*source.wii);
    const std::string player = incoming.kind == edit::FileKind::kBank ? "The bank" : "The file";
    for (std::size_t k = 0; k < named.size() && k < targets.size(); k++)
    {
        const auto target = static_cast<std::size_t>(targets[k]);
        if (targets[k] < 0 || target >= wave_data.size() || target >= counts.size() || !counts[target])
        {
            continue;
        }

        std::vector<uint32_t> lacked;
        for (const uint32_t wave : named[k].waves)
        {
            if (wave >= *counts[target])
            {
                lacked.push_back(wave);
            }
        }
        if (!lacked.empty())
        {
            warnings.push_back(wave_data[target].name + " holds " + WavesText(*counts[target]) + ". " + player +
                               " plays " + (lacked.size() == 1 ? "wave " : "waves ") + NumberList(lacked) +
                               " of wave archive " + std::to_string(named[k].id & kItemIndex) + " too. " +
                               (lacked.size() == 1 ? "Its notes are" : "Their notes are") + " silent.");
        }
    }
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
    view.source = source.file_name + ": a sequence file of the " +
                  (wii_file ? std::string("Wii") : song::ConsoleName(source.incoming.flavor)) + ", " +
                  SizeText(source.incoming.size);
    view.playable = !source.wii;
    FillSlots(view, source, source.banks ? *source.banks : SoundBanks(source, source.sound));

    // The groups that load the sound and lack a file of its new bank slots.
    if (source.archive)
    {
        const ntdlib::sar::SoundArchive& archive = *source.archive;
        const uint32_t item = ntdlib::sar::ItemId(ntdlib::sar::kSoundItem, source.sound);
        const std::vector<uint32_t> files = edit::FilesOfItem(archive, item);
        FillGroups(
            view, source, files, source.banks ? edit::FilesOfItem(archive, item, {.banks = source.banks}) : files,
            "Proposed: the groups that load " + sound_name + " and lack a bank or a wave archive of its slots.",
            "No group loads " + sound_name + ". A game that loads its sounds' files through groups doesn't load it.");
    }
    view.starts = StartsOf(source);
    if (view.starts.empty())
    {
        view.error = "The file holds no commands.";
        return view;
    }
    view.chosen = std::min(chosen.value_or(FirstStart(view.starts, source.file_name)), view.starts.size() - 1);
    const ReplaceStart& start = view.starts[view.chosen];

    // The programs, the variables, the tracks, and the replacement. A new label takes the start's name, the name of the
    // sound that starts there, or the file's for its first command.
    const format::File& file = *source.file;
    std::size_t undecided = 0;
    view.programs = RowsOf(source, start.item, programs, undecided);
    view.variables = VariableRowsOf(source, start.item, variables);
    const edit::VariableUses uses = edit::UsesOf(source.incoming.sequence, start.item);
    view.numbers = NumberNames(source, uses.reads | uses.sets, false);
    view.all_tracks = all_tracks;
    const std::string label = NewLabel(start, source.file_name);
    const uint32_t tracks = file.native->tables && source.sound < file.native->tables->sounds.size()
                                ? file.native->tables->sounds[source.sound].tracks
                            : source.wii ? source.wii->Sounds()[source.sound].allocate_track_flags
                                         : source.archive->Sounds()[source.sound].allocate_track_flags;
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
                                " where the sound starts. The archive's tables give no other start. The new sequence "
                                "plays in " +
                                (one ? "that sound" : "those sounds") + " too." +
                                (!source.banks ? ""
                                 : one         ? " It takes the same banks."
                                               : " They take the same banks."));
    }
    const uint32_t sequence = source.places[source.sound]->sequence;
    const bool changed = *view.result->sequences[sequence] != *file.sequences[sequence];
    if (!sharing_file.empty())
    {
        view.warnings.push_back("The sound's sequence file holds the sequences of " + NameList(sharing_file) +
                                " too. " +
                                (changed ? "The new commands go after the file's commands."
                                         : "The file holds the new commands already, and the sound starts at them.") +
                                " The other sounds keep their commands.");
    }

    // The console, the programs, the tracks and the size.
    const ntdlib::sar::Flavor flavor = file.native->flavor;
    if (!source.wii && source.incoming.flavor != flavor)
    {
        view.warnings.push_back("The file comes from the " + song::ConsoleName(source.incoming.flavor) +
                                ". Its commands go in as the " + song::ConsoleName(flavor) + " writes them.");
        view.warnings.insert(view.warnings.end(), source.incoming.warnings.begin(), source.incoming.warnings.end());
    }
    AddWiiConversionWarnings(source.incoming, view.warnings);
    if (!globals.empty())
    {
        view.warnings.push_back(edit::LaterGlobalsWarning(globals));
    }
    AddProgramWarnings(view.programs, undecided, view.warnings);
    AddVariableWarnings(source, view.variables, mapped.holders, false, view.warnings);
    if (added != 0)
    {
        view.warnings.push_back("The sound allocates " + TrackList(added) + " too. The new commands open " +
                                (std::popcount(added) == 1 ? "it." : "them."));
    }
    if (view.left_out != 0 && !all_tracks)
    {
        const bool one = std::popcount(view.left_out) == 1;
        view.warnings.push_back("The sound leaves out " + TrackList(view.left_out) + ". Its present commands open " +
                                (one ? "the track" : "the tracks") + " too. The game keeps " + (one ? "it" : "them") +
                                " silent that way. The new notes on " + (one ? "it" : "them") + " are silent as well.");
    }
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
        return view;
    }
    view.warnings.push_back("The commands of the sound's sequence file " + SizeChangeText(before, after, true) + ".");

    // The groups that hold the sequence file take it at its new size: its present size, and the change that the saved
    // commands make to a file that holds them (edit::NewSequenceFileSize).
    if (view.offers_groups && source.archive && source.sound < source.archive->Sounds().size())
    {
        try
        {
            const uint32_t id = source.archive->Sounds()[source.sound].file_id;
            const std::size_t present =
                id < source.file_sizes.size() ? source.file_sizes[id] : source.archive->FileData(id).size();
            const std::size_t new_size =
                present + edit::NewSequenceFileSize(file, saved_after) - edit::NewSequenceFileSize(file, saved_before);
            SizeGroups(view, source, {{id, new_size}});
        }
        catch (const std::exception&)
        {
            // The lines keep the sequence file at its present size.
        }
    }

    return view;
}

ReplaceView MakeImportView(const ReplaceSource& source, std::optional<std::size_t> chosen,
                           const edit::ProgramMap& programs, const edit::VariableMap& variables,
                           const std::string& name)
{
    // The file, the name, the sounds to copy and the starts.
    const ntdlib::sar::SoundArchive& archive = *source.archive;
    const format::File& file = *source.file;
    ReplaceView view;
    view.title = source.new_song ? "Add Sequence Sound" : "Import Sequence";
    view.action = source.new_song ? "Add" : "Import";
    view.import = true;
    view.playable = !source.new_song;
    if (source.new_song)
    {
        view.heading = "Sequence";
        view.source = "An empty song, which the editors write. The program edits it as a timeline.";
    }
    else
    {
        view.source = source.file_name + ": a sequence file of the " + song::ConsoleName(source.incoming.flavor) +
                      ", " + SizeText(source.incoming.size);
    }
    name.copy(view.name.data(), view.name.size() - 1);
    for (const uint32_t model : source.models)
    {
        view.models.push_back(archive.Sounds()[model].name);
    }
    view.model = static_cast<std::size_t>(std::ranges::find(source.models, source.sound) - source.models.begin());

    // The bank slots: the model's, or those that the user chose. The groups that load the model. Without a model, the
    // window set the slots when it opened.
    constexpr char kNoGroup[] =
        "No group loads the new sound. A game that loads its sounds' files through groups doesn't load it.";
    const std::vector<uint32_t> slots = source.banks            ? *source.banks
                                        : source.models.empty() ? std::vector<uint32_t>{kNoBank}
                                                                : archive.Sounds()[source.sound].banks;
    FillSlots(view, source, slots);
    const std::vector<uint32_t> new_files = NewSoundFiles(source, slots);
    if (source.models.empty())
    {
        FillGroups(view, source, {}, new_files, "", kNoGroup);
    }
    else
    {
        FillGroups(view, source, edit::FilesOfItem(archive, ntdlib::sar::ItemId(ntdlib::sar::kSoundItem, source.sound)),
                   new_files,
                   "Proposed: the groups that load " + archive.Sounds()[source.sound].name +
                       ". The new sound copies its settings.",
                   kNoGroup);
    }
    view.starts = StartsOf(source);
    if (view.starts.empty())
    {
        view.error = "The file holds no commands.";
        return view;
    }
    const auto song_start = std::ranges::find(view.starts, edit::kEmptySongStart, &ReplaceStart::item);
    const std::size_t first = source.new_song && song_start != view.starts.end()
                                  ? static_cast<std::size_t>(song_start - view.starts.begin())
                                  : FirstStart(view.starts, source.file_name);
    view.chosen = std::min(chosen.value_or(first), view.starts.size() - 1);
    const ReplaceStart& start = view.starts[view.chosen];
    std::size_t undecided = 0;
    view.programs = RowsOf(source, start.item, programs, undecided);
    view.variables = VariableRowsOf(source, start.item, variables);
    const edit::VariableUses uses = edit::UsesOf(source.incoming.sequence, start.item);
    view.numbers = NumberNames(source, uses.reads | uses.sets, true);

    // The name, and the new commands as the archive's console writes them. The import makes the same checks.
    const format::NativeTables& tables = *file.native->tables;
    const ntdlib::sar::Flavor flavor = file.native->flavor;
    edit::MappedVariables mapped;
    std::size_t size = 0;
    if (!HoldsNames(source))
    {
        view.error = edit::NoNamesReason("sound");
    }
    else if (name.empty())
    {
        view.error = "Give the new sound a name.";
    }
    else if (NameTaken(TakenNames(tables, &archive), name))
    {
        view.error = "The archive holds the name " + name + " already.";
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
    if (view.error.empty())
    {
        try
        {
            size = edit::Offsets(mapped.sequence, edit::ConsoleOf(file)).back();
        }
        catch (const std::exception& error)
        {
            view.error = std::string("The new commands can't be written for the archive's console. ") +
                         format::AsSentence(error.what());
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
    view.ready = view.error.empty();
    if (!view.ready)
    {
        return view;
    }

    // The console, the programs, the tracks, the sound's number and the size.
    if (source.incoming.flavor != flavor)
    {
        view.warnings.push_back("The file comes from the " + song::ConsoleName(source.incoming.flavor) +
                                ". Its commands go in as the " + song::ConsoleName(flavor) + " writes them.");
        view.warnings.insert(view.warnings.end(), source.incoming.warnings.begin(), source.incoming.warnings.end());
    }
    AddProgramWarnings(view.programs, undecided, view.warnings);
    AddVariableWarnings(source, view.variables, mapped.holders, true, view.warnings);
    const uint32_t tracks = edit::TracksOpened(mapped.sequence, mapped.start);
    if (tracks != 1)
    {
        view.warnings.push_back("The new sound allocates " + TrackList(tracks) + ". The new commands open " +
                                (std::popcount(tracks) == 1 ? "it." : "them."));
    }
    if (source.models.empty())
    {
        view.warnings.push_back(
            "The archive holds no sequence sound to copy. The new sound plays on the archive's first player, at a "
            "volume of 127, in the centre, with priorities of 64.");
    }
    if (std::ranges::all_of(slots, [](uint32_t bank) { return bank == kNoBank; }))
    {
        view.warnings.push_back(
            "The new sound's bank slots hold no bank. A note sounds only once a slot holds a bank with an instrument "
            "for the note's program. The editors show only the notes that sound.");
    }
    view.warnings.push_back("The new sound takes number " + std::to_string(tables.sounds.size()) +
                            Placed(tables.sounds.size(), "sounds") +
                            ". The game plays it only if its code asks for that number or the sound's name.");
    view.warnings.push_back("The new commands take " + SizeText(size) + ".");

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

namespace
{

// `view` with the reason why the Wii archive of `source` takes no new item (ReplaceSource::refused), and Import grey.
ReplaceView Refused(const ReplaceSource& source, ReplaceView view)
{
    if (source.refused)
    {
        view.error = *source.refused;
        view.ready = false;
    }

    return view;
}

// Gives `view` the waves that `read` reads from a new wave archive, for Play, with a line for each in the menu of
// `plays`. A wave archive that `read` can't read plays nothing.
template <typename Read>
void AddWaves(ReplaceView& view, Read read)
{
    try
    {
        view.waves = read();
    }
    catch (const std::exception&)
    {
        view.waves.clear();
    }
    for (std::size_t i = 0; i < view.waves.size(); i++)
    {
        view.plays.push_back("Wave " + std::to_string(i));
    }
}

} // namespace

ReplaceView MakeItemImportView(const ReplaceSource& source, const edit::IncomingFile& incoming,
                               const std::vector<edit::NamedWaveArchive>& named, const std::vector<int>& targets,
                               const std::string& name)
{
    // The file, the name and the sounds to copy. A wave archive can go in without a name. Its waves play one at a time.
    const ntdlib::sar::SoundArchive& archive = *source.archive;
    const ntdlib::sar::Flavor flavor = archive.ReadOptions().flavor;
    const bool bank = incoming.kind == edit::FileKind::kBank;
    const bool wave_sounds = incoming.kind == edit::FileKind::kWaveSoundFile;
    ReplaceView view;
    view.title = source.new_bank ? "Add Bank"
                 : bank          ? "Import Bank"
                 : wave_sounds   ? "Import Wave Sound File"
                                 : "Import Wave Archive";
    view.action = source.new_bank ? "Add" : "Import";
    view.import = true;
    view.playable = bank || wave_sounds;
    if (incoming.kind == edit::FileKind::kWaveArchive)
    {
        AddWaves(view, [&incoming] { return ntdlib::sar::WaveArchiveFile::Read(incoming.bytes).waves; });
    }
    view.sequence = false;
    view.heading = bank ? "Bank file" : wave_sounds ? "Wave sound file" : "Wave archive file";
    view.source =
        source.new_bank
            ? "An empty bank. The Bank Editor opens on it, and WAV files dropped on its list of programs make its "
              "instruments."
            : IncomingSource(source.file_name, wave_sounds ? "wave sound file" : edit::NameOf(incoming.kind) + " file",
                             incoming, flavor);
    name.copy(view.name.data(), view.name.size() - 1);
    view.name_hint = bank || wave_sounds ? "" : "No name";
    for (const uint32_t model : source.models)
    {
        view.models.push_back(archive.Sounds()[model].name);
    }
    view.model = static_cast<std::size_t>(std::ranges::find(source.models, source.sound) - source.models.begin());

    // The groups that load the sound that new wave sounds copy. No sound plays a new bank or wave archive yet. The new
    // file comes first, then the wave archives that the rows choose.
    std::vector<uint32_t> new_files{static_cast<uint32_t>(archive.FileCount())};
    for (const int target : targets)
    {
        const auto index = static_cast<std::size_t>(target);
        if (target >= 0 && index < archive.WaveArchives().size() &&
            std::ranges::find(new_files, archive.WaveArchives()[index].file_id) == new_files.end())
        {
            new_files.push_back(archive.WaveArchives()[index].file_id);
        }
    }
    const std::map<uint32_t, std::size_t> new_sizes = {{new_files.front(), incoming.bytes.size()}};
    if (wave_sounds && source.sound < archive.Sounds().size())
    {
        FillGroups(view, source, edit::FilesOfItem(archive, ntdlib::sar::ItemId(ntdlib::sar::kSoundItem, source.sound)),
                   new_files,
                   "Proposed: the groups that load " + archive.Sounds()[source.sound].name +
                       ". The new sounds copy its settings.",
                   "No group loads the new sounds. A game that loads its sounds' files through groups doesn't load "
                   "them.",
                   new_sizes);
    }
    else
    {
        const std::string item = bank ? "bank" : "wave archive";
        FillGroups(view, source, {}, new_files, "",
                   "No group loads the new " + item +
                       ". Hokuto proposes none: no sound plays it yet. A game that "
                       "loads its files through groups doesn't load it.",
                   new_sizes);
    }

    // The archive's wave archives with their waves, or nothing for a wave archive that Hokuto can't read.
    const std::vector<std::optional<std::size_t>> counts = WaveArchiveMenu(archive, view.wave_archives);

    // The rows of a bank or a wave sound file.
    view.wave_archive_rows = WaveArchiveRows(named, targets);

    // A wave sound file's wave sounds, and the names of the new sounds.
    std::size_t sounds = 0;
    if (wave_sounds)
    {
        try
        {
            sounds = edit::WaveSoundCount(incoming.bytes, flavor);
        }
        catch (const std::invalid_argument& error)
        {
            view.error = error.what();
            return view;
        }
    }
    const std::vector<std::string> names =
        wave_sounds ? edit::WaveSoundNames(name, sounds) : std::vector<std::string>{name};

    // The name and the rows. The import makes the same checks.
    const format::NativeTables& tables = *source.file->native->tables;
    const auto unchosen = [](const WaveArchiveRow& row)
    {
        return row.target < 0;
    };
    const std::vector<std::string> taken_names = TakenNames(tables, &archive);
    const auto taken = [&taken_names](const std::string& each)
    {
        return !each.empty() && NameTaken(taken_names, each);
    };
    const auto first_taken = std::ranges::find_if(names, taken);
    const std::string file_kind = bank ? "bank" : "wave sound file";
    if (!HoldsNames(source) && (bank || wave_sounds || !name.empty()))
    {
        view.error = edit::NoNamesReason(bank ? "bank" : wave_sounds ? "sound" : "wave archive");
    }
    else if (wave_sounds && sounds == 0)
    {
        view.error = "The file holds no wave sound.";
    }
    else if ((bank || wave_sounds) && name.empty())
    {
        view.error = bank          ? "Give the new bank a name."
                     : sounds == 1 ? "Give the new sound a name."
                                   : "Give the new sounds a name.";
    }
    else if (first_taken != names.end())
    {
        view.error = "The archive holds the name " + *first_taken + " already.";
    }
    else if (std::ranges::any_of(view.wave_archive_rows, unchosen) && archive.WaveArchives().empty())
    {
        view.error = "The archive holds no wave archive for the " + file_kind +
                     "'s waves. File > Import > Wave Archive… brings one in.";
    }
    else if (std::ranges::any_of(view.wave_archive_rows, unchosen))
    {
        view.error = "Choose a wave archive for each one that the " + file_kind + " names.";
    }
    view.ready = view.error.empty();
    if (!view.ready)
    {
        return view;
    }

    // The waves that the chosen wave archives lack.
    const std::string player = bank ? "The bank" : "The file";
    for (std::size_t k = 0; k < named.size(); k++)
    {
        const auto target = static_cast<std::size_t>(view.wave_archive_rows[k].target);
        const std::string& target_name = archive.WaveArchives()[target].name;
        if (!counts[target])
        {
            view.warnings.push_back("Hokuto can't read " + target_name + ". The notes that play the " + file_kind +
                                    "'s waves in it may be silent.");
            continue;
        }

        std::vector<uint32_t> lacked;
        for (const uint32_t wave : named[k].waves)
        {
            if (wave >= *counts[target])
            {
                lacked.push_back(wave);
            }
        }
        if (!lacked.empty())
        {
            view.warnings.push_back(target_name + " holds " + WavesText(*counts[target]) + ". " + player + " plays " +
                                    (lacked.size() == 1 ? "wave " : "waves ") + NumberList(lacked) + " of it too. " +
                                    (lacked.size() == 1 ? "Its notes are" : "Their notes are") + " silent.");
        }
    }

    // The item's number and the size.
    if (bank)
    {
        view.warnings.push_back("The new bank takes number " + std::to_string(archive.Banks().size()) +
                                Placed(archive.Banks().size(), "banks") +
                                ". A sound plays it once one of the sound's bank slots names it. File > Import > "
                                "Sequence… chooses a new sound's banks.");
    }
    else if (wave_sounds)
    {
        const std::size_t first = archive.Sounds().size();
        view.warnings.push_back(
            sounds == 1
                ? "The new sound takes number " + std::to_string(first) + Placed(first, "sounds") +
                      ". The game plays it only if its code asks for that number or the sound's name."
                : "The new sounds " + names.front() + " to " + names.back() + " take numbers " + std::to_string(first) +
                      " to " + std::to_string(first + sounds - 1) + Placed(first, "sounds") +
                      ". The game plays them only if its code asks for those numbers or the sounds' names.");
        if (source.sound < archive.Sounds().size() &&
            archive.Sounds()[source.sound].type == ntdlib::sar::SoundType::kSequence)
        {
            view.warnings.push_back(archive.Sounds()[source.sound].name + " is a sequence sound. " +
                                    (sounds == 1 ? "The new sound takes" : "The new sounds take") +
                                    " its player, volume, pan, priorities and 3D settings.");
        }
    }
    else
    {
        const std::size_t waves = ntdlib::sar::WaveArchive::Parse(incoming.bytes, archive.ReadOptions()).waves.size();
        view.warnings.push_back("The new wave archive holds " + WavesText(waves) + ". It takes number " +
                                std::to_string(archive.WaveArchives().size()) +
                                Placed(archive.WaveArchives().size(), "wave archives") +
                                ". File > Import > Bank… plays its waves through a new bank.");
    }
    if (const std::optional<std::string> warning = ConversionWarning(incoming, flavor))
    {
        view.warnings.push_back(*warning);
    }
    view.warnings.push_back("The new file takes " + SizeText(incoming.bytes.size()) + ".");

    return view;
}

std::vector<int> FirstWaveArchives(const ntdlib::sar::SoundArchive& archive, std::span<const uint8_t> file,
                                   const std::vector<edit::NamedWaveArchive>& named,
                                   const std::vector<edit::NamedWaveArchive>& present)
{
    // A bank or a wave sound of the archive with the same file names the archive's wave archives. So does a file whose
    // wave archives the present file names too.
    const std::size_t count = archive.WaveArchives().size();
    const auto same_bank = [&archive, &file](const ntdlib::sar::BankInfo& bank)
    {
        return std::ranges::equal(archive.FileData(bank.file_id), file);
    };
    const auto same_sound = [&archive, &file](const ntdlib::sar::SoundInfo& sound)
    {
        return sound.type == ntdlib::sar::SoundType::kWaveSound &&
               std::ranges::equal(archive.FileData(sound.file_id), file);
    };
    const auto in_archive = [count](const edit::NamedWaveArchive& wave_archive)
    {
        return wave_archive.id >> 24 == ntdlib::sar::kWaveArchiveItem && (wave_archive.id & kItemIndex) < count;
    };
    const auto in_present = [&present](const edit::NamedWaveArchive& wave_archive)
    {
        return std::ranges::find(present, wave_archive.id, &edit::NamedWaveArchive::id) != present.end();
    };
    std::vector<int> targets;
    const bool same = std::ranges::any_of(archive.Banks(), same_bank) ||
                      std::ranges::any_of(archive.Sounds(), same_sound) ||
                      (!present.empty() && std::ranges::all_of(named, in_present));
    if (same && std::ranges::all_of(named, in_archive))
    {
        for (const edit::NamedWaveArchive& wave_archive : named)
        {
            targets.push_back(static_cast<int>(wave_archive.id & kItemIndex));
        }
        return targets;
    }

    // Otherwise the archive's last wave archives, in order.
    for (std::size_t k = 0; k < named.size(); k++)
    {
        const std::size_t first = count > named.size() ? count - named.size() : 0;
        targets.push_back(count == 0 ? -1 : static_cast<int>(std::min(first + k, count - 1)));
    }

    return targets;
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
    const ntdlib::rsar::SoundArchive& archive = *source.wii;
    const format::File& file = *source.file;
    ReplaceView view;
    view.title = source.new_song ? "Add Sequence Sound" : "Import Sequence";
    view.action = source.new_song ? "Add" : "Import";
    view.import = true;
    if (source.new_song)
    {
        view.heading = "Sequence";
        view.source = "An empty song, which the editors write. The program edits it as a timeline.";
    }
    else
    {
        view.source = source.file_name + ": a sequence file of the " +
                      (source.incoming.converted ? song::ConsoleName(source.incoming.flavor) : std::string("Wii")) +
                      ", " + SizeText(source.incoming.size);
    }
    name.copy(view.name.data(), view.name.size() - 1);
    for (const uint32_t model : source.models)
    {
        view.models.push_back(archive.Sounds()[model].name);
    }
    view.model = static_cast<std::size_t>(std::ranges::find(source.models, source.sound) - source.models.begin());

    // The bank slot: the model's, or the one that the user chose. Without a model, the window chose it when it opened.
    // The groups with names that load the model.
    const std::vector<uint32_t> slots = source.banks            ? *source.banks
                                        : source.models.empty() ? std::vector<uint32_t>{kNoBank}
                                                                : SoundBanks(source, source.sound);
    FillSlots(view, source, slots);
    const std::string none = std::string(
                                 "No group with a name loads the new sound. A game that loads its sounds' "
                                 "files through those groups doesn't load it. ") +
                             kWiiLastGroup;
    const std::vector<uint32_t> new_files = NewSoundFiles(source, slots);
    if (source.models.empty())
    {
        FillWiiGroups(view, source, {}, new_files, "", none);
    }
    else
    {
        FillWiiGroups(view, source, WiiFilesOfSound(archive, source.sound), new_files,
                      "Proposed: the groups that load " + archive.Sounds()[source.sound].name +
                          ". The new sound copies its settings.",
                      none);
    }
    view.starts = StartsOf(source);
    if (view.starts.empty())
    {
        view.error = "The file holds no commands.";
        return Refused(source, std::move(view));
    }
    const auto song_start = std::ranges::find(view.starts, edit::kEmptySongStart, &ReplaceStart::item);
    const std::size_t first = source.new_song && song_start != view.starts.end()
                                  ? static_cast<std::size_t>(song_start - view.starts.begin())
                                  : FirstStart(view.starts, source.file_name);
    view.chosen = std::min(chosen.value_or(first), view.starts.size() - 1);
    const ReplaceStart& start = view.starts[view.chosen];
    std::size_t undecided = 0;
    view.programs = RowsOf(source, start.item, programs, undecided);
    view.variables = VariableRowsOf(source, start.item, variables);
    const edit::VariableUses uses = edit::UsesOf(source.incoming.sequence, start.item);
    view.numbers = NumberNames(source, uses.reads | uses.sets, true);

    // The name, the bank, and the new commands as the archive's build of NW4R SND writes them. The import makes the
    // same checks. A converted sequence gives the global variables the later consoles' starting value.
    edit::MappedVariables mapped;
    std::vector<int> globals;
    std::size_t size = 0;
    if (name.empty())
    {
        view.error = "Give the new sound a name.";
    }
    else if (NameTaken(WiiTakenNames(source), name))
    {
        view.error = "The archive holds the name " + name + " already.";
    }
    else if (archive.Banks().empty())
    {
        view.error = "The archive holds no bank. A sequence sound of the Wii plays one: Project > Add Bank… adds one.";
    }
    else if (view.slots.empty() || view.slots.front() < 0)
    {
        view.error = "A sequence sound of the Wii plays a bank. Choose one for its slot.";
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
    if (view.error.empty())
    {
        try
        {
            size = edit::Offsets(mapped.sequence, edit::ConsoleOf(file)).back();
        }
        catch (const std::exception& error)
        {
            view.error =
                std::string("The new commands can't be written for the Wii. ") + format::AsSentence(error.what());
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
    const uint32_t tracks = edit::TracksOpened(mapped.sequence, mapped.start);
    if (tracks != 1)
    {
        view.warnings.push_back("The new sound allocates " + TrackList(tracks) + ". The new commands open " +
                                (std::popcount(tracks) == 1 ? "it." : "them."));
    }
    if (source.models.empty())
    {
        view.warnings.push_back(
            "The archive holds no sequence sound to copy. The new sound plays on the archive's first player, at a "
            "volume of 127, with priorities of 64.");
    }
    view.warnings.push_back("The new sound takes number " + std::to_string(archive.Sounds().size()) +
                            Placed(archive.Sounds().size(), "sounds") +
                            ". The game plays it only if its code asks for that number or the sound's name.");
    view.warnings.push_back("The new commands take " + SizeText(size) + ".");
    AddWiiGroupWarning(view);

    return Refused(source, std::move(view));
}

edit::IncomingFile WiiImportedFile(const ReplaceSource& source, const edit::IncomingFile& incoming,
                                   const std::vector<edit::NamedWaveArchive>& named, const std::vector<int>& targets)
{
    const ntdlib::rsar::SoundArchive& archive = *source.wii;
    const std::vector<song::WiiWaveArchive> wave_data = song::WaveArchivesOf(archive);
    const std::string kind = edit::NameOf(incoming.kind);
    if (source.new_bank)
    {
        return incoming;
    }

    // A wave archive's waves go in as the wave data of a new bank without instruments.
    if (incoming.kind == edit::FileKind::kWaveArchive)
    {
        std::vector<ntdlib::rsar::WaveFile> waves;
        try
        {
            for (const std::vector<uint8_t>& wave : ntdlib::rsar::WaveArchiveFile::Read(incoming.bytes).waves)
            {
                waves.push_back(ntdlib::rsar::WaveFile::Read(wave));
            }
        }
        catch (const std::exception& error)
        {
            throw std::invalid_argument(std::string("Hokuto can't read the wave archive file's waves. ") +
                                        format::AsSentence(error.what()));
        }
        ntdlib::rsar::BankFile::Files files = edit::EmptyBank(archive, std::move(waves));
        return {edit::FileKind::kBank, edit::NewWiiFileVersion(archive, edit::FileKind::kBank), std::move(files.file),
                incoming.converted, std::move(files.wave_data)};
    }

    // A file of another console takes the wave data chosen in each row.
    if (source.later)
    {
        std::map<uint32_t, uint32_t> map;
        for (std::size_t k = 0; k < named.size(); k++)
        {
            const int target = k < targets.size() ? targets[k] : -1;
            if (target < 0 || static_cast<std::size_t>(target) >= wave_data.size())
            {
                throw std::invalid_argument("Choose wave data of the archive for each wave archive that the " + kind +
                                            " names.");
            }
            map[named[k].id] = wave_data[static_cast<std::size_t>(target)].file_id;
        }
        return edit::WiiFileOf(incoming, *source.later, archive, map, edit::NewWiiFileVersion(archive, incoming.kind));
    }

    // A file of the Wii takes the RWAR beside it, or a copy of the waves of a file of the archive.
    const int target = targets.empty() ? -1 : targets.front();
    const std::size_t beside = source.beside.empty() ? 0 : 1;
    if (target < 0 || static_cast<std::size_t>(target) >= wave_data.size() + beside)
    {
        throw std::invalid_argument("Choose the " + kind + "'s wave data.");
    }
    const auto index = static_cast<std::size_t>(target);
    const edit::WiiWaveData data = index < beside ? edit::ReadWiiWaveData(source.beside)
                                                  : edit::WiiWaveDataOf(archive, wave_data[index - beside].file_id);
    return edit::WiiFileWithWaves(incoming, data, edit::NewWiiFileVersion(archive, incoming.kind));
}

std::vector<int> FirstWiiWaveData(const ReplaceSource& source, const edit::IncomingFile& incoming,
                                  const std::vector<edit::NamedWaveArchive>& named, std::optional<uint32_t> present)
{
    if (source.new_bank || incoming.kind == edit::FileKind::kWaveArchive)
    {
        return {};
    }

    // The place of a file's wave data in the menus.
    const ntdlib::rsar::SoundArchive& archive = *source.wii;
    const std::vector<song::WiiWaveArchive> wave_data = song::WaveArchivesOf(archive);
    const auto place_of = [&wave_data](uint32_t file_id)
    {
        const auto found = std::ranges::find(wave_data, file_id, &song::WiiWaveArchive::file_id);
        return found != wave_data.end() ? static_cast<int>(found - wave_data.begin()) : -1;
    };

    // A file of another console: the present file's wave data, or else the last bank's, or else the last in order. A
    // wave archive that comes in becomes the wave data of a new last bank.
    if (source.later)
    {
        int first = present ? place_of(*present) : -1;
        if (first < 0 && !archive.Banks().empty())
        {
            first = place_of(archive.Banks().back().file_id);
        }
        if (first < 0)
        {
            first = static_cast<int>(wave_data.size()) - 1;
        }
        return std::vector<int>(named.size(), first);
    }

    // A file of the Wii: the RWAR beside it, or else the wave data of a file of the archive with the same bytes.
    if (!source.beside.empty())
    {
        return {0};
    }
    for (std::size_t i = 0; i < wave_data.size(); i++)
    {
        if (std::ranges::equal(archive.FileData(wave_data[i].file_id), incoming.bytes))
        {
            return {static_cast<int>(i)};
        }
    }

    return {-1};
}

ReplaceView MakeWiiItemImportView(const ReplaceSource& source, const edit::IncomingFile& incoming,
                                  const std::vector<edit::NamedWaveArchive>& named, const std::vector<int>& targets,
                                  const std::string& name)
{
    // The file, the name and the sounds to copy. A wave archive goes in as the wave data of a new bank. The new bank
    // takes a name.
    const ntdlib::rsar::SoundArchive& archive = *source.wii;
    const bool bank = incoming.kind == edit::FileKind::kBank;
    const bool wave_sounds = incoming.kind == edit::FileKind::kWaveSoundFile;
    ReplaceView view;
    view.title = source.new_bank ? "Add Bank"
                 : bank          ? "Import Bank"
                 : wave_sounds   ? "Import Wave Sound File"
                                 : "Import Wave Archive";
    view.action = source.new_bank ? "Add" : "Import";
    view.import = true;
    view.sequence = false;
    view.heading = bank ? "Bank file" : wave_sounds ? "Wave sound file" : "Wave archive file";
    if (!source.new_bank && incoming.kind == edit::FileKind::kWaveArchive)
    {
        AddWaves(view, [&incoming] { return ntdlib::rsar::WaveArchiveFile::Read(incoming.bytes).waves; });
    }
    view.source = source.new_bank ? "An empty bank. The Bank Editor opens on it, and WAV files dropped on its list of "
                                    "programs make its instruments."
                                  : WiiIncomingSource(source, incoming);
    name.copy(view.name.data(), view.name.size() - 1);
    for (const uint32_t model : source.models)
    {
        view.models.push_back(archive.Sounds()[model].name);
    }
    view.model = static_cast<std::size_t>(std::ranges::find(source.models, source.sound) - source.models.begin());

    // The groups with names that load the sound that new wave sounds copy. No sound plays a new bank yet. The new file
    // holds its wave data.
    const std::vector<uint32_t> new_files{archive.FileCount()};
    if (wave_sounds && source.sound < archive.Sounds().size())
    {
        FillWiiGroups(view, source, WiiFilesOfSound(archive, source.sound), new_files,
                      "Proposed: the groups that load " + archive.Sounds()[source.sound].name +
                          ". The new sounds copy its settings.",
                      std::string("No group with a name loads the new sounds. A game that loads its sounds' files "
                                  "through those groups doesn't load them. ") +
                          kWiiLastGroup);
    }
    else
    {
        FillWiiGroups(view, source, {}, new_files, "",
                      std::string("No group with a name loads the new bank. Hokuto proposes none: no sound plays it "
                                  "yet. ") +
                          kWiiLastGroup);
    }

    // The rows: the wave data of a bank or a wave sound file of the Wii, or wave data for each wave archive that a file
    // of another console names.
    std::vector<std::optional<std::size_t>> counts;
    if (!source.new_bank && incoming.kind != edit::FileKind::kWaveArchive)
    {
        view.rows_heading = "Wave data";
        counts = WiiWaveDataMenu(source, !source.later, view.wave_archives);
        if (source.later)
        {
            view.wave_archive_rows = WaveArchiveRows(named, targets);
        }
        else
        {
            WaveArchiveRow& row = view.wave_archive_rows.emplace_back();
            row.label = bank ? "The bank's waves" : "The file's waves";
            row.target = targets.empty() ? -1 : targets.front();
        }
    }

    // A wave sound file's wave sounds, and the names of the new sounds.
    std::size_t sounds = 0;
    if (wave_sounds)
    {
        try
        {
            sounds = source.later ? edit::WaveSoundCount(incoming.bytes, *source.later)
                                  : ntdlib::rsar::WaveSoundFile::Parse(incoming.bytes, ntdlib::rvl::snd::kRsarOptions)
                                        .Sounds()
                                        .size();
        }
        catch (const std::exception& error)
        {
            view.error = std::string("Hokuto can't read the wave sound file. ") + format::AsSentence(error.what());
            return Refused(source, std::move(view));
        }
    }
    const std::vector<std::string> names =
        wave_sounds ? edit::WaveSoundNames(name, sounds) : std::vector<std::string>{name};

    // The file that goes in, with its wave data.
    const bool unchosen =
        std::ranges::any_of(view.wave_archive_rows, [](const WaveArchiveRow& row) { return row.target < 0; });
    edit::IncomingFile imported;
    std::string failure;
    if (!unchosen)
    {
        try
        {
            imported = WiiImportedFile(source, incoming, named, ChosenWaveArchives(view));
        }
        catch (const std::invalid_argument& error)
        {
            failure = error.what();
        }
    }

    // The name and the rows. The import makes the same checks.
    const std::vector<std::string> taken_names = WiiTakenNames(source);
    const auto taken = [&taken_names](const std::string& each)
    {
        return NameTaken(taken_names, each);
    };
    const auto first_taken = std::ranges::find_if(names, taken);
    const std::string file_kind = bank ? "bank" : "wave sound file";
    if (wave_sounds && sounds == 0)
    {
        view.error = "The file holds no wave sound.";
    }
    else if (name.empty())
    {
        view.error = !wave_sounds  ? "Give the new bank a name."
                     : sounds == 1 ? "Give the new sound a name."
                                   : "Give the new sounds a name.";
    }
    else if (first_taken != names.end())
    {
        view.error = "The archive holds the name " + *first_taken + " already.";
    }
    else if (unchosen && view.wave_archives.empty())
    {
        view.error = "The archive holds no wave data for the " + file_kind +
                     "'s waves. File > Import > Wave Archive… brings waves in as a new bank.";
    }
    else if (unchosen)
    {
        view.error = source.later
                         ? "Choose wave data of the archive for each wave archive that the " + file_kind + " names."
                         : "Choose the " + file_kind + "'s wave data.";
    }
    else if (!failure.empty())
    {
        view.error = failure;
    }
    view.ready = view.error.empty();
    if (!view.ready)
    {
        return Refused(source, std::move(view));
    }

    // The waves that the chosen wave data lacks.
    if (source.later)
    {
        AddLackedWiiWaves(source, incoming, named, ChosenWaveArchives(view), counts, view.warnings);
    }

    // The item's number.
    if (wave_sounds)
    {
        const std::size_t first = archive.Sounds().size();
        view.warnings.push_back(
            sounds == 1
                ? "The new sound takes number " + std::to_string(first) + Placed(first, "sounds") +
                      ". The game plays it only if its code asks for that number or the sound's name."
                : "The new sounds " + names.front() + " to " + names.back() + " take numbers " + std::to_string(first) +
                      " to " + std::to_string(first + sounds - 1) + Placed(first, "sounds") +
                      ". The game plays them only if its code asks for those numbers or the sounds' names.");
        if (source.sound < archive.Sounds().size() &&
            archive.Sounds()[source.sound].type == ntdlib::rsar::SoundType::kSequence)
        {
            view.warnings.push_back(archive.Sounds()[source.sound].name + " is a sequence sound. " +
                                    (sounds == 1 ? "The new sound takes" : "The new sounds take") +
                                    " its player, volume, priorities and 3D settings.");
        }
    }
    else
    {
        view.warnings.push_back("The new bank takes number " + std::to_string(archive.Banks().size()) +
                                Placed(archive.Banks().size(), "banks") +
                                ". A sound plays it once its bank slot names it. File > Import > Sequence… chooses a "
                                "new sound's bank.");
    }

    // The wave data, the conversion and the sizes.
    if (incoming.kind == edit::FileKind::kWaveArchive)
    {
        std::size_t waves = 0;
        try
        {
            waves = ntdlib::rsar::WaveArchiveFile::Read(incoming.bytes).waves.size();
        }
        catch (const std::exception&)
        {
            // WiiImportedFile read the waves already.
        }
        view.warnings.push_back(
            "The Wii keeps each wave in the wave data of a bank or a wave sound file. The wave archive's " +
            WavesText(waves) +
            " go in as the wave data of the new bank, which holds no instruments. The Bank Editor gives the waves "
            "regions, and File > Import > Bank… plays them through another bank.");
    }
    else if (!source.new_bank && !source.later && !view.wave_archive_rows.empty())
    {
        const auto target = static_cast<std::size_t>(view.wave_archive_rows.front().target);
        const bool beside = !source.beside.empty() && target == 0;
        view.warnings.push_back(
            beside ? "The new file's wave data holds the waves of " + source.beside_name + "."
                   : "The new file's wave data holds a copy of the waves of " +
                         song::WaveArchivesOf(archive)[target - (source.beside.empty() ? 0 : 1)].name + ".");
    }
    if (source.later && imported.converted)
    {
        view.warnings.push_back("The file comes from the " + song::ConsoleName(*source.later) +
                                ". Hokuto converts it: a " + edit::NameOf(incoming.kind) +
                                (wave_sounds ? "" : " file") + " of the Wii, version " +
                                WiiVersionText(imported.version) + ", with new wave data of the waves that it names.");
    }
    else if (imported.converted && imported.converted->wii)
    {
        view.warnings.push_back("Hokuto writes the file again in version " + WiiVersionText(imported.version) +
                                ", the version of the archive's files of its kind. The values stay the same. A game's "
                                "build of NW4R SND may refuse a later version.");
    }
    else if (imported.converted)
    {
        view.warnings.push_back("The file comes from the " + song::ConsoleName(imported.converted->flavor) +
                                ". Hokuto converts its waves: the same samples in waves of the Wii.");
    }
    view.warnings.push_back("The new file takes " + SizeText(imported.bytes.size()) + ", and its wave data " +
                            SizeText(imported.wave_data.size()) + ".");
    AddWiiGroupWarning(view);

    const auto padded = [](std::size_t bytes)
    {
        return (bytes + 31) / 32 * 32;
    };
    SizeGroups(view, source, {{new_files.front(), padded(imported.bytes.size()) + padded(imported.wave_data.size())}});

    return Refused(source, std::move(view));
}

ReplaceView MakeWiiNewWaveView(const ReplaceSource& source, uint32_t file_id, uint32_t wave,
                               const edit::IncomingWave& incoming, bool wav, std::optional<format::File> result,
                               std::string error)
{
    // The file, and the menus of the wave data and of a WAV file's encodings.
    const ntdlib::rsar::SoundArchive& archive = *source.wii;
    ReplaceView view;
    view.sequence = false;
    view.action = "Import";
    view.title = wav ? "Import Audio File" : "Import Wave";
    view.heading = wav ? "WAV file" : "Wave file";
    view.source = wav ? WaveSourceLine(source.file_name, incoming, true, ntdlib::sar::Flavor::kNx)
                      : WiiWaveSourceLine(source.file_name, incoming);
    if (wav)
    {
        view.encoding = incoming.encoding;
        view.rates = RatesOf(incoming.wav_rate);
        view.rate = static_cast<std::size_t>(std::ranges::find(view.rates, incoming.rate) - view.rates.begin());
    }
    view.wave = incoming.bytes;
    const std::vector<song::WiiWaveArchive> wave_data = song::WaveArchivesOf(archive);
    const auto place = std::ranges::find(wave_data, file_id, &song::WiiWaveArchive::file_id);
    view.wave_archive = place != wave_data.end() ? static_cast<int>(place - wave_data.begin()) : 0;
    WiiWaveDataMenu(source, false, view.wave_archives);
    view.result = std::move(result);
    view.error = std::move(error);
    if (!view.result)
    {
        return view;
    }

    // Where the new wave goes, the wave file of another console, a WAV file's samples, and the sizes.
    const std::string target =
        place != wave_data.end() ? place->name : "the wave data of file " + std::to_string(file_id);
    view.warnings.push_back("The new wave becomes wave " + std::to_string(wave) + " of " + target +
                            (wave == 0 ? ", its first wave." : ", after its other waves.") +
                            " No region or wave sound plays it yet.");
    WiiWaveWarnings(incoming, wav, view.warnings);
    view.warnings.push_back(std::string("The new wave is ") + EncodingText(incoming.encoding) + " and takes " +
                            SizeText(incoming.bytes.size()) + ". " + target + "'s wave data takes " +
                            SizeText(archive.WaveData(file_id).size()) +
                            " now. A game loads the wave data with its file into memory that it sets aside. More "
                            "may not fit.");

    return view;
}

std::optional<uint32_t> ChosenRate(const ReplaceView& view)
{
    return view.rate > 0 && view.rate < view.rates.size() ? std::optional<uint32_t>(view.rates[view.rate])
                                                          : std::nullopt;
}

std::span<const uint8_t> ChosenWave(const ReplaceView& view)
{
    return view.play < view.waves.size() ? std::span<const uint8_t>(view.waves[view.play]) : view.wave;
}

std::optional<edit::WaveLoop> ChosenLoop(const ReplaceView& view)
{
    return view.take_loop ? view.present_loop : std::nullopt;
}

std::vector<int> ChosenWaveArchives(const ReplaceView& view)
{
    std::vector<int> targets;
    for (const WaveArchiveRow& row : view.wave_archive_rows)
    {
        targets.push_back(row.target);
    }

    return targets;
}

std::vector<edit::ProgramFix> ChosenFixes(const ReplaceView& view)
{
    std::vector<edit::ProgramFix> fixes;
    for (const ProgramFixRow& row : view.program_fixes)
    {
        if (row.choice >= 0 && static_cast<std::size_t>(row.choice) < row.fixes.size())
        {
            fixes.push_back(row.fixes[static_cast<std::size_t>(row.choice)]);
        }
    }

    return fixes;
}

std::vector<uint32_t> ChosenBanks(const ReplaceView& view)
{
    std::size_t length = 0;
    for (std::size_t slot = 0; slot < view.slots.size(); slot++)
    {
        length = view.slots[slot] >= 0 ? slot + 1 : length;
    }

    std::vector<uint32_t> banks;
    for (std::size_t slot = 0; slot < std::min(length, view.slots.size()); slot++)
    {
        const int bank = view.slots[slot];
        banks.push_back(bank < 0 ? kNoBank : ntdlib::sar::ItemId(ntdlib::sar::kBankItem, static_cast<uint32_t>(bank)));
    }

    return banks;
}

std::string NameOf(const ReplaceView& view)
{
    return view.name.data();
}

std::string ItemName(const ntdlib::sar::SoundArchive& archive, edit::FileKind kind, uint32_t item)
{
    switch (kind)
    {
    case edit::FileKind::kBank:
        return archive.Banks()[item].name;
    case edit::FileKind::kWaveArchive:
        return archive.WaveArchives()[item].name;
    case edit::FileKind::kWaveSoundFile:
        break;
    }

    return archive.Sounds()[item].name;
}

namespace
{

// ReplacedName for either kind of archive.
template <class Archive>
std::string ReplacedNameOf(const Archive& archive, edit::FileKind kind, uint32_t item)
{
    if (kind == edit::FileKind::kWaveSoundFile)
    {
        try
        {
            return song::ItemName(archive, {.kind = song::ItemKind::kWaveSoundFile, .index = item});
        }
        catch (const std::runtime_error&)
        {
        }
    }

    return ItemName(archive, kind, item);
}

// The wave sounds of a replaced wave sound file: `sound`, then the other sounds of `sharing` in their order.
std::vector<uint32_t> WithReplacedSound(uint32_t sound, const std::vector<uint32_t>& sharing)
{
    std::vector<uint32_t> sounds = {sound};
    for (const uint32_t other : sharing)
    {
        if (other != sound)
        {
            sounds.push_back(other);
        }
    }

    return sounds;
}

// A WAV file without a loop can take the present wave's loop if the loop lies inside the WAV file's samples. Otherwise
// `view` says why the new wave can't take it. `wav` tells whether `incoming` comes from a WAV file.
void OfferPresentLoop(ReplaceView& view, const edit::IncomingWave& incoming, bool wav, const edit::WaveChanges& changes)
{
    if (!wav || incoming.loops != 0 || !changes.loop || changes.rate == 0)
    {
        return;
    }

    const edit::WaveLoop loop{
        static_cast<uint32_t>(audio::ResampledPlace(changes.loop_start, changes.rate, incoming.wav_rate)),
        static_cast<uint32_t>(audio::ResampledPlace(changes.loop_end, changes.rate, incoming.wav_rate))};
    if (loop.start < loop.end && loop.end <= incoming.wav_frames + incoming.after_loop)
    {
        view.present_loop = loop;
        view.take_loop = incoming.loop_from_present;
    }
    else
    {
        view.warnings.push_back("The present wave's loop, from sample " + Grouped(loop.start) + " to " +
                                Grouped(loop.end) + " at the WAV file's rate, ends past the WAV file's " +
                                "samples. The new wave can't take it.");
    }
}

} // namespace

std::string ReplacedName(const ntdlib::sar::SoundArchive& archive, edit::FileKind kind, uint32_t item)
{
    return ReplacedNameOf(archive, kind, item);
}

std::string ReplacedName(const ntdlib::rsar::SoundArchive& archive, edit::FileKind kind, uint32_t item)
{
    return ReplacedNameOf(archive, kind, item);
}

uint32_t ReplacedItem(edit::FileKind kind, uint32_t item)
{
    switch (kind)
    {
    case edit::FileKind::kBank:
        return ntdlib::sar::ItemId(ntdlib::sar::kBankItem, item);
    case edit::FileKind::kWaveArchive:
        return ntdlib::sar::ItemId(ntdlib::sar::kWaveArchiveItem, item);
    case edit::FileKind::kWaveSoundFile:
        break;
    }

    return ntdlib::sar::ItemId(ntdlib::sar::kSoundItem, item);
}

ReplaceView MakeFileReplaceView(const ReplaceSource& source, const edit::IncomingFile& incoming,
                                const edit::FileChanges& changes, std::optional<format::File> result, std::string error,
                                const std::vector<edit::NamedWaveArchive>& named, const std::vector<int>& targets,
                                std::span<const edit::ProgramFix> fixes)
{
    // The item, and the file that goes in its place.
    const ntdlib::sar::SoundArchive& archive = *source.archive;
    const std::string kind = edit::NameOf(incoming.kind);
    const std::string file = incoming.kind == edit::FileKind::kWaveSoundFile ? kind : kind + " file";
    ReplaceView view;
    view.sequence = false;
    view.title = "Replace " + ReplacedName(archive, incoming.kind, source.sound);
    view.heading = file;
    view.heading[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(view.heading[0])));
    view.source = IncomingSource(source.file_name, file, incoming, archive.ReadOptions().flavor);
    view.result = std::move(result);
    view.error = std::move(error);

    // Play plays a sound that plays the item: the replaced wave sound first.
    view.playable = true;
    view.play_sounds = edit::SoundsPlaying(archive, incoming.kind, source.sound);
    if (incoming.kind == edit::FileKind::kWaveSoundFile)
    {
        std::erase(view.play_sounds, source.sound);
        view.play_sounds.insert(view.play_sounds.begin(), source.sound);
    }
    for (const uint32_t sound : view.play_sounds)
    {
        view.plays.push_back(archive.Sounds()[sound].name);
    }

    // The rows of a bank or a wave sound file. The replacement waits for a wave archive in each.
    if (!named.empty())
    {
        WaveArchiveMenu(archive, view.wave_archives);
        view.wave_archive_rows = WaveArchiveRows(named, targets);
        const auto unchosen = [](const WaveArchiveRow& row)
        {
            return row.target < 0;
        };
        if (std::ranges::any_of(view.wave_archive_rows, unchosen))
        {
            view.result.reset();
            view.error = archive.WaveArchives().empty()
                             ? "The archive holds no wave archive for the " + kind +
                                   "'s waves. File > Import > Wave Archive… brings one in."
                             : "Choose a wave archive for each one that the " + kind + " names.";
            return view;
        }
    }

    // The groups that load the item and lack a file that the new file requires.
    const uint32_t item = ReplacedItem(incoming.kind, source.sound);
    const std::string name = ReplacedName(archive, incoming.kind, source.sound);
    const std::vector<uint32_t> after = edit::FilesOfItem(archive, item, {.file = incoming.bytes});
    FillGroups(view, source, edit::FilesOfItem(archive, item), after,
               "Proposed: the groups that load " + name + " and lack a wave archive that the new file names.",
               "No group loads " + name + ". A game that loads its files through groups doesn't load it.",
               after.empty() ? std::map<uint32_t, std::size_t>{}
                             : std::map<uint32_t, std::size_t>{{after.front(), incoming.bytes.size()}});

    // The names of the items at their places in the archive's lists, and a verb for one or more of them.
    const auto names = [&archive](edit::FileKind of, const std::vector<uint32_t>& places)
    {
        std::vector<std::string> list;
        for (const uint32_t place : places)
        {
            list.push_back(ItemName(archive, of, place));
        }

        return NameList(list);
    };

    const auto verb = [](const std::vector<uint32_t>& items, const char* one, const char* more)
    {
        return std::string(items.size() == 1 ? one : more);
    };

    // The other items whose file it is. The window names a wave sound file by its sound group, and the warning names
    // each of its wave sounds.
    const std::vector<uint32_t>& sharing = changes.sharing;
    if (!sharing.empty() && incoming.kind == edit::FileKind::kWaveSoundFile)
    {
        view.warnings.push_back("The file holds the wave sounds of " +
                                names(edit::FileKind::kWaveSoundFile, WithReplacedSound(source.sound, sharing)) +
                                ". Each sound plays the wave sound at its place in the new file.");
    }
    else if (!sharing.empty())
    {
        view.warnings.push_back(names(incoming.kind, sharing) + verb(sharing, " has", " have") + " the same file and " +
                                verb(sharing, "takes", "take") + " the new " + kind + " too.");
    }

    // The copies in groups that hold other bytes. A copy that names the present file's waves at other places takes the
    // new file with those places.
    if (changes.placed_copies > 0)
    {
        const bool one = changes.placed_copies == 1;
        view.warnings.push_back((one ? std::string("A copy") : std::to_string(changes.placed_copies) + " copies") +
                                " of the present file in " + (one ? "a group names" : "groups name") +
                                " its waves at other places. " + (one ? "That copy takes" : "Those copies take") +
                                " the new file with those places.");
        if (changes.unplaced_waves > 0)
        {
            const bool single = changes.unplaced_waves == 1;
            const std::string count = single ? "one" : std::to_string(changes.unplaced_waves);
            const std::string them = single ? "it" : "them";
            view.warnings.push_back("The present file doesn't name " + count + " of the new file's waves. " +
                                    (one ? "That copy names " : "Those copies name ") + them +
                                    " at the archive's places. " + (one ? "Its group" : "Their groups") +
                                    " may not load " + them + ".");
        }
    }
    if (changes.other_copies > 0)
    {
        const bool one = changes.other_copies == 1;
        view.warnings.push_back((one ? std::string("A copy") : std::to_string(changes.other_copies) + " copies") +
                                " of the present file in " + (one ? "a group holds" : "groups hold") +
                                " other bytes. The new file replaces " + (one ? "that copy" : "those copies") +
                                " too.");
    }

    // The waves that the new file names and the archive lacks. Each warning gives the waves, the silent notes and why.
    for (const edit::LackedWaves& lacked : changes.lacked)
    {
        const bool numbered = lacked.wave_archive < archive.WaveArchives().size();
        const std::string wave_archive = numbered ? archive.WaveArchives()[lacked.wave_archive].name
                                                  : "wave archive " + std::to_string(lacked.wave_archive);
        std::string text = "The new " + kind + " names ";
        if (!numbered || lacked.whole)
        {
            text += "waves of " + wave_archive + ". The notes that play them are silent. ";
            text += numbered ? "Hokuto can't read that wave archive." : "The archive has no such wave archive.";
        }
        else
        {
            text += verb(lacked.waves, "wave ", "waves ") + NumberList(lacked.waves) + " of " + wave_archive +
                    ". The notes that play " + verb(lacked.waves, "it", "them") + " are silent. That wave archive " +
                    "holds " + Grouped(lacked.count) + (lacked.count == 1 ? " wave." : " waves.");
        }
        view.warnings.push_back(text);
    }

    // A bank's programs.
    if (incoming.kind == edit::FileKind::kBank)
    {
        AddProgramFixes(source, incoming, changes, fixes, view);
    }

    // What plays waves or wave sounds past the new file's end. For a wave archive, the banks come first, then the wave
    // sounds.
    if (incoming.kind == edit::FileKind::kWaveArchive && (!changes.past_banks.empty() || !changes.past_sounds.empty()))
    {
        std::vector<std::string> list;
        for (const uint32_t bank : changes.past_banks)
        {
            list.push_back(ItemName(archive, edit::FileKind::kBank, bank));
        }
        for (const uint32_t sound : changes.past_sounds)
        {
            list.push_back(ItemName(archive, edit::FileKind::kWaveSoundFile, sound));
        }
        view.warnings.push_back("The new wave archive holds " + Grouped(changes.count) +
                                (changes.count == 1 ? " wave. " : " waves. ") + NameList(list) +
                                (list.size() == 1 ? " names" : " name") +
                                " later ones. The notes that play them are silent.");
    }
    else if (!changes.past_sounds.empty())
    {
        view.warnings.push_back("The new wave sound file holds " + Grouped(changes.count) +
                                (changes.count == 1 ? " wave sound. " : " wave sounds. ") +
                                names(edit::FileKind::kWaveSoundFile, changes.past_sounds) +
                                verb(changes.past_sounds, " plays a later one and is", " play later ones and are") +
                                " silent.");
    }

    // The conversion, the version and the size.
    if (const std::optional<std::string> warning = ConversionWarning(incoming, archive.ReadOptions().flavor))
    {
        view.warnings.push_back(*warning);
    }
    if (changes.version && *changes.version != incoming.version)
    {
        const ntdlib::sar::Flavor flavor = archive.ReadOptions().flavor;
        view.warnings.push_back("The new file has version " + song::VersionText(flavor, incoming.version) +
                                ". The present file has version " + song::VersionText(flavor, *changes.version) +
                                ". A game may refuse a version that its sound library doesn't read.");
    }
    if (changes.size == incoming.bytes.size())
    {
        view.warnings.push_back("Both files take " + SizeText(changes.size) + ".");
    }
    else
    {
        const auto [present, added] = SizeTexts(changes.size, incoming.bytes.size());
        view.warnings.push_back("The present file takes " + present + ". The new one takes " + added + ".");
    }

    return view;
}

std::string ItemName(const ntdlib::rsar::SoundArchive& archive, edit::FileKind kind, uint32_t item)
{
    switch (kind)
    {
    case edit::FileKind::kBank:
        return archive.Banks()[item].name;

    case edit::FileKind::kWaveArchive:
        for (const song::WiiWaveArchive& wave_archive : song::WaveArchivesOf(archive))
        {
            if (wave_archive.file_id == item)
            {
                return wave_archive.name;
            }
        }
        return "the wave data of file " + std::to_string(item);

    case edit::FileKind::kWaveSoundFile:
        break;
    }

    return archive.Sounds()[item].name;
}

ReplaceView MakeWiiFileReplaceView(const ReplaceSource& source, const edit::IncomingFile& incoming,
                                   const edit::FileChanges& changes, std::optional<format::File> result,
                                   std::string error, const std::vector<edit::NamedWaveArchive>& named,
                                   const std::vector<int>& targets)
{
    // The item, and the file that goes in its place. A file of another console pairs each wave archive that it names
    // with wave data of the archive.
    const ntdlib::rsar::SoundArchive& archive = *source.wii;
    const std::string kind = edit::NameOf(incoming.kind);
    const std::string file = incoming.kind == edit::FileKind::kWaveSoundFile ? kind : kind + " file";
    ReplaceView view;
    view.sequence = false;
    view.title = "Replace " + ReplacedName(archive, incoming.kind, source.sound);
    view.heading = file;
    view.heading[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(view.heading[0])));
    view.source = WiiIncomingSource(source, incoming);
    view.result = std::move(result);
    view.error = std::move(error);
    std::vector<std::optional<std::size_t>> counts;
    if (source.later)
    {
        view.rows_heading = "Wave data";
        counts = WiiWaveDataMenu(source, false, view.wave_archives);
        view.wave_archive_rows = WaveArchiveRows(named, targets);
        if (std::ranges::any_of(view.wave_archive_rows, [](const WaveArchiveRow& row) { return row.target < 0; }))
        {
            view.error = "Choose wave data of the archive for each wave archive that the " + file + " names.";
            view.result.reset();
        }
    }

    // A file of another console that Hokuto couldn't convert: the error says why.
    if (source.later && !incoming.converted)
    {
        return view;
    }

    // The names of the items at their places in the archive's lists, and a verb for one or more of them.
    const auto names = [&archive](edit::FileKind of, const std::vector<uint32_t>& places)
    {
        std::vector<std::string> list;
        for (const uint32_t place : places)
        {
            list.push_back(ItemName(archive, of, place));
        }

        return NameList(list);
    };

    const auto verb = [](const std::vector<uint32_t>& items, const char* one, const char* more)
    {
        return std::string(items.size() == 1 ? one : more);
    };

    // The other items whose file it is. The window names a wave sound file by its sound group, and the warning names
    // each of its wave sounds.
    const std::vector<uint32_t>& sharing = changes.sharing;
    if (!sharing.empty() && incoming.kind == edit::FileKind::kWaveSoundFile)
    {
        view.warnings.push_back("The file holds the wave sounds of " +
                                names(edit::FileKind::kWaveSoundFile, WithReplacedSound(source.sound, sharing)) +
                                ". Each sound plays the wave sound at its place in the new file.");
    }
    else if (!sharing.empty())
    {
        view.warnings.push_back(names(incoming.kind, sharing) + verb(sharing, " has", " have") + " the same file and " +
                                verb(sharing, "takes", "take") + " the new " + kind + " too.");
    }

    // The waves that the new file names and the wave data beside it lacks.
    for (const edit::LackedWaves& lacked : changes.lacked)
    {
        if (lacked.whole)
        {
            view.warnings.push_back("Hokuto can't read the new " + kind + "'s waves from the wave data beside the " +
                                    "present one. The notes that play them may be silent until Replace… of the wave " +
                                    "data gives the " + kind + " its waves, such as from an RWAR that Export… wrote.");
        }
        else
        {
            view.warnings.push_back("The new " + kind + " names " + verb(lacked.waves, "wave ", "waves ") +
                                    NumberList(lacked.waves) + ". The wave data beside it holds " +
                                    Grouped(lacked.count) + (lacked.count == 1 ? " wave" : " waves") +
                                    ". The notes that play " + verb(lacked.waves, "it", "them") + " are silent.");
        }
    }

    // A bank's programs that the new bank lacks.
    if (!changes.programs.empty())
    {
        view.warnings.push_back("The new bank lacks " + verb(changes.programs, "program ", "programs ") +
                                NumberList(changes.programs) + ". The notes of " +
                                verb(changes.programs, "it", "them") + " are silent.");
    }

    // What plays waves or wave sounds past the new file's end.
    if (incoming.kind == edit::FileKind::kWaveArchive && (!changes.past_banks.empty() || !changes.past_sounds.empty()))
    {
        std::vector<std::string> list;
        for (const uint32_t bank : changes.past_banks)
        {
            list.push_back(ItemName(archive, edit::FileKind::kBank, bank));
        }
        for (const uint32_t sound : changes.past_sounds)
        {
            list.push_back(ItemName(archive, edit::FileKind::kWaveSoundFile, sound));
        }
        view.warnings.push_back("The new wave archive holds " + Grouped(changes.count) +
                                (changes.count == 1 ? " wave. " : " waves. ") + NameList(list) +
                                (list.size() == 1 ? " names" : " name") +
                                " later ones. The notes that play them are silent.");
    }
    else if (!changes.past_sounds.empty())
    {
        view.warnings.push_back("The new wave sound file holds " + Grouped(changes.count) +
                                (changes.count == 1 ? " wave sound. " : " wave sounds. ") +
                                names(edit::FileKind::kWaveSoundFile, changes.past_sounds) +
                                verb(changes.past_sounds, " plays a later one and is", " play later ones and are") +
                                " silent.");
    }

    // The conversion of a file of another console, and the waves that its wave data lacks.
    if (source.later && incoming.converted)
    {
        view.warnings.push_back("The file comes from the " + song::ConsoleName(*source.later) +
                                ". Hokuto converts it: a " + file + " of the Wii, version " +
                                WiiVersionText(incoming.version) + ", with new wave data of the waves that it names.");
        AddLackedWiiWaves(source, incoming, named, targets, counts, view.warnings);
        view.warnings.push_back(
            "The new wave data takes the place of the present one. Each region or note plays the wave at its place "
            "in the chosen wave data.");
    }

    // The version and the size.
    if (changes.version && *changes.version != incoming.version)
    {
        view.warnings.push_back("The new file has version " + WiiVersionText(incoming.version) +
                                ". The present file has version " + WiiVersionText(*changes.version) +
                                ". A game may refuse a version that its build of NW4R SND doesn't read.");
    }
    if (changes.size == incoming.bytes.size())
    {
        view.warnings.push_back("Both files take " + SizeText(changes.size) + ".");
    }
    else
    {
        const auto [present, added] = SizeTexts(changes.size, incoming.bytes.size());
        view.warnings.push_back("The present file takes " + present + ". The new one takes " + added + ".");
    }

    return view;
}

ReplaceView MakeWaveReplaceView(const ReplaceSource& source, uint32_t wave, const edit::IncomingWave& incoming,
                                bool wav, const edit::WaveChanges& changes, const edit::FileChanges& archive_changes,
                                std::size_t archive_size, std::optional<format::File> result, std::string error)
{
    // The wave, and the WAV file or the wave file that goes in its place.
    const ntdlib::sar::SoundArchive& archive = *source.archive;
    const ntdlib::sar::Flavor flavor = archive.ReadOptions().flavor;
    ReplaceView view;
    view.sequence = false;
    view.title = "Replace " + song::ItemName(archive, {song::ItemKind::kWave, source.sound, wave});
    view.heading = wav ? "WAV file" : "Wave file";
    view.source = WaveSourceLine(source.file_name, incoming, wav, flavor);
    if (wav)
    {
        view.encoding = incoming.encoding;
        view.rates = RatesOf(incoming.wav_rate, changes.rate);
        view.rate = static_cast<std::size_t>(std::ranges::find(view.rates, incoming.rate) - view.rates.begin());
    }

    OfferPresentLoop(view, incoming, wav, changes);
    view.wave = incoming.bytes;
    view.result = std::move(result);
    view.error = std::move(error);

    // What plays the wave, and the other wave archives whose file it is.
    std::vector<std::string> players;
    for (const uint32_t bank : changes.banks)
    {
        players.push_back(ItemName(archive, edit::FileKind::kBank, bank));
    }
    for (const uint32_t sound : changes.sounds)
    {
        players.push_back(ItemName(archive, edit::FileKind::kWaveSoundFile, sound));
    }
    if (players.empty())
    {
        view.warnings.push_back("No bank or wave sound uses the wave.");
    }
    else
    {
        const bool one = players.size() == 1;
        view.warnings.push_back(NameList(players) + (one ? " uses the wave. It takes" : " use the wave. They take") +
                                " the new one.");
    }
    CopyWarnings(archive, archive_changes, view.warnings);

    // The loop.
    if (changes.loop && !incoming.loop)
    {
        view.warnings.push_back(
            "The present wave loops. The new one doesn't. A note that outlasts the new wave falls silent at "
            "the wave's end.");
    }
    else if (!changes.loop && incoming.loop)
    {
        view.warnings.push_back(
            "The new wave loops. The present one doesn't. A note no longer falls silent at the wave's end.");
    }
    IncomingWaveWarnings(incoming, wav, flavor, view.warnings);
    if (changes.channels != incoming.channels)
    {
        view.warnings.push_back("The present wave is " + ChannelsText(changes.channels) + ". The new one is " +
                                ChannelsText(incoming.channels) + ".");
    }
    if (changes.rate != incoming.rate)
    {
        view.warnings.push_back("The present wave plays at " + Grouped(changes.rate) + " Hz. The new one plays at " +
                                Grouped(incoming.rate) + " Hz.");
    }

    // The sizes. A game loads its wave archives into memory that it sets aside.
    const auto [present, added] = SizeTexts(changes.size, incoming.bytes.size());
    view.warnings.push_back(std::string("The present wave is ") + EncodingText(changes.encoding) + " and takes " +
                            present + ". The new one is " + EncodingText(incoming.encoding) + " and takes " + added +
                            ".");
    if (archive_size > archive_changes.size)
    {
        view.warnings.push_back("The wave archive " + SizeChangeText(archive_changes.size, archive_size) +
                                ". A game loads its wave archives into memory that it sets aside. A larger wave "
                                "archive may not fit.");
    }

    return view;
}

ReplaceView MakeWiiWaveReplaceView(const ReplaceSource& source, uint32_t wave, const edit::IncomingWave& incoming,
                                   bool wav, const edit::WaveChanges& changes, std::optional<format::File> result,
                                   std::string error)
{
    // The wave, and the WAV file or the wave file of the Wii that goes in its place.
    const ntdlib::rsar::SoundArchive& archive = *source.wii;
    ReplaceView view;
    view.sequence = false;
    view.title = "Replace " + song::ItemName(archive, {song::ItemKind::kWave, source.sound, wave});
    view.heading = wav ? "WAV file" : "Wave file";
    if (wav)
    {
        view.source = WaveSourceLine(source.file_name, incoming, true, ntdlib::sar::Flavor::kNx);
        view.encoding = incoming.encoding;
        view.rates = RatesOf(incoming.wav_rate, changes.rate);
        view.rate = static_cast<std::size_t>(std::ranges::find(view.rates, incoming.rate) - view.rates.begin());
    }
    else
    {
        view.source = WiiWaveSourceLine(source.file_name, incoming);
    }

    OfferPresentLoop(view, incoming, wav, changes);
    view.wave = incoming.bytes;
    view.result = std::move(result);
    view.error = std::move(error);

    // What plays the wave. The wave data belongs to one file: a bank's or a wave sound file's.
    std::vector<std::string> players;
    for (const uint32_t bank : changes.banks)
    {
        players.push_back(archive.Banks()[bank].name);
    }
    for (const uint32_t sound : changes.sounds)
    {
        players.push_back(archive.Sounds()[sound].name);
    }
    if (players.empty())
    {
        view.warnings.push_back("No bank or wave sound uses the wave.");
    }
    else
    {
        const bool one = players.size() == 1;
        view.warnings.push_back(NameList(players) + (one ? " uses the wave. It takes" : " use the wave. They take") +
                                " the new one.");
    }

    // The loop, the channels, the rate and the sizes.
    if (changes.loop && !incoming.loop)
    {
        view.warnings.push_back(
            "The present wave loops. The new one doesn't. A note that outlasts the new wave falls silent at "
            "the wave's end.");
    }
    else if (!changes.loop && incoming.loop)
    {
        view.warnings.push_back(
            "The new wave loops. The present one doesn't. A note no longer falls silent at the wave's end.");
    }
    WiiWaveWarnings(incoming, wav, view.warnings);
    if (changes.channels != incoming.channels)
    {
        view.warnings.push_back("The present wave is " + ChannelsText(changes.channels) + ". The new one is " +
                                ChannelsText(incoming.channels) + ".");
    }
    if (changes.rate != incoming.rate)
    {
        view.warnings.push_back("The present wave plays at " + Grouped(changes.rate) + " Hz. The new one plays at " +
                                Grouped(incoming.rate) + " Hz.");
    }
    const auto [present, added] = SizeTexts(changes.size, incoming.bytes.size());
    view.warnings.push_back(std::string("The present wave is ") + EncodingText(changes.encoding) + " and takes " +
                            present + ". The new one is " + EncodingText(incoming.encoding) + " and takes " + added +
                            ". A game loads its wave data into memory that it sets aside. More may not fit.");

    return view;
}

ReplaceView MakeNewWaveView(const ReplaceSource& source, uint32_t wave_archive, uint32_t wave,
                            const edit::IncomingWave& incoming, bool wav, const edit::FileChanges& archive_changes,
                            std::size_t archive_size, std::optional<format::File> result, std::string error)
{
    // The file, and the menus of the wave archives and of a WAV file's encodings.
    const ntdlib::sar::SoundArchive& archive = *source.archive;
    const ntdlib::sar::Flavor flavor = archive.ReadOptions().flavor;
    ReplaceView view;
    view.sequence = false;
    view.action = "Import";
    view.title = wav ? "Import Audio File" : "Import Wave";
    view.heading = wav ? "WAV file" : "Wave file";
    view.source = WaveSourceLine(source.file_name, incoming, wav, flavor);
    if (wav)
    {
        view.encoding = incoming.encoding;
        view.rates = RatesOf(incoming.wav_rate);
        view.rate = static_cast<std::size_t>(std::ranges::find(view.rates, incoming.rate) - view.rates.begin());
    }
    view.wave = incoming.bytes;
    view.wave_archive = static_cast<int>(wave_archive);
    WaveArchiveMenu(archive, view.wave_archives);
    view.result = std::move(result);
    view.error = std::move(error);
    if (!view.result)
    {
        return view;
    }

    // Where the new wave goes, and the other copies of the wave archive.
    const std::string target = ItemName(archive, edit::FileKind::kWaveArchive, wave_archive);
    view.warnings.push_back("The new wave becomes wave " + std::to_string(wave) + " of " + target +
                            (wave == 0 ? ", its first wave." : ", after its other waves.") +
                            " No bank or wave sound plays it yet.");
    CopyWarnings(archive, archive_changes, view.warnings);

    // A WAV file's samples, or a wave file of the other console.
    IncomingWaveWarnings(incoming, wav, flavor, view.warnings);

    // The sizes. A game loads its wave archives into memory that it sets aside.
    view.warnings.push_back(std::string("The new wave is ") + EncodingText(incoming.encoding) + " and takes " +
                            SizeText(incoming.bytes.size()) + ". " + target + " " +
                            SizeChangeText(archive_changes.size, archive_size) +
                            ". A game loads its wave archives into memory that it sets aside. A larger wave archive "
                            "may not fit.");

    return view;
}

namespace
{

// The line that names `file_name`, a WAV file or a stream file (`wav`) for an archive of `flavor`, and gives what its
// stream `incoming` holds.
std::string StreamSource(const std::string& file_name, const edit::IncomingStream& incoming, bool wav,
                         ntdlib::sar::Flavor flavor)
{
    const std::optional<edit::ConvertedFile>& converted = incoming.converted;
    const std::string format = wav ? SamplesText(incoming)
                                   : "a stream of the " + song::ConsoleName(converted ? converted->flavor : flavor) +
                                         ", version " +
                                         song::VersionText(converted ? converted->flavor : flavor,
                                                           converted ? converted->version : incoming.version) +
                                         ", " + EncodingText(incoming.encoding);
    std::string text = file_name + ": " + format + ", " + ChannelsText(incoming.channels) + ", " +
                       Grouped(incoming.rate) + " Hz, " +
                       LengthText(incoming.frames + incoming.after_loop, incoming.rate);
    if (incoming.loop)
    {
        text += ", with a loop from sample " + Grouped(incoming.loop_start) + " to " + Grouped(incoming.frames);
    }

    return text;
}

// Why a stream of `flavor` takes version `version` (edit::IncomingStreamVersion), in a sentence for the warnings.
std::string StreamVersionReason(uint32_t version, ntdlib::sar::Flavor flavor)
{
    if (flavor == ntdlib::sar::Flavor::kCtr)
    {
        return version == song::ExportVersions(song::ItemKind::kStream, flavor).file
                   ? "That's the oldest version among the 3DS's games, or the game's streams in the file hold it."
                   : "The game's streams in the file hold that version.";
    }
    if (flavor != ntdlib::sar::Flavor::kCafe)
    {
        return "Hokuto's checks hold no stream of the Switch.";
    }

    return version < 0x00040000 ? "Most of the Wii U's games read stream files only up to that version."
                                : "The game's streams in the file hold that version or a later one.";
}

// The warnings of a WAV file's samples (`wav`), or of a stream file that goes in converted or written again, for a
// stream of `flavor`.
void StreamFormatWarnings(const edit::IncomingStream& incoming, bool wav, ntdlib::sar::Flavor flavor,
                          std::vector<std::string>& warnings)
{
    const std::string version = song::VersionText(flavor, incoming.version);
    const std::string reason = StreamVersionReason(incoming.version, flavor);
    if (wav)
    {
        WavWarnings(incoming, warnings, true);
        warnings.push_back("Hokuto writes the stream in version " + version + ", as NintendoWare's tools lay it out. " +
                           reason);
    }
    else if (incoming.converted && incoming.converted->flavor == flavor)
    {
        warnings.push_back("Hokuto writes the stream again in version " + version + ", with the same samples. " +
                           reason);
    }
    else if (incoming.converted)
    {
        warnings.push_back("The file comes from the " + song::ConsoleName(incoming.converted->flavor) +
                           ". Hokuto converts it: the same samples in the " + song::ConsoleName(flavor) +
                           "'s byte order, with version " + version + ". " + reason);
    }
}

} // namespace

ReplaceView MakeStreamReplaceView(const ReplaceSource& source, const edit::IncomingStream& incoming, bool wav,
                                  const edit::StreamChanges& changes,
                                  const std::optional<std::vector<ntdlib::sar::StreamFile::Track>>& given,
                                  std::optional<format::File> result, std::string error)
{
    // The sound, and the WAV file or the stream file that goes in the place of its stream.
    const ntdlib::sar::SoundArchive& archive = *source.archive;
    const ntdlib::sar::Flavor flavor = archive.ReadOptions().flavor;
    const std::string sound = archive.Sounds().at(source.sound).name;
    ReplaceView view;
    view.sequence = false;
    view.title = "Replace " + sound;
    view.heading = wav ? "WAV file" : "Stream file";
    view.source = StreamSource(source.file_name, incoming, wav, flavor);
    if (wav)
    {
        view.encoding = incoming.encoding;
    }
    view.wave = incoming.bytes;
    view.result = std::move(result);
    view.error = std::move(error);

    // The other sounds that play the stream, and where an export writes it.
    if (!changes.sounds.empty())
    {
        std::vector<std::string> names;
        for (const uint32_t other : changes.sounds)
        {
            names.push_back(archive.Sounds()[other].name);
        }
        const bool one = names.size() == 1;
        view.warnings.push_back(NameList(names) + (one ? " plays" : " play") + " the same stream. " +
                                (one ? "It takes" : "They take") + " the new one too.");
    }
    if (const std::optional<std::filesystem::path> path = song::StreamPath(archive, changes.file))
    {
        view.warnings.push_back("An export writes the new stream beside the archive, at " + xsflib::Utf8(*path) +
                                ". The game looks for it there.");
    }

    // The sound plays the stream's channels as tracks. The game sets up buffers for the channels that the sound's entry
    // gives. The lines count the channels from 1. The stream's properties count them the same way.
    std::size_t needed = 0;
    std::vector<std::string> tracks;
    for (const std::vector<uint8_t>& track : changes.tracks)
    {
        std::vector<std::string> channels;
        for (const uint8_t channel : track)
        {
            channels.push_back(std::to_string(channel + 1));
            needed = std::max<std::size_t>(needed, channel + 1u);
        }
        tracks.push_back(channels.empty() ? std::string("none") : NameList(channels));
    }
    if (changes.tracks_in_file)
    {
        // Before version 2.3.0.0 of a 3DS archive, the stream file gives the tracks.
        const bool same = given && *given == changes.file_tracks;
        view.warnings.push_back(
            std::string("The archive's sounds play the tracks of their stream files. ") +
            (!given ? "The new stream's tracks play."
             : same ? "The new stream takes the present stream's " + std::to_string(given->size()) +
                          (given->size() == 1 ? " track." : " tracks.")
                    : "The new stream takes a track for each pair of its channels: " + std::to_string(given->size()) +
                          (given->size() == 1 ? " track." : " tracks.")));
        needed = 0;
    }
    else if (!tracks.empty())
    {
        view.warnings.push_back("The sound plays " + std::to_string(tracks.size()) +
                                (tracks.size() == 1 ? " track. Its channels: " : " tracks. Their channels: ") +
                                NameList(tracks) + ". They stay as they are.");
    }
    if (incoming.channels < needed)
    {
        view.warnings.push_back("The sound's tracks play channels up to channel " + std::to_string(needed) +
                                ". The new stream is " + ChannelsText(incoming.channels) +
                                ". A track without its channels may fail in the game.");
    }
    if (incoming.channels > changes.table_channels)
    {
        view.warnings.push_back("The sound's entry gives " + std::to_string(changes.table_channels) +
                                (changes.table_channels == 1 ? " channel" : " channels") + ". The new stream is " +
                                ChannelsText(incoming.channels) +
                                ". The game sets up buffers for the entry's channels. More channels may fail in the "
                                "game.");
    }

    // The present stream.
    if (!changes.present)
    {
        view.warnings.push_back(
            "The file holds no present stream of the sound: the import found none beside the "
            "archive.");
    }
    else
    {
        if (changes.loop && !incoming.loop)
        {
            view.warnings.push_back("The present stream loops. The new one doesn't. The sound ends at its end.");
        }
        else if (!changes.loop && incoming.loop)
        {
            view.warnings.push_back(
                "The new stream loops. The present one doesn't. The sound plays until the game stops it.");
        }
        if (changes.channels != incoming.channels)
        {
            view.warnings.push_back("The present stream is " + ChannelsText(changes.channels) + ". The new one is " +
                                    ChannelsText(incoming.channels) + ".");
        }
        if (changes.rate != incoming.rate)
        {
            view.warnings.push_back("The present stream plays at " + Grouped(changes.rate) +
                                    " Hz. The new one plays at " + Grouped(incoming.rate) + " Hz.");
        }
    }

    // A WAV file's samples, or a stream file of the other console.
    StreamFormatWarnings(incoming, wav, flavor, view.warnings);

    // The sizes. A stream plays from the disc, a block at a time.
    const std::string size = "The new stream is " + std::string(EncodingText(incoming.encoding)) + " and takes " +
                             SizeText(incoming.bytes.size()) + ".";
    view.warnings.push_back((changes.present ? std::string("The present stream is ") + EncodingText(changes.encoding) +
                                                   " and takes " + SizeText(changes.size) + ". "
                                             : std::string()) +
                            size +
                            " A game plays a stream from the disc, a block at a time. Its size doesn't count "
                            "against the sound heap.");

    return view;
}

ReplaceView MakeAacStreamReplaceView(const ReplaceSource& source, const edit::IncomingAac& incoming,
                                     const edit::AacChanges& changes, std::optional<format::File> result,
                                     std::string error)
{
    const ntdlib::sar::SoundArchive& archive = *source.archive;
    const std::string sound = archive.Sounds().at(source.sound).name;

    const auto loop_text = [](const std::optional<song::AacLoop>& loop)
    {
        return loop ? "a loop from sample " + Grouped(loop->start) + " to " + Grouped(loop->end)
                    : std::string("no loop");
    };

    ReplaceView view;
    view.sequence = false;
    view.title = "Replace " + sound;
    view.heading = incoming.encoded ? "File" : "AAC file";
    view.source = source.file_name + ": " + ChannelsText(incoming.channels) + ", " + Grouped(incoming.rate) + " Hz" +
                  (incoming.encoded ? "" : ", AAC");
    view.result = std::move(result);
    view.error = std::move(error);

    // What the new stream is, and how it came to be.
    if (!changes.present)
    {
        view.warnings.push_back("The sound's entry marks an AAC stream, and the file lacks it. The new stream is AAC.");
    }
    if (incoming.encoded)
    {
        // Audio Toolbox keeps the rate constant. Media Foundation's encoder aims at it (song::EncodesConstantRate).
        view.warnings.push_back(
            std::string("The system's encoder encodes the new stream as AAC-LC at ") +
            (song::EncodesConstantRate() ? "a constant " : "a target of ") + std::to_string(changes.bit_rate / 1000) +
            (changes.present ? " kbit/s, the nearest rate to the present stream's." : " kbit/s, as the game's are.") +
            " It loses some detail.");
        if (incoming.rate != incoming.stream_rate)
        {
            view.warnings.push_back(
                "The file plays at " + Grouped(incoming.rate) + " Hz. The new stream takes " +
                (changes.present ? "the present stream's " + Grouped(incoming.stream_rate) + " Hz."
                                 : Grouped(incoming.stream_rate) + " Hz: the encoder takes 44,100 or 48,000 Hz."));
        }
        if (incoming.channels != incoming.stream_channels)
        {
            view.warnings.push_back(
                incoming.stream_channels == 2
                    ? std::string("The file is mono. Both channels of the new stream hold it.")
                    : std::string("The file is stereo. The new stream holds the mean of its channels: ") +
                          (changes.present ? "the present stream is mono." : "the sound's entry gives one channel."));
        }
        view.warnings.push_back("The encoder puts " + Grouped(incoming.delay) +
                                " samples of silence before the samples." +
                                (incoming.loop ? " The loop moves with them." : ""));
        if (incoming.after_loop > 0)
        {
            view.warnings.push_back("The " + Grouped(incoming.after_loop) +
                                    " samples after the file's loop stay out: the stream plays up to its loop's end.");
        }
    }
    else if (incoming.present_loop)
    {
        view.warnings.push_back(changes.present
                                    ? "An AAC file holds no loop. The new stream takes the present stream's loop."
                                    : "An AAC file holds no loop. The new stream takes the loop that the sound's entry "
                                      "gives.");
    }
    view.warnings.push_back("The new stream holds " + LengthText(incoming.length, incoming.stream_rate) +
                            " up to its end, with " + loop_text(incoming.loop) + "." +
                            (changes.present ? " The present one holds " + LengthText(changes.length, changes.rate) +
                                                   ", with " + loop_text(changes.loop) + "."
                                             : " The sound's entry gives " + loop_text(changes.loop) + " now."));
    if (changes.loop && !incoming.loop)
    {
        view.warnings.push_back(
            std::string(changes.present ? "The present stream loops." : "The sound's entry gives a loop.") +
            " The new stream doesn't loop. The sound ends at its end.");
    }
    else if (!changes.loop && incoming.loop)
    {
        view.warnings.push_back(changes.present
                                    ? "The new stream loops. The present one doesn't. The sound plays until the "
                                      "game stops it."
                                    : "The new stream loops, and the sound's entry takes its loop. The sound plays "
                                      "until the game stops it.");
    }

    // The sound's entry marks the stream as AAC, and gives the loop or none. Its new values require the model form.
    const bool loops = changes.loop || incoming.loop;
    view.warnings.push_back(std::string(loops ? "The sound's entry in the archive gives the loop."
                                              : "The sound's entry in the archive says that the stream doesn't loop.") +
                            " The file goes into the model form for the new values: an export lays the archive out as "
                            "NintendoWare's tools do.");
    if (!changes.sounds.empty())
    {
        std::vector<std::string> names;
        for (const uint32_t other : changes.sounds)
        {
            names.push_back(archive.Sounds()[other].name);
        }
        const bool one = names.size() == 1;
        view.warnings.push_back(NameList(names) + (one ? " plays" : " play") + " the same stream. " +
                                (one ? "It takes" : "They take") +
                                (incoming.loop ? " the new one and its loop too." : " the new one too."));
    }
    if (const std::optional<std::filesystem::path> path = song::StreamPath(archive, changes.file))
    {
        view.warnings.push_back("An export writes the new stream beside the archive, at " + xsflib::Utf8(*path) +
                                ". The game looks for it there.");
    }
    if (incoming.encoded)
    {
        view.warnings.push_back(
            "A game's AAC files can hold a CRC in each frame's header. The system's encoder writes "
            "none. Nobody has played such a stream on a 3DS.");
    }
    const auto [present, added] = SizeTexts(changes.size, incoming.bytes.size());
    view.warnings.push_back((changes.present ? "The present stream takes " + present + ", and the new one "
                                             : std::string("The new stream takes ")) +
                            added +
                            ". A game plays a stream from the disc, a block at a time. Its size doesn't count against "
                            "the sound heap.");

    return view;
}

ReplaceView MakeStreamImportView(const ReplaceSource& source, const edit::IncomingStream& incoming, bool wav,
                                 const std::string& name, const std::optional<edit::NewStream>& made,
                                 const std::string& error)
{
    // The WAV file or the stream file, the name and the sounds to copy.
    const ntdlib::sar::SoundArchive& archive = *source.archive;
    const ntdlib::sar::Flavor flavor = archive.ReadOptions().flavor;
    ReplaceView view;
    view.title = "Import Stream";
    view.action = "Import";
    view.import = true;
    view.sequence = false;
    view.heading = wav ? "WAV file" : "Stream file";
    view.source = StreamSource(source.file_name, incoming, wav, flavor);
    name.copy(view.name.data(), view.name.size() - 1);
    for (const uint32_t model : source.models)
    {
        view.models.push_back(archive.Sounds()[model].name);
    }
    view.model = static_cast<std::size_t>(std::ranges::find(source.models, source.sound) - source.models.begin());
    if (wav)
    {
        view.encoding = incoming.encoding;
    }
    view.wave = made ? made->bytes : incoming.bytes;

    // The name, and the stream. The import makes the same checks.
    const format::NativeTables& tables = *source.file->native->tables;
    if (!HoldsNames(source))
    {
        view.error = edit::NoNamesReason("sound");
    }
    else if (name.empty())
    {
        view.error = "Give the new sound a name.";
    }
    else if (NameTaken(TakenNames(tables, &archive), name))
    {
        view.error = "The archive holds the name " + name + " already.";
    }
    else if (!made)
    {
        view.error = error;
    }
    view.ready = view.error.empty();
    if (!view.ready)
    {
        return view;
    }

    // The new sound's number, and what it copies.
    const ntdlib::sar::SoundInfo& model = archive.Sounds()[source.sound];
    view.warnings.push_back("The new sound takes number " + std::to_string(archive.Sounds().size()) +
                            Placed(archive.Sounds().size(), "sounds") +
                            ". The game plays it only if its code asks for that number or the sound's name.");
    if (model.type != ntdlib::sar::SoundType::kStream)
    {
        view.warnings.push_back(model.name +
                                " isn't a stream sound. The new sound takes its player, volume, pan, "
                                "priorities and 3D settings. Its tracks take full volume, the centre and the full "
                                "main send.");
    }

    // The tracks, numbered from 1 as the stream's properties number them, with their channels. Before version 2.3.0.0
    // of a 3DS archive, the stream file holds them.
    std::vector<ntdlib::sar::StreamTrackInfo> tracks = made->details.tracks;
    if (made->tracks_in_file)
    {
        const ntdlib::sar::StreamFile stream = ntdlib::sar::StreamFile::Read(made->bytes);
        for (const ntdlib::sar::StreamFile::Track& track :
             stream.tracks.value_or(std::vector<ntdlib::sar::StreamFile::Track>()))
        {
            tracks.push_back({.channels = track.channels, .volume = track.volume, .pan = track.pan});
        }
    }
    std::string text = "The new sound plays " + std::to_string(tracks.size()) +
                       (tracks.size() == 1 ? " track." : " tracks.") +
                       (made->file_tracks ? std::string(" The stream file gives them.") : std::string()) +
                       (made->tracks_in_file && !made->file_tracks
                            ? std::string(" The archive's sounds play the tracks of their stream files. The new "
                                          "stream takes a track for each pair of its channels.")
                            : std::string());
    for (std::size_t t = 0; t < tracks.size(); t++)
    {
        text += " Track " + std::to_string(t + 1) + " plays " + ChannelsOf(tracks[t].channels) + " at volume " +
                std::to_string(tracks[t].volume) + " and pan " + PanName(tracks[t].pan) + ".";
    }
    view.warnings.push_back(text);
    view.warnings.push_back("An export writes the new stream beside the archive, at " + made->path +
                            ". The game looks for it there.");

    // A WAV file's samples, or a stream file of the other console, and the size.
    StreamFormatWarnings(incoming, wav, flavor, view.warnings);
    view.warnings.push_back("The new stream is " + std::string(EncodingText(incoming.encoding)) + " and takes " +
                            SizeText(incoming.bytes.size()) +
                            ". A game plays a stream from the disc, a block at a time. Its size doesn't count "
                            "against the sound heap.");

    return view;
}

bool WiiNameTaken(const ntdlib::rsar::SoundArchive& archive, const std::string& name)
{
    const auto holds = [&name](const auto& items)
    {
        return std::ranges::any_of(items, [&name](const auto& each) { return each.name == name; });
    };
    return holds(archive.Sounds()) || holds(archive.Banks()) || holds(archive.Players());
}

namespace
{

// A count of tracks in words: "1 track", "4 tracks".
std::string TracksText(std::size_t count)
{
    return std::to_string(count) + (count == 1 ? " track" : " tracks");
}

// The line that names `file_name`, a WAV file or a stream file (`wav`) for an archive of the Wii, and gives what its
// stream `incoming` holds (StreamSource).
std::string WiiStreamSource(const std::string& file_name, const edit::IncomingStream& incoming, bool wav)
{
    const std::optional<edit::ConvertedFile>& converted = incoming.converted;
    std::string format;
    if (wav)
    {
        format = SamplesText(incoming);
    }
    else if (converted)
    {
        format = "a stream of the " + song::ConsoleName(converted->flavor) + ", version " +
                 song::VersionText(converted->flavor, converted->version) + ", " + EncodingText(incoming.encoding);
    }
    else
    {
        format =
            "a stream of the Wii, version " + WiiVersionText(incoming.version) + ", " + EncodingText(incoming.encoding);
    }
    std::string text = file_name + ": " + format + ", " + ChannelsText(incoming.channels) + ", " +
                       Grouped(incoming.rate) + " Hz, " +
                       LengthText(incoming.frames + incoming.after_loop, incoming.rate);
    if (incoming.loop)
    {
        text += ", with a loop from sample " + Grouped(incoming.loop_start) + " to " + Grouped(incoming.frames);
    }

    return text;
}

// The warnings of the tracks that stream sound `info` of the Wii's `archive` plays of `stream` on `build` of NW4R SND
// (ntdlib::rvl::snd::StrmPlayer::TracksOf). The tracks count from 0. So do the channels.
void WiiStreamTrackWarnings(const ntdlib::rsar::SoundArchive& archive, const ntdlib::rsar::SoundInfo& info,
                            const ntdlib::SndBuild& build, const ntdlib::rsar::StreamFile& stream,
                            std::vector<std::string>& warnings)
{
    using ntdlib::rvl::snd::StrmPlayer;
    if (!build.SinceMarch2008())
    {
        warnings.emplace_back(
            "The game's build of NW4R SND plays the stream's first two channels at full volume and in the centre, "
            "whatever its tracks.");
        return;
    }
    const std::optional<std::vector<StrmPlayer::Track>> played =
        StrmPlayer::TracksOf(build, info, archive.Version(), stream);
    if (!played)
    {
        warnings.push_back(info.name +
                           "'s entry names a track or a channel that the new stream lacks. The game plays none of its "
                           "tracks.");
        return;
    }

    // The tracks that play, and the channels that the player keeps.
    std::string text;
    std::size_t playing = 0;
    for (std::size_t t = 0; t < played->size(); t++)
    {
        const StrmPlayer::Track& track = (*played)[t];
        if (!track.active)
        {
            continue;
        }
        text += " Track " + std::to_string(t + 1) + " plays " + ChannelsOf(track.channels) + " at volume " +
                std::to_string(track.volume) + " and pan " + PanName(track.pan) + ".";
        playing++;
    }
    warnings.push_back(info.name + " plays " + TracksText(playing) + "." + text);
    const int kept = StrmPlayer::ChannelsKept(build, info, archive.Version(), stream);
    if (kept >= 0 && static_cast<std::size_t>(kept) < stream.channels.size())
    {
        warnings.push_back("Its entry gives " + std::to_string(kept) + (kept == 1 ? " channel" : " channels") +
                           ". The game keeps the first " + std::to_string(kept) + " of the new stream's " +
                           std::to_string(stream.channels.size()) + ".");
    }
}

// The warnings of a WAV file's samples (`wav`), or of a stream file of a later console that goes in converted, for a
// stream of the Wii.
void WiiStreamFormatWarnings(const edit::IncomingStream& incoming, bool wav, std::vector<std::string>& warnings)
{
    if (wav)
    {
        WavWarnings(incoming, warnings, true);
        warnings.emplace_back("Hokuto writes the stream in version 1.0, as NW4R's tools lay it out.");
    }
    else if (incoming.converted)
    {
        warnings.push_back("The file comes from the " + song::ConsoleName(incoming.converted->flavor) +
                           ". Hokuto converts it: the same samples in a stream file of the Wii, version 1.0.");
    }
}

} // namespace

ReplaceView MakeWiiStreamReplaceView(const ReplaceSource& source, const edit::IncomingStream& incoming, bool wav,
                                     const edit::StreamChanges& changes, std::optional<format::File> result,
                                     std::string error)
{
    // The sound, and the WAV file or the stream file that goes in the place of its stream.
    const ntdlib::rsar::SoundArchive& archive = *source.wii;
    const ntdlib::rsar::SoundInfo& info = archive.Sounds().at(source.sound);
    ReplaceView view;
    view.sequence = false;
    view.title = "Replace " + info.name;
    view.heading = wav ? "WAV file" : "Stream file";
    view.source = WiiStreamSource(source.file_name, incoming, wav);
    if (wav)
    {
        view.encoding = incoming.encoding;
    }
    view.wave = incoming.bytes;
    view.result = std::move(result);
    view.error = std::move(error);

    // The other sounds that play the stream, and where an export writes it.
    if (!changes.sounds.empty())
    {
        std::vector<std::string> names;
        for (const uint32_t other : changes.sounds)
        {
            names.push_back(archive.Sounds()[other].name);
        }
        const bool one = names.size() == 1;
        view.warnings.push_back(NameList(names) + (one ? " plays" : " play") + " the same stream. " +
                                (one ? "It takes" : "They take") + " the new one too.");
    }
    if (const std::optional<std::filesystem::path> path = song::StreamPath(archive, changes.file))
    {
        view.warnings.push_back("An export writes the new stream beside the archive, at " + xsflib::Utf8(*path) +
                                ". The game looks for it there. The archive's file table takes its size.");
    }

    // The sound plays the tracks of its stream file. A WAV file, or a stream file without tracks, takes the present
    // stream's tracks where its channels allow.
    ntdlib::rsar::StreamFile stream;
    try
    {
        stream = ntdlib::rsar::StreamFile::Read(incoming.bytes);
    }
    catch (const std::exception&)
    {
        return view;
    }
    std::string tracks = "The sound plays the tracks of its stream file. ";
    if (changes.present && stream.tracks == changes.wii_tracks)
    {
        tracks += "The new stream holds the present stream's " + TracksText(stream.tracks.size()) + ".";
    }
    else
    {
        tracks += "The new stream holds " + TracksText(stream.tracks.size()) + ".";
        if (changes.present)
        {
            tracks += " The present one holds " + TracksText(changes.wii_tracks.size()) + ".";
        }
    }
    view.warnings.push_back(tracks);
    WiiStreamTrackWarnings(archive, info, format::BuildOfWiiArchive(*source.file->native), stream, view.warnings);

    // The present stream.
    if (!changes.present)
    {
        view.warnings.emplace_back(
            "The file holds no present stream of the sound: the import found none beside the archive.");
    }
    else
    {
        if (changes.loop && !incoming.loop)
        {
            view.warnings.emplace_back("The present stream loops. The new one doesn't. The sound ends at its end.");
        }
        else if (!changes.loop && incoming.loop)
        {
            view.warnings.emplace_back(
                "The new stream loops. The present one doesn't. The sound plays until the game stops it.");
        }
        if (changes.channels != incoming.channels)
        {
            view.warnings.push_back("The present stream is " + ChannelsText(changes.channels) + ". The new one is " +
                                    ChannelsText(incoming.channels) + ".");
        }
        if (changes.rate != incoming.rate)
        {
            view.warnings.push_back("The present stream plays at " + Grouped(changes.rate) +
                                    " Hz. The new one plays at " + Grouped(incoming.rate) + " Hz.");
        }
    }

    // A WAV file's samples, or a stream file of a later console, and the sizes. A stream plays from the disc.
    WiiStreamFormatWarnings(incoming, wav, view.warnings);
    const std::string size = "The new stream is " + std::string(EncodingText(incoming.encoding)) + " and takes " +
                             SizeText(incoming.bytes.size()) + ".";
    view.warnings.push_back((changes.present ? std::string("The present stream is ") + EncodingText(changes.encoding) +
                                                   " and takes " + SizeText(changes.size) + ". "
                                             : std::string()) +
                            size +
                            " A game plays a stream from the disc, a block at a time. Its size doesn't count "
                            "against the sound heap.");

    return view;
}

ReplaceView MakeWiiStreamImportView(const ReplaceSource& source, const edit::IncomingStream& incoming, bool wav,
                                    const std::string& name, const std::optional<edit::NewStream>& made,
                                    const std::string& error)
{
    // The WAV file or the stream file, the name and the sounds to copy.
    const ntdlib::rsar::SoundArchive& archive = *source.wii;
    ReplaceView view;
    view.title = "Import Stream";
    view.action = "Import";
    view.import = true;
    view.sequence = false;
    view.heading = wav ? "WAV file" : "Stream file";
    view.source = WiiStreamSource(source.file_name, incoming, wav);
    name.copy(view.name.data(), view.name.size() - 1);
    for (const uint32_t model : source.models)
    {
        view.models.push_back(archive.Sounds()[model].name);
    }
    view.model = static_cast<std::size_t>(std::ranges::find(source.models, source.sound) - source.models.begin());
    if (wav)
    {
        view.encoding = incoming.encoding;
    }
    view.wave = made ? made->bytes : incoming.bytes;

    // The name, and the stream. No two items of a Wii archive share a name. The import checks the groups' names too:
    // ntdlib's reader of the archive doesn't give them.
    if (name.empty())
    {
        view.error = "Give the new sound a name.";
    }
    else if (WiiNameTaken(archive, name))
    {
        view.error = "The archive holds the name " + name + " already.";
    }
    else if (!made || !made->wii_sound)
    {
        view.error = error;
    }
    view.ready = view.error.empty();
    if (!view.ready)
    {
        return Refused(source, std::move(view));
    }

    // The new sound's number, and what it copies.
    const ntdlib::rsar::SoundInfo& model = archive.Sounds()[source.sound];
    view.warnings.push_back("The new sound takes number " + std::to_string(archive.Sounds().size()) +
                            Placed(archive.Sounds().size(), "sounds") +
                            ". The game plays it only if its code asks for that number or the sound's name.");
    if (model.type != ntdlib::rsar::SoundType::kStream)
    {
        view.warnings.push_back(model.name +
                                " isn't a stream sound. The new sound takes its player, volume, priority, pan and 3D "
                                "settings.");
    }

    // The tracks of the stream file and the tracks that the game plays. The new sound's entry names every track of the
    // stream file.
    const ntdlib::rsar::StreamFile stream = ntdlib::rsar::StreamFile::Read(made->bytes);
    const std::size_t channels = stream.channels.size();
    view.warnings.push_back(
        "The new sound's entry names the stream's " + std::to_string(channels) +
        (channels == 1 ? " channel and " : " channels and ") + TracksText(stream.tracks.size()) + "." +
        (made->file_tracks ? std::string(" The stream file gives the tracks.")
                           : std::string(" The new stream takes a track for each pair of its channels.")));
    WiiStreamTrackWarnings(archive, *made->wii_sound, format::BuildOfWiiArchive(*source.file->native), stream,
                           view.warnings);
    view.warnings.push_back("An export writes the new stream beside the archive, at " + made->path +
                            ". The game looks for it there. The stream lies in no group.");

    // A WAV file's samples, or a stream file of a later console, and the size.
    WiiStreamFormatWarnings(incoming, wav, view.warnings);
    view.warnings.push_back("The new stream is " + std::string(EncodingText(incoming.encoding)) + " and takes " +
                            SizeText(incoming.bytes.size()) +
                            ". A game plays a stream from the disc, a block at a time. Its size doesn't count "
                            "against the sound heap.");

    return Refused(source, std::move(view));
}

edit::ProgramMap ChosenPrograms(const ReplaceView& view)
{
    edit::ProgramMap programs;
    for (const ProgramRow& row : view.programs)
    {
        if (row.target != row.program.number)
        {
            programs[row.program] = row.target;
        }
    }

    return programs;
}

edit::VariableMap ChosenVariables(const ReplaceView& view)
{
    edit::VariableMap variables;
    for (const VariableRow& row : view.variables)
    {
        if (row.choice == VariableChoice::kMove && row.to != row.number)
        {
            variables.moves[row.number] = row.to;
        }
        else if (row.choice == VariableChoice::kFix && row.read)
        {
            variables.fixed[row.number] = row.value;
        }
    }

    return variables;
}

ReplaceChoice DrawReplaceWindow(ReplaceView& view, std::optional<ImVec2>& place)
{
    if (!ImGui::IsPopupOpen(kReplaceWindow))
    {
        ImGui::OpenPopup(kReplaceWindow);
    }

    // A window over the middle of the main window, in the style of the Export window. It fits its contents, up to the
    // main window's height less the margins. The user can drag it by its title bar. It then opens where the drag left
    // it until Hokuto quits: `place` keeps its centre. The window grows about its centre and stays inside the main
    // window. While the drag goes on, the drag places the window.
    const std::string title = view.title + kReplaceWindow;
    const ImGuiWindow* window = ImGui::FindWindowByName(title.c_str());
    const bool dragged = window != nullptr && ImGui::GetCurrentContext()->MovingWindow == window;
    const ImVec2 centre = CentreInside(place.value_or(ImGui::GetMainViewport()->GetCenter()),
                                       window != nullptr ? window->Size : ImVec2(0.0f, 0.0f), Px(kMargin));
    const float tallest = std::max(ImGui::GetMainViewport()->WorkSize.y - Px(kMargin) * 2.0f, Px(200.0f));
    if (!dragged)
    {
        ImGui::SetNextWindowPos(centre, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    }
    ImGui::SetNextWindowSize(ImVec2(Px(560.0f), 0.0f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, tallest));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Px(theme::kDialogPadding));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::ColorConvertU32ToFloat4(theme::kRaised));
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(theme::kBorderStrong));
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
    bool open = true;
    const bool visible = ImGui::BeginPopupModal(title.c_str(), &open, kFlags);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
    if (!visible)
    {
        return ReplaceChoice::kCancel;
    }

    // Escape cancels, unless the new item's name is being typed in. The first Escape there ends the field alone.
    if (EscapeCloses())
    {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return ReplaceChoice::kCancel;
    }

    // Each window opens with its choices, its warnings and the panes inside the choices scrolled to the top. The panes
    // keep their names from one window to the next, and Dear ImGui keeps their scroll with the names.
    const bool appearing = ImGui::IsWindowAppearing();

    // A drag that moves the window gives its new place. A click on the title bar that moves nothing keeps the old one.
    if (dragged)
    {
        const ImVec2 corner = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();
        const ImVec2 moved(corner.x + size.x * 0.5f, corner.y + size.y * 0.5f);
        if (std::abs(moved.x - centre.x) > 1.0f || std::abs(moved.y - centre.y) > 1.0f)
        {
            place = moved;
        }
    }

    // The choices and the warnings scroll where they don't fit. The title bar, the padding, the buttons and the space
    // above them keep their room below the window's tallest height.
    const ImGuiStyle& style = ImGui::GetStyle();
    const float kept = ImGui::GetFrameHeight() * 2.0f + Px(theme::kDialogPadding).y * 2.0f + style.ItemSpacing.y * 3.0f;
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, std::max(tallest - kept, Px(80.0f))));
    if (appearing)
    {
        ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));
    }
    ImGui::BeginChild("##choices", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AutoResizeY);

    // The file, and a sequence's choices. The headings follow the Export window's.
    ImGui::PushTextWrapPos(0.0f);

    const auto heading = [](const char* text)
    {
        DrawCaps(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), theme::kTextMuted, text);
        ImGui::Dummy(CapsSize(text));
    };

    ReplaceChoice choice = ReplaceChoice::kNone;
    heading(view.heading.c_str());
    TextWrappedAtSpaces(view.source);
    // While the edit's work runs, the choices keep their look and take no input. The mixer's strips do the same.
    const bool working = !view.working.empty();
    ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha, 1.0f);
    ImGui::BeginDisabled(working);
    ImGui::PopStyleVar();
    if (view.import)
    {
        DrawImportChoices(view, heading, choice);
    }
    if (view.sequence)
    {
        DrawSequenceChoices(view, heading, appearing, choice);
    }
    if (!view.wave_archive_rows.empty())
    {
        DrawWaveArchiveRows(view, heading, choice);
    }
    if (!view.program_fixes.empty())
    {
        DrawProgramFixRows(view, heading, appearing, choice);
    }
    if (view.offers_groups)
    {
        DrawGroupChoices(view, heading);
    }

    // A new wave: a menu of the wave archives.
    if (view.wave_archive)
    {
        ImGui::Spacing();
        heading("Wave archive");
        if (FilteredCombo("##wave_archive", view.wave_archives, *view.wave_archive, nullptr, view.filter))
        {
            choice = ReplaceChoice::kChange;
        }
    }

    // A WAV file's wave: a menu of the encodings.
    if (view.encoding)
    {
        ImGui::Spacing();
        heading("Encoding");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##encoding", EncodingText(*view.encoding)))
        {
            for (const ntdlib::sar::WaveEncoding encoding : kEncodings)
            {
                if (ImGui::Selectable(EncodingText(encoding), encoding == *view.encoding) && encoding != *view.encoding)
                {
                    view.encoding = encoding;
                    choice = ReplaceChoice::kChange;
                }
            }
            ImGui::EndCombo();
        }
    }

    // A WAV file's wave: a menu of the rates, and one of the loops.
    if (view.rates.size() > 1)
    {
        ImGui::Spacing();
        heading("Sample rate");
        std::vector<std::string> names;
        for (std::size_t i = 0; i < view.rates.size(); i++)
        {
            names.push_back(Grouped(view.rates[i]) + " Hz" + (i == 0 ? ", the WAV file's" : ""));
        }
        int rate = static_cast<int>(view.rate);
        if (FilteredCombo("##rate", names, rate, nullptr, view.filter) && rate >= 0)
        {
            view.rate = static_cast<std::size_t>(rate);
            choice = ReplaceChoice::kChange;
        }
    }
    if (view.present_loop)
    {
        ImGui::Spacing();
        heading("Loop");
        const std::vector<std::string> names = {"None", "From sample " + Grouped(view.present_loop->start) + " to " +
                                                            Grouped(view.present_loop->end) +
                                                            ", as the present wave loops"};
        int loop = view.take_loop ? 1 : 0;
        if (FilteredCombo("##loop", names, loop, nullptr, view.filter) && loop >= 0)
        {
            view.take_loop = loop == 1;
            choice = ReplaceChoice::kChange;
        }
    }
    ImGui::EndDisabled();

    // The warnings, after the reason why the sequence or the file can't go in. They scroll once they fill the window's
    // height.
    ImGui::Spacing();
    heading("Warnings");
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, Px(220.0f)));
    if (appearing)
    {
        ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));
    }
    ImGui::BeginChild("##warnings", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AutoResizeY);
    ImGui::PushTextWrapPos(0.0f);
    if (!view.error.empty())
    {
        ImGui::Bullet();
        TextWrappedAtSpaces(view.error);
    }
    // The growth of the chosen groups follows the choice without a new view.
    const std::vector<std::string> growth = view.error.empty() ? GroupGrowthLines(view) : std::vector<std::string>();
    if (view.error.empty() && view.warnings.empty() && growth.empty())
    {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::kTextMuted), "None");
    }
    for (const std::string& warning : view.warnings)
    {
        ImGui::Bullet();
        TextWrappedAtSpaces(warning);
    }
    for (const std::string& line : growth)
    {
        ImGui::Bullet();
        TextWrappedAtSpaces(line);
    }
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    ImGui::PopTextWrapPos();
    ImGui::EndChild();

    // Cancel and the action sit at the bottom right, with the action on the right. Play sits at the bottom left. The
    // work of a confirmed import shows there too, and so does the work of a replacement that puts files into groups. An
    // import without a result and such a replacement close the window once their work is done.
    ImGui::Spacing();
    ImGui::Spacing();
    const float buttons = ImGui::CalcTextSize("Cancel").x + ImGui::CalcTextSize(view.action.c_str()).x +
                          style.FramePadding.x * 4.0f + style.ItemSpacing.x;
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - buttons;
    if (working)
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::kTextMuted), "%s", view.working.c_str());
        ImGui::SameLine();
    }
    else if (!view.wave.empty() || !view.waves.empty() || view.playable)
    {
        // A menu of the sounds or the waves that can play sits beside Play. While the preview is being prepared, Play
        // waits. While the preview plays, the button reads Stop and stops it. The browser's Play does the same.
        ImGui::BeginDisabled(!view.previewing && (view.preparing || !(view.result || view.ready || !view.wave.empty() ||
                                                                      !view.waves.empty())));
        if (ImGui::Button(view.previewing ? "Stop###play" : "Play###play"))
        {
            choice = view.previewing ? ReplaceChoice::kStop : ReplaceChoice::kPlay;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (view.plays.size() > 1)
        {
            int play = static_cast<int>(view.play);
            const float preparing = view.preparing ? ImGui::CalcTextSize("Preparing…").x + style.ItemSpacing.x : 0.0f;
            const float width = std::max(right - ImGui::GetCursorPosX() - style.ItemSpacing.x - preparing, Px(80.0f));
            if (FilteredCombo("##play", view.plays, play, nullptr, view.filter, width))
            {
                view.play = static_cast<std::size_t>(play);
            }
            ImGui::SameLine();
        }
        if (view.preparing)
        {
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::kTextMuted), "Preparing…");
            ImGui::SameLine();
        }
    }
    ImGui::SetCursorPosX(right);
    if (ImGui::Button("Cancel"))
    {
        ImGui::CloseCurrentPopup();
        choice = ReplaceChoice::kCancel;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled((!view.result && !view.ready) || choice == ReplaceChoice::kChange || working);
    if (PrimaryButton(view.action.c_str()))
    {
        if (view.result && view.joined.empty() && view.new_groups.empty())
        {
            ImGui::CloseCurrentPopup();
        }
        choice = ReplaceChoice::kReplace;
    }
    ImGui::EndDisabled();
    ImGui::EndPopup();

    return choice;
}

void CloseReplaceWindow()
{
    if (ImGui::IsPopupOpen(kReplaceWindow) && ImGui::BeginPopupModal(kReplaceWindow))
    {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

} // namespace hokuto::app
