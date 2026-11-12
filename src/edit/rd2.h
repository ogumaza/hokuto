// SPDX-License-Identifier: MIT

// The edits of a song of Nintendo R&D2's GBA driver, by the rules of its bytes (ntdlib/rd2/commands.h). As on the later
// consoles, a note contains its key, its velocity and its length, and a wait its length. A note or a wait of one byte
// reuses the values of the last note or wait of the track that set them (Command::encoding.repeated). After an edit,
// such a note or wait stores its values in its bytes wherever the track would read other values there.
//
// A song's sequence starts with a header: a count of tracks, then the start of each track. The driver's player plays 10
// tracks at most. A new track, a copy and a pasted track take a free place of the header, or the place after its last.
// Their commands go before the bytes at the sequence's end that no track reads. A removal leaves the track's place of
// the header without a track, and the commands stay in the sequence. A sound effect plays one track of its sequence,
// and accepts no edit of its tracks.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/rd2/driver.h"

#include "edit/bytecode.h"
#include "edit/commands.h"
#include "edit/drivers.h"
#include "edit/notes.h"
#include "edit/tracks.h"
#include "format/file.h"
#include "song/archive.h"
#include "song/sequence.h"

namespace hokuto::edit
{

// The edits of Nintendo R&D2's driver (edit/drivers.h).
const DriverEdits& Rd2Edits();

// The revision of Nintendo R&D2's driver whose bytes `console` follows, or nothing for another driver or console.
std::optional<ntdlib::rd2::Revision> Rd2Of(const song::Console& console);

// Whether `sequence` is one of the sound effects' sequences: its header contains only the starts of its tracks.
bool IsRd2EffectSequence(const ntdlib::commands::Sequence& sequence);

// The banks that slots 0 to `last` of the list of banks of sequence `sequence` of `file` hold. Each is an index of the
// driver's banks. Throws std::invalid_argument with a message for the user when Hokuto can't find the list.
std::vector<uint32_t> Rd2BanksOf(const format::File& file, uint32_t sequence, int last);

// The key of a note command that the arrangement shows at `shown`, on program `program` of the bank in slot `slot` of
// sequence `sequence`'s list of banks, on a track that transposes its notes by `transpose`. The arrangement shows the
// notes of a square and of the wave at the pitch that the tuning of the region sets, and other notes at their keys
// (ntdlib's converter). Of the keys that show at `shown`, the key nearest `shown` less the transpose, or nothing when
// none does. Throws std::invalid_argument with a message for the user when Hokuto can't read the driver's tables.
std::optional<int> Rd2CommandKey(const format::File& file, uint32_t sequence, int slot, uint32_t program, int transpose,
                                 int shown);

// The type of voice that a note of program `program` of the bank in slot `slot` of the list of banks of sequence
// `sequence` plays where the arrangement shows it at `shown`, on a track without a transpose (Rd2CommandKey): a sample,
// a square, the wave or the noise (ntdlib/rd2/sequencer.h). Nothing when no key shows there. Throws
// std::invalid_argument with a message for the user when Hokuto can't read the driver's tables.
std::optional<uint8_t> Rd2VoiceTypeOf(const format::File& file, uint32_t sequence, int slot, uint32_t program,
                                      int shown);

// The Project menu's edits of tracks on Nintendo R&D2's driver (see the top of this file).
TrackEdit AddRd2Track(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                      const song::Sequence& song, std::optional<Program> instrument);
TrackEdit DuplicateRd2Track(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                            int track);
CopiedTrack CopyRd2Track(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                         int track);
TrackEdit PasteRd2Track(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                        const CopiedTrack& copied);
format::File RemoveRd2Track(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                            int track);

} // namespace hokuto::edit
