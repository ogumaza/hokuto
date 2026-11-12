// SPDX-License-Identifier: MIT

#include "edit/mix.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
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
#include "ntdlib/dse/formats.h"
#include "ntdlib/sequence_export/performance.h"

#include "edit/bytecode.h"
#include "edit/drivers.h"
#include "edit/konami.h"
#include "edit/mp2k.h"
#include "song/archive.h"
#include "song/driver.h"

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

// DSE's commands that move a setting. The common form has no kinds for them. A slide contains a time of 2 bytes and its
// target, and a change contains an amount.
constexpr uint16_t kDseVolumeChange = ntdlib::dse::kVolumeChange;
constexpr uint16_t kDseVolumeSlide = ntdlib::dse::kVolumeSlide;
constexpr uint16_t kDsePanChange = ntdlib::dse::kPanChange;
constexpr uint16_t kDsePanSlide = ntdlib::dse::kPanSlide;

// The byte of a DSE pan at the centre.
constexpr int kDsePanCentre = 0x40;

// Whether `command` is a slide of `setting` on DSE.
bool IsDseSlide(const Command& command, Mix setting)
{
    const uint16_t opcode = setting == Mix::kVolume ? kDseVolumeSlide : setting == Mix::kPan ? kDsePanSlide : 0;
    return opcode != 0 && command.kind == Kind::kOther && command.encoding.opcode == opcode &&
           command.bytes.size() == 3;
}

// Whether `command` is a change of `setting` on DSE.
bool IsDseChange(const Command& command, Mix setting)
{
    const uint16_t opcode = setting == Mix::kVolume ? kDseVolumeChange : setting == Mix::kPan ? kDsePanChange : 0;
    return opcode != 0 && command.kind == Kind::kOther && command.encoding.opcode == opcode &&
           command.bytes.size() == 1;
}

// The target of `slide`, a slide of `setting`, in whole units of the setting.
int64_t DseSlideTarget(const Command& slide, Mix setting)
{
    return static_cast<int8_t>(slide.bytes[2]) - (setting == Mix::kPan ? kDsePanCentre : 0);
}

// Whether `command`, a command of Konami's driver, sets the next track's volume in its `velocity`.
bool SetsNextVolume(const Command& command)
{
    return KonamiPlaysOnNextTrack(command) || (command.kind == Kind::kVolume && command.index == 2);
}

// The setting's name in a message.
std::string NameOf(Mix setting)
{
    switch (setting)
    {
    case Mix::kVolume:
        return "Volume";
    case Mix::kPan:
        return "Pan";
    case Mix::kSendA:
        return "Send to FX A";
    case Mix::kSendB:
        return "Send to FX B";
    case Mix::kSendC:
        break;
    }

    return "Send to FX C";
}

// A value in whole units.
int64_t Whole(int64_t value)
{
    return std::llround(static_cast<double>(value) / kOne);
}

// The items of a sequence that set a setting on a track, and whether a note plays before the first of them. A track
// that runs none plays every note before them. `slides` and `changes` hold DSE's slides and changes (IsDseSlide and
// IsDseChange). A slide with a time, or a change, that comes first starts from the default. `notes` contains the items
// of Konami's driver that store the volume in their `velocity`: the track's notes, and what the track before it sets
// for it (`previous`).
struct Run
{
    std::set<uint32_t> items;
    std::set<uint32_t> slides;
    std::set<uint32_t> changes;
    std::set<uint32_t> notes;
    bool early = false;

    bool Empty() const
    {
        return items.empty() && slides.empty() && changes.empty() && notes.empty();
    }
};

