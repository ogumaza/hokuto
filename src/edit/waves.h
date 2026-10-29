// SPDX-License-Identifier: MIT

// Edits of a wave of a wave archive through its model (format::WaveModel): its loop, its rate and its encoding, for the
// properties of a wave. Edits of a wave sound's values in its wave sound file too. Hokuto doesn't edit the samples. A
// wave that the user hasn't edited stays as the game's bytes. An edited wave keeps its model in the file. Every copy of
// its wave archive holds the wave that the model gives (ReplaceFile), and the wave archive's other waves keep their
// bytes and their models.
//
// A loop or a rate alone keeps the wave's encoded samples. A loop in DSP-ADPCM then starts at a frame of 14 samples,
// and the wave keeps the decoder's state at that start (ntdlib's wave::DspAdpcmStateAt). The loop can end at any of the
// wave's samples. A rate changes the pitch, and the samples stay. Another encoding makes the wave from PCM16 samples:
// the model's, or the wave's decoded. The model keeps them, and a later encoding starts from them too. A loop in
// DSP-ADPCM that starts between frames then moves forward to the next frame, and the samples between repeat after the
// loop's end. The wave then ends there.
//
// A wave sound file holds the values of its wave sounds: each one's pan, pitch, filters, sends and envelope where its
// settings hold places for them, and the wave of each note (ntdlib's sar/wave_sound_edit.h). A note also holds a root
// key. The Switch and the Wii U don't read it: they play the wave at the wave's rate times the sound's pitch.
//
// The Wii keeps the waves of a bank or of a wave sound file in the file's wave data, and Hokuto takes that wave data
// for a wave archive, by the file's ID (song::WaveArchivesOf). ntdlib's models of the two kinds of file read their
// waves and lay the file and its wave data out again as NintendoWare for Revolution's tools do (rsar::BankFile and
// rsar::WsdFile). Every group that holds the file then holds the new file and wave data (ReplaceFile). The file's other
// waves keep their models. The Wii decodes DSP-ADPCM as its DSP does (wave::DspAdpcmSum::kWide), and its PCM16 is
// big-endian. A wave sound plays the wave of its first note, with that note's envelope. NW4R SND reads the envelope's
// hold only in its builds from 2008 on.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/wave_file.h"
#include "ntdlib/rsar/wsd_file.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/wave_file.h"
#include "ntdlib/sar/wave_sound_edit.h"
#include "ntdlib/wave/decode.h"

#include "edit/files.h"
#include "format/file.h"

