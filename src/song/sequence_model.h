// SPDX-License-Identifier: MIT

// The model that plays a sequence, with snapshots of its state. A Switch archive plays on ntdlib's model of nn::atk and
// the audio renderer, the model that HSF's archive mode plays. A Wii U archive plays on ntdlib's model of nw::snd and
// AX, the model that CSF's archive mode plays. A 3DS archive plays on ntdlib's archive mode of the 3DS: models of
// nw::snd and nn::snd, and a model of the DSP's audio firmware (song/ctr_model.h). 3SF's archive mode runs the firmware
// itself, in Teakra. The 3DS's DSP mixes every frame. The 3DS's effects run in nn::snd's aux callbacks, on ntdlib's
// models of the libraries' ARM11 effects (ntdlib::ctr_fx). A Wii archive plays on ntdlib's model of NW4R SND and AX,
// the model that WSF's archive mode plays, with ntdlib's model of AX's microcode of the game's build, December or
// October 2006, in place of the DSP (ntdlib::rvl::ax::Renderer), and the effects of the SDK's AXFX library on its aux
// buses. A seek returns the model to the latest snapshot before the target and runs it on from there. The model takes a
// snapshot every 2 s of its output, the first time it runs that far. Past 128 snapshots, it keeps every other one and
// takes them half as often. xsflib's playback does the same for the players. Snapshots cover the sequence's first pass,
// the part that the arrangement shows. On the Switch's music, a snapshot takes about 300 KB. After a run that mixed
// every frame, a seek runs at most 2 s of the model, in about 8 ms.
//
// Frames without mixing reach a place quickly (SetMixing). Such a frame plays the sequence and moves the voices on as a
// mixed frame does. Its output is silent, and the effects and the voices' filters miss it (ntdlib's Renderer::SetMixing
// and System::SetMixing). The frames mix again for a lead before the place (LeadFrames). The effects then take in what
// leads up to it, and the filters settle. A state is settled when every frame in the lead before it has mixed. A seek
// gives a settled state. Close to the latest snapshot that can lead to a settled state, the seek runs on from it and
// mixes throughout. Further away, the frames before the lead don't mix. A snapshot that isn't settled gives way to a
// settled one in its stretch of frames. The frames always mix once the sequence has ended: the output's end depends on
// what they give.
//
// The model's position is the player's tick count while the sequence plays. Once the sequence has ended, the releases
// of its last notes can go on sounding, and so can the tails of the effects. The position then goes on at the tempo at
// the end. A DAW's position does the same.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <variant>
#include <vector>

#include "ntdlib/cafe/snd/effect_setup.h"
#include "ntdlib/ctr_fx/effects.h"
#include "ntdlib/nx/audio/i3dl2.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rvl/snd/effect_setup.h"
#include "ntdlib/sar/formats.h"

#include "song/sequence.h"

namespace ntdlib::nx::atk
{

struct ArchiveModel;

} // namespace ntdlib::nx::atk

namespace ntdlib::cafe::snd
{

struct ArchiveModel;

} // namespace ntdlib::cafe::snd

namespace hokuto::song
{

class CtrModel;

// An effect of the Wii U on an aux bus of the TV: nw::snd's class, and the words of its parameter structure. A float
// takes its bits, and a reverb's mode its whole number (see ntdlib::cafe::snd::EffectSetup).
struct WiiUEffect
{
    friend bool operator==(const WiiUEffect&, const WiiUEffect&) = default;

    ntdlib::cafe::snd::EffectType type = ntdlib::cafe::snd::EffectType::kReverbHi;
    std::vector<uint32_t> words;
};

// An effect of the Wii on an aux bus: NW4R SND's class, and the words of its parameter structure. A float takes its
// bits, and a 2008 reverb's mode its whole number (see ntdlib::rvl::snd::EffectSetup).
struct WiiEffect
{
    friend bool operator==(const WiiEffect&, const WiiEffect&) = default;