Run RunOf(const ntdlib::commands::Sequence& sequence, const std::vector<uint32_t>& offsets,
          std::span<const song::Stretch> stretches, Mix setting, bool dse, bool konami,
          std::span<const song::Stretch> previous = {})
{
    Run run;
    for (const song::Stretch& stretch : previous)
    {
        const std::optional<uint32_t> first = ItemAtOffset(offsets, stretch.first);
        const std::optional<uint32_t> last = ItemAtOffset(offsets, stretch.last);
        for (uint32_t i = first.value_or(1); setting == Mix::kVolume && first && last && i <= *last; ++i)
        {
            if (SetsNextVolume(sequence.items[i]))
            {
                run.notes.insert(i);
            }
        }
    }

    bool note = false;
    for (const song::Stretch& stretch : stretches)
    {
        const std::optional<uint32_t> first = ItemAtOffset(offsets, stretch.first);
        const std::optional<uint32_t> last = ItemAtOffset(offsets, stretch.last);
        if (!first || !last || *last < *first)
        {
            throw std::invalid_argument("The track commands aren't where the mixer shows them.");
        }

        for (uint32_t i = *first; i <= *last; ++i)
        {
            const Command& command = sequence.items[i];
            if (konami && setting == Mix::kVolume && command.kind == Kind::kNote)
            {
                if (!KonamiPlaysOnNextTrack(command))
                {
                    run.notes.insert(i);
                }
            }
            else if (Sets(command, setting))
            {
                run.early = run.Empty() ? note : run.early;
                run.items.insert(i);
            }
            else if (dse && IsDseSlide(command, setting))
            {
                const bool timed = command.bytes[0] != 0 || command.bytes[1] != 0;
                run.early = run.Empty() ? note || timed : run.early;
                run.slides.insert(i);
            }
            else if (dse && IsDseChange(command, setting))
            {
                run.early = run.Empty() ? true : run.early;
                run.changes.insert(i);
            }

            note = note || command.kind == Kind::kNote;
        }
    }

    // A note of Konami's driver sets its volume. No default stands before the first of them.
    run.early = !(konami && setting == Mix::kVolume) && (run.early || run.Empty());

    return run;
}

} // namespace

int MixLowest(Mix setting)
{
    return setting == Mix::kPan ? -64 : 0;
}

int MixHighest(Mix setting, bool to_127)
{
    return setting == Mix::kPan ? 63 : to_127 ? 127 : 255;
}

Setters MixSetters(const ntdlib::commands::Sequence& commands, const std::vector<uint32_t>& offsets,
                   std::span<const song::Stretch> stretches, Mix setting, bool dse, bool konami,
                   std::span<const song::Stretch> previous)
{
    Run run = RunOf(commands, offsets, stretches, setting, dse, konami, previous);
    run.items.insert(run.slides.begin(), run.slides.end());
    run.items.insert(run.changes.begin(), run.changes.end());
    run.items.insert(run.notes.begin(), run.notes.end());
    return {std::move(run.items), run.early};
}

