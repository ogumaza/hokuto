// SPDX-License-Identifier: MIT

// Edits of any command of a sequence, for the List Editor. The editor lists a track's commands in the order that the
// track ran them: each stretch of its commands, from the stretch's first command to its last (song::Stretch). A command
// that several stretches ran, such as a command of a subroutine with two calls, shows once for each. An edit of the
// command changes every pass that runs it.
//
// A change gives a command new values: a note's key, velocity or length, a wait's length, a setting's value, the time
// over which a setting moves to its value, the track, bus, LFO or variable that a command addresses, the target of a
// jump, a call or a track start, and whether the command runs only if the last comparison held. A value can come from a
// random number or a variable wherever the console's bytes allow it. Each value has to fit the console's bytes, and
// takes the nearest value that the console holds. Every other command stays as it is. A longer wait therefore moves the
// commands after it later.
//
// A new command goes in before another command. Where the track entered the sequence at that command, by its start, a
// jump, a call, a return or a loop's end, code that enters there runs the new command first. So does every sound that
// starts at the bytecode's first command. Elsewhere, only the command before it runs on into it. A removed command
// takes the code that entered at it to the command after it.
//
// Notes keep the Key Editor's rules (notes.h). A new key, velocity or length, a move and a removal of a note leave the
// other commands at their ticks. A note that waits for its length leaves a wait of that length when it goes.
//
// A Wii sequence takes these edits by the rules of the file's build of NW4R SND. A command that the build doesn't run,
// such as a bank select, can't go in there.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/commands/sequence.h"

#include "edit/bytecode.h"
#include "edit/notes.h"
#include "format/command_values.h"
#include "format/file.h"
#include "song/sequence.h"

namespace hokuto::edit
{

// A command of a track as the List Editor lists it: the command's offset in the sequence's bytecode, the stretch of the
// track's commands that ran it, by its index among the track's stretches, and the tick at which it ran. After a wait
// whose length the bytecode doesn't give, a tick counts back from the next tick that's known: the start of a note that
// the arrangement shows, the start of the track's next stretch, or the tick of the first command after the end of the
// loop's first pass. The rows stop there. The tick is missing where no such count reaches.
struct CommandRow
{
    friend bool operator==(const CommandRow&, const CommandRow&) = default;

    uint32_t command = 0;
    uint32_t stretch = 0;
    std::optional<uint32_t> tick;
};

// The commands of a track of sequence `sequence` of `file`, in the order that the track ran them in `stretches`
// (song::Track::stretches). `notes` are the track's notes (song::Track::notes). With a loop's end at `end`, the rows
// stop at the end of the loop's first pass: a stretch that starts at `end` or later stays out, and so does a command
// that runs after it.
std::vector<CommandRow> CommandRows(const format::File& file, uint32_t sequence,
                                    std::span<const song::Stretch> stretches, std::span<const song::Note> notes,
                                    std::optional<uint32_t> end);

// The same rows of the sequence's commands `commands`, with the offset of each item in `offsets` (Offsets). A caller
// that lists several tracks works out the offsets once.
std::vector<CommandRow> CommandRows(const ntdlib::commands::Sequence& commands, const std::vector<uint32_t>& offsets,
                                    std::span<const song::Stretch> stretches, std::span<const song::Note> notes,
                                    std::optional<uint32_t> end);

// The command at offset `command` of sequence `sequence` of `file`, with its target as an item of the sequence. Throws
// std::invalid_argument with a message for the user for an offset where no command starts.
const ntdlib::commands::Command& CommandAt(const format::File& file, uint32_t sequence, uint32_t command);

// Whether a command that `song` ran up to the end of the loop's first pass draws a random number: as a value, as a time
// or into a variable. Sequence `sequence` of `file` played `song`. Each play of such a sound can differ.
bool DrawsRandom(const format::File& file, uint32_t sequence, const song::Sequence& song);

// A label of a sequence: its name and the offset of the command that it names, or of the end of the bytecode.
struct LabelPlace
{
    friend bool operator==(const LabelPlace&, const LabelPlace&) = default;