    ntdlib::rvl::snd::EffectType type = ntdlib::rvl::snd::EffectType::kReverbHi;
    std::vector<uint32_t> words;
};

// A reverb of the 3DS: nw::snd's FxReverb, or one of the two versions of nn::snd's FxReverb. The 3DS's sound libraries
// run it on the ARM11 over an aux bus (ntdlib::ctr_fx::Reverb).
struct CtrReverb
{
    ntdlib::ctr_fx::Library library = ntdlib::ctr_fx::Library::kNwSnd;
    ntdlib::ctr_fx::ReverbParam param;
};

// A delay of the 3DS: nw::snd's FxDelay or nn::snd's FxDelay. The two compute the same (ntdlib::ctr_fx::Delay).
struct CtrDelay
{
    ntdlib::ctr_fx::DelayParam param;
};

// An effect on an aux bus: the Switch's I3DL2 reverb, or an effect of the Wii U, the Wii or the 3DS. nn::atk runs the
// reverb on an aux effect's samples. A console's model plays the effects of that console and leaves out the others.
using AuxEffect = std::variant<ntdlib::nx::audio::I3dl2ReverbParameterSet, WiiUEffect, WiiEffect, CtrReverb, CtrDelay>;

// The aux buses: A, B and C.
inline constexpr std::size_t kAuxBuses = 3;

// How a console's model mixes. Strict mixing follows the console to the bit. Enhanced mixing runs ntdlib's enhanced
// mode: the sequencer, the instruments and the effects play as on the console, with up to 1,024 voices, mixed at 48 kHz
// in floating point and resampled with a long windowed-sinc filter.
enum class Mixing
{
    kStrict,
    kEnhanced,
};

// The effects on the aux buses A to C, each bus's in the order that they process it. ntdlib's model of nn::atk sends
// each bus to the output, through its effects if it has any. ntdlib's model of AX sends a bus to the output only
// through its effects. So does the 3DS's model. The 3DS's buses are A and B.
using AuxEffects = std::array<std::vector<AuxEffect>, kAuxBuses>;

// The gain of each of the aux buses A to C as it joins the output, after its effects. 1 is full volume.
using AuxVolumes = std::array<float, kAuxBuses>;
inline constexpr AuxVolumes kFullVolumes = {1.0f, 1.0f, 1.0f};

// The parameters that a new effect of the Wii U starts from, in the order of its parameter structure, with a reverb's
// modes as whole numbers. Hokuto chooses them: a reverb of 3 s after 20 ms, a chorus of 10 ms, and a delay of 160 ms
// that feeds 0.4 of its output back.
std::vector<float> WiiUStartingParameters(ntdlib::cafe::snd::EffectType type);

// The effect of `type` with `parameters` in the order of its parameter structure. Each float becomes its bits, and each
// of a reverb's modes its whole number. A negative mode becomes 0.
WiiUEffect WiiUEffectOf(ntdlib::cafe::snd::EffectType type, std::span<const float> parameters);

// The same for the Wii's effects. Hokuto chooses the starting values as for the Wii U: a reverb of 3 s after 20 ms, a
// chorus of 10 ms, and a delay of 160 ms that feeds 0.4 of its output back.
std::vector<float> WiiStartingParameters(ntdlib::rvl::snd::EffectType type);
WiiEffect WiiEffectOf(ntdlib::rvl::snd::EffectType type, std::span<const float> parameters);

// The Wii's effects among `effects` on the aux buses, in the order that they process each bus. NW4R SND sets them up in
// that order. A SequenceModel of the Wii sets them up, and so does the length analysis (song/lengths.h).
std::vector<ntdlib::rvl::snd::EffectSetup> WiiEffectSetups(const AuxEffects& effects);

// The return volume of an aux bus of the Wii in AX's 1.15 for `volume`. The largest value is just under 2.
uint16_t WiiReturnVolume(float volume);

// The frames of 3 ms that the effects of the Wii U or of the Wii among `effects` may ring for after the output last had
// sound (louder than ±8): a second, and the longest time that a bus's effects take to give their input back. A delay's
// echo comes back after a quiet gap that can last the delay's time. A reverb's input comes back after its pre-delay,
// and a chorus's after its delay. 0 without an effect of that console.
int WiiURingingFrames(const AuxEffects& effects);
int WiiRingingFrames(const AuxEffects& effects);

// An I3DL2 reverb's twelve parameters, in the order of the fields of nn::audio's I3dl2ReverbParameterSet. A Hokuto file
// keeps them in that order. I3dl2Reverb makes the set, and I3dl2Parameters takes it apart.
inline constexpr std::size_t kI3dl2Parameters = 12;
ntdlib::nx::audio::I3dl2ReverbParameterSet I3dl2Reverb(std::span<const float, kI3dl2Parameters> parameters);
std::array<float, kI3dl2Parameters> I3dl2Parameters(const ntdlib::nx::audio::I3dl2ReverbParameterSet& reverb);

// ntdlib's model of the Switch or the Wii U for `archive`, with `effects` on the aux buses at `volumes`. A
// SequenceModel sets its model up the same way. The length analysis plays a sound on it (song/lengths.h).
std::unique_ptr<ntdlib::nx::atk::ArchiveModel> SwitchModel(const ntdlib::sar::SoundArchive& archive,
                                                           const AuxEffects& effects, const AuxVolumes& volumes);
std::unique_ptr<ntdlib::cafe::snd::ArchiveModel> WiiUModel(const ntdlib::sar::SoundArchive& archive,
                                                           const AuxEffects& effects, const AuxVolumes& volumes);

// Copies the last frame's output of `model` to `out` in floating point, with the channels interleaved and full scale at
// 1.0. The enhanced mode gives its samples without rounding. The strict mode's 16-bit samples take the scale. Each
// sample past the output's end becomes 0.
void CopyOutput(const ntdlib::nx::atk::ArchiveModel& model, std::span<float> out);
void CopyOutput(const ntdlib::cafe::snd::ArchiveModel& model, std::span<float> out);
void CopyOutput(const CtrModel& model, std::span<float> out);

// A console's model of its sound library, with the player of a sequence, and a copy of its state (sequence_model.cpp).
class ConsoleModel;
struct ConsoleSnapshot;

// The players of a sequence share its model. Only the render worker's thread may use it.
class SequenceModel
{
public:
    // The tracks of the consoles' sequence players.
    static constexpr int kTracks = 16;

