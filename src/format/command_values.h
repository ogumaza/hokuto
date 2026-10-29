// SPDX-License-Identifier: MIT

// The values that a console's sequences hold in a command's arguments. The text form checks each command that it reads
// against them, and the List Editor's ranges come from them (edit::ArgumentRange).

#pragma once

#include <optional>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/nwsnd/commands.h"
#include "ntdlib/sar/formats.h"

#include "format/file.h"

namespace hokuto::format
{

// The rules by which the console of `flavor` reads its sequences: the Wii U's for kCafe, the 3DS's for kCtr, and
// otherwise the Switch's.
ntdlib::nwsnd::Grammar GrammarOf(ntdlib::sar::Flavor flavor);

// The rules by which the console of `native` reads its sequences. On the Wii, the build of NW4R SND gives them
// (BuildOfWiiArchive).
ntdlib::nwsnd::Grammar GrammarOf(const NativeArchive& native);

// Whether the console's bytes hold `command` by the rules of `grammar`: the encoder writes it alone, without its
// target.
bool Holds(const ntdlib::nwsnd::Grammar& grammar, ntdlib::commands::Command command);

// The lowest and the highest value that an argument can take.
struct ValueRange
{
    friend bool operator==(const ValueRange&, const ValueRange&) = default;

    ntdlib::commands::Fixed lowest = 0;
    ntdlib::commands::Fixed highest = 0;
};

// The velocities that the bytes of `command`, a note, can hold by the rules of `grammar`. The range is in levels.
std::optional<ValueRange> HeldVelocities(const ntdlib::nwsnd::Grammar& grammar,
                                         const ntdlib::commands::Command& command);

// The values that the bytes of `command` can hold by the rules of `grammar` in its value from `source`, or with `time`
// in its time: in the unit of the command's kind, in ticks for a time, or as a variable's number from a variable. A
// random value's range holds both ends. Nothing where the command can't take such an argument.
std::optional<ValueRange> HeldRange(const ntdlib::nwsnd::Grammar& grammar, const ntdlib::commands::Command& command,
                                    ntdlib::commands::Source source, bool time = false);

} // namespace hokuto::format
