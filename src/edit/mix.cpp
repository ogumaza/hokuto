// SPDX-License-Identifier: MIT

#include "edit/mix.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/sequence_export/performance.h"

#include "edit/bytecode.h"
#include "song/archive.h"

namespace hokuto::edit
{
namespace
{

using ntdlib::commands::Argument;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Source;
using song::Mix;

// The bus that a send goes to, from 0 for A, or nothing for another setting.
std::optional<uint8_t> BusOf(Mix setting)
{
    switch (setting)
    {
    case Mix::kSendA:
        return 0;
    case Mix::kSendB:
        return 1;
    case Mix::kSendC:
        return 2;
    default:
        return std::nullopt;
    }
}

// The kind of the commands that set `setting`.
Kind KindOf(Mix setting)
{
    switch (setting)
    {
    case Mix::kVolume:
        return Kind::kVolume;
    case Mix::kPan:
        return Kind::kPan;
    default:
        return Kind::kSend;
    }
}

// Whether `command` sets `setting`.
bool Sets(const Command& command, Mix setting)
{
    return command.kind == KindOf(setting) && (command.kind != Kind::kSend || command.index == BusOf(setting));
}

// The setting's name in a message.
std::string NameOf(Mix setting)
{
    switch (setting)
    {
    case Mix::kVolume:
        return "volume";
    case Mix::kPan:
        return "pan";
    case Mix::kSendA:
        return "send to FX A";
    case Mix::kSendB:
        return "send to FX B";
    case Mix::kSendC:
        break;
    }

    return "send to FX C";
}

// A value in whole units.
int64_t Whole(int64_t value)
{
    return std::llround(static_cast<double>(value) / kOne);
}

// The items of a sequence that set a setting on a track, and whether a note plays before the first of them. A track
// that runs none plays every note before them.
struct Run
{
    std::set<uint32_t> items;
    bool early = false;
};

Run RunOf(const ntdlib::commands::Sequence& sequence, const std::vector<uint32_t>& offsets,
          std::span<const song::Stretch> stretches, Mix setting)
{
    Run run;
    bool note = false;
    for (const song::Stretch& stretch : stretches)
    {
        const std::optional<uint32_t> first = ItemAtOffset(offsets, stretch.first);
        const std::optional<uint32_t> last = ItemAtOffset(offsets, stretch.last);
        if (!first || !last || *last < *first)
        {
            throw std::invalid_argument("The track's commands aren't where the mixer shows them.");
        }

        for (uint32_t i = *first; i <= *last; ++i)
        {
            const Command& command = sequence.items[i];
            if (Sets(command, setting))
            {
                run.early = run.items.empty() ? note : run.early;
                run.items.insert(i);
            }
            note = note || command.kind == Kind::kNote;
        }
    }
    run.early = run.early || run.items.empty();

    return run;
}

} // namespace

int MixLowest(Mix setting)
{
    return setting == Mix::kPan ? -64 : 0;
}

int MixHighest(Mix setting)
{
    return setting == Mix::kPan ? 63 : 255;
}

Setters MixSetters(const ntdlib::commands::Sequence& commands, const std::vector<uint32_t>& offsets,
                   std::span<const song::Stretch> stretches, Mix setting)
{
    Run run = RunOf(commands, offsets, stretches, setting);
    return {std::move(run.items), run.early};
}

Edited EditMix(const format::File& file, uint32_t sequence, int track, std::span<const song::Stretch> stretches,
               const MixEdit& edit)
{
    const ntdlib::commands::Sequence& commands = SequenceToEdit(file, sequence);
    if (!song::Runs(ConsoleOf(file), KindOf(edit.setting), BusOf(edit.setting).value_or(0)))
    {
        throw std::invalid_argument("The 3DS's tracks send to FX A and FX B only.");
    }
    const std::vector<uint32_t> offsets = Offsets(commands, ConsoleOf(file));
    const Run run = RunOf(commands, offsets, stretches, edit.setting);
    const std::string name = NameOf(edit.setting);

    // The values that the track's notes play with: those of the commands, and the default before the first command.
    std::set<int64_t> values;
    bool random = false;
    for (const uint32_t item : run.items)
    {
        const Argument& value = commands.items[item].value;
        if (value.source == Source::kVariable)
        {
            throw std::invalid_argument("This track's " + name +
                                        " comes from a variable. The List Editor changes its commands: Edit > Open "
                                        "List Editor.");
        }

        random = random || value.source == Source::kRandom;
        values.insert(Whole(value.value));
    }
    if (run.early)
    {
        values.insert(song::MixDefault(edit.setting));
    }

    // A setting that holds one value takes the new one. Otherwise each value moves with the edit.
    const int64_t lowest = MixLowest(edit.setting);
    const int64_t highest = MixHighest(edit.setting);
    const bool trim = random || values.size() > 1;
    const bool level = edit.setting != Mix::kPan;
    if (trim && level && edit.from <= 0)
    {
        throw std::invalid_argument("This track's " + name +
                                    " is at -∞ dB here, and it changes as the track plays. Change it where it's "
                                    "above -∞ dB.");
    }

    // The new value of a value in whole units, in the fixed point of the commands' values.
    const auto new_value = [&](int64_t value)
    {
        int64_t result = edit.to;
        if (trim && level)
        {
            result = std::llround(static_cast<double>(value) * edit.to / edit.from);
        }
        else if (trim)
        {
            result = value + edit.to - edit.from;
        }

        return std::clamp(result, lowest, highest) * kOne;
    };

    // Each command takes its new value. A random value's range moves with it. Every item keeps its index.
    Edited result{file, {}, {}};
    for (uint32_t item = 0; item < commands.items.size(); item++)
    {
        result.items.emplace_back(item);
    }
    ntdlib::commands::Sequence changed = commands;
    for (const uint32_t item : run.items)
    {
        Argument& value = changed.items[item].value;
        value.value = new_value(Whole(value.value));
        if (value.source == Source::kRandom)
        {
            value.high = new_value(Whole(value.high));
        }
    }
    result.file.sequences[sequence] = std::move(changed);

    // The notes before the first command take the edited default from a new command at the track's start. A default
    // that the edit leaves as it is takes none. The items from the track's first command on move past the new ones.
    const int64_t opening = new_value(song::MixDefault(edit.setting));
    if (run.early && opening != song::MixDefault(edit.setting) * kOne)
    {
        if (stretches.empty() || stretches.front().entry != ntdlib::sequence_export::Entry::kOpen)
        {
            throw std::invalid_argument("The track's commands aren't where the mixer shows them.");
        }
        if (Fixed(commands))
        {
            throw std::invalid_argument("This sequence's commands can't move. Hokuto can't add a command to it.");
        }

        Command command;
        command.kind = KindOf(edit.setting);
        command.index = BusOf(edit.setting).value_or(0);
        command.value = Argument{Source::kValue, opening, 0};
        const uint32_t first = *ItemAtOffset(offsets, stretches.front().first);
        const OpeningInsertion added = InsertAtOpening(result.file, sequence, first, track, {command});
        for (std::optional<uint32_t>& item : result.items)
        {
            if (*item >= added.first)
            {
                *item += added.count;
            }
        }
    }

    return result;
}

} // namespace hokuto::edit