    // The frames from one snapshot to the next at first. 0 stands for 2 s of the model's frames.
    static constexpr uint64_t kSnapshotInterval = 0;

    // Plays sequence `sequence` of `archive` with `effects` on the aux buses, in `mixing`, and keeps all three. The
    // archive's flavor selects the console's model. The sequence's variables start at its starting values, and its
    // changes come at the starts of their frames. The model starts at the first Seek. A test can give a shorter
    // interval between snapshots. Throws std::invalid_argument for an archive of a console that Hokuto doesn't play.
    SequenceModel(std::shared_ptr<const ntdlib::sar::SoundArchive> archive, std::shared_ptr<const Sequence> sequence,
                  AuxEffects effects = {}, Mixing mixing = Mixing::kStrict,
                  uint64_t snapshot_interval = kSnapshotInterval);

    // Plays sequence `sequence` of the Wii's `archive` with `effects` on the aux buses, and keeps all three. The
    // archive has to be read by ntdlib::rvl::snd::kRsarOptions. Without the fields of playback, the sequence doesn't
    // start. The model follows the sequence's build of NW4R SND, the game's, or else the build that the archive's
    // version gives (SettingsOf). Its effects follow that build's AXFX library. Every frame mixes. A mute sets the
    // track's volume to 0 through the sound's handle.
    SequenceModel(std::shared_ptr<const ntdlib::rsar::SoundArchive> archive, std::shared_ptr<const Sequence> sequence,
                  AuxEffects effects = {}, uint64_t snapshot_interval = kSnapshotInterval);

    SequenceModel(const SequenceModel&) = delete;
    SequenceModel& operator=(const SequenceModel&) = delete;

    ~SequenceModel();

    const Sequence& GetSequence() const;

    // The model's output rate, and the stereo frames in each of its frames: 240 at 48 kHz on the Switch, a frame of
    // 5 ms, 144 at 48 kHz on the Wii U, a frame of 3 ms, 160 at 32,728 Hz on the 3DS, a frame of about 4.889 ms, and 96
    // at 32 kHz on the Wii, a frame of 3 ms. The 3DS's DSP runs at 268111856 / 8192 Hz, and its players report
    // 32,728 Hz. In enhanced mixing, the 3DS's frame holds 235 at about 48,070 Hz. The other consoles' frames stay the
    // same in both modes.
    int SampleRate() const;
    std::size_t FrameSamples() const;

