// SPDX-License-Identifier: MIT

// What else an edit of a track's mix or instrument changes. The edits change the commands that set the setting (mix.h,
// instruments.h). The sound's other tracks can run the same commands, and so can the other sounds that play the same
// sequence file, such as the callers of a subroutine. A new command at a track's opening reaches the other sounds that
// open the track there too. The mixer and the inspector list them before an edit. The tempo row lists the other sounds
// that run a command that sets the tempo.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "ntdlib/sequence_export/performance.h"

#include "edit/replace.h"
#include "format/file.h"
#include "song/sequence.h"

namespace hokuto::edit
{

// The other tracks and sounds that an edit reaches: the sound's tracks that ran the commands, by their numbers, and the
// other sounds whose commands reach them, by their places in the sound table. A sound counts where any path through its
// commands reaches them. Both ways of each comparison count.
struct Reach
{
    std::vector<int> tracks;
    std::vector<uint32_t> sounds;
};

// What an edit of each setting of a track's mix reaches besides the track, by song::Mix, and what an edit of the
// instrument of the track's first note reaches.
struct TrackReach
{
    std::array<Reach, ntdlib::sequence_export::kMixSettingCount> mix;
    Reach instrument;
};

// What an edit of each track of `read` reaches besides the track, in the order of `read`'s tracks. `read` is sound
// `sound` of `file` as ntdlib's performer read it, and `places` gives where each sound plays (SoundPlaces). A track
// whose commands aren't where its stretches show them reaches nothing. Throws std::invalid_argument with a message for
// the user for a sound that plays no sequence that Hokuto edits.
std::vector<TrackReach> ReachOfTracks(const format::File& file, std::span<const std::optional<SoundPlace>> places,
                                      uint32_t sound, const song::Sequence& read);

// The other sounds of `file` whose commands reach each of `commands`, by their places in the sound table. `commands`
// are offsets in the sequence that sound `sound` plays, and `places` gives where each sound plays (SoundPlaces). A
// sound counts where any path through its commands reaches the command, through the track starts too. An offset where
// no command starts reaches nothing. Throws std::invalid_argument with a message for the user for a sound that plays no
// sequence that Hokuto edits.
std::vector<std::vector<uint32_t>> SoundsReaching(const format::File& file,
                                                  std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                                                  std::span<const uint32_t> commands);

} // namespace hokuto::edit
