// SPDX-License-Identifier: MIT

// The rules of the commands of Brownie Brown's GBA driver in a Hokuto file (format/brownie_items.h).

#include "format/brownie_items.h"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <tuple>

namespace hokuto::format
{
namespace
{

// The byte of `item`, a command of a channel, with the bytes of `revision` that read as another command. Empty for a
// note and for a command of another kind, after the checks of their bytes. Throws std::invalid_argument with the
// reason for a command that the bytes can't hold.
std::optional<uint8_t> BrownieCommandByte(const ntdlib::commands::Command& item, ntdlib::brownie::Revision revision)
{
    std::optional<uint8_t> usual;
    switch (item.kind)
    {
    case ntdlib::commands::Kind::kNote:
        if (item.key >= 0x80)
        {
            throw std::invalid_argument("a note's byte lies past the driver's notes");
        }

        return std::nullopt;

    case ntdlib::commands::Kind::kWait:
        usual = 0xFE;
        break;

    case ntdlib::commands::Kind::kNoteOff:
        usual = item.index == 0 ? 0xEC : 0xED;
        break;

    case ntdlib::commands::Kind::kRelease:
        usual = 0xE0;
        break;

    case ntdlib::commands::Kind::kProgram:
        usual = item.index == 0 ? 0xE1 : item.index == 1 ? 0xF0 : item.index == 2 ? 0xF1 : 0xFC;
        break;

    case ntdlib::commands::Kind::kPan:
        usual = item.index == 0 ? 0xEA : 0xFD;
        break;

    case ntdlib::commands::Kind::kVolume:
        usual = 0xF2;
        break;

    case ntdlib::commands::Kind::kTranspose:
        usual = 0xF3;
        break;

    case ntdlib::commands::Kind::kTune:
        usual = 0xF9;
        break;

    case ntdlib::commands::Kind::kSweep:
        usual = item.index == 0 ? 0xFA : 0xFB;
        break;

    case ntdlib::commands::Kind::kLoopStart:
        usual = 0xF4;
        break;

    case ntdlib::commands::Kind::kLoopEnd:
        usual = 0xF5;
        break;

    case ntdlib::commands::Kind::kCall:
        usual = 0xF6;
        break;

    case ntdlib::commands::Kind::kReturn:
        usual = 0xF7;
        break;

    case ntdlib::commands::Kind::kJump:
        usual = 0xF8;
        break;

    case ntdlib::commands::Kind::kEnd:
        usual = 0xFF;
        break;

    case ntdlib::commands::Kind::kOther:
        {
            const uint8_t opcode = static_cast<uint8_t>(item.encoding.opcode);
            if (opcode < 0x80 || item.bytes.size() + 1 != ntdlib::brownie::CommandSize(revision, opcode))
            {
                throw std::invalid_argument("a command holds another number of bytes than the driver reads");
            }

            return std::nullopt;
        }

    default:
        throw std::invalid_argument("Brownie Brown's driver has no " + std::string(ntdlib::commands::Name(item.kind)) +
                                    " command");
    }

    return usual;
}

// Checks the argument's byte of `item`. Throws std::invalid_argument when the value doesn't fit in it.
void CheckBrownieArgument(const ntdlib::commands::Command& item, ntdlib::brownie::Revision revision)
{
    const auto check = [&item, revision](int64_t low, int64_t high)
    {
        const int64_t value =
            ntdlib::commands::FromUnit(item.value.value, ntdlib::brownie::ScaleOf(item.kind, revision));
        if (item.value.source != ntdlib::commands::Source::kValue || value < low || value > high)
        {
            throw std::invalid_argument("a value of a " + std::string(ntdlib::commands::Name(item.kind)) +
                                        " command doesn't fit in the driver's bytes");
        }
    };

    switch (item.kind)
    {
    case ntdlib::commands::Kind::kRelease:
    case ntdlib::commands::Kind::kProgram:
    case ntdlib::commands::Kind::kPan:
    case ntdlib::commands::Kind::kVolume:
    case ntdlib::commands::Kind::kLoopStart:
        check(0, 255);
        break;

    case ntdlib::commands::Kind::kSweep:
        if (item.index == 0)
        {
            check(0, 255);
        }
        break;

    case ntdlib::commands::Kind::kTranspose:
    case ntdlib::commands::Kind::kTune:
        check(-128, 127);
        break;

    case ntdlib::commands::Kind::kCall:
    case ntdlib::commands::Kind::kJump:
        if (item.index > 0xFF)
        {
            throw std::invalid_argument("a call or a jump names a word past the table's 256");
        }
        break;

    default:
        break;
    }
}

// Throws std::invalid_argument with the reason when the bytes of `revision` can't hold `item`, a command of a
// channel: a command that the revision doesn't hold or ignores, or a value past its byte.
void CheckBrownieCommand(const ntdlib::commands::Command& item, ntdlib::brownie::Revision revision)
{
    // The command's byte, with the revision's bytes that read as another command.
    const std::optional<uint8_t> usual = BrownieCommandByte(item, revision);
    if (!usual)
    {
        return;
    }

    if (item.encoding.opcode & ntdlib::brownie::kKeptBytes)
    {
        if (ntdlib::brownie::CommandOf(revision, static_cast<uint8_t>(item.encoding.opcode)) != *usual)
        {
            throw std::invalid_argument("a " + std::string(ntdlib::commands::Name(item.kind)) +
                                        " command keeps a byte that the driver reads otherwise");
        }
    }
    else if (*usual >= 0xE0 && ntdlib::brownie::IsNoOp(revision, *usual))
    {
        throw std::invalid_argument("this revision of the driver does nothing with a " +
                                    std::string(ntdlib::commands::Name(item.kind)) + " command");
    }

    CheckBrownieArgument(item, revision);
}

} // namespace

bool BrownieIsWord(const ntdlib::commands::Command& item)
{
    return item.kind == ntdlib::commands::Kind::kOpenTrack ||
           (item.kind == ntdlib::commands::Kind::kJump && item.encoding.opcode == ntdlib::brownie::kAddressWord) ||
           (item.kind == ntdlib::commands::Kind::kEnd && item.index == 1);
}

std::vector<uint32_t> BrownieItemOffsets(const ntdlib::commands::Sequence& sequence, ntdlib::brownie::Revision revision)
{
    // The bytes that ntdlib's encoder writes for each item. A word starts on a multiple of 4 in the ROM. A region
    // starts with a word, or with the bytes before its first word. Those bytes keep the layout of the ROM.
    const auto size_of = [](const ntdlib::commands::Command& item) -> uint32_t
    {
        switch (item.kind)
        {
        case ntdlib::commands::Kind::kOpenTrack:
            return 8;
        case ntdlib::commands::Kind::kData:
            return static_cast<uint32_t>(item.bytes.size());
        case ntdlib::commands::Kind::kNote:
        case ntdlib::commands::Kind::kWait:
        case ntdlib::commands::Kind::kNoteOff:
        case ntdlib::commands::Kind::kLoopEnd:
        case ntdlib::commands::Kind::kReturn:
            return 1;
        case ntdlib::commands::Kind::kOther:
            return 1 + static_cast<uint32_t>(item.bytes.size());
        case ntdlib::commands::Kind::kSweep:
            return item.index == 0 ? 2 : 1;
        case ntdlib::commands::Kind::kEnd:
            return item.index == 1 ? 4 : 1;
        case ntdlib::commands::Kind::kJump:
            return item.encoding.opcode == ntdlib::brownie::kAddressWord ? 4 : 2;
        case ntdlib::commands::Kind::kRelease:
        case ntdlib::commands::Kind::kProgram:
        case ntdlib::commands::Kind::kPan:
        case ntdlib::commands::Kind::kVolume:
        case ntdlib::commands::Kind::kTranspose:
        case ntdlib::commands::Kind::kTune:
        case ntdlib::commands::Kind::kLoopStart:
        case ntdlib::commands::Kind::kCall:
            return 2;
        default:
            throw std::invalid_argument("Brownie Brown's driver has no " +
                                        std::string(ntdlib::commands::Name(item.kind)) + " command");
        }
    };

    uint32_t before = 0;
    for (const ntdlib::commands::Command& item : sequence.items)
    {
        if (BrownieIsWord(item))
        {
            break;
        }

        before += size_of(item);
    }

    const uint32_t remainder = (4 - before % 4) % 4;

    // Each command has to be one that the revision reads, with values that its bytes hold.
    std::vector<uint32_t> offsets;
    offsets.reserve(sequence.items.size() + 1);
    uint32_t offset = 0;
    for (const ntdlib::commands::Command& item : sequence.items)
    {
        if (BrownieIsWord(item))
        {
            offset += (4 - (remainder + offset) % 4) % 4;
        }

        if (!BrownieIsWord(item) && item.kind != ntdlib::commands::Kind::kData)
        {
            CheckBrownieCommand(item, revision);
        }

        offsets.push_back(offset);
        offset += size_of(item);
    }

    offsets.push_back(offset);

    return offsets;
}

bool BrownieWaits(const ntdlib::commands::Command& item)
{
    switch (item.kind)
    {
    case ntdlib::commands::Kind::kNote:
    case ntdlib::commands::Kind::kWait:
    case ntdlib::commands::Kind::kNoteOff:
    case ntdlib::commands::Kind::kRelease:
        return true;
    default:
        return false;
    }
}

std::optional<uint8_t> BrownieLengthByte(const ntdlib::commands::Command& item)
{
    if (item.kind != ntdlib::commands::Kind::kOther || (item.encoding.opcode & ntdlib::brownie::kKeptBytes) ||
        item.encoding.opcode < 0x80 || item.encoding.opcode >= 0xE0)
    {
        return std::nullopt;
    }

    return static_cast<uint8_t>(item.encoding.opcode);
}

std::optional<int> BrownieLengthSet(const ntdlib::commands::Command& item, ntdlib::brownie::Revision revision)
{
    if (revision != ntdlib::brownie::Revision::kSwordOfMana || item.kind != ntdlib::commands::Kind::kOther ||
        item.encoding.opcode != 0xEE || item.bytes.size() != 1)
    {
        return std::nullopt;
    }

    return item.bytes[0];
}

uint32_t BrownieWaitOf(const ntdlib::commands::Command& item)
{
    const ntdlib::commands::Argument* frames = &item.value;
    if (item.kind == ntdlib::commands::Kind::kRelease)
    {
        if (!item.time)
        {
            return 0;
        }

        frames = &*item.time;
    }
    if (frames->source != ntdlib::commands::Source::kValue || frames->value <= 0 ||
        frames->value % ntdlib::commands::kOne != 0)
    {
        return 0;
    }

    return static_cast<uint32_t>(frames->value / ntdlib::commands::kOne);
}

void SetBrownieWait(ntdlib::commands::Command& item, uint32_t frames)
{
    const ntdlib::commands::Argument wait{ntdlib::commands::Source::kValue,
                                          static_cast<int64_t>(frames) * ntdlib::commands::kOne, 0};
    if (item.kind == ntdlib::commands::Kind::kRelease)
    {
        item.time = frames != 0 ? std::optional(wait) : std::nullopt;
        return;
    }

    item.value = frames != 0 ? wait : ntdlib::commands::Argument{};
}

BrownieReadings ReadBrownieLengths(const ntdlib::commands::Sequence& sequence, ntdlib::brownie::Revision revision,
                                   const BrownieLength& length)
{
    using ntdlib::commands::Kind;
    const std::vector<ntdlib::commands::Command>& items = sequence.items;
    BrownieReadings readings{std::vector<std::set<uint32_t>>(items.size()), std::vector<std::set<int>>(items.size())};

    // A channel's place: the item, the length, the length set, the item after the loop's start, and the item that a
    // return goes back to. -1 stands for none.
    using State = std::tuple<uint32_t, uint32_t, int, int64_t, int64_t>;
    std::set<State> seen;
    std::vector<State> pending;
    for (const ntdlib::commands::Command& item : items)
    {
        if (item.kind == Kind::kOpenTrack && !item.target_outside)
        {
            pending.emplace_back(item.target, 256, 0, -1, -1);
        }
    }
    while (!pending.empty())
    {
        const State state = pending.back();
        pending.pop_back();
        if (!seen.insert(state).second)
        {
            continue;
        }

        const auto [at, frames, set, loop, back] = state;
        if (at >= items.size())
        {
            continue;
        }

        const ntdlib::commands::Command& item = items[at];
        readings.sets[at].insert(set);
        readings.lengths[at].insert(frames);
        const auto on = [&](uint32_t next, uint32_t new_frames, int new_set, int64_t new_loop, int64_t new_back)
        {
            pending.emplace_back(next, new_frames, new_set, new_loop, new_back);
        };
        if (const std::optional<uint8_t> byte = BrownieLengthByte(item))
        {
            on(at + 1, length(*byte, set), set, loop, back);
            continue;
        }

        if (const std::optional<int> chosen = BrownieLengthSet(item, revision))
        {
            on(at + 1, frames, *chosen, loop, back);
            continue;
        }

        switch (item.kind)
        {
        case Kind::kNote:
        case Kind::kWait:
        case Kind::kNoteOff:
        case Kind::kRelease:
            on(at + 1, frames, set, loop, back);
            break;
        case Kind::kLoopStart:
            on(at + 1, frames, set, at + 1, back);
            break;
        case Kind::kLoopEnd:
            if (loop >= 0)
            {
                on(static_cast<uint32_t>(loop), frames, set, loop, back);
            }

            on(at + 1, frames, set, loop, back);
            break;
        case Kind::kCall:
            if (!item.target_outside)
            {
                on(item.target, frames, set, loop, at + 1);
            }
            break;
        case Kind::kReturn:
            if (back >= 0)
            {
                on(static_cast<uint32_t>(back), frames, set, loop, -1);
            }
            break;
        case Kind::kJump:
            if (item.encoding.opcode != ntdlib::brownie::kAddressWord && !item.target_outside)
            {
                on(item.target, frames, set, loop, back);
            }
            break;
        case Kind::kEnd:
        case Kind::kOpenTrack:
        case Kind::kData:
            break;
        default:
            on(at + 1, frames, set, loop, back);
            break;
        }
    }

    return readings;
}

void FillBrownieWaits(ntdlib::commands::Sequence& sequence, ntdlib::brownie::Revision revision,
                      const BrownieLength& length)
{
    const BrownieReadings readings = ReadBrownieLengths(sequence, revision, length);
    for (std::size_t item = 0; item < sequence.items.size(); item++)
    {
        ntdlib::commands::Command& command = sequence.items[item];
        if (BrownieWaits(command))
        {
            const std::set<uint32_t>& read = readings.lengths[item];
            SetBrownieWait(command, read.size() == 1 ? *read.begin() : 0);
        }
    }
}

} // namespace hokuto::format
