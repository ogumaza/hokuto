// SPDX-License-Identifier: MIT

#include "edit/mp2k.h"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/mp2k/commands.h"
#include "ntdlib/mp2k/song.h"

#include "edit/engine_edits.h"
#include "format/file.h"

namespace hokuto::edit
{
namespace
{

using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Sequence;
using ntdlib::commands::Source;

// The most ticks that the bytes of a note add to the length of its code.
constexpr uint32_t kMostExtra = 127;

// The length of the longest code up to `ticks`.
uint32_t LongestCodeUpTo(uint32_t ticks)
{
    return static_cast<uint32_t>(ntdlib::mp2k::ClockLength(ntdlib::mp2k::UsualLengthCode(ticks)));
}

// The waits whose lengths add up to `ticks`, each a code's length, the longest first. The codes hold every length up to
// 24 ticks. The rest therefore always comes out.
std::vector<uint32_t> WaitLengths(uint32_t ticks)
{
    std::vector<uint32_t> lengths;
    while (ticks > 0)
    {
        const uint32_t length = LongestCodeUpTo(ticks);
        lengths.push_back(length);
        ticks -= length;
    }

    return lengths;
}

// Whether `command` leaves out bytes that the state of the track supplies.
bool LeavesOut(const Command& command)
{
    return command.encoding.repeated || command.encoding.arguments;
}

// Writes `command`'s bytes in full.
void WriteInFull(Command& command)
{
    command.encoding.repeated = false;
    command.encoding.arguments.reset();
}

// The arguments that the bytes of `note`, a note, hold after its code. A note whose code is shorter than its length
// stores the rest in its third argument.
int ArgumentsOf(const Command& note)
{
    const std::optional<uint32_t> ticks = WholeTicks(note.value);
    const std::optional<uint8_t> code = note.encoding.length_code;
    const bool extra =
        ticks && *ticks != (code ? static_cast<uint32_t>(ntdlib::mp2k::ClockLength(*code)) : LongestCodeUpTo(*ticks));
    return note.encoding.arguments.value_or(extra ? 3 : 2);
}

// Whether a note or an end of a tie reads the byte after it as an argument when that byte is below 0x80: it leaves out
// its last arguments. A note reads up to `note_arguments` bytes.
bool TakesNextByte(const Command& command, int note_arguments)
{
    if (command.kind == Kind::kNoteOff)
    {
        return command.encoding.arguments && *command.encoding.arguments < 1;
    }

    return command.kind == Kind::kNote && ArgumentsOf(command) < note_arguments;
}

// Whether the driver reads the bytes of `command` by themselves back as the command, where a note reads up to
// `note_arguments` bytes. A repeated command contains only its arguments, and the driver reads a first byte at or above
// 0x80 as a command of its own, such as a volume of 128.
bool ReadsAlone(const Command& command, int note_arguments)
{
    Sequence alone;
    alone.items = {command, Command{.kind = Kind::kEnd}};
    try
    {
        ntdlib::mp2k::EncodeSequence(alone, ntdlib::agb::kRomBase, note_arguments);
        return true;
    }
    catch (const std::invalid_argument&)
    {
        return false;
    }
}

// The items of a sequence that its bytes play otherwise than they say (Misread). The decoder keeps the bytes of the
// items in `unreached` as data: no track reaches them.
struct Misreading
{
    std::vector<uint32_t> items;
    std::set<uint32_t> unreached;
};

// The items of `sequence` that the bytes of `encoded` play otherwise than they say. `decoded` reads the bytes with
// notes of up to `note_arguments` bytes. A target counts by the place that it names. A data item counts as read.
Misreading Misread(const Sequence& sequence, const ntdlib::mp2k::Encoded& encoded, const ntdlib::mp2k::Decoded& decoded,
                   int note_arguments)
{
    Misreading misread;
    if (decoded.opaque)
    {
        for (uint32_t item = 0; item < sequence.items.size(); item++)
        {
            misread.items.push_back(item);
        }

        return misread;
    }

    const std::vector<uint32_t> read_offsets =
        ntdlib::mp2k::EncodeSequence(decoded.sequence, ntdlib::agb::kRomBase, note_arguments).offsets;
    std::map<uint32_t, uint32_t> read_at;
    for (uint32_t item = 0; item < decoded.sequence.items.size(); item++)
    {
        read_at.emplace(read_offsets[item], item);
    }

    const auto meaning = [](Command command, const std::vector<uint32_t>& offsets)
    {
        if (ntdlib::commands::HasTarget(command.kind) && !command.target_outside && command.target < offsets.size())
        {
            command.target = offsets[command.target];
        }

        command.encoding = {};
        return command;
    };
    for (uint32_t item = 0; item < sequence.items.size(); item++)
    {
        if (sequence.items[item].kind == Kind::kData)
        {
            continue;
        }

        const uint32_t offset = encoded.offsets[item];
        const auto read = read_at.find(offset);
        if (read == read_at.end() || meaning(sequence.items[item], encoded.offsets) !=
                                         meaning(decoded.sequence.items[read->second], read_offsets))
        {
            misread.items.push_back(item);

            // The decoded item that contains the command's first byte.
            auto holder = read_at.upper_bound(offset);
            if (holder != read_at.begin() && decoded.sequence.items[std::prev(holder)->second].kind == Kind::kData &&
                offset < read_offsets[std::prev(holder)->second + 1])
            {
                misread.unreached.insert(item);
            }
        }
    }

    return misread;
}

// The items of sequence `sequence` of `file` where a track starts: those that the fields name.
std::vector<uint32_t> Entries(const format::File& file, uint32_t sequence)
{
    std::vector<uint32_t> entries;
    for (const ntdlib::sar::Field& field : file.native->fields)
    {
        if (NamesItemOf(*file.native, field.to, sequence))
        {
            entries.push_back(field.to.offset);
        }
    }

    return entries;
}

// Writes in full each command of `sequence` that leaves something out and that the driver would read otherwise from
// the bytes, with the tracks starting at the items `entries` and notes of up to `note_arguments` bytes.
void KeepStates(Sequence& sequence, const std::vector<uint32_t>& entries, int note_arguments)
{
    // A note that leaves out its last arguments reads a repeated command's first byte as one. A repeated command whose
    // first byte the driver reads as a command can't repeat.
    for (std::size_t item = 0; item < sequence.items.size(); item++)
    {
        Command& command = sequence.items[item];
        if (command.encoding.repeated && ((item > 0 && TakesNextByte(sequence.items[item - 1], note_arguments)) ||
                                          !ReadsAlone(command, note_arguments)))
        {
            command.encoding.repeated = false;
        }
    }

    // Each round writes in full the commands that the bytes play otherwise. A command that has nothing more to write
    // can't help. The commands that no track reaches can stay as the bytes hold them, such as the commands of a
    // subroutine whose last call gave way to them. A track that reached them would pass a command that reads otherwise
    // first.
    for (;;)
    {
        const ntdlib::mp2k::Encoded encoded =
            ntdlib::mp2k::EncodeSequence(sequence, ntdlib::agb::kRomBase, note_arguments);
        std::vector<uint32_t> addresses;
        for (const uint32_t entry : entries)
        {
            addresses.push_back(ntdlib::agb::kRomBase + encoded.offsets.at(entry));
        }

        const ntdlib::mp2k::Decoded decoded =
            ntdlib::mp2k::DecodeSequence(encoded.bytes, ntdlib::agb::kRomBase, addresses, note_arguments);

        bool written = false;
        bool reached = false;
        const Misreading misread = Misread(sequence, encoded, decoded, note_arguments);
        for (const uint32_t item : misread.items)
        {
            if (LeavesOut(sequence.items[item]))
            {
                WriteInFull(sequence.items[item]);
                written = true;
            }
            else if (!misread.unreached.contains(item))
            {
                reached = true;
            }
        }
        if (misread.items.empty() || (!written && !reached))
        {
            return;
        }
        if (!written)
        {
            throw std::invalid_argument(
                "Can't write the song commands in a form that MusicPlayer2000 reads correctly.");
        }
    }
}

} // namespace

Edited Mp2kWritable(Edited edited, uint32_t sequence)
{
    const Sequence& before = *edited.file.sequences[sequence];
    const int note_arguments = ConsoleOf(edited.file).note_arguments;

    // Each wait splits into the codes' lengths. Each old item's first new item, then the end.
    Sequence after;
    after.labels = before.labels;
    std::vector<uint32_t> firsts;
    for (const Command& item : before.items)
    {
        firsts.push_back(static_cast<uint32_t>(after.items.size()));
        const std::optional<uint32_t> ticks = WholeTicks(item.value);
        if (item.kind == Kind::kWait && item.encoding.opcode == 0 && ticks && LongestCodeUpTo(*ticks) != *ticks)
        {
            for (const uint32_t length : WaitLengths(*ticks))
            {
                Command wait = item;
                wait.value.value = static_cast<int64_t>(length) * kOne;
                wait.encoding = {};
                after.items.push_back(std::move(wait));
            }

            continue;
        }

        Command command = item;
        if (command.kind == Kind::kNote && ticks)
        {
            if (*ticks > kLongestMp2kNote)
            {
                throw std::invalid_argument("MusicPlayer2000 notes can be at most " + std::to_string(kLongestMp2kNote) +
                                            " ticks long without a tie. This note would last " +
                                            std::to_string(*ticks) + " ticks.");
            }
            if (note_arguments < 3 && LongestCodeUpTo(*ticks) != *ticks)
            {
                throw std::invalid_argument(
                    "Notes in this game can last 1 to 24 ticks, or 28 to 96 ticks in steps "
                    "of 2 to 4, without a tie. This note would last " +
                    std::to_string(*ticks) + " ticks.");
            }

            const std::optional<uint8_t> code = command.encoding.length_code;
            const auto code_length = static_cast<uint32_t>(code ? ntdlib::mp2k::ClockLength(*code) : 0);
            if (code && (code_length > *ticks || *ticks - code_length > (note_arguments < 3 ? 0 : kMostExtra)))
            {
                command.encoding.length_code.reset();
            }

            const uint32_t length = command.encoding.length_code ? code_length : LongestCodeUpTo(*ticks);
            if (length != *ticks && command.encoding.arguments && *command.encoding.arguments < 3)
            {
                command.encoding.arguments.reset();
            }
        }

        after.items.push_back(std::move(command));
    }

    firsts.push_back(static_cast<uint32_t>(after.items.size()));

    RenumberItems(after, edited, sequence, firsts, firsts);

    KeepStates(after, Entries(edited.file, sequence), note_arguments);
    edited.file.sequences[sequence] = std::move(after);

    return edited;
}

Edited Writable(const format::File& before, Edited edited, uint32_t sequence)
{
    const song::Console console = ConsoleOf(edited.file);

    return EditsOf(console).Writable(before, std::move(edited), sequence);
}

} // namespace hokuto::edit