    // The ticks that a frame plays at the tempo at tick `ticks` of the sequence.
    double TicksPerFrame(double ticks) const;

    // Runs the model up to and including the frame that plays tick `ticks`: the first frame after which the position is
    // past `ticks`. Output() then holds that frame, settled (see above). Returns false if the sequence ends and its
    // last note fades before that frame, if the clock stops before it (ClockStopped), or if the model can't play it.
    // The mixing is on afterwards.
    bool Seek(uint64_t ticks);

    // Runs the model up to and including frame `frame`, counted from 0 at the model's start. Output() then holds that
    // frame, settled. The frames of two models of a sequence line up in time, whatever their effects. A new model takes
    // over playback at the old model's frame this way. Several frames can play the same tick at a slow tempo. Returns
    // false if the output ends before that frame, or if the model can't play it. The mixing is on afterwards.
    bool SeekFrame(uint64_t frame);

    // Whether the frames that RunFrame runs from now on mix (see above). A host turns the mixing off to reach a place
    // quickly, and on again in the lead before it (InLead).
    void SetMixing(bool mixing);

    // The frames of the lead: 0.5 s, or the longest time in which an effect's tail falls by 60 dB, up to 20 s. The
    // voices' filters settle well within 0.5 s. A chorus's LFO moves on only in the frames that mix. Its phase then
    // never comes back to the phase of a run that mixes every frame. With a chorus, every frame mixes, and the lead
    // holds the largest number of frames.
    uint64_t LeadFrames() const;

    // Whether the next frame lies in the lead before frame `frame`, or after the lead's start. A seek from far away
    // mixes such frames. A host that runs the model up to frame `frame` and mixes them reaches a settled state.
    bool InLead(uint64_t frame) const;

    // The same for the frame that plays tick `ticks` (see Seek). The lead's ticks follow the tempo at `ticks`.
    bool InLeadOfTick(double ticks) const;

    // Runs the next frame. Returns false once the sequence has ended, no channel sounds and the last frame came out
    // silent, or if the model fails. The effects' tails sound on after the last channel. The 3DS's reverbs can leave a
    // constant offset of a step or two in the output. A delay that feeds back on the Wii U or the Wii can leave samples
    // of ±1 for ever. With an effect of the 3DS, an effect of the Wii U that rings or an effect on a bus of the Wii, a
    // frame within ±8 therefore counts as silent once the output has stayed there for a while (see sequence_model.cpp).
    // Otherwise a silent frame holds only zeros. So does one on the Switch. Damaged data in the archive makes the model
    // fail.
    bool RunFrame();

    // Mutes the tracks in `tracks`, a bit for each track's number, and unmutes the others. A muted track plays on, and
    // only what's heard changes (see ntdlib's Engine::MuteTrack). A change fades across the next frame. On the 3DS, it
    // reaches the output a few frames later: the DSP's output lags its mix. The mutes hold through seeks.
    void SetMutes(uint32_t tracks);

    // Sets the volumes of the aux buses. On the Switch, the channels of a bus reach the output at its volume
    // (nn::audio's SetSubMixMixVolume). On the Wii U, the volume is the return volume of the TV's bus
    // (AXSetAuxReturnVolume), held below 2. On the Wii, it's the bus's return volume (AXSetAuxAReturnVolume and so on),
    // held below 2 too. On the 3DS, a volume up to 1 is the bus's return volume (nn::snd's SetAuxReturnVolume). Above
    // 1, the bus returns at full volume, and its samples take the volume after its effects. A change takes effect in
    // the next frame. On the Wii U and the Wii, it moves across that frame. The volumes hold through seeks. Each bus
    // starts at full volume.
    void SetBusVolumes(const AuxVolumes& volumes);

    // The last frame's output in interleaved stereo, and the position before and after it. FloatOutput gives the output
    // in floating point, with full scale at 1.0 (see CopyOutput).
    std::span<const int16_t> Output() const;
    std::span<const float> FloatOutput();
    double Before() const;
    double After() const;

    // Whether the player's clock gives no ticks: the sequence set a tempo or a timebase of 0. A game's sequence can do
    // that, such as with a tempo from a variable that the game's code never set. The position then stays where it is,
    // and the frames play on. None of the sequence's commands starts the clock again.
    bool ClockStopped() const;

