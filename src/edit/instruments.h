// SPDX-License-Identifier: MIT

// The edit of the instrument that a track's notes play, for the inspector's Bank and Program. A note plays the program
// that the track's last program change selected, of the bank in the sound's slot that the track's last bank select
// selected. A track starts with program 0 and slot 0. The edit gives the notes of a track that play one instrument
// another instrument. It changes the program changes and the bank selects that set their instrument. Where the track's
// start sets it, a new command at the track's start sets the new one. Only the track's opening runs the new command.
// The mixer's new commands go in the same way (mix.h).
//
// The edit refuses where a command that it would change sets the instrument of other notes too, such as a bank select
// before notes of two programs. It also refuses where the program or the slot of such notes comes from a variable, a
// random number or a comparison. Other tracks and sounds that run the same commands change with the track, such as the
// callers of a subroutine. The inspector lists them before an edit (reach.h).
//
// A sound of the Wii plays one bank, in slot 0. A track of the Wii takes another program only. A GBA song plays one
// voice group, its programs 0 to 127. A GBA track starts with no program: MusicPlayer2000 plays a square wave of its
// own until a program change selects a voice (ntdlib's MP2K sequencer, StartTrack). A DS sequence plays the bank that
// its entry in the archive names, in slot 0. A DS track starts at program 0. A DSE track starts at program 0 of the
// song's bank. Hokuto accepts another program there, and leaves the track's commands of banks as they are. A note of a
// sample of Konami's driver names its sample, and that sample is its program. The edit sets the new sample in each such
// note. A PSG track of that driver selects a duty or a wave, its program, with its commands. Those start at 0. A bank
// select of Nintendo R&D2's driver also selects program 0 of its bank. The edit refuses another program for notes that
// play program 0 from a bank select: a program change after the bank select would have to go in.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "ntdlib/commands/sequence.h"

#include "edit/bytecode.h"
#include "edit/notes.h"
#include "edit/replace.h"
#include "format/file.h"
#include "song/sequence.h"

namespace hokuto::edit
{

// The program changes and bank selects of `commands` that set the instrument of the notes that play `instrument`, on a
// track of `console` that ran `stretches` (song::Track::stretches). `offsets` gives the offset of each item (Offsets).
// An edit of those notes' instrument changes them. The track's start sets the instrument where no command before the
// notes sets its program or its slot. A DSE track's notes all count as slot 0 here. On Brownie Brown's driver, the
// track's number `track` decides which command selects the program. Throws std::invalid_argument with a message for the
// user where the track's commands aren't where `stretches` shows them.
Setters InstrumentSetters(const ntdlib::commands::Sequence& commands, const std::vector<uint32_t>& offsets,
                          std::span<const song::Stretch> stretches, Program instrument, const song::Console& console,
                          std::span<const song::Stretch> previous = {}, int track = -1);

// The file after the notes of track `track` of sequence `sequence` of `file` that play `from` play `to` in its place,
// and the new index of each of the sequence's items. The track's commands ran in `stretches` (song::Track::stretches).
// Throws std::invalid_argument with a message for the user for an instrument that no note of the track plays, a slot
// outside 0 to 3, another slot on the Wii or the GBA, a program past 127 on the GBA, a command that sets the instrument
// of other notes too, a program or a slot from a variable, a random number or a comparison, and a new command in a
// sequence whose commands can't move. On Konami's driver, `previous` contains the stretches of the track before it: a
// note there can play on this track. On Brownie Brown's driver, the command that the track's notes take their program
// from changes (song::BrownieProgramIndex), and the edit refuses a noise channel: its notes give the noise's settings.
Edited EditInstrument(const format::File& file, uint32_t sequence, int track, std::span<const song::Stretch> stretches,
                      Program from, Program to, std::span<const song::Stretch> previous = {});

// The DSE command that selects the bank with id `id`.
ntdlib::commands::Command DseBankSelect(uint16_t id);

// The ids of the banks in the slots of sequence `sequence` of `file`, a file of DSE's files. A sequence that no sound
// plays has none.
std::vector<uint16_t> DseBankIds(const format::File& file, uint32_t sequence);

// Whether the opening of `track`, a DSE track of sequence `sequence` of `file`, selects a bank before time passes. A
// channel starts without one of the sound's banks. A program that the channel selects then plays nothing, and the
// driver selects program 0 in its place (0x02073BC8). Throws std::invalid_argument as OpeningInstrument does.
bool DseOpeningSelectsBank(const format::File& file, uint32_t sequence, const song::Track& track);

// The instrument that the opening of `track`, a track of sequence `sequence` of `file`, selects: the program changes
// and the bank selects that the track runs in its first stretch before time passes. A track that selects a program and
// no slot plays slot 0, and a track that selects a slot and no program plays program 0. Nothing when the opening
// selects neither, or a value from a variable, a random number or a comparison. A new track selects its instrument this
// way (AddTrack). Throws std::invalid_argument where the track's commands aren't where its stretches show them.
std::optional<Program> OpeningInstrument(const format::File& file, uint32_t sequence, const song::Track& track);

// The file after `track`, a track of sequence `sequence` of `file` without notes, opens with `to` as its instrument.
// The program change and the bank select that its opening runs take the new values, and a new command at the track's
// start sets what the opening doesn't. Throws std::invalid_argument with a message for the user for a track with notes,
// an instrument that EditInstrument refuses, a program or a slot from a variable, a random number or a comparison, and
// a new command in a sequence whose commands can't move.
Edited EditOpeningInstrument(const format::File& file, uint32_t sequence, const song::Track& track, Program to);

// The file after `notes` go into `track`, a track of sequence `sequence` of `file` without notes and without an
// instrument in its opening, and new commands at the track's start select `to` before them. On DSE, the opening can
// hold a program change and no bank select (DseOpeningSelectsBank). The program change then selects the program of
// `to`, and a new bank select is inserted before it. Throws std::invalid_argument as EditNotes and
// EditOpeningInstrument do.
Edited EditNotesWithInstrument(const format::File& file, uint32_t sequence, const song::Track& track,
                               std::span<const NewNote> notes, Program to);

} // namespace hokuto::edit
