// SPDX-License-Identifier: MIT

// The models of the consoles that SequenceModel plays a sequence on (song/sequence_model.h). A console's file defines
// its model and the factory below. Only the files of the sequence model include it.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "song/driver.h"
#include "song/ds.h"
#include "song/dse.h"
#include "song/gba.h"
#include "song/sequence_model.h"

namespace hokuto::song
{

// A copy of a console's model between frames.
struct ConsoleSnapshot
{
    virtual ~ConsoleSnapshot() = default;
};

// A console's model of its sound library, and the player of the sequence that it plays. The calls follow ntdlib's
// engines and its SequenceSoundPlayer.
class ConsoleModel
{
public:
    virtual ~ConsoleModel() = default;

    // Starts sequence sound `sound`. Returns false if the sound doesn't start. Damaged data in the archive throws.
    virtual bool StartSequence(uint32_t sound) = 0;

    virtual void SetVariable(int number, int16_t value) = 0;

    // The player's local variables, then the global ones.
    virtual std::array<int16_t, kGameVariables> Variables() const = 0;

    virtual uint32_t TickCount() const = 0;
    virtual bool IsFinished() const = 0;

    // The arrangement of the sound that started, for a driver whose clock counts other ticks (DriverPlayer).
    virtual void SetArrangement(const Sequence&)
    {
    }

    // Whether the player's clock gives no ticks: a tempo or a timebase of 0 (ntdlib's ClockStopped). The sequence then
    // plays on without ticks, and none of its commands starts the clock again.
    virtual bool ClockStopped() const = 0;

    virtual bool IsBusy() const = 0;
    virtual void RunFrame() = 0;

    // Whether the last frame came out silent. A model that isn't busy ends after such a frame.
    virtual bool Silent() const = 0;

    // Whether the frames mix (see SequenceModel::SetMixing).
    virtual void SetMixing(bool mixing) = 0;

    // The host's mutes and meters of the player's tracks.
    virtual void MuteTrack(int track, bool mute) = 0;
    virtual void MeterTracks() = 0;
    virtual std::array<int32_t, 2> TrackPeaks(int track) const = 0;

    // The host's volumes and meters of the aux buses. A bus's peaks are at its volume.
    virtual void SetBusVolume(int bus, float volume) = 0;
    virtual void MeterBuses() = 0;
    virtual std::array<int32_t, 2> BusPeaks(int bus) const = 0;

    // The last frame's output in interleaved stereo, and the same in floating point (see CopyOutput).
    virtual std::span<const int16_t> Output() const = 0;
    virtual void CopyOutput(std::span<float> out) const = 0;

    virtual std::unique_ptr<const ConsoleSnapshot> Save() const = 0;
    virtual void Restore(const ConsoleSnapshot& snapshot) = 0;
};

namespace console_model
{

// The variables that ConsoleModel::Variables returns: the 16 of `local` from the first, the 16 of `global` from
// kFirstGlobal, and -1 for the rest.
std::array<int16_t, kGameVariables> GameVariables(const std::array<int16_t, 16>& local,
                                                  const std::array<int16_t, 16>& global);

// Whether every sample of `output` is within `level` of 0 (ConsoleModel::Silent).
bool QuietWithin(std::span<const int16_t> output, int level);

// Copies `floats` to `out`, or else `samples` with the scale of a 16-bit sample, and zeros after them.
void CopyScaled(std::span<const float> floats, std::span<const int16_t> samples, std::span<float> out);

// The frame facts of the model of `played` in `mixing` (FrameFacts). Throws std::logic_error when nothing plays.
FrameFacts FrameFactsOf(const Played& played, Mixing mixing);

// `options` with only what the console of `played` supports (ModelOptions): no effects on the GBA and the DS, strict
// mixing on the Wii, and `dry` on the DS and the GBA's other drivers alone.
ModelOptions SupportedOptions(const Played& played, ModelOptions options);

// The model of the console of `played`, to play `sequence` with `options`. The Wii's model follows the sequence's build
// of NW4R SND, or else the build of the archive's version.
std::unique_ptr<ConsoleModel> NewModel(const Played& played, const Sequence& sequence, const ModelOptions& options);

// The frames of the lead (SequenceModel::LeadFrames): 0.5 s, or the longest tail of the effects, up to 20 s. A chorus
// makes every frame mix.
uint64_t LeadFramesOf(const AuxEffects& effects, int sample_rate, std::size_t frame_samples);

// The models of the GBA, and of the GBA's other drivers (console_gba.cpp). A driver's model can leave out its echo.
std::unique_ptr<ConsoleModel> NewGbaModel(const GbaRom& rom, Mixing mixing, std::size_t frame_samples);
std::unique_ptr<ConsoleModel> NewDriverModel(const DriverRom& rom, Mixing mixing, std::size_t frame_samples, bool dry);

// The length of the model's frames on the GBA (see Gba), and the stereo samples in each.
FrameMs GbaFrameMs(Mixing mixing);
std::size_t GbaFrameSamples(const GbaRom& rom, Mixing mixing);
std::size_t DriverFrameSamples(const DriverRom& rom, Mixing mixing);

// The models of NITRO-Composer and of DSE on the DS (console_ds.cpp), and the frames and the sample rate of both.
std::unique_ptr<ConsoleModel> NewDsModel(const DsArchive& archive, Mixing mixing, bool dry);
std::unique_ptr<ConsoleModel> NewDseModel(const DseSet& set, Mixing mixing);
std::size_t DsFrameSamples(Mixing mixing);
int DsSampleRate(Mixing mixing);

// The models of the Switch and the Wii U (console_switch_wiiu.cpp).
std::unique_ptr<ConsoleModel> NewSwitchModel(const ntdlib::sar::SoundArchive& archive, const AuxEffects& effects,
                                             Mixing mixing);
std::unique_ptr<ConsoleModel> NewWiiUModel(const ntdlib::sar::SoundArchive& archive, const AuxEffects& effects,
                                           Mixing mixing);

// The model of the 3DS (console_ctr.cpp).
std::unique_ptr<ConsoleModel> NewCtrModel(const ntdlib::sar::SoundArchive& archive, const AuxEffects& effects,
                                          Mixing mixing);

// The model of the Wii (console_wii.cpp), with the sample rate of AX and the stereo samples of its frames.
std::unique_ptr<ConsoleModel> NewWiiModel(const ntdlib::rsar::SoundArchive& archive, const ntdlib::SndBuild& build,
                                          const AuxEffects& effects);
int WiiSampleRate();
std::size_t WiiFrameSamples();

} // namespace console_model

} // namespace hokuto::song
