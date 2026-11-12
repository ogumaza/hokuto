// SPDX-License-Identifier: MIT

// The track edits of each family of engines: a GBA song (gba_tracks.cpp), a sound of DSE (dse_tracks.cpp), and a
// sequence sound of a sound archive of the later consoles, the Wii or the DS (tracks.cpp and track_copies.cpp). The
// classes of the families (edit/engine_edits.h) call them for AddTrack, DuplicateTrack, CopyTrack, PasteTrack and
// RemoveTrack (edit/tracks.h). Only the files of the track edits include it.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "edit/replace.h"
#include "edit/tracks.h"
#include "format/file.h"
#include "song/sequence.h"

namespace hokuto::edit::track_detail
{

// AddTrack on the GBA. MusicPlayer2000 starts a track at volume 0 (ntdlib's sequencer). The new track sets full volume
// and its instrument first.
TrackEdit AddGbaTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                      const song::Sequence& song, std::optional<Program> instrument);

// DuplicateTrack on the GBA. The copy's field follows the header's others.
TrackEdit DuplicateGbaTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                            int track);

// CopyTrack on the GBA. Every track starts at tick 0, and a song has no labels. `banks` contains the voice group of the
// song.
CopiedTrack CopyGbaTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                         const song::Sequence& song, int track);

// PasteTrack on the GBA. A song plays one voice group, and a track of a song of another voice group can't be added.
TrackEdit PasteGbaTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                        const song::Sequence& song, const CopiedTrack& copied);

// RemoveTrack on the GBA. The fields of the tracks after the track move up a place in the header, and its last place is
// removed. The track's commands stay in the song.
format::File RemoveGbaTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                            int track);

// AddTrack on DSE. A channel's volume and pan start where the driver starts them. A channel starts without one of the
// sound's banks, and a track with an instrument selects its bank first. The end of a track jumps back to its loop
// point. The loop point comes at the sound's loop.
TrackEdit AddDseTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                      const song::Sequence& song, std::optional<Program> instrument);

TrackEdit DuplicateDseTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                            int track);

// CopyTrack on DSE. Every track starts at tick 0, and a sequence has no labels of its tracks. `banks` contains the ids
// of the banks of the sound.
CopiedTrack CopyDseTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                         const song::Sequence& song, int track);

// PasteTrack on DSE. A track's bank selects name banks by their ids. The sound has to hold each bank of the other
// sound. A slide's time has no unit of the common form, and a track of other ticks to a quarter note can't go in.
TrackEdit PasteDseTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                        const song::Sequence& song, const CopiedTrack& copied);

// RemoveTrack on DSE. A removal deletes the commands of the track, and the tracks after it move up a number. The
// channels keep their count. Other tracks of the track's channel lose the settings that it made. The track that sets
// the tempo stays: every other track would play at another speed.
format::File RemoveDseTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                            int track);

// The track edits of a sequence sound of a sound archive of the later consoles, the Wii or the DS. A DS archive has no
// tables of the sounds' tracks, and its banks follow the sound.
TrackEdit AddArchiveTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                          const song::Sequence& song, std::optional<Program> instrument);
TrackEdit DuplicateArchiveTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places,
                                uint32_t sound, int track);
CopiedTrack CopyArchiveTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places,
                             uint32_t sound, const song::Sequence& song, int track);
TrackEdit PasteArchiveTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                            const song::Sequence& song, const CopiedTrack& copied);
format::File RemoveArchiveTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places,
                                uint32_t sound, int track);

} // namespace hokuto::edit::track_detail