    // The frames from the model's start up to and including the last one. A seek counts the frames that a run from the
    // start would have run.
    uint64_t Frames() const;

    // The peaks of the meter of track `track` in the last frame, by the track's number: left and right, on the scale of
    // the output's samples (see ntdlib's Engine::MeterTracks). A track's meter adds up the output of its notes. A muted
    // track's meter stays at 0. On the 3DS, the meters show the frame that the DSP mixed last, about three frames
    // before the output plays it.
    std::array<int32_t, 2> TrackPeaks(int track) const;

    // The peaks of what aux bus `bus` added to the output in the last frame, at the bus's volume: left and right, on
    // the scale of the output's samples (see ntdlib's Renderer::MeterMix and System::AuxReturnPeaks, and the Wii's
    // ArchiveModel::AuxReturnPeaks). On the 3DS, the peaks are those of the bus's front channels after its effects, in
    // the frame that the effects processed last. The DSP adds that frame to the output two frames later, and the output
    // plays it about three frames after that.
    std::array<int32_t, 2> BusPeaks(int bus) const;

    // The values of the variables that a game can set, after the last frame, by their numbers. Before the model starts,
    // they're the sequence's starting values.
    std::array<int16_t, kGameVariables> Variables() const;

    std::size_t SnapshotCount() const;

private:
    // A snapshot before frame `frame`, counted from the model's start, when the player's tick count was `ticks`. Each
    // frame before it from frame `mixed_from` on mixed. The player's tick count was `mixed_from_ticks` before that
    // frame.
    struct Saved
    {
        uint64_t frame = 0;
        uint64_t ticks = 0;
        uint64_t mixed_from = 0;
        double mixed_from_ticks = 0.0;
        std::unique_ptr<const ConsoleSnapshot> snapshot;
    };

    // Past this many snapshots, the interval doubles.
    static constexpr std::size_t kMaxSnapshots = 128;

    // Starts the model and the sequence, and takes the first snapshot. Returns false if the sequence can't start.
    bool Start();

    // Runs a frame, after a snapshot if one is due.
    bool Step();

    void SaveIfDue();
    void Restore(const Saved& saved);

    // The frame where the lead before frame `frame` starts, and the position where the lead before the frame that plays
    // tick `ticks` starts. Neither lies before the model's start.
    uint64_t LeadStart(uint64_t frame) const;
    double LeadStartOfTick(double ticks) const;

    // Whether a state before frame `frame` is settled, when each frame from frame `mixed_from` on has mixed.
    bool Settled(uint64_t frame, uint64_t mixed_from) const;

    // Makes the game's changes of the variables that come in the next frame (Sequence::changes). A game makes them
    // through the sound's handle.
    void ChangeVariables();

    // Gives the mutes to the engine, and the buses' volumes to the console's mixer.
    void ApplyMutes();
    void ApplyBusVolumes();

    std::shared_ptr<const ntdlib::sar::SoundArchive> archive_;
    std::shared_ptr<const ntdlib::rsar::SoundArchive> wii_archive_; // in place of archive_ on the Wii
    std::shared_ptr<const Sequence> sequence_;
    AuxEffects effects_;
    Mixing mixing_;
    ntdlib::sar::Flavor flavor_;
    FrameMs frame_ms_; // the length of the console's frames
    std::size_t frame_samples_;
    std::unique_ptr<ConsoleModel> model_;
    bool playing_ = false; // the sequence started
    bool started_ = false;
    bool failed_ = false;
    uint64_t frame_ = 0;            // the frames run since the model's start
    bool mixes_ = true;             // whether the next frames mix
    uint64_t mixed_from_ = 0;       // each frame from this one on has mixed
    double mixed_from_ticks_ = 0.0; // the position before that frame
    uint64_t lead_;
    double before_ = 0.0;
    double after_ = 0.0;
    uint32_t mutes_ = 0;
    AuxVolumes bus_volumes_ = kFullVolumes;
    uint64_t interval_;
    std::vector<Saved> saved_; // in order of their frames, and so of their ticks
    std::vector<float> float_output_;
};

} // namespace hokuto::song
