// SPDX-License-Identifier: MIT

// The waits of Ubisoft Milan's GBA driver in a Hokuto file (format/ubimilan.h).

#include "format/ubimilan.h"

#include <stdexcept>
#include <string>

namespace hokuto::format
{

bool IsUbiMilanFirstWait(const ntdlib::commands::Sequence& sequence, std::size_t item)
{
    return item == 1 && sequence.items.size() > 1 && sequence.items[0].kind == ntdlib::commands::Kind::kData;
}

ntdlib::commands::Sequence UbiMilanTicks(ntdlib::commands::Sequence sequence)
{
    using ntdlib::commands::kOne;
    for (std::size_t item = 0; item < sequence.items.size(); item++)
    {
        ntdlib::commands::Command& command = sequence.items[item];
        if (command.kind == ntdlib::commands::Kind::kWait && !IsUbiMilanFirstWait(sequence, item) &&
            command.value.value != 0)
        {
            command.value.value += kOne;
        }
    }

    return sequence;
}

ntdlib::commands::Sequence UbiMilanCountdowns(ntdlib::commands::Sequence sequence)
{
    using ntdlib::commands::kOne;
    for (std::size_t item = 0; item < sequence.items.size(); item++)
    {
        ntdlib::commands::Command& command = sequence.items[item];
        if (command.kind != ntdlib::commands::Kind::kWait)
        {
            continue;
        }

        const int64_t ticks = command.value.value / kOne;
        if (command.value.source != ntdlib::commands::Source::kValue || command.value.value % kOne != 0 || ticks < 0)
        {
            throw std::invalid_argument("a wait has to last a whole number of ticks");
        }

        if (IsUbiMilanFirstWait(sequence, item))
        {
            if (ticks > kUbiMilanLongestCountdown)
            {
                throw std::invalid_argument("the first command can start at most " +
                                            std::to_string(kUbiMilanLongestCountdown) + " ticks in");
            }

            continue;
        }

        if (ticks == 1)
        {
            throw std::invalid_argument("the driver can't play a command one tick after another");
        }
        if (ticks > kUbiMilanLongestCountdown + 1)
        {
            throw std::invalid_argument("a wait can last at most " + std::to_string(kUbiMilanLongestCountdown + 1) +
                                        " ticks");
        }

        command.value.value = ticks == 0 ? 0 : (ticks - 1) * kOne;
    }

    return sequence;
}

std::vector<uint32_t> UbiMilanItemOffsets(const ntdlib::commands::Sequence& sequence)
{
    using ntdlib::commands::Command;
    using ntdlib::commands::Kind;
    const std::vector<Command>& items = sequence.items;
    const bool whole = items.size() > 1 && items[0].kind == Kind::kData &&
                       items[0].bytes.size() == ntdlib::ubimilan::kSequenceHeader &&
                       items[0].bytes[0] == ntdlib::ubimilan::kSequence && items[1].kind == Kind::kWait;
    if (whole)
    {
        return ntdlib::ubimilan::EncodeSequence(UbiMilanCountdowns(sequence)).offsets;
    }

    // Each item is placed between a first wait and an end. It then uses the same bytes as in any sequence.
    const Command header{.kind = Kind::kData, .bytes = {ntdlib::ubimilan::kSequence, 0, 0, 0}};
    const Command wait{.kind = Kind::kWait};
    const Command end{.kind = Kind::kEnd};
    const Command command{.kind = Kind::kProgram};
    std::vector<uint32_t> offsets;
    uint32_t size = 0;
    for (const Command& item : items)
    {
        offsets.push_back(size);
        if (item.kind == Kind::kData)
        {
            size += static_cast<uint32_t>(item.bytes.size());
            continue;
        }

        ntdlib::commands::Sequence alone;
        alone.items = item.kind == Kind::kWait  ? std::vector<Command>{header, wait, command, item, end}
                      : item.kind == Kind::kEnd ? std::vector<Command>{header, wait, item}
                                                : std::vector<Command>{header, wait, item, wait, end};
        const std::size_t place = item.kind == Kind::kWait ? 3 : 2;
        const std::vector<uint32_t> encoded = ntdlib::ubimilan::EncodeSequence(UbiMilanCountdowns(alone)).offsets;
        size += encoded[place + 1] - encoded[place];
    }

    offsets.push_back(size);

    return offsets;
}

} // namespace hokuto::format
