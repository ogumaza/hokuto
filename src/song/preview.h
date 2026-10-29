// SPDX-License-Identifier: MIT

// Plays an item of a sound archive by itself. The browser plays items this way. A wave of a wave archive plays once as
// the archive holds it. A note of a bank's instrument plays on ntdlib's model of nn::atk, or of nw::snd and AX for a
// Wii U archive, or on the 3DS's archive mode for a 3DS archive (song/ctr_model.h). A note of a Wii bank plays on
// ntdlib's model of NW4R SND and AX, with ntdlib's model of AX's microcode of the game's build. A Wii sequence plays on
// these models too (song/sequence_model.h). A wave sound and a stream sound play on the same model. The game plays them
// the same way. The Wii's stream sounds play on ntdlib's model of NW4R SND's stream player. A sequence plays its notes
// on the same model. The Key Editor plays a note of a sequence sound's instrument the same way. The model mixes in the
// mixing mode of the sound that the item belongs to. The Wii mixes only strictly.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

#include "ntdlib/cafe/snd/engine.h"
#include "ntdlib/nx/atk/engine.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rvl/snd/engine.h"
#include "ntdlib/sar/decode.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sound_build.h"

#include "audio/renderer.h"
#include "song/ctr_model.h"
#include "song/sequence_model.h"

namespace hokuto::song
{

class WavePreview final : public audio::Renderer
{
public:
    // Plays wave `wave` of wave archive `wave_archive` once, from its start to its end, at its sample rate. A mono wave
    // plays in both channels. Throws std::runtime_error if the archive lacks the wave, or if the wave can't be read.
    WavePreview(const ntdlib::sar::SoundArchive& archive, std::size_t wave_archive, std::size_t wave);

    // Plays `pcm` the same way, at its sample rate, such as a wave from the Replace window. With `loops`, a wave with a
    // loop plays round it after its end until the preview stops. The properties of a wave play it that way. Throws
    // std::runtime_error if `pcm` has no sample rate or no channels.
    explicit WavePreview(ntdlib::sar::Pcm pcm, bool loops = false);

    int SampleRate() const override;
    std::size_t Render(float* out, std::size_t frames) override;

private:
    ntdlib::sar::Pcm pcm_;
    bool loops_ = false;
    std::size_t position_ = 0;
};

// A preview on ntdlib's model of the Switch, the Wii U, the 3DS or the Wii: the model's frames, one at a time, until
// nothing sounds or the preview reaches its limit. The last frame of a preview that reaches its limit fades out. The
// model starts at the first Render, on the render worker's thread.
class ModelPreview : public audio::Renderer
{
public:
    // The most frames of the model that the preview lasts. A frame lasts 5 ms on the Switch, 3 ms on the Wii U and the
    // Wii, and about 4.89 ms on the 3DS.
    uint64_t LongestFrames() const;

    int SampleRate() const override;
    std::size_t Render(float* out, std::size_t frames) override;

protected:
    // The console's model, once the preview has started.
    using Model =
        std::variant<std::unique_ptr<ntdlib::nx::atk::ArchiveModel>, std::unique_ptr<ntdlib::cafe::snd::ArchiveModel>,
                     std::unique_ptr<CtrModel>, std::unique_ptr<ntdlib::rvl::snd::ArchiveModel>>;

    // A preview of an item of `archive` that lasts `longest_ms` milliseconds at most, mixed in `mixing`.
    ModelPreview(std::shared_ptr<const ntdlib::sar::SoundArchive> archive, uint64_t longest_ms, Mixing mixing);

    // The same for an item of the Wii's `archive`, on `build` of NW4R SND. The archive has to be read by
    // ntdlib::rvl::snd::kRsarOptions.
    ModelPreview(std::shared_ptr<const ntdlib::rsar::SoundArchive> archive, ntdlib::SndBuild build,
                 uint64_t longest_ms);

    // A new model of the Switch or the Wii U for the archive, in the preview's mixing mode, or of the Wii.
    std::unique_ptr<ntdlib::nx::atk::ArchiveModel> NewSwitchModel() const;
    std::unique_ptr<ntdlib::cafe::snd::ArchiveModel> NewWiiUModel() const;
    std::unique_ptr<ntdlib::rvl::snd::ArchiveModel> NewWiiModel() const;

    // Makes the model and starts what the preview plays. Returns false if nothing can start.
    virtual bool StartModel() = 0;

    // What a preview does before each of the model's frames.
    virtual void BeforeFrame(ntdlib::nx::atk::Engine&)
    {
    }

    virtual void BeforeFrame(ntdlib::cafe::snd::Engine&)
    {
    }

    virtual void BeforeFrame(ntdlib::ctr::snd::Engine&)
    {
    }

    virtual void BeforeFrame(ntdlib::rvl::snd::Engine&)
    {
    }

    // The model's frames run so far.
    uint64_t Frames() const
    {
        return frames_;
    }

    // The length of the console's frames.
    FrameMs FrameLength() const;

