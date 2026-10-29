// SPDX-License-Identifier: MIT

#include "edit/replace.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/nwsnd/commands.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/sequence_file.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/layout.h"
#include "ntdlib/sar/sequence_file.h"

#include "edit/bytecode.h"
#include "edit/converter.h"
#include "edit/other_console.h"
#include "edit/timelines.h"
#include "format/kinds.h"
#include "song/archive.h"
#include "song/biquads.h"

namespace hokuto::edit
{
namespace
{

using ntdlib::commands::Argument;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Label;
using ntdlib::commands::Sequence;
using ntdlib::commands::Source;

// A sound's bank slots, and the bits of an item's ID that give its place in its table.
constexpr std::size_t kBankSlots = 4;
constexpr uint32_t kIndexBits = 0x00FFFFFF;

// The message for a sound that plays no sequence of a file.
constexpr char kNoSequence[] =
    "The sound plays no sequence that Hokuto can edit. Its sequence file holds bytes that no command reaches, or it "
    "isn't a sequence sound.";

// What the player reaches from an item of a sequence: the items, and the bank slots that each program change can run
// with. A slot from a random number or a variable is -1.
struct Reach
{
    std::vector<bool> items;
    std::map<uint32_t, std::set<int>> slots;
};

// The items that the player reaches from item `start` of `sequence`, with the bank slot of each path. A track that a
// command opens starts with slot 0. After a call, the track goes on with the slot that it called with. A return inside
// a call and an end stop a path, and so do bytes that no command reaches. A return outside every call has no call to
// return to, and the player runs on past it.
Reach Walk(const Sequence& sequence, uint32_t start)
{
    const std::vector<Command>& items = sequence.items;
    Reach reach;
    reach.items.assign(items.size(), false);
    std::set<std::tuple<uint32_t, int, bool>> seen;
    std::vector<std::tuple<uint32_t, int, bool>> next = {{start, 0, false}};
    while (!next.empty())
    {
        const auto [item, slot, in_call] = next.back();
        next.pop_back();
        if (item >= items.size() || !seen.insert({item, slot, in_call}).second)
        {
            continue;
        }

        reach.items[item] = true;
        const Command& command = items[item];
        const bool target = ntdlib::commands::HasTarget(command.kind) && !command.target_outside;
        switch (command.kind)
        {
        case Kind::kBankSelect:
            {
                const bool decided = command.value.source == Source::kValue;
                next.push_back({item + 1, decided ? static_cast<int>(command.value.value / kOne) : -1, in_call});
                if (command.conditional)
                {
                    next.push_back({item + 1, slot, in_call});
                }
                break;
            }

        case Kind::kProgram:
            reach.slots[item].insert(slot);
            next.push_back({item + 1, slot, in_call});
            break;

        case Kind::kOpenTrack:
            if (target)
            {
                next.push_back({command.target, 0, false});
            }
            next.push_back({item + 1, slot, in_call});
            break;

        case Kind::kJump:
        case Kind::kCall:
            if (target)
            {
                next.push_back({command.target, slot, in_call || command.kind == Kind::kCall});
            }
            if (command.conditional || command.kind == Kind::kCall)
            {
                next.push_back({item + 1, slot, in_call});
            }
            break;

        case Kind::kReturn:
            if (command.conditional || !in_call)
            {
                next.push_back({item + 1, slot, in_call});
            }
            break;

        case Kind::kEnd:
            if (command.conditional)
            {
                next.push_back({item + 1, slot, in_call});
            }
            break;

        case Kind::kData:
            break;

        default:
            next.push_back({item + 1, slot, in_call});
            break;
        }
    }

    return reach;
}

// The bytes that a variable-length value takes at the least: 7 bits in each.
uint8_t VariableLengthBytes(int64_t value)
{
    uint8_t bytes = 1;
    for (int64_t rest = value >> 7; rest > 0; rest >>= 7)
    {
        bytes++;
    }

    return bytes;
}

// Where `reach` found the program changes of `sequence`, puts the programs of `programs` in the place of those that
// they select. A program change that can run with several slots takes a new program only when the map gives each slot
// the same one. A value that takes more bytes than its padding holds drops the padding.
void MapPrograms(Sequence& sequence, const Reach& reach, const ProgramMap& programs)
{
    for (const auto& [item, slots] : reach.slots)
    {
        Command& command = sequence.items[item];
        if (command.value.source != Source::kValue || slots.contains(-1))
        {
            continue;
        }

        const int number = static_cast<int>(command.value.value / kOne);
        std::set<int> mapped;
        for (const int slot : slots)
        {
            const auto found = programs.find({slot, number});
            mapped.insert(found != programs.end() ? found->second : number);
        }
        if (mapped.size() != 1 || *mapped.begin() == number)
        {
            continue;
        }

        command.value.value = int64_t{*mapped.begin()} * kOne;
        if (command.encoding.value_bytes < VariableLengthBytes(*mapped.begin()))
        {
            command.encoding.value_bytes = 0;
        }
    }
}

// Puts a jump to item `target` before the first item of `sequence`. The labels and the targets move with their items.
void InsertJump(Sequence& sequence, uint32_t target)
{
    for (Command& item : sequence.items)
    {
        if (ntdlib::commands::HasTarget(item.kind) && !item.target_outside)
        {
            item.target++;
        }
    }
    for (Label& label : sequence.labels)
    {
        label.item++;
    }

    Command jump;
    jump.kind = Kind::kJump;
    jump.target = target + 1;
    sequence.items.insert(sequence.items.begin(), jump);
}

// Whether `sequence` would give the file `bytes` back as a file of `read`'s kind and version, with `read`'s labels. Its
// commands encode by the rules of `grammar`. `read` is a sequence file of the 3DS, the Wii U or the Switch, or of the
// Wii.
template <class SequenceFile>
bool GivesBack(const Sequence& sequence, const SequenceFile& read, const ntdlib::nwsnd::Grammar& grammar,
               std::span<const uint8_t> bytes)
{
    try
    {
        const ntdlib::nwsnd::Encoded encoded = ntdlib::nwsnd::EncodeSequence(sequence, grammar);
        SequenceFile file = read;
        file.data = encoded.bytes;
        file.labels.clear();
        for (const Label& label : sequence.labels)
        {
            file.labels.push_back({label.name, encoded.offsets.at(label.item)});
        }

        const std::vector<uint8_t> written = file.Write();
        return std::ranges::equal(written, bytes);
    }
    catch (const std::exception&)
    {
        return false;
    }
}

// The commands of `read`, a sequence file read from `bytes`, by the rules of `grammar`: from its labels and `starts`,
// or from its first command when it has neither. The zeros that pad the bytecode stay out when the writer adds them
// back. No label may name them.
template <class SequenceFile>
Sequence CommandsOf(const SequenceFile& read, const ntdlib::nwsnd::Grammar& grammar, std::span<const uint8_t> bytes,
                    std::span<const uint32_t> starts = {})
{
    std::vector<ntdlib::nwsnd::LabelOffset> labels;
    for (const auto& label : read.labels)
    {
        labels.push_back({label.name, label.offset});
    }
    std::vector<uint32_t> first(starts.begin(), starts.end());
    if (labels.empty() && first.empty() && !read.data.empty())
    {
        first.push_back(0);
    }
    ntdlib::nwsnd::Decoded decoded;
    try
    {
        decoded = ntdlib::nwsnd::DecodeSequence(read.data, labels, grammar, first);
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Its commands can't be read. ") + format::AsSentence(error.what()));
    }
    if (decoded.opaque)
    {
        throw std::invalid_argument("Its commands overlap or jump into one another.");
    }

    Sequence sequence = std::move(decoded.sequence);
    const std::vector<Command>& items = sequence.items;
    const auto names_last = [&items](const Label& label)
    {
        return label.item + 1 >= items.size();
    };
    const bool padding = !items.empty() && items.back().kind == Kind::kData &&
                         std::ranges::all_of(items.back().bytes, [](uint8_t byte) { return byte == 0; }) &&
                         std::ranges::none_of(sequence.labels, names_last);
    if (padding)
    {
        Sequence trimmed = sequence;
        trimmed.items.pop_back();
        if (GivesBack(trimmed, read, grammar, bytes))
        {
            sequence = std::move(trimmed);
        }
    }

    return sequence;
}

// Whether `bytes` start with `magic`.
bool StartsWith(std::span<const uint8_t> bytes, std::string_view magic)
{
    const auto same = [](uint8_t byte, char c)
    {
        return byte == static_cast<uint8_t>(c);
    };
    return bytes.size() >= magic.size() && std::ranges::equal(bytes.first(magic.size()), magic, same);
}

// `old` with the commands of `mapped` that `reach` found after its commands, in their order, and a label at the start.
// Every target of a jump, a call or a track start among them is among them, or names the end. `first` takes the item
// where the start lands. Throws std::invalid_argument when either sequence's commands can't move.
Sequence Appended(const Sequence& old, const Sequence& mapped, const Reach& reach, uint32_t start,
                  const std::string& label, uint32_t& first)
{
    if (Fixed(old))
    {
        throw std::invalid_argument(
            "The sound's sequence file holds targets that are byte offsets. Its commands can't take others after "
            "them.");
    }

    // The commands that the start reaches.
    Sequence appended = old;
    std::map<uint32_t, uint32_t> moved;
    for (uint32_t i = 0; i < mapped.items.size(); i++)
    {
        const Command& item = mapped.items[i];
        if (!reach.items[i])
        {
            continue;
        }
        if (item.kind == Kind::kData || (ntdlib::commands::HasTarget(item.kind) && item.target_outside))
        {
            throw std::invalid_argument(
                "The new sequence runs into bytes that no command reaches, or jumps to a byte offset. Its commands "
                "can't move.");
        }

        moved[i] = static_cast<uint32_t>(appended.items.size());
        appended.items.push_back(item);
    }

    // Their targets, and the label.
    moved[static_cast<uint32_t>(mapped.items.size())] = static_cast<uint32_t>(appended.items.size());
    for (const auto& [from, to] : moved)
    {
        if (to < appended.items.size() && ntdlib::commands::HasTarget(appended.items[to].kind))
        {
            appended.items[to].target = moved.at(appended.items[to].target);
        }
    }
    first = moved.at(start);
    appended.labels.push_back({UniqueLabel(old.labels, label), first});

    return appended;
}

// How a replacement moves the places of a sequence's items in the tables.
struct Moves
{
    std::optional<uint32_t> start_field; // the field of the sound's start
    uint32_t first = 0;                  // where the sound starts in the new sequence
    bool shared = false;                 // other sounds play the sequence
    bool jump = false;                   // a jump went in before the first item
    uint32_t old_end = 0;
    uint32_t new_end = 0;
};

// Moves the places of the items of sequence `sequence` in `native`'s fields. The sound's start goes to its new item. A
// place at the end goes to the new end. The other sounds' starts move with their items. A field's first place names the
// bytecode's start, and stays. Throws std::invalid_argument for a place that names an item of a sequence whose commands
// are all new, other than its start and its end.
void MovePlaces(format::NativeArchive& native, uint32_t sequence, const Moves& moves)
{
    for (std::size_t i = 0; i < native.fields.size(); i++)
    {
        ntdlib::sar::Field& field = native.fields[i];
        if (moves.start_field && i == *moves.start_field)
        {
            field.to.offset = moves.first;
            continue;
        }

        if (NamesItemOf(native, field.from, sequence) && field.from.offset != 0)
        {
            throw std::invalid_argument("The archive's tables name a distance from inside the sound's sequence.");
        }
        if (!NamesItemOf(native, field.to, sequence))
        {
            continue;
        }

        if (field.to.offset == moves.old_end)
        {
            field.to.offset = moves.new_end;
        }
        else if (!moves.shared)
        {
            throw std::invalid_argument("The archive's tables name other commands of the sound's sequence.");
        }
        else if (moves.jump)
        {
            field.to.offset++;
        }
    }
}

} // namespace

Incoming ReadIncoming(std::span<const uint8_t> bytes, const std::optional<song::Console>& into)
{
    // A sequence file of the Wii goes only into an archive of the Wii.
    const std::optional<ntdlib::SndBuild> wii = into ? into->wii : std::nullopt;
    const bool rseq = StartsWith(bytes, "RSEQ");
    if (rseq && !wii)
    {
        throw std::invalid_argument("It's a sequence file of the Wii. It goes only into an archive of the Wii.");
    }
    if (!rseq && !StartsWith(bytes, "CSEQ") && !StartsWith(bytes, "FSEQ"))
    {
        throw std::invalid_argument(format::NotKindText(bytes, "a sequence file"));
    }
    if (wii && rseq)
    {
        ntdlib::rsar::SequenceFile read;
        try
        {
            read = ntdlib::rsar::SequenceFile::Read(bytes);
        }
        catch (const std::exception& error)
        {
            throw std::invalid_argument(std::string("Hokuto can't read it as a sequence file of the Wii: ") +
                                        error.what() + ".");
        }

        return {.sequence = CommandsOf(read, song::GrammarOf(song::Console{.wii = *wii}), bytes),
                .size = bytes.size(),
                .wii = true};
    }

    ntdlib::sar::SequenceFile read;
    try
    {
        read = ntdlib::sar::SequenceFile::Read(bytes);
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Hokuto can't read it as a sequence file: ") + error.what() + ".");
    }
    Incoming incoming{
        .flavor = read.flavor, .sequence = CommandsOf(read, song::GrammarOf(read.flavor), bytes), .size = bytes.size()};
    if (!wii)
    {
        // The biquad filters of another console play at the other console's rate.
        if (into && into->flavor != read.flavor)
        {
            const song::Console from{.flavor = read.flavor};
            const song::BiquadChanges changes = song::MapBiquads(incoming.sequence, {}, from, *into);
            if (const std::optional<std::string> warning = song::BiquadWarning(changes, from, *into))
            {
                incoming.warnings.push_back(*warning);
            }
        }

        return incoming;
    }

    // A sequence file of another console goes into an archive of the Wii as NW4R SND reads the same commands.
    CommandsForWii converted = WiiCommandsOf(incoming.sequence, read.flavor, *wii);
    incoming.sequence = std::move(converted.sequence);
    incoming.wii = true;
    incoming.converted = true;
    incoming.warnings = std::move(converted.warnings);

    return incoming;
}

ntdlib::commands::Sequence SequenceFileCommands(const ntdlib::sar::SequenceFile& read, std::span<const uint8_t> bytes,
                                                std::span<const uint32_t> starts)
{
    return CommandsOf(read, song::GrammarOf(read.flavor), bytes, starts);
}

ntdlib::commands::Sequence SequenceFileCommands(const ntdlib::rsar::SequenceFile& read, std::span<const uint8_t> bytes,
                                                const ntdlib::SndBuild& build, std::span<const uint32_t> starts)
{
    return CommandsOf(read, song::GrammarOf(song::Console{.wii = build}), bytes, starts);
}

Selections ProgramsSelected(const Sequence& sequence, uint32_t start)
{
    const Reach reach = Walk(sequence, start);
    std::set<Program> programs;
    Selections selections;
    for (const auto& [item, slots] : reach.slots)
    {
        const Argument& value = sequence.items[item].value;
        if (value.source != Source::kValue || slots.contains(-1))
        {
            selections.undecided++;
            continue;
        }

        for (const int slot : slots)
        {
            programs.insert({slot, static_cast<int>(value.value / kOne)});
        }
    }
    selections.programs.assign(programs.begin(), programs.end());

    return selections;
}

Sequence WithPrograms(const Sequence& sequence, uint32_t start, const ProgramMap& programs)
{
    Sequence mapped = sequence;
    MapPrograms(mapped, Walk(sequence, start), programs);

    return mapped;
}

uint32_t TracksOpened(const Sequence& sequence, uint32_t start)
{
    // Track 0, and the tracks that the reached track starts open.
    uint32_t tracks = 1;
    const Reach reach = Walk(sequence, start);
    for (uint32_t i = 0; i < sequence.items.size(); i++)
    {
        const Command& command = sequence.items[i];
        if (reach.items[i] && command.kind == Kind::kOpenTrack && command.index < 32)
        {
            tracks |= uint32_t{1} << command.index;
        }
    }

    // An allocation at the start.
    if (start < sequence.items.size())
    {
        const Command& first = sequence.items[start];
        if (first.kind == Kind::kAllocateTracks && first.value.source == Source::kValue)
        {
            tracks |= static_cast<uint32_t>(first.value.value / kOne);
        }
    }

    return tracks;
}

std::vector<std::optional<SoundPlace>> SoundPlaces(const ntdlib::sar::SoundArchive& archive,
                                                   std::span<const std::optional<uint32_t>> sequences,
                                                   std::span<const std::optional<uint32_t>> start_fields)
{
    std::vector<std::optional<SoundPlace>> places;
    for (std::size_t i = 0; i < archive.Sounds().size(); i++)
    {
        const ntdlib::sar::SoundInfo& sound = archive.Sounds()[i];
        std::optional<SoundPlace>& place = places.emplace_back();
        if (sound.type == ntdlib::sar::SoundType::kSequence && sound.file_id < sequences.size() &&
            sequences[sound.file_id])
        {
            place = SoundPlace{*sequences[sound.file_id], i < start_fields.size() ? start_fields[i] : std::nullopt};
        }
    }

    return places;
}

std::vector<std::optional<SoundPlace>> SoundPlaces(const ntdlib::rsar::SoundArchive& archive,
                                                   std::span<const std::optional<uint32_t>> sequences,
                                                   std::span<const std::optional<uint32_t>> start_fields)
{
    std::vector<std::optional<SoundPlace>> places;
    for (std::size_t i = 0; i < archive.Sounds().size(); i++)
    {
        const ntdlib::rsar::SoundInfo& sound = archive.Sounds()[i];
        std::optional<SoundPlace>& place = places.emplace_back();
        if (sound.type == ntdlib::rsar::SoundType::kSequence && sound.file_id < sequences.size() &&
            sequences[sound.file_id])
        {
            place = SoundPlace{*sequences[sound.file_id], i < start_fields.size() ? start_fields[i] : std::nullopt};
        }
    }

    return places;
}

std::vector<uint32_t> SharingStart(std::span<const std::optional<SoundPlace>> places, uint32_t sound)
{
    std::vector<uint32_t> sounds;
    if (sound >= places.size() || !places[sound] || places[sound]->start_field)
    {
        return sounds;
    }

    for (const uint32_t other : SharingFile(places, sound))
    {
        if (!places[other]->start_field)
        {
            sounds.push_back(other);
        }
    }

    return sounds;
}

std::vector<uint32_t> SharingFile(std::span<const std::optional<SoundPlace>> places, uint32_t sound)
{
    std::vector<uint32_t> sounds;
    if (sound >= places.size() || !places[sound])
    {
        return sounds;
    }

    for (std::size_t i = 0; i < places.size(); i++)
    {
        if (i != sound && places[i] && places[i]->sequence == places[sound]->sequence)
        {
            sounds.push_back(static_cast<uint32_t>(i));
        }
    }

    return sounds;
}

std::vector<StartingSound> SoundStarts(const format::File& file, std::span<const std::optional<SoundPlace>> places,
                                       const Sequence& incoming)
{
    // Each sequence of the file is compared once. A timeline that holds the commands gives the item of each of its
    // items in its saved form. A program gives no items: its items are those of its saved form.
    std::map<uint32_t, std::optional<std::vector<std::optional<uint32_t>>>> same;
    std::vector<StartingSound> starts;
    for (std::size_t i = 0; i < places.size(); i++)
    {
        if (!places[i])
        {
            continue;
        }

        // A timeline's sounds start in its header or at a label. They therefore start at the same commands in its saved
        // form.
        const SoundPlace& place = *places[i];
        const auto [found, added] = same.try_emplace(place.sequence);
        if (added)
        {
            const Sequence saved = SavedSequence(file, place.sequence);
            const auto same = [](const Command& a, const Command& b)
            {
                return song::SameButBiquads(a, b);
            };
            if (std::ranges::equal(saved.items, incoming.items, same))
            {
                found->second = IsTimeline(file, place.sequence) ? MatchingItems(*file.sequences[place.sequence], saved)
                                                                 : std::vector<std::optional<uint32_t>>{};
            }
        }
        if (found->second)
        {
            const uint32_t item = place.start_field ? file.native->fields.at(*place.start_field).to.offset : 0;
            const std::optional<uint32_t> saved_item =
                found->second->empty() ? std::optional<uint32_t>(item) : found->second->at(item);
            if (saved_item)
            {
                starts.push_back({static_cast<uint32_t>(i), *saved_item});
            }
        }
    }

    return starts;
}

uint32_t TracksToAdd(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                     const Sequence& incoming, uint32_t start)
{
    if (sound >= places.size() || !places[sound])
    {
        throw std::invalid_argument(kNoSequence);
    }

    // The sound's start is the item that its field names, or the file's first item.
    const SoundPlace& place = *places[sound];
    const uint32_t now = place.start_field ? file.native->fields.at(*place.start_field).to.offset : 0;
    return TracksOpened(incoming, start) & ~TracksOpened(SequenceToEdit(file, place.sequence), now);
}

void CheckBankSlots(const format::NativeTables& tables, std::span<const uint32_t> slots)
{
    const auto past = [&tables](uint32_t bank)
    {
        return bank != ntdlib::sar::kNoItem &&
               (bank >> 24 != ntdlib::sar::kBankItem || (bank & kIndexBits) >= tables.banks.size());
    };
    if (slots.size() > kBankSlots || std::ranges::any_of(slots, past))
    {
        throw std::invalid_argument("A sound has four bank slots. Each holds one of the archive's banks, or none.");
    }
}

format::File ReplaceSequence(const format::File& file, std::span<const std::optional<SoundPlace>> places,
                             uint32_t sound, const Sequence& incoming, uint32_t start, const ProgramMap& programs,
                             const std::string& label, uint32_t tracks,
                             const std::optional<std::vector<uint32_t>>& banks)
{
    if (sound >= places.size() || !places[sound])
    {
        throw std::invalid_argument(kNoSequence);
    }
    if (start >= incoming.items.size())
    {
        throw std::invalid_argument("The start lies past the end of the new sequence.");
    }

    // The new commands, with the programs of the map, and the item where the file's sequence holds them already. A
    // saved file holds a timeline with its subroutines. The timeline holds the item only in its header or at a label.
    const SoundPlace& place = *places[sound];
    const Reach reach = Walk(incoming, start);
    Sequence mapped = WithPrograms(incoming, start, programs);
    const bool timeline = IsTimeline(file, place.sequence);
    const Sequence saved = SavedSequence(file, place.sequence);
    std::optional<uint32_t> held;
    if (mapped == saved)
    {
        held = timeline ? MatchingItems(saved, *file.sequences[place.sequence]).at(start) : start;
    }

    // A timeline whose commands the new ones replace goes in as a program. The program holds the commands as the file
    // holds them. The caller finds its mode again.
    const bool shared = !SharingFile(places, sound).empty();
    const format::File base = timeline && !shared && !held ? AsProgram(file, place.sequence) : file;
    const Sequence& old = SequenceToEdit(base, place.sequence);

    // The sequence that the file then holds, and the item where the sound starts in it. A sound without a start in its
    // entry starts at a jump to its commands. The commands after the jump move by an item.
    uint32_t first = held.value_or(start);
    Sequence result = held ? old : !shared ? std::move(mapped) : Appended(old, mapped, reach, start, label, first);
    const bool jump = !place.start_field && first != 0;
    if (jump && Fixed(result))
    {
        throw std::invalid_argument(
            "The sound starts where its sequence file starts, and the file's commands can't move to make room for a "
            "jump to the new commands.");
    }
    if (jump)
    {
        InsertJump(result, first);
    }

    // The console's bytecode has to hold every command.
    try
    {
        ntdlib::nwsnd::EncodeSequence(result, song::GrammarOf(ConsoleOf(file)));
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("The new commands can't be written for the archive's console. ") +
                                    format::AsSentence(error.what()));
    }

