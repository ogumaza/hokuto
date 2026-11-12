// SPDX-License-Identifier: MIT

// The views of the Replace window on DSE, and of the imports of a MIDI file and a SoundFont on the GBA.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "app/replace_view.h"
#include "edit/files.h"
#include "edit/gba_midi.h"
#include "edit/gba_sound_fonts.h"
#include "format/file.h"
#include "song/dse.h"
#include "song/gba.h"
#include "song/sequence.h"

namespace hokuto::app
{

// The source gets the tracks of `source.midi` on DSE, with the source's pairings of the drum notes
// (edit::DseSongOfMidi). The programs' pairings apply in the replacement.
void ReadDseMidi(ReplaceSource& source);

// The source gets the pairings that the file's map of bank `bank` of the set `source.played.Dse()` contains for its
// MIDI file.
void ProposeDseMidiMap(ReplaceSource& source, uint32_t bank);

// The place in the set's banks of the first bank that sound `sound` of `set` plays, or nothing for a sound without one.
std::optional<uint32_t> DseFirstBank(const song::DseSet& set, uint32_t sound);

// The view of the MIDI file `midi`, named `source.file_name`, in the place of the commands of song `songs[song]` of
// `rom`, the GBA ROM that `source.file` builds (File > Import > Sequence…). `source.places` lists where each song
// plays. Each instrument that the file's notes play gets its voice from `voices`, or else the voice that
// edit::ProposedGbaVoices proposes in the song's voice group. The view contains the file that the import makes. Without
// it, `error` says why the MIDI file can't go in. Play plays the song from the new commands.
ReplaceView MakeGbaMidiImportView(const ReplaceSource& source, std::span<const uint8_t> midi, const song::GbaRom& rom,
                                  std::span<const int> songs, std::size_t song, const edit::GbaVoiceMap& voices);

// The view of the SoundFont `voices` as voices of the voice group of song `songs[song]` of `rom`, the GBA ROM that
// `source.file` builds (File > Import > Bank…). `source.file_name` names the SoundFont. `source.places` lists where
// each song plays. `rate` is the chosen rate's place among the view's rates. The first is the highest of the
// SoundFont's samples. The view lists the program that each preset gets. It contains the file that the import makes.
// Without it, `error` says why the presets can't go in.
ReplaceView MakeGbaSoundFontImportView(const ReplaceSource& source, const edit::GbaSoundFontVoices& voices,
                                       const song::GbaRom& rom, std::span<const int> songs, std::size_t song,
                                       std::size_t rate);

// The view of `source.dse_song`, the tracks of a DSE song file or of a MIDI file, in the place of the tracks of sound
// `source.sound` of the set `source.played.Dse()`. A row for each program that the tracks select offers the programs of
// the sound's first bank. A MIDI file also adds a row for each note of MIDI's drum channel. The view contains the
// result, or the reason why the edit can't be made. Play plays the sound.
ReplaceView MakeDseSoundReplaceView(const ReplaceSource& source);

// The view of `source.dse_bank`, a DSE wave bank, in the place of bank `source.sound` of the set `source.played.Dse()`.
// MakeDseSoundReplaceView makes views of the same form. Play plays a sound that plays the bank, or else a note of its
// first program.
ReplaceView MakeDseBankReplaceView(const ReplaceSource& source);

// The view of `incoming`, a WAV file's wave, in the place of wave `wave` of bank `source.sound` of the set
// `source.played.Dse()`. `changes` describes what the current wave contains (edit::ChangesOfDseWave).
ReplaceView MakeDseWaveReplaceView(const ReplaceSource& source, uint32_t wave, const edit::IncomingWave& incoming,
                                   const edit::WaveChanges& changes, std::optional<format::File> result,
                                   std::string error);

} // namespace hokuto::app