    std::shared_ptr<const ntdlib::sar::SoundArchive> archive_;
    std::shared_ptr<const ntdlib::rsar::SoundArchive> wii_archive_; // in place of archive_ on the Wii
    ntdlib::SndBuild build_;                                        // the Wii's build of NW4R SND
    bool wii_u_ = false;
    bool ctr_ = false; // a 3DS archive
    Mixing mixing_;
    Model model_;

private:
    // Runs the next frame of the model into frame_. Returns false at the end of the preview.
    bool NextFrame();

    // NextFrame's run of a frame on `model`. Returns false once nothing sounds, or if the model fails.
    template <class ArchiveModel>
    bool RunFrame(ArchiveModel& model);
    bool RunFrame(CtrModel& model);
    bool RunFrame(ntdlib::rvl::snd::ArchiveModel& model);

    // Copies the output of the frame that `model` ran to frame_. The Wii's model gives its output to RunFrame. RunFrame
    // keeps it in wii_output_.
    template <class ArchiveModel>
    void CopyFrame(const ArchiveModel& model);
    void CopyFrame(const ntdlib::rvl::snd::ArchiveModel& model);

    uint64_t longest_ms_;
    bool started_ = false;
    uint64_t frames_ = 0;
    bool ended_ = false;

    // The frame that Render copies from, two floats for each stereo frame, and the floats copied from it so far.
    std::vector<float> frame_;
    std::size_t frame_read_;
    std::vector<int16_t> wii_output_; // the Wii's last frame, in interleaved stereo
};

class NotePreview final : public ModelPreview
{
public:
    // The note's key and velocity. When the key has no sound, the nearest key with one takes its place.
    static constexpr int kKey = 60;
    static constexpr int kVelocity = 100;

    // How long the note holds for, and the most that a preview lasts, in milliseconds.
    static constexpr uint64_t kHoldMs = 1000;
    static constexpr uint64_t kLongestMs = 10000;

    // Plays a note of program `program` of bank `bank` on ntdlib's model, mixed in `mixing`. The note holds for 1 s,
    // and then a note-off releases it. The preview ends once the note has faded, or after 10 s. The last frame then
    // fades out. Throws std::runtime_error if the archive lacks the bank, or if the bank can't be read. The model
    // starts at the first Render, on the render worker's thread.
    NotePreview(std::shared_ptr<const ntdlib::sar::SoundArchive> archive, std::size_t bank, uint32_t program,
                Mixing mixing = Mixing::kStrict);

    // Plays `key` at `velocity` on program `program` of the bank in slot `slot` of `banks`, a sequence sound's bank
    // slots. The note starts the way that a note of the sound starts. The note holds until `release` is set, and then a
    // note-off releases it. The preview ends the same way. A slot without a bank, or a key without a sound, plays
    // nothing. The Key Editor's Acoustic Feedback plays its notes this way.
    NotePreview(std::shared_ptr<const ntdlib::sar::SoundArchive> archive, std::vector<uint32_t> banks, int slot,
                uint32_t program, int key, int velocity, std::shared_ptr<const std::atomic<bool>> release,
                Mixing mixing = Mixing::kStrict);

    // The same two previews on the Wii's `archive`, on `build` of NW4R SND, or else on the build that the archive's
    // version gives (ntdlib's BuildOfRsarVersion). The archive has to be read by ntdlib::rvl::snd::kRsarOptions. `bank`
    // and `banks` give the banks by their indices in the archive's bank table. A Wii sequence sound has one bank slot.
    NotePreview(std::shared_ptr<const ntdlib::rsar::SoundArchive> archive, std::size_t bank, uint32_t program,
                const std::optional<ntdlib::SndBuild>& build = {});
    NotePreview(std::shared_ptr<const ntdlib::rsar::SoundArchive> archive, std::vector<uint32_t> banks, int slot,
                uint32_t program, int key, int velocity, std::shared_ptr<const std::atomic<bool>> release,
                const std::optional<ntdlib::SndBuild>& build = {});

    // The key that a preview of `instrument` plays: kKey, or the nearest key with a sound. Of two keys equally near,
    // the lower one plays. -1 when no key has a sound. The same for program `program` of the Wii's `bank`.
    static int KeyOf(const ntdlib::sar::Instrument& instrument);
    static int KeyOf(const ntdlib::rsar::Bank& bank, uint32_t program);

    // The model's frames that the note holds for.
    uint64_t HoldFrames() const;

private:
    bool StartModel() override;
    void BeforeFrame(ntdlib::nx::atk::Engine& engine) override;
    void BeforeFrame(ntdlib::cafe::snd::Engine& engine) override;
    void BeforeFrame(ntdlib::ctr::snd::Engine& engine) override;
    void BeforeFrame(ntdlib::rvl::snd::Engine& engine) override;

    // After the hold, or once the note is let go, each channel gets a note-off.
    template <class Engine>
    void LetGo(Engine& engine);

