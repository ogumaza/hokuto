// SPDX-License-Identifier: MIT

// Edits of a track's mix: its volume, its pan and its sends to the aux buses. Commands set each of them (song::Mix). An
// edit of a setting changes every command of the setting that the track runs. Where those commands give the setting one
// value throughout, each of them takes the new value. Where the value changes as the track plays, such as in a fade,
// the edit trims every value alike: a volume or a send by the same factor, and a pan by the same amount. A value stays
// inside its range.
//
// Notes that play before a track's first command of the setting play at the setting's default value. Such a track gets
// a new command at its start, before its first command. The edit counts the default as one of the track's values. Only
// the track's opening runs the new command: the sound's start for track 0, and the commands that open the other tracks.
// A jump back to the track's first command still leads to the command that was first. Another sound can start before
// the track's start and run on into it. A jump then takes that path past the new command.
//
// Other tracks and sounds that run the same commands change with the track, such as the callers of a subroutine. The
// mixer lists them before an edit (reach.h).
//
// A Wii sequence takes these edits by the rules of the file's build of NW4R SND.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "ntdlib/commands/sequence.h"

#include "edit/bytecode.h"
#include "format/file.h"
#include "song/sequence.h"

namespace hokuto::edit
{

// A change to a setting of a track's mix, in the units of song::Track's settings. `from` is the value that the setting
// had where the user changed it.
struct MixEdit
{
    song::Mix setting = song::Mix::kVolume;
    int from = 0;
    int to = 0;
};

// The lowest and the highest value of `setting`.
int MixLowest(song::Mix setting);
int MixHighest(song::Mix setting);

// The commands of `commands` that set `setting` on a track that ran `stretches` (song::Track::stretches). `offsets`
// gives the offset of each item (Offsets). The track's start sets the setting where notes play before the first of the
// commands, or where the track runs none. Throws std::invalid_argument with a message for the user where the track's
// commands aren't where `stretches` shows them.
Setters MixSetters(const ntdlib::commands::Sequence& commands, const std::vector<uint32_t>& offsets,
                   std::span<const song::Stretch> stretches, song::Mix setting);

// The file after `edit` to track `track` of sequence `sequence` of `file`, and the new index of each of the sequence's
// items. The track's commands ran in `stretches` (song::Track::stretches). Throws std::invalid_argument with a message
// for the user for an edit that the sequence can't take: a setting from a variable, a volume or send to trim from 0, a
// new command in a sequence whose commands can't move, and a send to FX C on the 3DS.
Edited EditMix(const format::File& file, uint32_t sequence, int track, std::span<const song::Stretch> stretches,
               const MixEdit& edit);

} // namespace hokuto::edit