namespace
{

// Throws std::invalid_argument with a message for the user where the mixer can't change `setting` of track `track` on
// `console` (EditMix).
void CheckMixable(const song::Console& console, int track, Mix setting)
{
    if (console.driver)
    {
        EditsOf(*console.driver).CheckMixable(console, track, setting);
    }

    if (!song::Runs(console, KindOf(setting), BusOf(setting).value_or(0)))
    {
        throw std::invalid_argument(
            console.engine == format::Engine::kGba   ? "GBA tracks have no effect sends."
            : console.engine == format::Engine::kDse ? "DSE tracks have no effect sends."
            : console.driver && setting == Mix::kVolume
                ? "Tracks of " + std::string(console.driver->Title()) + " have no volume."
            : console.driver && setting == Mix::kPan ? EditsOf(*console.driver).RefusedPan(*console.driver)
            : console.driver ? "Tracks of " + std::string(console.driver->Title()) + " have no effect sends."
                             : "3DS tracks send only to FX A and FX B.");
    }
}

// The values that a track's notes play with (EditMix): those of the commands, and the default before the first command.
// `random` says whether a command uses a random value.
struct TrackValues
{
    std::set<int64_t> values;
    bool random = false;
    int64_t default_value = 0;
};

TrackValues TrackValuesOf(const ntdlib::commands::Sequence& commands, const Run& run, const song::Console& console,
                          Mix setting, const std::string& name)
{
    // The values that the track's notes play with: those of the commands, and the default before the first command.
    std::set<int64_t> values;
    bool random = false;
    for (const uint32_t item : run.items)
    {
        const Argument& value = commands.items[item].value;
        if (value.source == Source::kVariable)
        {
            throw std::invalid_argument(name +
                                        " comes from a variable on this track. To change it, edit the commands "
                                        "in the List Editor (Edit > Open List Editor).");
        }

        random = random || value.source == Source::kRandom;
        values.insert(Whole(value.value));
    }

    const int64_t default_value =
        setting == Mix::kVolume && console.engine == format::Engine::kDse ? song::kDseDefaultVolume
        : setting == Mix::kVolume && console.driver ? console.driver->StartVolume()
                                                    : song::MixDefault(setting, console.engine == format::Engine::kGba);
    if (run.early)
    {
        values.insert(default_value);
    }

    for (const uint32_t item : run.slides)
    {
        values.insert(DseSlideTarget(commands.items[item], setting));
    }

    for (const uint32_t item : run.notes)
    {
        values.insert(Whole(commands.items[item].velocity));
    }

    return {std::move(values), random, default_value};
}

// Adds a command at the opening of track `track` of sequence `sequence` of `result` that sets `setting` to `opening`,
// for the notes before the track's first command (EditMix). `commands` and `offsets` give the sequence before the
// edit, and `stretches` the track's stretches. The items of `result` from the track's first command on move past the
// new one.
void AddOpeningValue(const format::File& file, uint32_t sequence, int track, std::span<const song::Stretch> stretches,
                     const ntdlib::commands::Sequence& commands, const std::vector<uint32_t>& offsets,
                     const song::Console& console, Mix setting, int64_t opening, Edited& result)
{
    if (stretches.empty() || stretches.front().entry != ntdlib::sequence_export::Entry::kOpen)
    {
        throw std::invalid_argument("The track commands aren't where the mixer shows them.");
    }
    if (KeepsPlaces(file, commands))
    {
        throw std::invalid_argument("Can't add a command. Commands in this sequence must keep their positions.");
    }

    Command command;
    command.kind = KindOf(setting);
    command.index = BusOf(setting).value_or(0);
    command.value = Argument{Source::kValue, opening, 0};
    const uint32_t first = *ItemAtOffset(offsets, stretches.front().first);
    if (console.driver)
    {
        EditsOf(*console.driver).IndexOpening(commands, first, command);
    }

    const OpeningInsertion added = InsertAtOpening(result.file, sequence, first, track, {command});
    for (std::optional<uint32_t>& item : result.items)
    {
        if (*item >= added.first)
        {
            *item += added.count;
        }
    }
}

// How an edit of a mix setting moves a value (EditMix). With `trim`, each value moves with the edit: a level by the
// edit's factor (`level`), a pan by its step. Otherwise every value becomes the new one. `lowest` and `highest` give
// the setting's range.
struct MixScale
{
    const MixEdit& edit;
    bool trim = false;
    bool level = false;
    int64_t lowest = 0;
    int64_t highest = 0;

    // The new value of a value in whole units, before the setting's range stops it.
    int64_t Moved(int64_t value) const
    {
        if (trim && level)
        {
            return std::llround(static_cast<double>(value) * edit.to / edit.from);
        }
        if (trim)
        {
            return value + edit.to - edit.from;
        }

        return edit.to;
    }

    // Whether the range stops a value, and the new value in the fixed point of the commands' values.
    bool Stops(int64_t value) const
    {
        const int64_t result = Moved(value);
        return result < lowest || result > highest;
    }

