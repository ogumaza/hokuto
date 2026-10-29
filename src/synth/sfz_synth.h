// SPDX-License-Identifier: MIT

// A sequence's MIDI file played through its SFZ files by sfizz, the SFZ player that the exports are made for. The
// Export window measures with it what an export to MIDI and SFZ would sound like.

#pragma once

#include <cstdint>
#include <functional>
#include <span>

#include "ntdlib/sequence_export/sfz.h"

#include "synth/render.h"

namespace hokuto::synth
{

// The time of the last event of MIDI file `midi`, in seconds, by its tempo changes. Throws std::runtime_error with a
// message for the user when the file can't be read.
double MidiSeconds(std::span<const uint8_t> midi);

// Renders MIDI file `midi` through the SFZ files and samples of `instruments`, for `seconds` seconds at `rate` samples
// a second. ntdlib's converter makes both. Each MIDI track plays through the file of its name, on a separate synth: a
// track named "Track 3" or "Track 3 (layer 2)" plays through Track 3.sfz, whatever its channel. A track without a note
// before the render's end plays nothing, and requires no file. A file without regions plays nothing. Each event plays
// at its exact sample. The tracks' renders add up. sfizz plays at unity gain, with 256 voices for each track and its
// default interpolation, Hermite's. The converter's files hold no effects. `progress` gets the fraction done, from 0 to
// 1, and returns false to stop the render. A render that stops returns no samples. Throws std::runtime_error with a
// message for the user when the MIDI file can't be read, when a track that plays has no SFZ file, when sfizz leaves out
// regions of a file, such as those whose samples it can't read, and when sfizz gives samples that aren't finite.
// Renders can run on several threads at once. sfizz's synths are made, loaded and freed one at a time, and render side
// by side.
StereoRender RenderSfz(std::span<const uint8_t> midi, const ntdlib::sequence_export::SfzInstruments& instruments,
                       int rate, double seconds, const std::function<bool(double)>& progress = {});

} // namespace hokuto::synth
