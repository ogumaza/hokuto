// SPDX-License-Identifier: MIT

// The length bytes of the edits of edit/brownie.h: the value ranges of the driver's commands, the splits of a wait into
// the lengths of a channel's length set, the alignment of the tables, and the check that the driver reads an edited
// sequence as its items say (BrownieWritable).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/brownie/commands.h"
#include "ntdlib/brownie/driver.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"

#include "edit/brownie_parts.h"
#include "edit/bytecode.h"
#include "edit/commands.h"
#include "edit/drivers.h"
#include "format/brownie_items.h"
#include "format/drivers.h"

namespace hokuto::edit
{
namespace
{

namespace brownie = ntdlib::brownie;
using brownie_parts::Barrier;
using brownie_parts::EndsBlock;
using brownie_parts::Hold;
using brownie_parts::Tables;
using brownie_parts::TablesOf;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Sequence;
using ntdlib::commands::Source;

// The most frames that the value of one command can express here. A longer wait splits into commands of the lengths
// that the channel's length set has.
constexpr uint32_t kMostFrames = 0xFFFF;

// How many times Writable puts length bytes in before it gives up. Each round settles the commands that wait up to the
// next length byte of their channels. In practice two or three rounds settle an edit.
constexpr int kMostRounds = 64;

// The lengths that length set `set` contains, from the longest. Each command that waits splits into some of them.
std::vector<uint32_t> LengthsOf(const Tables& tables, int set)
{
    std::set<uint32_t> lengths;
    for (int byte = 0x80; byte < 0xE0; byte++)
    {
        lengths.insert(tables.Frames(static_cast<uint8_t>(byte), set));
    }

    return {lengths.rbegin(), lengths.rend()};
}

// `frames` as a sum of `lengths`, with as few of them as the search finds, from the longest. Nothing when no sum equals
// the frames.
std::optional<std::vector<uint32_t>> SplitFrames(uint32_t frames, std::span<const uint32_t> lengths)
{
    // The fewest lengths for each number of frames up to `frames`.
    std::vector<uint32_t> fewest(frames + 1, UINT32_MAX);
    std::vector<uint32_t> last(frames + 1, 0);
    fewest[0] = 0;
    for (uint32_t total = 1; total <= frames; total++)
    {
        for (const uint32_t length : lengths)
        {
            if (length <= total && fewest[total - length] != UINT32_MAX && fewest[total - length] + 1 < fewest[total])
            {
                fewest[total] = fewest[total - length] + 1;
                last[total] = length;
            }
        }
    }
    if (fewest[frames] == UINT32_MAX)
    {
        return std::nullopt;
    }

    std::vector<uint32_t> parts;
    for (uint32_t total = frames; total > 0; total -= last[total])
    {
        parts.push_back(last[total]);
    }

    std::ranges::sort(parts, std::greater<>());
    return parts;
}

// The first table of a sequence whose start is off a multiple of 4 in the ROM (AlignTables): its item, how many bytes
// past a multiple of 4 it starts, and whether the item before it starts a table too.
struct Misaligned
{
    uint32_t start = 0;
    uint32_t past = 0;
    bool table_before = false;
};

// The first table of `changed`, a sequence of `revision`, whose start is off a multiple of 4 in the ROM, or nothing
// when every table starts at one (AlignTables).
std::optional<Misaligned> MisalignedTable(const Sequence& changed, brownie::Revision revision)
{
    std::set<uint32_t> tables;
    for (uint32_t i = 0; i + 1 < changed.items.size(); i++)
    {
        const Command& word = changed.items[i + 1];
        if (changed.items[i].kind == Kind::kOpenTrack && format::BrownieIsWord(word) && !word.target_outside &&
            word.target < changed.items.size() && changed.items[word.target].kind == Kind::kData)
        {
            tables.insert(word.target);
        }
    }
    if (tables.empty())
    {
        return std::nullopt;
    }

    const std::vector<uint32_t> offsets = format::BrownieItemOffsets(changed, revision);
    const auto first_word = std::ranges::find_if(changed.items, format::BrownieIsWord);
    const uint32_t remainder = (4 - offsets[first_word - changed.items.begin()] % 4) % 4;
    const auto off = std::ranges::find_if(
        tables, [&offsets, remainder](uint32_t table) { return (remainder + offsets[table]) % 4 != 0; });
    if (off == tables.end())
    {
        return std::nullopt;
    }

    const uint32_t start = *off;
    return Misaligned{start, (remainder + offsets[start]) % 4, start > 0 && tables.contains(start - 1)};
}

// Moves the items of `changed`, sequence `sequence` of `edited`, and every place that names them, by `moved`. An item
// that has no place in `moved` leaves the places that named it.
template <typename Moved>
void MoveItems(Sequence& changed, Edited& edited, uint32_t sequence, const Moved& moved)
{
    const auto moved_or_kept = [&moved](uint32_t item)
    {
        return moved(item).value_or(item);
    };
    MoveTargets(changed, moved_or_kept);
    for (std::optional<uint32_t>& item : edited.items)
    {
        if (item)
        {
            item = moved(*item);
        }
    }

    for (uint32_t& item : edited.added)
    {
        item = moved_or_kept(item);
    }

    MoveItemPlaces(edited.file, sequence, moved_or_kept);
}

// Erases item `gone` of `changed`, sequence `sequence` of `edited`, and moves the items after it and every place that
// names them back by one (MoveItems). The places that named item `gone` leave it.
void EraseItem(Sequence& changed, Edited& edited, uint32_t sequence, uint32_t gone)
{
    changed.items.erase(changed.items.begin() + gone);
    MoveItems(changed, edited, sequence, [gone](uint32_t item) -> std::optional<uint32_t>
              { return item < gone    ? std::optional(item)
                       : item == gone ? std::nullopt
                                      : std::optional(item - 1); });
}

// Moves the table that `off` names in `changed`, sequence `sequence` of `edited`, to a multiple of 4: zeros of data
// before it give way where they can, and new zeros go in otherwise (AlignTables).
void PadBeforeTable(Sequence& changed, Edited& edited, uint32_t sequence, const Misaligned& off)
{
    const uint32_t start = off.start;
    const uint32_t past = off.past;
    Command* zeros = start > 0 ? &changed.items[start - 1] : nullptr;
    if (zeros == nullptr || zeros->kind != Kind::kData || off.table_before ||
        std::ranges::any_of(zeros->bytes, [](uint8_t byte) { return byte != 0; }))
    {
        zeros = nullptr;
    }
    if (zeros != nullptr && zeros->bytes.size() > past)
    {
        zeros->bytes.resize(zeros->bytes.size() - past);
    }
    else if (zeros != nullptr && zeros->bytes.size() == past)
    {
        EraseItem(changed, edited, sequence, start - 1);
    }
    else if (zeros != nullptr)
    {
        zeros->bytes.insert(zeros->bytes.end(), 4 - past, uint8_t{0});
    }
    else
    {
        Command padding;
        padding.kind = Kind::kData;
        padding.bytes.assign(4 - past, uint8_t{0});
        changed.items.insert(changed.items.begin() + start, std::move(padding));
        MoveItems(changed, edited, sequence,
                  [start](uint32_t item) -> std::optional<uint32_t> { return item >= start ? item + 1 : item; });
    }
}

// Puts each channel's table in `changed`, sequence `sequence` of `edited`, that starts with data back on a multiple of
// 4. A list names each channel's table, and the driver reads a call's word at 4 bytes for each index from the table's
// start. The decoder keeps the entries that no call reads as data, and an edit before the table moves it with the bytes
// before it. Zeros of data before the table give way where they can, and new zeros go in otherwise. No channel reads a
// byte of data, and neither changes anything that plays. The sequence's first word starts at a multiple of 4 in the ROM
// (format::BrownieItemOffsets).
void AlignTables(Sequence& changed, Edited& edited, uint32_t sequence, brownie::Revision revision)
{
    // The first table off a multiple of 4 moves, and the tables after it move with it.
    for (;;)
    {
        const std::optional<Misaligned> off = MisalignedTable(changed, revision);
        if (!off)
        {
            return;
        }

        PadBeforeTable(changed, edited, sequence, *off);
    }
}

// The item of `sequence` after the start of the loop whose end is item `end`, or nothing where no start comes before
// it.
std::optional<uint32_t> LoopBodyOf(const Sequence& sequence, uint32_t end)
{
    int depth = 0;
    for (uint32_t item = end; item-- > 0;)
    {
        const Kind kind = sequence.items[item].kind;
        if (kind == Kind::kLoopEnd)
        {
            depth++;
        }
        else if (kind == Kind::kLoopStart && depth-- == 0)
        {
            return item + 1;
        }
    }

    return std::nullopt;
}

// Whether no channel that reads the length byte at item `byte` of `sequence` reads the length that it chooses. Each of
// the channel's ways from the byte leads through commands that don't wait to another length byte or an end. A way leads
// into calls and back, through jumps, and back to a loop's start as well as on. A command that waits reads the length.
// A way that Hokuto can't follow, such as through bytes that no command contains, counts as a read.
bool LengthUnread(const Sequence& sequence, uint32_t byte)
{
    struct Way
    {
        uint32_t next = 0;
        std::vector<uint32_t> returns;
    };
    std::vector<Way> ways = {{byte + 1, {}}};
    std::set<std::pair<uint32_t, std::vector<uint32_t>>> seen;
    while (!ways.empty())
    {
        Way way = std::move(ways.back());
        ways.pop_back();
        // The way ends at a length byte, at an end, or where it joins a way already followed.
        while (way.next < sequence.items.size() && seen.emplace(way.next, way.returns).second)
        {
            const Command& item = sequence.items[way.next];
            if (format::BrownieLengthByte(item) || (item.kind == Kind::kEnd && item.index == 0))
            {
                break;
            }

            if (format::BrownieWaits(item) || Barrier(item) || format::BrownieIsWord(item) || item.target_outside ||
                item.kind == Kind::kEnd || item.kind == Kind::kOpenTrack)
            {
                return false;
            }

            if (item.kind == Kind::kCall)
            {
                way.returns.push_back(way.next + 1);
                way.next = item.target;
            }
            else if (item.kind == Kind::kJump)
            {
                way.next = item.target;
            }
            else if (item.kind == Kind::kReturn)
            {
                if (way.returns.empty())
                {
                    return false;
                }

                way.next = way.returns.back();
                way.returns.pop_back();
            }
            else if (item.kind == Kind::kLoopEnd)
            {
                const std::optional<uint32_t> body = LoopBodyOf(sequence, way.next);
                if (!body)
                {
                    return false;
                }

                ways.push_back({*body, way.returns});
                way.next++;
            }
            else
            {
                way.next++;
            }
        }
        if (way.next >= sequence.items.size())
        {
            return false;
        }
    }

    return true;
}

// Removes from `changed`, sequence `sequence` of `edited`, the length bytes that a removed command leaves unread
// (LengthUnread). Code that entered at such a byte enters at the item after it. `before` contains the sequence before
// the edit.
void DropUnreadLengths(Sequence& changed, Edited& edited, uint32_t sequence, const Sequence& before)
{
    std::set<uint32_t> unread;
    for (uint32_t old = 1; old < edited.items.size() && old < before.items.size(); old++)
    {
        const std::optional<uint32_t> byte = edited.items[old - 1];
        if (!edited.items[old] && byte && format::BrownieLengthByte(before.items[old - 1]) &&
            LengthUnread(changed, *byte))
        {
            unread.insert(*byte);
        }
    }

    for (auto gone = unread.rbegin(); gone != unread.rend(); ++gone)
    {
        EraseItem(changed, edited, sequence, *gone);
    }
}

// Throws std::invalid_argument with a message for the user for a command of `changed` that waits for other lengths, or
// longer than a command can wait (BrownieWritable).
void CheckWaits(const Sequence& changed)
{
    for (const Command& item : changed.items)
    {
        if (format::BrownieWaits(item) && format::BrownieWaitOf(item) == 0)
        {
            throw std::invalid_argument(
                "The channels that read this sequence wait for other lengths at some of its commands. Hokuto can't "
                "write an edit of it.");
        }
        if (format::BrownieWaits(item) && format::BrownieWaitOf(item) > kMostFrames)
        {
            throw std::invalid_argument("A command waits " + std::to_string(kMostFrames) + " frames at most here.");
        }
    }
}

// The items of `changed`, sequence `sequence` of `file`, where code enters: the targets, the labels and the places of
// the archive's tables (BrownieWritable).
std::set<uint32_t> EnteredItems(const Sequence& changed, const format::File& file, uint32_t sequence)
{
    std::set<uint32_t> entered;
    for (const Command& item : changed.items)
    {
        if (ntdlib::commands::HasTarget(item.kind) && !item.target_outside)
        {
            entered.insert(item.target);
        }
    }

    for (const ntdlib::commands::Label& label : changed.labels)
    {
        entered.insert(label.item);
    }

    ForEachItemPlace(*file.native, sequence,
                     [&entered](const ntdlib::sar::Place& place) { entered.insert(place.offset); });

    return entered;
}

// The commands of `changed` that their channels read with another length than their frames, by item, with the length
// set that their channels read and the frames of the length bytes that they take (BrownieWritable). `readings` contains
// the lengths that the channels read, and `entered` the items where code enters.
std::map<uint32_t, std::pair<int, std::vector<uint32_t>>> WrongLengths(const Sequence& changed,
                                                                       const format::BrownieReadings& readings,
                                                                       const std::set<uint32_t>& entered,
                                                                       const Tables& tables, brownie::Revision revision)
{
    std::map<uint32_t, std::pair<int, std::vector<uint32_t>>> wrong;
    std::optional<uint32_t> run;
    for (uint32_t i = 0; i < changed.items.size(); i++)
    {
        const Command& item = changed.items[i];
        if (entered.contains(i) || format::BrownieLengthByte(item) || format::BrownieLengthSet(item, revision) ||
            EndsBlock(item) || format::BrownieIsWord(item) || Barrier(item))
        {
            run.reset();
        }

        const std::set<uint32_t>& read = readings.lengths[i];
        if (!format::BrownieWaits(item) || read.empty())
        {
            continue;
        }

        const uint32_t frames = format::BrownieWaitOf(item);
        const bool reads_frames = run ? *run == frames : read.size() == 1 && *read.begin() == frames;
        if (reads_frames)
        {
            continue;
        }

        const std::set<int>& sets = readings.sets[i];
        if (sets.size() != 1)
        {
            throw std::invalid_argument(
                "The channels that read this command reach it with other length sets (command EE). Hokuto can't "
                "write its length.");
        }

        const int set = *sets.begin();
        std::optional<std::vector<uint32_t>> parts = SplitFrames(frames, LengthsOf(tables, set));
        if (!parts)
        {
            throw std::invalid_argument("No length byte of the channel's length set gives a wait of " +
                                        std::to_string(frames) + " frames.");
        }

        run = parts->back();
        wrong.emplace(i, std::pair(set, std::move(*parts)));
    }

    return wrong;
}

// A length byte is inserted before each such command. Code that entered at the command enters at the byte. A command
// whose frames match no byte splits into the command and holds. InsertLengthBytes inserts those of `wrong`
// (WrongLengths) into `changed`, sequence `sequence` of `edited`.
void InsertLengthBytes(Sequence& changed, const std::map<uint32_t, std::pair<int, std::vector<uint32_t>>>& wrong,
                       const Tables& tables, Edited& edited, uint32_t sequence)
{
    Sequence after;
    after.labels = changed.labels;
    std::vector<uint32_t> firsts(changed.items.size() + 1);
    std::vector<uint32_t> places(changed.items.size() + 1);
    for (uint32_t i = 0; i < changed.items.size(); i++)
    {
        firsts[i] = static_cast<uint32_t>(after.items.size());
        const auto found = wrong.find(i);
        if (found == wrong.end())
        {
            places[i] = firsts[i];
            after.items.push_back(std::move(changed.items[i]));
            continue;
        }

        const auto& [set, parts] = found->second;
        Command command = std::move(changed.items[i]);
        for (std::size_t part = 0; part < parts.size(); part++)
        {
            uint8_t byte = 0x80;
            while (tables.Frames(byte, set) != parts[part])
            {
                byte++;
            }

            Command length;
            length.kind = Kind::kOther;
            length.encoding.opcode = byte;
            after.items.push_back(std::move(length));

            Command piece = part == 0 ? command : Hold();
            format::SetBrownieWait(piece, parts[part]);
            if (part == 0)
            {
                places[i] = static_cast<uint32_t>(after.items.size());
            }

            after.items.push_back(std::move(piece));
        }
    }

    firsts.back() = static_cast<uint32_t>(after.items.size());
    places.back() = firsts.back();
    MoveTargets(after, [&firsts](uint32_t item) { return firsts.at(item); });

    for (std::optional<uint32_t>& item : edited.items)
    {
        if (item)
        {
            item = places.at(*item);
        }
    }

    for (uint32_t& item : edited.added)
    {
        item = places.at(item);
    }

    MoveItemPlaces(edited.file, sequence, [&firsts](uint32_t item) { return firsts.at(item); });
    changed = std::move(after);
}

// Throws std::invalid_argument with a message for the user where the bytes of revision `revision` of Brownie Brown's
// driver can't hold `changed` (BrownieWritable).
void CheckBytes(const Sequence& changed, brownie::Revision revision)
{
    // The bytes have to hold each command where its channels read it, or an export would refuse the edit much later.
    const auto encode = [&]
    {
        format::DriverOf(format::Engine::kBrownie)->Encode(changed, {}, static_cast<uint8_t>(revision));
    };
    CheckEncodes("Brownie Brown's driver can't hold this edit: ", encode);
}

} // namespace

namespace brownie_parts
{

std::optional<ValueRange> BrownieRange(const Command& command, Source source, bool time, brownie::Revision revision)
{
    if (source != Source::kValue)
    {
        return std::nullopt;
    }

    if (time)
    {
        if (command.kind != Kind::kRelease)
        {
            return std::nullopt;
        }

        return ValueRange{kOne, int64_t{kMostFrames} * kOne};
    }

    int64_t lowest = 0;
    int64_t highest = 0xFF;
    switch (command.kind)
    {
    case Kind::kNote:
    case Kind::kWait:
    case Kind::kNoteOff:
        return ValueRange{kOne, int64_t{kMostFrames} * kOne};
    case Kind::kRelease:
    case Kind::kProgram:
    case Kind::kPan:
    case Kind::kVolume:
    case Kind::kLoopStart:
        break;
    case Kind::kSweep:
        if (command.index != 0)
        {
            return std::nullopt;
        }
        break;
    case Kind::kTranspose:
    case Kind::kTune:
        lowest = -0x80;
        highest = 0x7F;
        break;
    default:
        return std::nullopt;
    }

    const ntdlib::commands::Scale scale = brownie::ScaleOf(command.kind, revision);

    return ValueRange{ntdlib::commands::ToUnit(lowest, scale), ntdlib::commands::ToUnit(highest, scale)};
}

Edited BrownieWritable(const format::File& before, Edited edited, uint32_t sequence)
{
    const std::shared_ptr<const Tables> tables = TablesOf(before);
    const brownie::Revision revision = tables->revision;
    Sequence changed = *edited.file.sequences[sequence];
    DropUnreadLengths(changed, edited, sequence, *before.sequences[sequence]);
    CheckWaits(changed);

    for (int round = 0;; round++)
    {
        // The commands that their channels read with another length than their frames. A length byte is inserted before
        // each, and the commands after it read its length up to the next length byte, call, jump or place where code
        // enters. Those whose frames match the byte require none.
        const format::BrownieReadings readings = format::ReadBrownieLengths(changed, revision, tables->Length());
        const std::set<uint32_t> entered = EnteredItems(changed, edited.file, sequence);
        const std::map<uint32_t, std::pair<int, std::vector<uint32_t>>> wrong =
            WrongLengths(changed, readings, entered, *tables, revision);
        if (wrong.empty())
        {
            break;
        }
        if (round == kMostRounds)
        {
            throw std::invalid_argument("Hokuto can't settle the length bytes of this edit.");
        }

        InsertLengthBytes(changed, wrong, *tables, edited, sequence);
    }

    AlignTables(changed, edited, sequence, revision);

    CheckBytes(changed, revision);
    edited.file.sequences[sequence] = std::move(changed);

    return edited;
}

} // namespace brownie_parts

} // namespace hokuto::edit