    std::vector<uint32_t> banks_; // the banks' items in a sound's bank slots
    int slot_ = 0;
    uint32_t program_;
    int key_ = -1;
    int velocity_ = kVelocity;

    // The flag that lets the note go, or nothing for a note that holds for 1 s, and whether the note-off has come.
    std::shared_ptr<const std::atomic<bool>> release_;
    bool released_ = false;
};

class SoundPreview final : public ModelPreview
{
public:
    // The most that a preview lasts, in milliseconds: 10 minutes.
    static constexpr uint64_t kLongestMs = 600000;

    // Plays sound `sound` of `archive` on ntdlib's model, mixed in `mixing`. The model plays it as the game does. A
    // wave sound plays the wave of its first note with the sound's volume, pan, pitch, sends, filters and envelope
    // (ntdlib's player of wave sounds). A stream sound plays `stream`, the bytes of its stream file: each track that
    // plays when the sound starts, with the track's volume, pan, sends and filters, and the sound's volume, pitch and
    // sends (ntdlib's player of stream sounds). On the 3DS, a wave sound has no filters, a track has no sends or
    // filters, and the sound has no pitch or sends: the 3DS's players read none. A track's flags can send its channels
    // to the DSP's bus of two channels. The 3DS's stereo output leaves that bus out. The preview ends with the sound. A
    // sound that loops plays on for 10 minutes at most. Throws std::runtime_error with a message for the user if the
    // archive lacks the sound or its files, if the sound is neither kind, if a wave sound has no wave to play, and if
    // `stream` isn't a stream file of the archive's console.
    SoundPreview(std::shared_ptr<const ntdlib::sar::SoundArchive> archive, uint32_t sound,
                 std::vector<uint8_t> stream = {}, Mixing mixing = Mixing::kStrict);

    // Plays a stream sound of `info` with `stream` the same way. The archive needn't hold the sound. The Replace window
    // plays a new stream like this.
    SoundPreview(std::shared_ptr<const ntdlib::sar::SoundArchive> archive, ntdlib::sar::SoundInfo info,
                 std::vector<uint8_t> stream, Mixing mixing = Mixing::kStrict);

    // Plays wave sound `sound` of the Wii's `archive` the same way, on ntdlib's model of NW4R SND and AX, on `build` of
    // NW4R SND, or else on the build that the archive's version gives (ntdlib's BuildOfRsarVersion). The wave sound
    // plays the wave of its first note with the sound's volume, pan, pitch, sends and envelope (ntdlib's WsdPlayer).
    // The archive has to be read by ntdlib::rvl::snd::kRsarOptions. Throws std::runtime_error with a message for the
    // user if the archive lacks the sound or its file, if the sound isn't a wave sound, and if it has no wave to play.
    // A stream sound plays with its stream (below): the call throws for one.
    SoundPreview(std::shared_ptr<const ntdlib::rsar::SoundArchive> archive, uint32_t sound,
                 const std::optional<ntdlib::SndBuild>& build = {});

    // Plays stream sound `sound` of the Wii's `archive` with `stream`, the bytes of its stream file, the same way, on
    // ntdlib's model of NW4R SND's stream player (ntdlib::rvl::snd::StrmPlayer). A build before March 2008 plays the
    // stream's first two channels with the sound's volume. A later build plays the tracks that the sound's entry
    // allocates, each with the stream file's volume and pan of the track. The voices start at once. The stream's first
    // blocks count as loaded. Throws std::runtime_error with a message for the user if the archive lacks the sound, if
    // the sound isn't a stream sound, if `stream` isn't a stream file of the Wii, and if the sound can't play it.
    SoundPreview(std::shared_ptr<const ntdlib::rsar::SoundArchive> archive, uint32_t sound, std::vector<uint8_t> stream,
                 const std::optional<ntdlib::SndBuild>& build);

    // Plays a stream sound of `info`, an entry that the Wii's archive needn't hold, with `stream` the same way. The
    // Import window plays a new stream sound of the Wii like this.
    SoundPreview(std::shared_ptr<const ntdlib::rsar::SoundArchive> archive, ntdlib::rsar::SoundInfo info,
                 std::vector<uint8_t> stream, const std::optional<ntdlib::SndBuild>& build);

private:
    bool StartModel() override;

    // Starts the sound on `engine`. Returns false if it can't start.
    template <class Engine>
    bool StartSound(Engine& engine);

    // Throws std::runtime_error with a message for the user if the stream sound can't play stream_.
    void CheckStream() const;

    // The same for stream sound `info` of the Wii.
    void CheckWiiStream(const ntdlib::rsar::SoundInfo& info) const;

    uint32_t sound_ = 0; // the sound's index in the archive
    ntdlib::sar::SoundInfo info_;
    std::vector<uint8_t> stream_;
    std::optional<ntdlib::rsar::SoundInfo> wii_info_; // the entry of a stream sound of the Wii outside the archive
};

} // namespace hokuto::song