namespace hokuto::edit
{

// The samples of a frame of DSP-ADPCM.
inline constexpr uint32_t kDspAdpcmFrame = 14;

// Wave `wave` of wave archive `wave_archive` of `archive` as its file holds it. Throws std::invalid_argument with a
// message for the user for an archive without the wave and a wave that Hokuto can't read.
ntdlib::sar::WaveFile PresentWave(const ntdlib::sar::SoundArchive& archive, uint32_t wave_archive, uint32_t wave);

// The samples that each channel of `wave` holds, up to the last whole sample of its data. A frame of DSP-ADPCM holds
// 14 samples. The data can hold zeros after the wave's samples.
uint32_t HeldSamples(const ntdlib::sar::WaveFile& wave);

// The first `count` samples of each channel of `wave`, as PCM16, decoded as the Wii U and the Switch decode them.
// Throws std::invalid_argument for more samples than a channel holds.
std::vector<std::vector<int16_t>> DecodedSamples(const ntdlib::sar::WaveFile& wave, uint32_t count);

// A wave of two channels as a wave of one, and the ratio of signal to noise of a new wave in DSP-ADPCM.
struct MonoWave
{
    ntdlib::sar::WaveFile wave;
    std::optional<double> signal_to_noise; // in dB, of the decoded samples against the mean; nothing for PCM
};

// `wave`, of two channels, as a wave of one. Each sample is the mean of the two channels' samples times `gain`, rounded
// half away from zero and held to 16 bits, in the wave's encoding. The rate and the loop stay. A loop in DSP-ADPCM that
// starts between frames moves forward to the next frame. A change of encoding moves it the same way (see the top).
// Throws std::invalid_argument for a wave of another number of channels.
MonoWave MonoWaveOf(const ntdlib::sar::WaveFile& wave, double gain = 1.0);

// The bytes of the file of MonoWaveOf(wave).wave, without the encoding. Throws as MonoWaveOf does.
std::size_t MonoWaveBytes(const ntdlib::sar::WaveFile& wave);

// The levels of the two channels of `wave` and of their mean, over the samples that MonoWaveOf takes: each channel's
// mean power, the mean of the products of the two channels' samples, and the mean's peak. Throws as MonoWaveOf does.
struct MonoLevel
{
    double left = 0.0;
    double right = 0.0;
    double both = 0.0;
    int peak = 0; // the largest magnitude among the mean's samples, rounded as MonoWaveOf rounds them
};
MonoLevel MonoLevelOf(const ntdlib::sar::WaveFile& wave);

// The model of wave `wave` of wave archive `wave_archive` of `archive`, the archive that `file` builds: the file's
// model where the user edited the wave, or the wave's values without samples. Throws as PresentWave does.
format::WaveModel WaveModelOf(const format::File& file, const ntdlib::sar::SoundArchive& archive, uint32_t wave_archive,
                              uint32_t wave);

// `file` with wave `wave` of wave archive `wave_archive` of `archive` made from `model`. The comment at the top says
// how. The file keeps the model. Throws std::invalid_argument with a message for the user for a model that
// format::WaveModelProblem refuses, a length other than the wave's where the samples stay, a loop in DSP-ADPCM that
// starts between frames where the samples stay, and where PresentWave or ReplaceFile refuse.
format::File WithWaveModel(const format::File& file, const ntdlib::sar::SoundArchive& archive, uint32_t wave_archive,
                           uint32_t wave, format::WaveModel model);

// `file` with `incoming`, a wave from a WAV file or a wave file (ReadIncomingWave and ReadIncomingWaveFile), in the
// place of wave `wave` of wave archive `wave_archive` of `archive`, or after the wave archive's waves without `wave`.
// Every copy of the wave archive holds the new wave. The other waves keep their bytes and their models. The new wave
// keeps the model of a WAV file's wave (IncomingWave::model). Throws as WaveArchiveWith and ReplaceFile do.
format::File WithIncomingWave(const format::File& file, const ntdlib::sar::SoundArchive& archive, uint32_t wave_archive,
                              std::optional<uint32_t> wave, const IncomingWave& incoming);

// `file` with `incoming` after the waves of wave archive `wave_archive` of `archive`, in their order. Each goes in as
// WithIncomingWave puts one. The new waves take the wave archive's next indices. Throws as WithIncomingWave does.
format::File WithNewWaves(const format::File& file, const ntdlib::sar::SoundArchive& archive, uint32_t wave_archive,
                          const std::vector<IncomingWave>& incoming);

// A wave sound's values in its wave sound file, the places that its settings hold for them, and the wave of its first
// note. The sound libraries play that wave. The surround pan shares the pan's place.
struct WaveSoundFields
{
    ntdlib::sar::WaveSoundFile::WaveSound values;
    ntdlib::sar::WaveSoundPlaces places;
    ntdlib::sar::WaveId wave;
};

// The fields of wave sound `sound` of `archive`. On the 3DS, ntdlib reads no filters of a wave sound, and only its
// sends to buses A and B. The fields have no places for the others. Throws std::invalid_argument with a message for the
// user for a sound that isn't a wave sound, and a wave sound file that ntdlib can't read or that holds no note for the
// sound.
WaveSoundFields WaveSoundFieldsOf(const ntdlib::sar::SoundArchive& archive, uint32_t sound);

// `file` with the values of wave sound `sound` of `archive` from `values`, and its first note playing `wave`. The other
// wave sounds of its file keep their values, and every copy of the file takes the new bytes. Throws as
// WaveSoundFieldsOf does, and std::invalid_argument with a message for the user for a value without a place that
// differs from the value without it, and where ReplaceFile refuses.
format::File WithWaveSoundFields(const format::File& file, const ntdlib::sar::SoundArchive& archive, uint32_t sound,
                                 const ntdlib::sar::WaveSoundFile::WaveSound& values, ntdlib::sar::WaveId wave);

// The waves of the wave data of file `file_id` of the Wii's `archive`, a bank's or a wave sound file's. ntdlib's model
// of the file reads them. Throws std::invalid_argument with a message for the user for a file that is neither, one that
// the archive lacks, and one that NintendoWare for Revolution's tools didn't lay out.
std::vector<ntdlib::rsar::WaveFile> WavesOf(const ntdlib::rsar::SoundArchive& archive, uint32_t file_id);

// Wave `wave` of the wave data of file `file_id` of the Wii's `archive`. Throws as WavesOf does, and
// std::invalid_argument with a message for the user for a wave past the wave data's waves.
ntdlib::rsar::WaveFile PresentWave(const ntdlib::rsar::SoundArchive& archive, uint32_t file_id, uint32_t wave);

// The samples that each channel of the Wii's `wave` holds. HeldSamples gives those of the other consoles' waves.
uint32_t HeldSamples(const ntdlib::rsar::WaveFile& wave);

// The first `count` samples of each channel of the Wii's `wave`, as PCM16, decoded as the Wii decodes them. Throws as
// DecodedSamples does for the other consoles' waves.
std::vector<std::vector<int16_t>> DecodedSamples(const ntdlib::rsar::WaveFile& wave, uint32_t count);

// The Wii's `wave` as NW4R SND plays it, for a preview: its first two channels, decoded up to the end of its loop or
// its last sample, with its rate and its loop. The Replace and Import windows play a new wave of the Wii this way.
ntdlib::wave::Pcm WavePcm(const ntdlib::rsar::WaveFile& wave);

// The model of wave `wave` of the wave data of file `file_id` of the Wii's `archive`, the archive that `file` builds.
// WaveModelOf gives the other consoles' models. Throws as PresentWave does.
format::WaveModel WaveModelOf(const format::File& file, const ntdlib::rsar::SoundArchive& archive, uint32_t file_id,
                              uint32_t wave);

// `file` with wave `wave` of the wave data of file `file_id` of the Wii's `archive` made from `model`. WithWaveModel
// does likewise for the other consoles' waves. A loop that stays where the wave holds it keeps the wave's state at its
// start, on a frame or between frames. A loop that moves starts at a frame in DSP-ADPCM. Throws as WithWaveModel does,
// and as WavesOf does.
format::File WithWaveModel(const format::File& file, const ntdlib::rsar::SoundArchive& archive, uint32_t file_id,
                           uint32_t wave, format::WaveModel model);

// `file` with `waves` as the waves of the wave data of file `file_id` of the Wii's `archive`. Each wave of `waves` that
// the wave data holds at the same index keeps its model, and the others lose theirs. Throws std::invalid_argument with
// a message for the user as WavesOf and ReplaceFile do, and for a wave that can't be written.
format::File WithWaves(const format::File& file, const ntdlib::rsar::SoundArchive& archive, uint32_t file_id,
                       std::vector<ntdlib::rsar::WaveFile> waves);

// The fields of the Wii's wave sound `sound` of `archive` in the other consoles' terms, for the Sound sections: the
// sound's pan, surround pan, pitch and sends, its first note's envelope and root key, and that note's wave. The wave's
// ID names the wave archive by the file's ID (song::WaveArchivesOf). The file has a place for every value but the
// filters. The Wii's wave sounds have no filters. Throws std::invalid_argument with a message for the user for a sound
// that isn't a wave sound, and for a wave sound file that ntdlib's model can't read or that holds no note for the
// sound.
WaveSoundFields WaveSoundFieldsOf(const ntdlib::rsar::SoundArchive& archive, uint32_t sound);

// `file` with the values of the Wii's wave sound `sound` of `archive` from `values`, and its first note playing wave
// `wave` of its file's wave data. `values` takes the form that WaveSoundFieldsOf gives. The other wave sounds of the
// file keep their values, and the file's waves their models. Every group that holds the file takes the new bytes.
// Throws as WaveSoundFieldsOf does, and std::invalid_argument with a message for the user for filters other than the
// Wii's lack of them, a wave of another file, and where ReplaceFile refuses.
format::File WithWaveSoundFields(const format::File& file, const ntdlib::rsar::SoundArchive& archive, uint32_t sound,
                                 const ntdlib::sar::WaveSoundFile::WaveSound& values, ntdlib::sar::WaveId wave);

} // namespace hokuto::edit
