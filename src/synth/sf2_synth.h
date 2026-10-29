// SPDX-License-Identifier: MIT

// Hokuto's renderer of MIDI files through SoundFont 2 files. The Export window renders a sequence's MIDI file and
// SoundFont with it. The render stands for what a SoundFont player plays. The window compares the render with the song
// as Hokuto plays it.
//
// The renderer follows the SoundFont 2.04 specification. Where the specification leaves a choice open, or where the
// common SoundFont players share a departure from it, the renderer takes these conventions:
//
// - A zone's initialAttenuation attenuates at 0.4 of its value. The E-mu hardware, FluidSynth, BASSMIDI and the other
//   players found do the same. The modulators' attenuation counts in full.
// - The low-pass filter reads a resonance of r centibels as a peak of r / 10 - 3.01 dB at the cutoff. The players read
//   it the same way. The filter's passband falls by half the resonance, r / 20 dB. The specification asks for that
//   fall. The cutoff stays from 1500 to 13500 absolute cents, and below 0.45 times the rate.
// - The default modulator of the pitch wheel targets fineTune with 12800 cents times the bend range / 128. FluidSynth
//   does the same. Section 8.4.10 gives Initial Pitch with 12700 cents. Initial Pitch isn't a generator.
// - At a whole controller value v, the concave curve of the modulators' sources is -20/96 log10(((127 - v) / 127)^2).
//   The convex curve is 1 - concave(127 - v). The specification's figures show these curves. Velocity v attenuates by
//   40 log10(127 / v) dB under the default modulator.
// - MIDI channel 10 plays the presets of bank 128, whatever its bank select. A channel starts with CC7 at 100, CC10 at
//   64, CC11 at 127 and a bend range of 2 semitones. General MIDI gives those values.
// - A note-on of a key that already sounds on its channel acts as the key's note-off first. FluidSynth does the same.
// - A voice's sample starts to play when its volume envelope's delay ends. FluidSynth's voices start the same way.
//
// Where the specification is clear, the renderer follows it:
//
// - The volume envelope's decay and release fall 100 dB in their times. The sustain attenuates by its value in
//   centibels. FluidSynth's decays and releases fall 96 dB in those times. FluidSynth's sustains attenuate by 0.96 of
//   their values.
// - A note of an exclusive class ends the class's other notes of its preset on its channel. The specification asks for
//   a rapid end. The renderer releases those notes over -7200 timecents: 100 dB in about 16 ms. From version 2.4.3 on,
//   FluidSynth releases them over -2000 timecents, about 0.31 s. Earlier versions take 0.89 s. FluidSynth also ends
//   the class's notes of other presets.
//
// The render is dry. CC91 and CC93 set the sends of effects that the renderer doesn't have. The renderer has no limit
// of voices. Each voice reads its sample with 4-point cubic (Catmull-Rom) interpolation. A voice holds its LFOs, its
// modulation envelope, its pitch and its filter for 64 samples at a time. The volume envelope moves every sample.

#pragma once

#include <cstdint>
#include <functional>
#include <span>

#include "synth/render.h"

namespace hokuto::synth
{

// Renders the Standard MIDI File `midi` through the SoundFont 2 file `sf2` at `rate` samples a second. The render runs
// to the file's last event, and on while voices still sound, for 10 s at most. It stops at `most_seconds` if it gets
// there first. `progress`, when it's set, gets the share of the render done from time to time. When it returns false,
// the render stops there and returns the samples so far. Throws std::runtime_error with a message for the user when a
// file can't be read, and std::invalid_argument for a rate or a length that isn't positive.
StereoRender RenderSoundFont(std::span<const uint8_t> midi, std::span<const uint8_t> sf2, int rate, double most_seconds,
                             const std::function<bool(double)>& progress = {});

} // namespace hokuto::synth
