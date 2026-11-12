// SPDX-License-Identifier: MIT

// The rules by which a check tells notes apart on each driver: the GBA's PSG channels, the room in a ROM, the ticks of
// note commands and whether an archive contains a sequence (tools/checked_archive.h).

#pragma once

#include "checked_archive.h"

namespace hokuto::tools
{

// The PSG channel, 1 to 4, that a note of `program` and `key` plays on with the voice group at `voices` of `rom`, or 0
// for a DirectSound sample.
int GbaPsgChannel(const ntdlib::agb::Rom& rom, uint32_t voices, uint32_t program, int key);

// Whether `rom` has no room for a song that outgrows its place. ntdlib::mp2k::Join puts such a song into the free
// space: from a multiple of 256 bytes that is 256 bytes or more into the run of 0xFF bytes at the ROM's end, or else
// from the ROM's end. A ROM that fills the 32 MiB that the GBA maps without such free space refuses every song that
// grows.
bool Roomless(const song::GbaRom& rom);

// Whether the export of `edited` refuses it for want of room in the ROM's 32 MiB, or in the room of a list of songs on
// Quintet's driver. ntdlib's joins name the 32 MiB or the room when they refuse such songs. Any other refusal of the
// export throws std::runtime_error: a check that catches the edit's refusals as std::invalid_argument then doesn't
// count it among them.
bool ExportOutgrows(const format::File& edited);

// Whether `edited`, a file of `archive`, contains a song of sequence `sequence` that outgrows its place, in a GBA ROM
// without room for it (Roomless). A check then leaves the sequence's edit out, and counts it apart. In a file that
// allows filler runs (format::NativeArchive::gba_filler), the song outgrows the ROM only where the filler runs also
// have no room for it: the export then refuses the file. A ROM with another of the GBA's drivers lays its songs out the
// same way. Throws std::runtime_error when the export refuses the file for another reason (ExportOutgrows).
bool Outgrows(const CheckedArchive& archive, const format::File& edited, uint32_t sequence);

// The ticks at which each track of `song`, a read of sequence `sequence` of `file`, runs a note command: the track's
// number and the tick, from the commands of the track's stretches. On Nintendo R&D2's driver, the arrangement shows no
// note that the driver's voices kept out, and these ticks tell where one can start.
std::set<std::pair<int, uint32_t>> NoteCommandTicks(const format::File& file, uint32_t sequence,
                                                    const song::Sequence& song);

// A rule that leaves some notes of a sound out of a check's comparisons.
using NotesApart = std::function<bool(const song::Track&, const song::Note&)>;

// The notes of song `song` of `rom` on a PSG channel, on the tracks in `changed`. A PSG channel's envelope moves
// through the channel's 16 steps of volume from the note's level: its velocity times the sum of its levels on the left
// and the right. The volume and pan of the note's track set those levels. A note whose envelope falls to silence before
// its command ends therefore ends elsewhere at another volume or pan. A quiet note can fall silent at once, and a note
// that was silent can sound.
NotesApart PsgNotesApart(const song::GbaRom& rom, uint32_t song, const std::set<int>& changed);

// The notes of Brownie Brown's driver on the PSG's tracks in `changed`. A volume there lowers the envelope's level, and
// a pan can leave the channel no side. A note can then fall silent, or a silent note sound.
NotesApart BrowniePsgNotesApart(const std::set<int>& changed);

// The notes of sound `sound` of Nintendo R&D2's driver on the tracks in `changed` whose instrument plays a square, the
// wave or the noise at some key. The driver ends a PSG voice's note once the voice's volume falls to 0, and the track's
// volume and pan set that volume. Such a note therefore ends elsewhere at another volume or pan. A note's key is the
// MIDI file's key. On a square or the wave, that key differs from the driver's key. The rule therefore uses every key
// of the instrument. A drum kit's entries for keys outside the kit can name bytes that aren't a region, and a type past
// the noise's doesn't count.
NotesApart Rd2PsgNotesApart(const song::Rd2Rom& rom, uint32_t sound, const std::set<int>& changed);

// The notes of Konami's driver on a PSG track, in a revision whose records hold a pan. There, a PSG track off centre
// plays its right side on its channel and its left side on the next, in place of the next track. A pan that is inserted
// or moves therefore decides which track's notes play, and the notes of the other track stay lost. The Eternal Duelist
// and Captain Tsubasa revisions start a PSG note again only for a new volume. A volume that is inserted or moves
// therefore joins notes or parts them.
NotesApart KonamiPsgNotesApart(ntdlib::konami::Revision revision);

// Whether `got`, a sequence of the file that an import of an exported archive of the kind of `archive` makes, contains
// `want`, the sequence that the export came from. On the GBA, the bytes of `got` start with those of `want`, and zeros
// follow them. A song that shrank leaves zeros in its place up to its header, and an export aligns the header of a song
// that moved. The cut treats the zeros as data after the song's items (ntdlib::mp2k::Join). On DSE, the bytes of `got`
// are those of `want`: a note that sets its octave comes back as two commands. On the GBA's other drivers, the items of
// `got` start with those of `want`, whatever bytes their values took. A song that shrank leaves zeros in its place, and
// the cut treats them as data after the song's items. A song that moved leaves behind the bytes that followed its
// commands. No track reads those bytes. On Brownie Brown's driver, the bytes of `got` start with those of `want`. The
// zeros that put a word on a multiple of 4 come back as data.
bool HoldsSequence(const CheckedArchive& archive, const ntdlib::commands::Sequence& want,
                   const ntdlib::commands::Sequence& got);

} // namespace hokuto::tools