    int64_t NewValue(int64_t value) const
    {
        return std::clamp(Moved(value), lowest, highest) * kOne;
    }
};

// Sets the commands of `run` in `changed`, a copy of `commands` on `console`, to their new values by `scale` (EditMix).
// Returns how many values the range stops.
std::size_t ApplyValues(ntdlib::commands::Sequence& changed, const ntdlib::commands::Sequence& commands, const Run& run,
                        const song::Console& console, const MixScale& scale)
{
    const MixEdit& edit = scale.edit;
    const DriverEdits* driver_edits = console.driver ? &EditsOf(*console.driver) : nullptr;
    std::size_t stopped = 0;
    for (const uint32_t item : run.items)
    {
        Argument& value = changed.items[item].value;
        const bool ranged = value.source == Source::kRandom;
        stopped += (scale.Stops(Whole(value.value)) || (ranged && scale.Stops(Whole(value.high)))) ? 1u : 0u;
        value.value = scale.NewValue(Whole(value.value));
        if (ranged)
        {
            value.high = scale.NewValue(Whole(value.high));
        }

        if (driver_edits != nullptr && edit.setting == Mix::kPan)
        {
            driver_edits->FollowPan(console, changed.items[item]);
        }
    }

    for (const uint32_t item : run.slides)
    {
        stopped += scale.Stops(DseSlideTarget(commands.items[item], edit.setting)) ? 1u : 0u;
        const int64_t target = Whole(scale.NewValue(DseSlideTarget(commands.items[item], edit.setting)));
        changed.items[item].bytes[2] = static_cast<uint8_t>(target + (edit.setting == Mix::kPan ? kDsePanCentre : 0));
    }

    for (const uint32_t item : run.notes)
    {
        stopped += scale.Stops(Whole(commands.items[item].velocity)) ? 1u : 0u;
        changed.items[item].velocity = scale.NewValue(Whole(commands.items[item].velocity));
    }

    for (const uint32_t item : run.changes)
    {
        if (scale.level)
        {
            uint8_t& amount = changed.items[item].bytes[0];
            const double scaled = static_cast<double>(static_cast<int8_t>(amount)) * edit.to / edit.from;
            amount = static_cast<uint8_t>(std::clamp<int64_t>(std::llround(scaled), INT8_MIN, INT8_MAX));
        }
    }

    return stopped;
}

} // namespace

MixEdited EditMix(const format::File& file, uint32_t sequence, int track, std::span<const song::Stretch> stretches,
                  const MixEdit& edit, std::span<const song::Stretch> previous)
{
    const ntdlib::commands::Sequence& commands = SequenceToEdit(file, sequence);
    const song::Console console = ConsoleOf(file);
    CheckMixable(console, track, edit.setting);
    const bool gba = console.engine == format::Engine::kGba;
    const std::vector<uint32_t> offsets = Offsets(commands, ConsoleOf(file));
    const DriverEdits* driver_edits = console.driver ? &EditsOf(*console.driver) : nullptr;
    const bool notes_set_volume = driver_edits != nullptr && driver_edits->NotesSetVolume();
    const Run run = RunOf(commands, offsets, stretches, edit.setting, console.engine == format::Engine::kDse,
                          notes_set_volume, previous);
    const std::string name = NameOf(edit.setting);
    if (driver_edits != nullptr)
    {
        driver_edits->CheckMixEdit(commands, offsets, stretches, edit.setting, run.Empty());
    }

    const auto [values, random, default_value] = TrackValuesOf(commands, run, console, edit.setting, name);

    // A setting that holds one value takes the new one. Otherwise each value moves with the edit.
    const int64_t lowest = MixLowest(edit.setting);
    const int64_t highest = console.driver && edit.setting == Mix::kVolume
                                ? console.driver->MostVolume(track)
                                : MixHighest(edit.setting, gba || console.engine == format::Engine::kDse ||
                                                               console.engine == format::Engine::kDs);
    const bool trim = random || values.size() > 1 || !run.changes.empty();
    const bool level = edit.setting != Mix::kPan;
    if (trim && level && edit.from <= 0)
    {
        throw std::invalid_argument(name +
                                    " is at -∞ dB at this point and changes as the track plays. Change it at a "
                                    "point where it's above -∞ dB.");
    }

    const MixScale scale{edit, trim, level, lowest, highest};

    // Each command takes its new value. A random value's range moves with it. Every item keeps its index. The values
    // that the range stops count once for each command.
    Edited result{file, {}, {}};
    for (uint32_t item = 0; item < commands.items.size(); item++)
    {
        result.items.emplace_back(item);
    }

    ntdlib::commands::Sequence changed = commands;
    std::size_t stopped = ApplyValues(changed, commands, run, console, scale);
    result.file.sequences[sequence] = std::move(changed);

    // The notes before the first command take the edited default from a new command at the track's start. A default
    // that the edit leaves as it is takes none. The items from the track's first command on move past the new ones.
    stopped += (run.early && scale.Stops(default_value)) ? 1u : 0u;
    const int64_t opening = scale.NewValue(default_value);
    if (run.early && opening != default_value * kOne)
    {
        AddOpeningValue(file, sequence, track, stretches, commands, offsets, console, edit.setting, opening, result);
    }

    return MixEdited{Writable(file, std::move(result), sequence), stopped};
}

} // namespace hokuto::edit
