// SPDX-License-Identifier: MIT

// Edits of the labels and the subroutines of a sequence that the program edits as a program (edit/timelines.h). A label
// names a command of the sequence, or the end of its bytecode. NintendoWare's tools write a file's labels in the order
// of their names, and the edits keep that order. A label's name starts with a letter or an underscore, and holds only
// letters, digits and underscores. No two labels of a sequence share a name.
//
// A label that moves takes the jumps, the calls and the track starts that go to its command with it. The sounds that
// start there move with it too. In the text form of a sequence, they all name the label. When another label stays at
// the command, they stay as well. A sound without a start in the tables starts at the bytecode's first command still.
//
// A subroutine runs from the command that a call goes to, up to the return that ends it. Commands that follow each
// other become a subroutine: they go after the sequence's commands, under a new label, with a return after them. A call
// to them takes their place. Every track and every sound that ran the commands then runs them through the call. A call
// can give way to its subroutine's commands: a copy of them, without the return, takes the call's place. A call can
// also take a copy of its subroutine. That copy then plays for the call alone. The copy goes after the sequence's
// commands, under new labels. The other calls keep the subroutine.
//
// A Wii sequence takes these edits by the rules of the file's build of NW4R SND, with the Wii's depth of calls.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ntdlib/commands/sequence.h"

#include "edit/bytecode.h"
#include "format/file.h"

namespace hokuto::edit
{

// Why `name` can't name a new label of `commands`: it isn't an identifier, or a label has the name already. Nothing for
// a name that can.
std::optional<std::string> LabelRefusal(const ntdlib::commands::Sequence& commands, std::string_view name);

// The file after a label named `name` goes in at the command at offset `command` of sequence `sequence` of `file`, or
// at the end of its bytecode. In `Edited::items`, each item keeps its index. Throws std::invalid_argument with a
// message for the user for a name that LabelRefusal refuses, an offset where no command starts, and a timeline.
Edited AddLabel(const format::File& file, uint32_t sequence, uint32_t command, const std::string& name);

// The file after label `name` of sequence `sequence` of `file` takes the name `new_name`. Throws std::invalid_argument
// with a message for the user for a label that the sequence lacks, a name that LabelRefusal refuses, and a timeline.
Edited RenameLabel(const format::File& file, uint32_t sequence, const std::string& name, const std::string& new_name);

// The file after label `name` of sequence `sequence` of `file` names the command at offset `command`, or the end of the
// bytecode. The jumps, calls, track starts and sounds' starts at its old command go with it, unless another label stays
// there. Throws std::invalid_argument with a message for the user for a label that the sequence lacks, an offset where
// no command starts, and a timeline.
Edited MoveLabel(const format::File& file, uint32_t sequence, const std::string& name, uint32_t command);

// The file after label `name` of sequence `sequence` of `file` goes. The jumps, calls, track starts and sounds' starts
// at its command stay there. Throws std::invalid_argument with a message for the user for a label that the sequence
// lacks, and a timeline.
Edited RemoveLabel(const format::File& file, uint32_t sequence, const std::string& name);

// The calls of `commands` that go to item `item`, by their items.
std::vector<uint32_t> CallsTo(const ntdlib::commands::Sequence& commands, uint32_t item);

// The file after the commands at the offsets `selected` of sequence `sequence` of `file` become a subroutine named
// `name`, with a call to it in their place. The commands follow each other in the bytecode. `depth` counts the calls
// and loops on the track's stack where the track ran the commands, at their deepest, the runs of their calls included.
// The call adds one, and the console's stack has to hold it. A jump, a call, a track start, a label or a sound's start
// at the first command goes to the call. Nothing else may go into the commands, and they hold no jump, return or end:
// every track that reaches the first command then runs them in order, through the call. `Edited::added` holds the
// call's item. Throws std::invalid_argument with a message for the user for commands with gaps between them, for a
// jump, a return, an end, an allocation of tracks or a loop that the commands don't close, for a jump, a call, a track
// start, a label or a sound's start at any other of the commands, for a stack too deep, for a name that LabelRefusal
// refuses, for commands that can't move and for a timeline.
Edited ToSubroutine(const format::File& file, uint32_t sequence, std::span<const uint32_t> selected, int depth,
                    const std::string& name);

// The file after the call at offset `call` of sequence `sequence` of `file` gives way to a copy of its subroutine's
// commands, without the return. A jump, a call, a track start, a label or a sound's start at the call goes to the first
// copied command. The subroutine stays for the other calls. `Edited::added` holds the items of the copies. Throws
// std::invalid_argument with a message for the user for a command that isn't a call, a call that runs only if the last
// comparison held, a call inside its subroutine, a subroutine that doesn't run straight to an unconditional return or
// that opens a loop that it doesn't close, commands that can't move and a timeline.
Edited DissolveCall(const format::File& file, uint32_t sequence, uint32_t call);

// The file after the call at offset `call` of sequence `sequence` of `file` goes to a copy of its subroutine, after the
// sequence's commands. Each label of the subroutine gives the copy of its command a label of the same name with a
// number after it (UniqueLabel). Without a label at its first command, the copy takes the name sub. The copy's jumps
// stay inside the copy, and its calls and track starts go where the subroutine's go. `Edited::added` holds the items of
// the copies. Throws std::invalid_argument with a message for the user for a command that isn't a call, a subroutine
// whose commands aren't in one piece, commands that can't move and a timeline.
Edited CopySubroutine(const format::File& file, uint32_t sequence, uint32_t call);

} // namespace hokuto::edit
