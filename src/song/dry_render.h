// SPDX-License-Identifier: MIT

// The render of a sequence sound on the console's model without the aux buses, for the Export window's measured
// warnings (song/audibility.h). The synth renders an export's MIDI file without its reverb and chorus. What the aux
// buses add has no counterpart there. The model's render therefore leaves them out. On the Switch, ntdlib's model sends
// a bus without effects to the output, and the render sets the buses' volumes to 0. On the Wii U, a bus reaches the
// output only through its effects, and the render has none. On the Wii, the render gives the model no effects either.

#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/sar/formats.h"

#include "song/sequence.h"
#include "song/sequence_model.h"

namespace hokuto::song
{

// Interleaved stereo at `rate`, with full scale at 1.0.
struct DryRender
{
    int rate = 0;
    std::vector<float> samples;
};

// The output of `sequence` of `archive` on the console's model in `mixing`, from the model's first frame, without the
// aux buses. The sequence's variables start at its starting values, and its changes come at the starts of their frames.
// Playback does the same. The render runs for `seconds`, or until the output ends. Once `cancel` is set, it stops and
// returns what it has. Throws std::invalid_argument for an archive of a console that Hokuto doesn't play.
DryRender RenderDry(std::shared_ptr<const ntdlib::sar::SoundArchive> archive, std::shared_ptr<const Sequence> sequence,
                    Mixing mixing, double seconds, const std::atomic<bool>& cancel);

// The same render of `sequence` of the Wii's `archive`, at the rate of the Wii's model: 32 kHz. The archive has to be
// read by ntdlib::rvl::snd::kRsarOptions.
DryRender RenderDry(std::shared_ptr<const ntdlib::rsar::SoundArchive> archive, std::shared_ptr<const Sequence> sequence,
                    double seconds, const std::atomic<bool>& cancel);

// `render` at `rate`, through the resampler (audio::ResampleFrames). Two renders compare at one rate. The 3DS's model
// renders at 32,728 Hz in strict mixing, and at about 48,070 Hz in enhanced mixing. The Wii's model renders at 32 kHz,
// and the models of the Wii U and the Switch at 48 kHz.
DryRender AtRate(const DryRender& render, int rate);

} // namespace hokuto::song
