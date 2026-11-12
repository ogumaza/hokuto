// SPDX-License-Identifier: MIT

// The views of the Replace window for a wave or a WAV file on the 3DS, the Wii U, the Switch and the Wii.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "ntdlib/sar/formats.h"

#include "app/replace_view.h"
#include "edit/files.h"
#include "format/file.h"

namespace hokuto::app
{

// The view of `incoming` as a new wave after the waves of the wave data of file `file_id` of `source.played.Wii()`, its
// wave `wave` (File > Import > Audio File… or Wave…). MakeNewWaveView makes the same view for the other consoles.
// Without a result, `error` says why the wave can't go in.
ReplaceView MakeWiiNewWaveView(const ReplaceSource& source, uint32_t file_id, uint32_t wave,
                               const edit::IncomingWave& incoming, bool wav, std::optional<format::File> result,
                               std::string error);

// The view of the wave of a WAV file or a wave file (`wav`) in the place of wave `wave` of wave archive `source.sound`.
// `incoming` gives the new wave in its encoding, `changes` the present wave and what plays it, and `archive_changes`
// what the new wave archive changes besides the wave archive. The new wave archive takes `archive_size` bytes. Without
// a result, `error` says why the wave can't go in.
ReplaceView MakeWaveReplaceView(const ReplaceSource& source, uint32_t wave, const edit::IncomingWave& incoming,
                                bool wav, const edit::WaveChanges& changes, const edit::FileChanges& archive_changes,
                                std::size_t archive_size, std::optional<format::File> result, std::string error);

// The same for a wave of the Wii in the place of wave `wave` of the wave data of file `source.sound` of
// `source.played.Wii()`. The wave data belongs to the file: the new waves stay beside it.
ReplaceView MakeWiiWaveReplaceView(const ReplaceSource& source, uint32_t wave, const edit::IncomingWave& incoming,
                                   bool wav, const edit::WaveChanges& changes, std::optional<format::File> result,
                                   std::string error);

// The view of `incoming` as new wave `wave` of wave archive `wave_archive` (File > Import > Audio File… or Wave…).
// `incoming` comes from a WAV file or a wave file (`wav`). A WAV file's wave has the encoding chosen in the window's
// menu. `archive_changes` gives what the new wave archive changes besides the wave archive. The new wave archive takes
// `archive_size` bytes. Without a result, `error` says why the wave can't go in.
ReplaceView MakeNewWaveView(const ReplaceSource& source, uint32_t wave_archive, uint32_t wave,
                            const edit::IncomingWave& incoming, bool wav, const edit::FileChanges& archive_changes,
                            std::size_t archive_size, std::optional<format::File> result, std::string error);

// The note that Play shows for wave sound `sound` of `archive` when the sound's wave archive has fewer waves than the
// wave that note 0 names (song::WaveSoundNoteOf). The consoles play nothing then, and the windows warn that notes using
// the wave will be silent. Nothing for a sound that isn't a wave sound, and when the wave archive contains the wave or
// contains no waves. Throws the exceptions of song::WaveSoundNoteOf and song::WaveCountOf.
std::optional<std::string> SilentWaveSoundNote(const ntdlib::sar::SoundArchive& archive, uint32_t sound);

} // namespace hokuto::app
