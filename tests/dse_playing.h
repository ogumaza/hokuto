// SPDX-License-Identifier: MIT

// What the tests of DSE's playback share: the output of ntdlib's dse::Player run directly, and the output of Hokuto's
// model of a set's sound, for the first frames of the model.

#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "ntdlib/dse/driver.h"
#include "ntdlib/dse/player.h"
#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/ntr/sound.h"

#include "song/dse.h"
#include "song/sequence.h"
#include "song/sequence_model.h"

namespace hokuto::testing
{

// The system clocks of the model's frames.
inline constexpr uint64_t kDseFrameClocks = 2 * ntdlib::ntr::snd::kFrameClocks;

// The frames of the model that the tests compare: about 2 s.
inline constexpr int kDseFrames = 180;

// The output of ntdlib's dse::Player run directly for kDseFrames of the model's frames, with a tick of `tick` and the
// song's start `start`, as 16-bit samples in strict mixing.
inline std::vector<int16_t> DirectDse(const song::DseSet& set, uint32_t sound, int tick,
                                      const ntdlib::dse::StartParameters& start = {})
{
    ntdlib::dse::Player player({.build = set.build, .tick_microseconds = tick});
    const ntdlib::dse::SequenceFiles sequence = set.sounds[sound].Files();
    player.LoadBanks(sequence);
    player.Start(sequence, start);

    std::vector<ntdlib::ntr::SoundOutput> out;
    for (int frame = 0; frame < kDseFrames; frame++)
    {
        player.Run(kDseFrameClocks, out);
    }

    std::vector<int16_t> samples;
    ntdlib::ntr::AppendPcm16(out, samples);

    return samples;
}

// The model's output for kDseFrames frames of sound `sound`, or fewer where the sound ends.
inline std::vector<int16_t> ModelledDse(const std::shared_ptr<const song::DseSet>& set, uint32_t sound)
{
    const auto sequence = std::make_shared<const song::Sequence>(song::ReadSequence(*set, sound));
    song::SequenceModel model(set, sequence);
    std::vector<int16_t> samples;
    int frame = 0;
    for (bool more = model.SeekFrame(0); more && frame < kDseFrames; more = model.RunFrame(), frame++)
    {
        samples.insert(samples.end(), model.Output().begin(), model.Output().end());
    }

    return samples;
}

// Whether `samples` are the first samples of `expected`, and sound.
inline bool StartsDse(const std::vector<int16_t>& samples, const std::vector<int16_t>& expected)
{
    return samples.size() <= expected.size() &&
           std::ranges::equal(samples, std::span(expected).first(samples.size())) &&
           std::ranges::any_of(samples, [](int16_t sample) { return sample != 0; });
}

} // namespace hokuto::testing
