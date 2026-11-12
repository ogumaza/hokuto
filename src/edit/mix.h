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
// A Wii sequence takes these edits by the rules of the file's build of NW4R SND. A GBA song takes them by
// MusicPlayer2000's rules (Mp2kWritable). Its tracks have a volume from 0 to 127, 0 until a command sets it, and no
// sends. The mix of a DSE track is the mix of its channel in the driver: an edit uses the stretches of the tracks of
// the channel (song::MixStretches). Its volume stops at 127 and starts at 100, and it has no sends. A slide's target
// (0xE2, 0xEA) moves with the edit as a command's value does, and so does a change's amount of volume (0xE1). A
// change's amount of pan (0xE9) stays. A change, or a slide with a time, before the setting's first command starts from
// the default.
//
// A track of Konami's driver sets its volume in each note's command as well as in its commands of volume. The edit
// moves them all. Its volume stops at song::KonamiMostVolume. The revision without a mixer, and the ring mixer's F0,
// can set the next track's volume from a track: an edit of a track's volume includes those commands of the track before
// it (`previous` in EditMix) too. The tracks have no sends, and some builds have no pan of a track.

#pragma once

#include <cstddef>
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

// The lowest and the highest value of `setting`, on the GBA, the DS and DSE with `to_127`. On the GBA and DSE, a
// track's volume ranges from 0 to 127. On the DS, the table of levels that the driver reads a track's volume in ends at
// 127.
int MixLowest(song::Mix setting);
int MixHighest(song::Mix setting, bool to_127 = false);

// The commands of `commands` that set `setting` on a track that ran `stretches` (song::Track::stretches). `offsets`
// gives the offset of each item (Offsets). The track's start sets the setting where notes play before the first of the
// commands, or where the track runs none. With `dse`, DSE's slides and changes count too. With `konami`, the note
// command of Konami's driver sets the volume too, and `previous` contains the stretches of the track before it. Throws
// std::invalid_argument with a message for the user where the track's commands aren't where `stretches` shows them.
Setters MixSetters(const ntdlib::commands::Sequence& commands, const std::vector<uint32_t>& offsets,
                   std::span<const song::Stretch> stretches, song::Mix setting, bool dse = false, bool konami = false,
                   std::span<const song::Stretch> previous = {});

// An edit of a track's mix (EditMix): the file after it and the new index of each of the sequence's items (Edited), and
// the number of the track's values that the edit stopped at the end of the setting's range. A volume or a send stops at
// its highest value, and a pan at its left or right edge. Each command counts once, a random range included, and so do
// a note of Konami's driver, a slide of DSE and the default before the track's first command.
struct MixEdited : Edited
{
    std::size_t stopped = 0;
};

// The file after `edit` to track `track` of sequence `sequence` of `file`, and the new index of each of the sequence's
// items. The track's commands ran in `stretches` (song::Track::stretches). Throws std::invalid_argument with a message
// for the user for an edit that the sequence can't take: a setting from a variable, a volume or send to trim from 0, a
// new command in a sequence whose commands can't move, a send to FX C on the 3DS, a send on the GBA, and the volume of
// a track of Konami's driver whose commands start no note. On Konami's driver, `previous` contains the stretches of the
// track before it, and `track` is also the channel.
MixEdited EditMix(const format::File& file, uint32_t sequence, int track, std::span<const song::Stretch> stretches,
                  const MixEdit& edit, std::span<const song::Stretch> previous = {});

} // namespace hokuto::edit