    // The sounds that play the new commands allocate the tracks to add, and take the bank slots.
    if (!file.native->tables && tracks != 0)
    {
        throw std::invalid_argument(
            "The new commands open tracks that the sound doesn't allocate. Hokuto can't read this archive's tables, "
            "and so can't make the sound allocate them.");
    }
    if (!file.native->tables && banks)
    {
        throw std::invalid_argument("Hokuto can't read this archive's tables, and so can't change the sound's banks.");
    }
    if (banks)
    {
        CheckBankSlots(*file.native->tables, *banks);
    }
    if (banks && file.native->wii && banks->size() > 1)
    {
        throw std::invalid_argument("A sound of the Wii plays one bank, in slot 0.");
    }
    std::vector<uint32_t> playing = SharingStart(places, sound);
    playing.push_back(sound);

    // The places of the items in the tables, the tracks, and the sequence.
    format::File replaced = base;
    const Moves moves{.start_field = place.start_field,
                      .first = first,
                      .shared = shared,
                      .jump = jump,
                      .old_end = static_cast<uint32_t>(old.items.size()),
                      .new_end = static_cast<uint32_t>(result.items.size())};
    const auto change = [&](format::NativeArchive& native)
    {
        MovePlaces(native, place.sequence, moves);
        for (const uint32_t other : playing)
        {
            if (native.tables && other < native.tables->sounds.size())
            {
                native.tables->sounds[other].tracks |= tracks;
                native.tables->sounds[other].banks = banks.value_or(native.tables->sounds[other].banks);
            }
        }
    };
    replaced.native.Edit(change);
    replaced.sequences[place.sequence] = std::move(result);

    return replaced;
}

} // namespace hokuto::edit
