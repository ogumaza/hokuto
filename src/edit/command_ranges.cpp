// SPDX-License-Identifier: MIT

// The ranges of the commands' values on each console and driver, and the nearest values that they hold
// (edit/commands.h).

#include <algorithm>
#include <cstdint>
#include <optional>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/dse/commands.h"
#include "ntdlib/konami/commands.h"
#include "ntdlib/mp2k/commands.h"
#include "ntdlib/ntr/snd/commands.h"
#include "ntdlib/nwsnd/commands.h"
#include "ntdlib/nwsnd/units.h"

#include "edit/commands.h"
#include "edit/drivers.h"
#include "edit/engine_edits.h"
#include "edit/engine_ranges.h"
#include "edit/konami.h"
#include "edit/mp2k.h"
#include "format/command_values.h"
#include "song/archive.h"
#include "song/konami.h"

namespace hokuto::edit
{
namespace
{

using ntdlib::commands::Argument;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::Source;
using ntdlib::nwsnd::Scale;

// The highest tempo that the players of the 3DS, the Wii U and the Switch play, and the Wii's from March 2008 on.
constexpr int64_t kHighestTempo = 1023;

// `argument` at the nearest value that the console holds on `scale`. A variable's number stays as it is.
void ToNearest(Argument& argument, const Scale& scale)
{
    if (argument.source == Source::kVariable)
    {
        return;
    }

    argument.value = ntdlib::nwsnd::ToUnit(ntdlib::nwsnd::FromUnit(argument.value, scale), scale);
    if (argument.source == Source::kRandom)
    {
        argument.high = ntdlib::nwsnd::ToUnit(ntdlib::nwsnd::FromUnit(argument.high, scale), scale);
    }
}

} // namespace

namespace range_detail
{

// The values that MusicPlayer2000's bytes hold in the value of `command` from `source` (ntdlib/mp2k/commands.h). A
// command takes a byte, and a key shift a signed byte. A pan, a bend and a fine tune take a byte below 0x80: the driver
// subtracts 0x40. A tempo starts at 1. A note lasts up to kLongestMp2kNote ticks, or up to kLongestMp2kCode where a
// note reads 2 bytes (`note_arguments`). A wait splits into the codes' waits (Mp2kWritable). An address takes 4 bytes.
// Only MEMACC's commands take a variable: a byte of the memory area, by its offset. No command takes a time or a random
// value.
std::optional<ValueRange> Mp2kRange(const Command& command, Source source, bool time, int note_arguments)
{
    const bool memacc = (command.kind >= Kind::kSetVariable && command.kind <= Kind::kSubtractVariable) ||
                        (command.kind >= Kind::kJumpIfEqual && command.kind <= Kind::kJumpIfLess);
    if (time || source == Source::kRandom || (source == Source::kVariable && !memacc))
    {
        return std::nullopt;
    }
    if (source == Source::kVariable)
    {
        return ValueRange{0, 0xFF};
    }

    int64_t lowest = 0;
    int64_t highest = 0xFF;
    switch (command.kind)
    {
    case Kind::kNote:
        highest = note_arguments < 3 ? kLongestMp2kCode : kLongestMp2kNote;
        break;

    case Kind::kWait:
        highest = 0xFFFF;
        break;

    case Kind::kTranspose:
        lowest = -0x80;
        highest = 0x7F;
        break;

    case Kind::kPan:
    case Kind::kPitchBend:
    case Kind::kTune:
        highest = 0x7F;
        break;

    case Kind::kTempo:
        lowest = 1;
        break;

    case Kind::kSampleWave:
    case Kind::kSampleOffset:
        highest = 0xFFFFFFFF;
        break;

    default:
        break;
    }

    const Scale scale = ntdlib::mp2k::ScaleOf(command.kind);

    return ValueRange{ntdlib::commands::ToUnit(lowest, scale), ntdlib::commands::ToUnit(highest, scale)};
}

// The values that NITRO-Composer's bytes hold in the value of `command` from `source` (ntdlib/ntr/snd/commands.h). A
// note's length and a wait take a variable-length number, and a program the driver's 16 bits of it. The commands from
// 0xC0 to 0xDF take a byte, a transpose and a bend a signed one. The commands of the variables, an LFO's delay, a tempo
// and a sweep take 16 signed bits. A tempo starts at 1. A random value takes two numbers of 16 signed bits, held to the
// command's values. A variable is one of the player's 16 or one of the 16 global ones (ntdlib's driver,
// VariableAddress). No command takes a time, and only the commands that read a byte or a number take a random value or
// a variable. HeldValuesOf in edit/ds_conversion.cpp returns the values that the DS plays instead, for a conversion.
std::optional<ValueRange> NtrRange(const Command& command, Source source, bool time)
{
    const Kind kind = command.kind;
    const bool targets = ntdlib::commands::HasTarget(kind);
    if (time || (source != Source::kValue && (targets || kind == Kind::kAllocateTracks || kind == Kind::kLoopEnd ||
                                              kind == Kind::kReturn || kind == Kind::kEnd)))
    {
        return std::nullopt;
    }

    if (source == Source::kVariable)
    {
        return ValueRange{0, 31};
    }

    int64_t lowest = -0x8000;
    int64_t highest = 0x7FFF;
    switch (kind)
    {
    case Kind::kNote:
    case Kind::kWait:
        lowest = 0;
        highest = source == Source::kRandom ? 0x7FFF : 0x7FFFFFFF;
        break;

    case Kind::kProgram:
        lowest = 0;
        highest = source == Source::kRandom ? 0x7FFF : 0xFFFF;
        break;

    case Kind::kTranspose:
    case Kind::kPitchBend:
        lowest = -0x80;
        highest = 0x7F;
        break;

    case Kind::kTempo:
        lowest = 1;
        break;

    case Kind::kLfoDelay:
    case Kind::kSweepPitch:
        break;

    default:
        if ((kind < Kind::kSetVariable || kind > Kind::kRandomVariable) &&
            (kind < Kind::kCompareEqual || kind > Kind::kCompareNotEqual))
        {
            lowest = 0;
            highest = 0xFF;
        }
        break;
    }

    const Scale scale = ntdlib::ntr::snd::ScaleOf(kind);

    return ValueRange{ntdlib::commands::ToUnit(lowest, scale), ntdlib::commands::ToUnit(highest, scale)};
}

// The values that DSE's bytes hold in the value of `command` from `source` (ntdlib/dse/commands.h). A note's length and
// a wait take up to 3 bytes. A program, a bend range and a repeat's count take a byte, and a tempo a byte from 1. The
// driver clamps a pan at 0x7F, and its later build reads a volume above 0x7F as a negative one. An expression scales
// that volume. No command takes a time, a random value or a variable.
std::optional<ValueRange> DseRange(const Command& command, Source source, bool time)
{
    if (time || source != Source::kValue)
    {
        return std::nullopt;
    }

    int64_t lowest = 0;
    int64_t highest = 0xFF;
    switch (command.kind)
    {
    case Kind::kNote:
    case Kind::kWait:
        highest = 0xFFFFFF;
        break;
    case Kind::kTempo:
        lowest = 1;
        break;
    case Kind::kPan:
    case Kind::kVolume:
    case Kind::kVolume2:
        highest = 0x7F;
        break;
    default:
        break;
    }

    const Scale scale = ntdlib::dse::ScaleOf(command.kind);

    return ValueRange{ntdlib::commands::ToUnit(lowest, scale), ntdlib::commands::ToUnit(highest, scale)};
}

} // namespace range_detail

// The values that the bytes of Konami's driver hold in the value of `command` (ntdlib/konami/commands.h). A delay
// accepts what the delays of the revision can store. A call plays from 1 to 255 commands. A volume runs from 0 to the
// most that track `track` plays (song::KonamiMostVolume): 15, or 127 on a sampled track of the builds that store a
// volume in a byte. A PSG channel of those builds starts at the low 4 bits of a higher volume. A pan runs across the
// revision's scale, and a pitch bend over a byte, or over 12 bits in those builds. A rest, a release, a loop's two ends
// and an end hold no value. The other commands take a byte. No command takes a time, a random value or a variable.
std::optional<ValueRange> KonamiRange(const Command& command, Source source, bool time,
                                      ntdlib::konami::Revision revision, int track)
{
    if (time || source != Source::kValue)
    {
        return std::nullopt;
    }

    const bool ultimate = ntdlib::konami::UltimateMasters(revision);
    const bool dungeon = revision == ntdlib::konami::Revision::kDungeonDiceMonsters;
    int64_t lowest = 0;
    int64_t highest = 0xFF;
    switch (command.kind)
    {
    case Kind::kNote:
    case Kind::kNoteOff:
    case Kind::kRelease:
    case Kind::kLoopStart:
    case Kind::kLoopEnd:
    case Kind::kEnd:
        return std::nullopt;

    case Kind::kWait:
        highest = KonamiLongestWait(revision);
        break;

    case Kind::kCall:
        lowest = 1;
        break;

    case Kind::kVolume:
        highest = song::KonamiMostVolume(revision, track);
        break;

    case Kind::kPan:
        lowest = ultimate ? 0 : -15;
        highest = ultimate ? 0x7F : 15;
        break;

    case Kind::kPitchBend:
        lowest = ultimate ? -0x400 : dungeon ? -32 : -0x40;
        highest = ultimate ? 0x3FF : dungeon ? 31 : 0x3F;
        break;

    default:
        break;
    }

    const Scale scale = ntdlib::konami::ScaleOf(command.kind, revision);

    return ValueRange{ntdlib::commands::ToUnit(lowest, scale), ntdlib::commands::ToUnit(highest, scale)};
}

std::optional<ValueRange> ArgumentRange(const song::Console& console, int track, const Command& command, Source source,
                                        bool time)
{
    if (command.kind == Kind::kData || command.kind == Kind::kOther ||
        (!time && !ntdlib::commands::UnitOf(command.kind)) || !song::Runs(console, command.kind, command.index))
    {
        return std::nullopt;
    }

    return EditsOf(console).Range(console, track, command, source, time);
}

namespace range_detail
{

std::optional<ValueRange> ArchiveRange(const song::Console& console, const Command& command, Source source, bool time)
{
    // The values that the encoder writes and the player reads back.
    const ntdlib::nwsnd::Grammar grammar = song::GrammarOf(console);
    const Scale scale = time ? ntdlib::nwsnd::kTicksScale : ntdlib::nwsnd::ScaleOf(command.kind, grammar);
    std::optional<ValueRange> range = format::HeldRange(grammar, command, source, time);

    // A tempo or a timebase of 0 stops the player's clock. A negative tempo plays as 0 on every console but the Wii
    // before March 2008. The same players hold a tempo to 1023 (ntdlib's TempoOf).
    const bool clock = command.kind == Kind::kTempo || command.kind == Kind::kTimebase;
    if (range && clock && !time && source != Source::kVariable)
    {
        range->lowest = std::max(range->lowest, ntdlib::nwsnd::ToUnit(1, scale));
        if (command.kind == Kind::kTempo && (!console.wii || console.wii->SinceMarch2008()))
        {
            range->highest = std::min(range->highest, ntdlib::nwsnd::ToUnit(kHighestTempo, scale));
        }
    }

    return range;
}

} // namespace range_detail

Command Nearest(const song::Console& console, Command command)
{
    if (command.kind == Kind::kData)
    {
        return command;
    }

    if (console.engine == format::Engine::kGba || console.engine == format::Engine::kDs ||
        console.engine == format::Engine::kDse || console.driver)
    {
        command.velocity = ntdlib::commands::ToUnit(ntdlib::commands::FromUnit(command.velocity, Scale{}), Scale{});
        ToNearest(command.value, console.engine == format::Engine::kGba  ? ntdlib::mp2k::ScaleOf(command.kind)
                                 : console.engine == format::Engine::kDs ? ntdlib::ntr::snd::ScaleOf(command.kind)
                                 : console.driver                        ? console.driver->ScaleOf(command.kind)
                                                                         : ntdlib::dse::ScaleOf(command.kind));
        return command;
    }

    const ntdlib::nwsnd::Grammar grammar = song::GrammarOf(console);
    command.velocity = ntdlib::nwsnd::ToUnit(ntdlib::nwsnd::FromUnit(command.velocity, ntdlib::nwsnd::kLevelScale),
                                             ntdlib::nwsnd::kLevelScale);
    ToNearest(command.value, ntdlib::nwsnd::ScaleOf(command.kind, grammar));
    if (command.time)
    {
        ToNearest(*command.time, ntdlib::nwsnd::kTicksScale);
    }

    return command;
}

} // namespace hokuto::edit