    std::string name;
    uint32_t command = 0;
};

// The labels of sequence `sequence` of `file`, in the order of the file.
std::vector<LabelPlace> LabelsOf(const format::File& file, uint32_t sequence);

// The item that starts at offset `command` of sequence `sequence` of `file`, for a command's target. The offset of the
// end of the bytecode gives the number of items. Throws std::invalid_argument for an offset where no item starts.
uint32_t ItemOf(const format::File& file, uint32_t sequence, uint32_t command);

// The lowest and the highest value that an argument can take.
using ValueRange = format::ValueRange;

// The values that the bytes of `command` can hold on `console` in its value from `source`, or with `time` in its time:
// in the unit of the command's kind, in ticks for a time, or as a variable's number from a variable. A random value's
// range holds both ends. A note's velocity runs from 0 to 127, and its key too. A tempo and a timebase start at 1: 0
// stops the player's clock. A tempo ends at 1023, except on the Wii before March 2008. The player holds a higher tempo
// at 1023. Nothing where the command can't take such an argument, or where the console's player skips the command
// (song::Runs).
std::optional<ValueRange> ArgumentRange(const song::Console& console, const ntdlib::commands::Command& command,
                                        ntdlib::commands::Source source, bool time = false);

// `command` with each value at the nearest one that `console` holds.
ntdlib::commands::Command Nearest(const song::Console& console, ntdlib::commands::Command command);

// A new command of `kind` with the values that the List Editor gives it first, in a sequence of `timebase` ticks to the
// quarter note. A note is middle C at velocity 100, a quarter note long. A wait lasts a quarter note. Where the console
// starts a track with a value, a setting takes that value. A jump, a call or a track start goes to item `target`.
ntdlib::commands::Command NewCommand(ntdlib::commands::Kind kind, int timebase, uint32_t target);

// A command of a sequence as a row of the List Editor finds it: its offset in the bytecode, and the stretch of the
// track's commands that ran it.
struct CommandPlace
{
    uint32_t command = 0;
    song::Stretch stretch;
};

// The file after the command at `place` of sequence `sequence` of `file` takes the values of `command`. A note's new
// key, velocity or length leaves the other commands at their ticks (EditNotes). Throws std::invalid_argument with a
// message for the user for a command of another kind, a value that the console's bytes can't hold, a target past the
// end of the sequence, and a change of a command's size in a sequence whose commands can't move.
Edited ChangeCommand(const format::File& file, uint32_t sequence, const CommandPlace& place,
                     ntdlib::commands::Command command);

// The file after the commands at `places` of sequence `sequence` of `file` take the values of `commands`, in order.
// Each command changes the way ChangeCommand changes one, and keeps its item. Throws std::invalid_argument with a
// message for the user for a note, and for each change that ChangeCommand refuses.
Edited ChangeCommands(const format::File& file, uint32_t sequence, std::span<const CommandPlace> places,
                      std::span<const ntdlib::commands::Command> commands);

// The file after `moves` take settings of sequence `sequence` of `file` to other ticks (MoveSettings), and the commands
// at `places` then take the values of `commands` (ChangeCommands). The places name commands of `file`. A drag in the
// Key Editor's controller lanes moves settings and changes their values in one edit. Throws std::invalid_argument with
// a message for the user for each move that MoveSettings refuses, and for each change that ChangeCommands refuses.
Edited MoveAndChangeCommands(const format::File& file, uint32_t sequence, std::span<const SettingMove> moves,
                             std::span<const CommandPlace> places, std::span<const ntdlib::commands::Command> commands);

// The file after `command` goes in before the command at `place` of sequence `sequence` of `file`. With `entered`, the
// track entered the sequence at that command: code that enters there runs the new command first. A target of the new
// command names an item before the edit. `Edited::added` holds the new command's item. Throws std::invalid_argument
// with a message for the user for a command that the console's bytes can't hold or its player skips (song::Runs), and
// for a sequence whose commands can't move.
Edited InsertCommand(const format::File& file, uint32_t sequence, const CommandPlace& place, bool entered,
                     ntdlib::commands::Command command);

// The file after the commands at `places` of sequence `sequence` of `file` go. A note goes as the Key Editor removes it
// (EditNotes). Throws std::invalid_argument with a message for the user for a sequence whose commands can't move, and
// for a note that can't go.
Edited RemoveCommands(const format::File& file, uint32_t sequence, std::span<const CommandPlace> places);

} // namespace hokuto::edit
