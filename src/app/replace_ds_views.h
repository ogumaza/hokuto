// SPDX-License-Identifier: MIT

// The views of the Replace window on the DS.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/sdat/formats.h"

#include "app/replace_view.h"
#include "edit/ds_new_items.h"
#include "edit/files.h"
#include "format/file.h"
#include "song/ds.h"
#include "song/sequence.h"

namespace hokuto::app
{

// The sequence file of the DS that `source.midi` converts to with the pairings of the source. The source gets the
// commands and the warnings of the song.
std::vector<uint8_t> ReadDsMidi(ReplaceSource& source);

// The source gets the pairings that the map of the file for bank `bank` of the DS contains for its MIDI file.
void ProposeDsMidiMap(ReplaceSource& source, uint32_t bank);

// The view of `bytes`, a bank file (`kind` kBank) or a wave archive file (kWaveArchive) of the DS, in the place of item
// `source.sound` of the DS's archive `source.played.Ds()`. The view contains the result, or the reason why the edit
// can't be made. Play plays a sound that plays the item. Throws std::invalid_argument with a message for the user for
// bytes that aren't such a file.
ReplaceView MakeDsFileReplaceView(const ReplaceSource& source, edit::FileKind kind, std::span<const uint8_t> bytes);

// The view of `bytes`, a sequence file of the DS, in the place of the sequence of sound `source.sound`.
// MakeDsFileReplaceView makes views of the same form.
ReplaceView MakeDsSequenceReplaceView(const ReplaceSource& source, std::span<const uint8_t> bytes);

// The view of `incoming` in the place of wave `wave` of wave archive `source.sound` of the DS's archive
// `source.played.Ds()`: from a WAV file (`wav`) or from a wave file. `changes` describes what the current wave contains
// (edit::ChangesOfDsWave).
ReplaceView MakeDsWaveReplaceView(const ReplaceSource& source, uint32_t wave, const edit::IncomingWave& incoming,
                                  bool wav, const edit::WaveChanges& changes, std::optional<format::File> result,
                                  std::string error);

// The name that the Import window offers first for the file `file_name` on the DS: the file's name without its
// extension, with a number after it while an item of the archive holds the name. An archive without names gets an empty
// name.
std::string FirstDsName(const song::DsArchive& archive, const std::string& file_name);

// What the rows of the Import window of `kind` offer first: for a sequence, the bank of sound `source.sound`, and for a
// bank, for each slot that it names, the archive's last wave archive that contains the waves that the slot requires.
// The values are places in the lists of song::DsBanks and song::DsWaveArchives.
std::vector<int> FirstDsRows(const ReplaceSource& source, edit::DsNewItem kind, std::span<const uint8_t> bytes);

// The wave archives of the slots of a new DS bank: for each slot of `slots`, the wave archive that its row of `rows`
// chooses, as a place in `wave_archives`. A slot without a choice gets ntdlib::sdat::kNoWaveArchive.
std::array<uint16_t, ntdlib::sdat::kWaveArchiveSlots> ChosenDsSlots(std::span<const edit::DsBankSlot> slots,
                                                                    std::span<const int> rows,
                                                                    std::span<const uint32_t> wave_archives);

// The Import window of `bytes` as a new item of `source.played.Ds()` named `name` (File > Import > Sequence…, Bank… or
// Wave Archive…, and Project > Add Sequence Sound…). `bytes` is a sequence file, a bank file or a wave archive file of
// the DS. `rows` contains the choice of each row. A new sequence sound copies the settings of sound `source.sound`. The
// view contains no result. Throws std::invalid_argument with a message for the user for bytes of another kind.
ReplaceView MakeDsImportView(const ReplaceSource& source, edit::DsNewItem kind, std::span<const uint8_t> bytes,
                             const std::string& name, const std::vector<int>& rows);

// The Replace window of a DS stream: `incoming`, the stream of a WAV file or a stream file, replaces stream
// `source.sound`. `result` contains the version that the edit makes, or `error` why it can't be made. Play plays the
// new stream.
ReplaceView MakeDsStreamReplaceView(const ReplaceSource& source, const edit::IncomingWave& incoming, bool wav,
                                    const edit::WaveChanges& changes, std::optional<format::File> result,
                                    std::string error);

// The Import window of a new DS stream of `incoming`, named `name`. Its settings copy stream `source.sound`, one of
// `source.models`, or else the default settings of Hokuto. The edit of the window uses the bytes that `view.wave`
// contains.
ReplaceView MakeDsStreamImportView(const ReplaceSource& source, const edit::IncomingWave& incoming, bool wav,
                                   const std::string& name);

// The Import window of `incoming` as a new wave after the waves of wave archive `source.sound` (File > Import > Audio
// File… or Wave…). `incoming` is a WAV file or a wave file of the DS. Without a result, `error` says why the wave can't
// go in.
ReplaceView MakeDsNewWaveView(const ReplaceSource& source, const edit::IncomingWave& incoming, bool wav,
                              std::optional<format::File> result, std::string error);

} // namespace hokuto::app
