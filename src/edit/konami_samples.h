// SPDX-License-Identifier: MIT

// The edits of the samples of Konami's GBA driver, where the sample table of the driver lists them
// (ntdlib::konami::SampleInfo). An entry of the table names a sample's header: its step at semitone 0, its length and
// its loop start, then its PCM. The note start reads the header, and the mixer plays the PCM at the step, without
// interpolation. In the Dungeon Dice Monsters revision, an entry names the PCM, and contains the length and the timer
// period of the sample rate. That revision plays no loop, and the later builds of the Ultimate Masters revision play no
// loop of a packed sample.
//
// Two entries can name the same header. An edit of the header changes both samples.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "format/file.h"
#include "song/konami.h"

namespace hokuto::edit
{

// The values of a sample that the Bank Editor edits: its rate at semitone 0, and the start of its loop. The loop runs
// to the sample's end. A sample without a loop plays once.
struct KonamiSampleValues
{
    friend bool operator==(const KonamiSampleValues&, const KonamiSampleValues&) = default;

    uint32_t rate = 0; // in Hz
    std::optional<uint32_t> loop_start;
};

// The rates that a sample of `rom` can take, in Hz: those whose step or period the sample table contains. In the
// Dungeon Dice Monsters revision, a note uses the period of its sample only at the pitches whose entry in the table of
// periods of the driver is 0 (`every_pitch` false). The table sets the periods of the other pitches.
struct KonamiRates
{
    uint32_t least = 0;
    uint32_t most = 0;
    bool every_pitch = true;
};
KonamiRates KonamiSampleRates(const song::KonamiRom& rom);

// Whether sample `sample` of `rom` can loop: not in the Dungeon Dice Monsters revision, and not when it's packed.
bool KonamiSampleLoops(const song::KonamiRom& rom, int sample);

// Why sample `sample` of `rom` can't loop, in a sentence for the user, or nothing for a sample that can loop.
std::string KonamiNoLoopReason(const song::KonamiRom& rom, int sample);

// The samples among `samples` of `rom` whose entries name the same header as sample `sample`, `sample` aside. An edit
// of `sample`'s values changes them too. In the Dungeon Dice Monsters revision, each entry contains its values, and no
// sample shares them.
std::vector<int> KonamiSamplesSharing(const song::KonamiRom& rom, int sample, std::span<const int> samples);

// `file`, a file of Konami's driver, with `values` in the entry or the header of sample `sample`. `rom` is the ROM that
// the file builds. The step or the period is set to the value nearest the rate. Throws std::invalid_argument with a
// message for the user for a sample missing from the ROM, a rate outside KonamiSampleRates, a loop on a sample that
// can't loop, and a loop that starts at the sample's end or past it.
format::File WithKonamiSampleValues(const format::File& file, const song::KonamiRom& rom, int sample,
                                    const KonamiSampleValues& values);

// A WAV file as a sample of Konami's driver: one channel of signed 8-bit PCM, with the mean of two channels, at the WAV
// file's rate, and its loop to the sample's end. A loop that ends before the WAV file's end ends the sample there.
struct IncomingKonamiSample
{
    std::vector<int8_t> pcm;
    uint32_t rate = 0;
    std::optional<uint32_t> loop_start;
    std::optional<double> signal_to_noise; // in dB, of the 16-bit samples to the error of their 8-bit values
};

// The WAV file in `bytes` as a sample (edit::ReadIncomingGbaSample reads it). Throws std::invalid_argument with a
// message for the user for bytes that aren't a WAV file of PCM, for no samples, and for a loop that the smpl chunk of
// the WAV file sets backwards or past its end.
IncomingKonamiSample ReadIncomingKonamiSample(std::span<const uint8_t> bytes);

// `file`, a file of Konami's driver, with `incoming` in the place of sample `sample`. `rom` is the ROM that the file
// builds. The new header and PCM go into the free space at the ROM's end, with 16 zeros after them for the mixer to
// read past the end, and the sample's entry names them. The samples that share the entry's header keep the old one. In
// the Dungeon Dice Monsters revision, the PCM is written there alone, padded with zeros to a whole block of 16 samples,
// and the entry records its length and period. Throws std::invalid_argument with a message for the user for a sample
// missing from the ROM, a rate outside KonamiSampleRates, a loop where the sample can't loop, a sample longer than the
// entry can store, and a ROM without room in its 32 MiB.
format::File WithKonamiSample(const format::File& file, const song::KonamiRom& rom, int sample,
                              const IncomingKonamiSample& incoming);

} // namespace hokuto::edit
