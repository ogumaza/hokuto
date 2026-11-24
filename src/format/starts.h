// SPDX-License-Identifier: MIT

// How a game starts a sequence sound through the sound's handle: the tracks that it quiets, the tracks' volumes, the
// tempo ratio, the pitch, the volume, the sends, the banks, the random generator's state, the low-pass and the
// variables that it sets. A game on DSE starts a song at a level instead. A file keeps a start for each sound that a
// game starts that way (File::starts), and other states of the sound as named starts. An import takes them from
// ntdlib's lists of known games, which find a sound by the SHA-1 of the game's sound archive and the sound's name: the
// 3DS's StartingSequenceTracks and SequenceArrangements, and the Wii U's StartOfSequence and SequenceArrangements. An
// edit changes the archive's SHA-1, so the file keeps what the import found. The levels of DSE's songs come from
// ntdlib's dse::GameStarts, by the game's image and the song's path (format::DseGameStarts). The header contains only
// the types and their constexpr code. The song layer may include it without linking the format layer.

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace hokuto::format
{

// The variables that a start sets, by their numbers in the commands: 0 to 15 are the player's variables, 16 to 31 the
// global ones, and 32 to 47 the track variables 0 to 15.
inline constexpr int kStartVariables = 48;

// The first track variable's number.
inline constexpr int kFirstTrackVariable = 32;

// The tracks that a start can give a mask, a volume or a variable: bit N of a mask for track N.
inline constexpr int kStartTracks = 32;

// A variable that a start sets: variable `variable` takes `value` at `time`, in milliseconds from the sound's start. A
// time of 0 sets it before the sound's first tick. The engine makes a later change at the start of its first frame that
// starts at that time or later. A track variable (32 to 47) of `track` alone, or of every track without one. Only a
// track variable has a track, and only a variable of every track changes after the start.
struct StartVariable
{
    friend bool operator==(const StartVariable&, const StartVariable&) = default;

    uint32_t time = 0;
    uint8_t variable = 0;
    std::optional<uint8_t> track;
    int16_t value = -1;
};

// The sends on aux buses A, B and C that a start gives the sound, from 0 to 1. Each adds to every track's send on its
// bus.
using StartSends = std::array<float, 3>;

// The banks that a start gives a sequence's four bank slots in place of the archive's, by item ID. 0xFFFFFFFF leaves a
// slot empty.
using StartBanks = std::array<uint32_t, 4>;

// The item ID of an empty bank slot in StartBanks.
inline constexpr uint32_t kNoStartBank = 0xFFFFFFFF;

// How a game starts sequence sound `sound`, or with a name, another state of the sound that the game's code moves it to
// later. A player plays the start without a name, and ignores the others. A field without a value leaves the sound as
// its archive starts it.
//
// - `muted`: the tracks that the game mutes, bit N for track N. They run their commands, and none of their notes
//   starts (nw::snd's SequenceSoundHandle::SetTrackMute).
// - `silenced`: the tracks that the game silences, or sets to a volume of 0. Their notes start and take their voices,
//   at a volume of 0 (SetTrackSilence, or SetTrackVolume with a volume of 0).
// - `track_volumes`: the volumes that the game gives tracks (SetTrackVolume), by the track, as factors of amplitude.
// - `tempo_ratio`: the ratio by which the game multiplies the sequence's tempo (SetTempoRatio).
// - `pitch`: the ratio of frequencies by which the game multiplies every voice's pitch (SoundHandle::SetPitch).
// - `volume`: the factor of amplitude by which the game multiplies everything that the sound plays
//   (SoundHandle::SetVolume).
// - `sends`: the sends that the game gives the sound on the aux buses (BasicSound::SetFxSend).
// - `banks`: the banks that the game gives the sequence's bank slots. Only the 3DS's sound library takes them.
// - `seed`: the random generator's state at the sound's first tick.
// - `low_pass`: the low-pass that the game gives the sound (SoundHandle::SetLpfFreq), from kLowestStartLowPass to 0.
//   The player adds it to each track's low-pass, (0xd8 argument - 64) / 64.
// - `variables`: the variables that the game sets, in the order of their times. At one time, the variables of every
//   track take their values before those of a single track.
// - `dse_level`: the level at which a game starts a song on DSE, from 0 to kFullDseLevel (ntdlib::dse::SongState).
//   DSE's driver takes the volume level × 127 / 256, rounded down, at once (ntdlib::dse::StartOf). Only a file of DSE
//   keeps it.
struct SoundStart
{
    friend bool operator==(const SoundStart&, const SoundStart&) = default;

    uint32_t sound = 0; // the sound's index in the archive's sound table
    std::string name;
    uint32_t muted = 0;
    uint32_t silenced = 0;
    std::map<uint8_t, float> track_volumes;
    std::optional<float> tempo_ratio;
    std::optional<float> pitch;
    std::optional<float> volume;
    std::optional<StartSends> sends;
    std::optional<StartBanks> banks;
    std::optional<uint32_t> seed;
    std::optional<float> low_pass;
    std::vector<StartVariable> variables;
    std::optional<int> dse_level;
};

// The lowest low-pass of a start: the sound library's range of SetLpfFreq runs from -1 to 0.
inline constexpr float kLowestStartLowPass = -1.0f;

// The full level of a start on DSE (ntdlib::dse::kFullLevel).
inline constexpr int kFullDseLevel = 256;

// The tracks that `start` quiets in either way.
constexpr uint32_t QuietedTracks(const SoundStart& start)
{
    return start.muted | start.silenced;
}

// Whether `start` sets nothing.
inline bool IsEmpty(const SoundStart& start)
{
    return start.muted == 0 && start.silenced == 0 && start.track_volumes.empty() && !start.tempo_ratio &&
           !start.pitch && !start.volume && !start.sends && !start.banks && !start.seed && !start.low_pass &&
           start.variables.empty() && !start.dse_level;
}

} // namespace hokuto::format
