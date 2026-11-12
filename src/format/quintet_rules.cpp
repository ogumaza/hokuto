// SPDX-License-Identifier: MIT

// The revision and the ticks of Quintet's GBA driver in a Hokuto file (format/quintet.h).

#include "format/quintet.h"

#include <stdexcept>

namespace hokuto::format
{

std::optional<uint8_t> QuintetRevision(const ntdlib::quintet::Traits& traits)
{
    if (traits.byte_notes)
    {
        return uint8_t{1};
    }
    if (traits.lengths == kQuintetLengths)
    {
        return uint8_t{0};
    }

    return std::nullopt;
}

bool QuintetTimed(ntdlib::commands::Kind kind)
{
    using ntdlib::commands::Kind;
    return kind == Kind::kNote || kind == Kind::kNoteOff || kind == Kind::kWait;
}

ntdlib::commands::Scale QuintetScale(ntdlib::commands::Kind kind)
{
    return QuintetTimed(kind) ? ntdlib::commands::Scale{kQuintetSongTicks * ntdlib::commands::kOne, 0}
                              : ntdlib::quintet::ScaleOf(kind);
}

bool QuintetFoldsDot(const ntdlib::commands::Sequence& sequence, std::size_t item)
{
    const ntdlib::commands::Command& command = sequence.items[item];
    return item > 0 && command.kind == ntdlib::commands::Kind::kOther && command.encoding.opcode == kQuintetDot &&
           command.bytes.empty() && QuintetTimed(sequence.items[item - 1].kind);
}

std::vector<uint32_t> QuintetFoldedItems(const ntdlib::commands::Sequence& sequence)
{
    std::vector<uint32_t> folded;
    uint32_t at = 0;
    for (std::size_t item = 0; item < sequence.items.size(); item++)
    {
        const bool dot = QuintetFoldsDot(sequence, item);
        at -= dot ? 1 : 0;
        folded.push_back(at);
        at++;
    }

    folded.push_back(at);

    return folded;
}

std::vector<uint32_t> QuintetUnfoldedItems(const ntdlib::commands::Sequence& sequence)
{
    std::vector<uint32_t> unfolded;
    uint32_t at = 0;
    for (const ntdlib::commands::Command& command : sequence.items)
    {
        unfolded.push_back(at);
        at += QuintetTimed(command.kind) && command.encoding.opcode == kQuintetDot ? 2 : 1;
    }

    unfolded.push_back(at);

    return unfolded;
}

ntdlib::commands::Sequence QuintetTicks(ntdlib::commands::Sequence sequence)
{
    ntdlib::commands::Sequence folded;
    folded.items.reserve(sequence.items.size());
    for (std::size_t item = 0; item < sequence.items.size(); item++)
    {
        if (QuintetFoldsDot(sequence, item))
        {
            ntdlib::commands::Command& before = folded.items.back();
            const int64_t ticks = before.value.value / ntdlib::commands::kOne;
            before.value.value = (ticks + ticks / 2) * ntdlib::commands::kOne;
            before.encoding.opcode = kQuintetDot;
            continue;
        }

        folded.items.push_back(std::move(sequence.items[item]));
    }

    for (ntdlib::commands::Command& command : folded.items)
    {
        if (QuintetTimed(command.kind))
        {
            command.value.value *= kQuintetSongTicks;
        }
    }

    sequence.items = std::move(folded.items);

    return sequence;
}

ntdlib::commands::Sequence QuintetDriverTicks(ntdlib::commands::Sequence sequence)
{
    std::vector<ntdlib::commands::Command> items;
    items.reserve(sequence.items.size());
    for (ntdlib::commands::Command& command : sequence.items)
    {
        if (!QuintetTimed(command.kind))
        {
            items.push_back(std::move(command));
            continue;
        }

        if (command.value.value % (kQuintetSongTicks * ntdlib::commands::kOne) != 0)
        {
            throw std::invalid_argument("a length has to be a whole number of the driver's ticks");
        }

        command.value.value /= kQuintetSongTicks;
        const bool dotted = command.encoding.opcode == kQuintetDot;
        if (dotted)
        {
            // A length of b ticks and its dot take b + b / 2: 3k for b = 2k, and 3k + 1 for b = 2k + 1.
            const int64_t ticks = command.value.value / ntdlib::commands::kOne;
            if (command.value.value % ntdlib::commands::kOne != 0 || ticks % 3 == 2)
            {
                throw std::invalid_argument("a dotted length has to be half again as long as a length");
            }

            command.value.value = (ticks % 3 == 0 ? ticks / 3 * 2 : ticks / 3 * 2 + 1) * ntdlib::commands::kOne;
            command.encoding.opcode = 0;
        }

        items.push_back(std::move(command));
        if (dotted)
        {
            ntdlib::commands::Command dot;
            dot.kind = ntdlib::commands::Kind::kOther;
            dot.encoding.opcode = kQuintetDot;
            items.push_back(std::move(dot));
        }
    }

    sequence.items = std::move(items);

    return sequence;
}

std::vector<uint32_t> QuintetFoldedOffsets(const ntdlib::commands::Sequence& sequence,
                                           const std::vector<uint32_t>& offsets)
{
    std::vector<uint32_t> folded;
    for (const uint32_t item : QuintetUnfoldedItems(sequence))
    {
        folded.push_back(offsets.at(item));
    }

    return folded;
}

ntdlib::quintet::Traits QuintetTraits(uint8_t revision)
{
    ntdlib::quintet::Traits traits;
    traits.byte_notes = revision == 1;
    if (!traits.byte_notes)
    {
        traits.lengths = kQuintetLengths;
    }

    return traits;
}

} // namespace hokuto::format
