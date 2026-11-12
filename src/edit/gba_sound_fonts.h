// SPDX-License-Identifier: MIT

// A SoundFont 2 file's presets as voices of a GBA voice group, for File > Import > Bank…. Each preset gets a program of
// the voice group that no song selects: the number of the preset where no song selects that, and otherwise an empty
// program (edit::FreeGbaPrograms), or else one that has a voice (edit::UnplayedGbaPrograms). Hokuto's renderer reads
// the SoundFont (synth/sound_font.h), and each key of a preset gets the voice that the renderer starts there at
// velocity 100.
//
// A voice of MusicPlayer2000 plays one sample at every velocity. A preset that plays one sample with one envelope over
// every key becomes one voice. A preset that plays several, or leaves keys out, becomes a key split with a row for each
// run of keys. A preset of MIDI bank 128, the SoundFont's percussion, becomes a drum kit with a row for each key. So
// does a preset whose scale tuning isn't 100 cents a key: a drum kit's voice names the key that it plays at, and a
// voice of a key split plays the track's key. A voice at a scale tuning of 0 that plays at the mixer's rate uses the
// fixed pitch of the driver instead, and joins the other keys as any other voice does. The mixer doesn't interpolate
// such a voice, and an ordinary voice a hair off that rate would lose its highest frequencies by turns.
//
// A sample is written into the ROM as 8-bit PCM (edit::EncodeGbaSample), once for each way that the voices play it. Its
// header states the rate at which key 60 plays it, from the root key and the tuning of the voice. The driver has no
// volume for a voice. The sample's points therefore take the voice's attenuation. Only a voice of a drum kit has a pan.
// The driver defines the envelope: the attack adds to the level each frame, the decay and the release scale it each
// frame, and the sustain is a level. A SoundFont's decay and release fall 100 dB in their times, and the driver's as
// fast. Camelot's mixer subtracts the release from the level in a straight line instead. Its voices get the release
// whose fade in ntdlib's export is nearest the release of the SoundFont. The warnings name what the voices leave out.
//
// The driver scales a sample channel by its master volume, and Camelot's mixer doesn't. The samples of a SoundFont that
// ntdlib's converter wrote from the ROM already hold that level. The points of every sample therefore rise by as much
// of it as the loudest point leaves room for, and such a SoundFont comes back at the level of the game's voices.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/mp2k/driver.h"
#include "ntdlib/mp2k/song.h"

#include "edit/gba_voices.h"
#include "format/file.h"
#include "song/gba.h"

namespace hokuto::edit
{

// A run of keys of a preset, the sample that they play, and the values of the voice. The voice's address and its wave
// wait for the sample's place.
struct GbaSoundFontRow
{
    int first_key = 0;
    int last_key = 127;
    std::size_t sample = 0;
    ntdlib::mp2k::Voice voice;
};

// What becomes of a preset.
struct GbaSoundFontPreset
{
    int midi_bank = 0;
    int number = 0;
    std::string name;
    bool drums = false;
    std::vector<GbaSoundFontRow> rows;
};

// A SoundFont's presets as voices, with the samples' bytes and what the voices leave out.
struct GbaSoundFontVoices
{
    std::vector<GbaSoundFontPreset> presets;
    std::vector<std::vector<uint8_t>> samples;
    uint32_t highest_rate = 0; // the highest rate among the samples that the presets play, before `rate` lowers it
    std::size_t bytes = 0;     // the samples and the tables of the key splits and the drum kits
    std::vector<std::string> warnings;
};

// The presets of the SoundFont 2 file `sf2`, in the order of their MIDI banks and numbers, for a ROM with `driver`.
// With `rate`, a sample above that rate is imported resampled to it (audio::Resample). Throws std::invalid_argument
// with a message for the user for a file that Hokuto can't read as a SoundFont, and one without a preset that plays a
// sample.
GbaSoundFontVoices GbaVoicesOfSoundFont(std::span<const uint8_t> sf2, const ntdlib::mp2k::DriverInfo& driver,
                                        std::optional<uint32_t> rate = std::nullopt);

// The program that each preset of `voices` uses, in the order of the presets, among the empty programs `free` and
// `unplayed`, those that have voices that no song selects. A preset gets its number where either list has that number,
// and otherwise the lowest of `free` that no preset took, or else of `unplayed`. A preset past them has none.
std::vector<std::optional<uint32_t>> GbaSoundFontPrograms(const GbaSoundFontVoices& voices,
                                                          std::span<const uint32_t> free,
                                                          std::span<const uint32_t> unplayed);

// `file`, a GBA file, with the presets of `voices` at their programs of the voice group at `group`
// (GbaSoundFontPrograms, with FreeGbaPrograms and UnplayedGbaPrograms). `rom` is the ROM that the file builds. The
// samples go into the free space, and the voice group's table moves there if edit::WithGbaVoicesAt says so. The key
// splits' and the drum kits' tables follow them. Throws std::invalid_argument with a message for the user for a voice
// group whose songs select all its programs, and as WithGbaNewBytes and WithGbaKeyRows do.
GbaGroupVersion WithGbaSoundFont(const format::File& file, const song::GbaRom& rom, uint32_t group,
                                 const GbaSoundFontVoices& voices);

} // namespace hokuto::edit
