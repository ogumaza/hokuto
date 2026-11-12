// SPDX-License-Identifier: MIT

// The render of a sequence sound on the console's model without the aux buses, for the Export window's measured
// warnings (song/audibility.h). The synth renders an export's MIDI file without its reverb and chorus. What the aux
// buses add has no counterpart there. The model's render therefore leaves them out. On the Switch, ntdlib's model sends
// a bus without effects to the output, and the render sets the buses' volumes to 0. On the Wii U, a bus reaches the
// output only through its effects, and the render has none. On the Wii, the render gives the model no effects either.
// On the GBA, the render leaves out the reverb of MP2K's mixer and the echo of another driver, and on the DS the
// effects of the capture units.

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

#include "song/played.h"
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

// The output of `sequence` of `played` on the console's model in `mixing`, from the model's first frame, without the
// aux buses. The sequence's variables start at its starting values, and its changes come at the starts of their frames.
// Playback does the same. The render runs for `seconds`, or until the output ends. Once `cancel` is set, it stops and
// returns what it has. Throws std::invalid_argument for an archive of a console that Hokuto doesn't play, and
// std::logic_error when nothing plays. The render has the rate of the model:
// - The Wii's model renders at 32 kHz, and ignores `mixing`. The archive has to be read by
//   ntdlib::rvl::snd::kRsarOptions.
// - The GBA's model renders at 32,768 Hz or 65,536 Hz in strict mixing, and about 48,022 Hz in enhanced mixing. MP2K's
//   mixer applies the reverb, and the render leaves it out. The reverb of the driver and the reverb of the song header
//   are then 0. The song's MIDI file (song::ConvertToMidi) sends the reverb with control change 91, and a song sent to
//   another console sends it to bus A.
// - The DS's model renders at 32,728 Hz in strict mixing, and about 47,989 Hz in enhanced mixing. The DS has no aux
//   buses. The sound plays with the profile of its game, but without the effects that the code of a game sets up
//   through the capture units: a reverb or the SDK's output effect (DryDsProfile). Playback plays them. The synth
//   renders a MIDI file without such effects, and a sound sent to another console has neither. DSE's sounds render at
//   the same rates.
// - On one of the GBA's other drivers (song/driver.h), the render leaves out an echo that the driver mixes
//   (DriverRules::MixesEcho). A MIDI file can't store it, and a song sent to another console sends it to bus A.
DryRender RenderDry(const Played& played, std::shared_ptr<const Sequence> sequence, Mixing mixing, double seconds,
                    const std::atomic<bool>& cancel);

// A sequence and the dry render of it on the model of the console that plays it. `render(seconds, cancel)` returns what
// RenderDry returns for the archive, the ROM or the set, the sequence and the mixing mode that DryOf took. The measure
// reads the sequence for the length of its renders.
struct DrySource
{
    std::shared_ptr<const Sequence> sequence;
    std::function<DryRender(double seconds, const std::atomic<bool>& cancel)> render;
};

// The DrySource of `sequence` of what `played` holds. Its render matches RenderDry. The Wii's model ignores `mixing`.
// Throws std::invalid_argument when nothing is open.
DrySource DryOf(const Played& played, std::shared_ptr<const Sequence> sequence, Mixing mixing = Mixing::kStrict);

// `render` at `rate`, through the resampler (audio::ResampleFrames). Two renders compare at one rate. The 3DS's model
// renders at 32,728 Hz in strict mixing, and at about 48,070 Hz in enhanced mixing. The Wii's model renders at 32 kHz,
// the GBA's at 32,768 Hz or 65,536 Hz, the DS's at 32,728 Hz or about 47,989 Hz, and the models of the Wii U and the
// Switch at 48 kHz.
DryRender AtRate(const DryRender& render, int rate);

} // namespace hokuto::song
