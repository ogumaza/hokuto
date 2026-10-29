// SPDX-License-Identifier: MIT

#include "song/preview.h"

#include <algorithm>
#include <exception>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/ctr/snd/sequence.h"
#include "ntdlib/rsar/stream_file.h"
#include "ntdlib/rvl/snd/stream_sound.h"
#include "ntdlib/sar/stream_file.h"
#include "ntdlib/sequence_export/consoles.h"

#include "song/items.h"
#include "song/sequence.h"

namespace hokuto::song
{
namespace
{

namespace atk = ntdlib::nx::atk;
namespace cafe = ntdlib::cafe::snd;
namespace rvl = ntdlib::rvl::snd;
namespace sar = ntdlib::sar;

// A sample as a float, from -1 to just under 1.
constexpr float kSampleScale = 1.0f / 32768.0f;

} // namespace

WavePreview::WavePreview(const sar::SoundArchive& archive, std::size_t wave_archive, std::size_t wave)
    : WavePreview(DecodeItemWave(archive, static_cast<uint32_t>(wave_archive), static_cast<uint32_t>(wave)))
{
}

WavePreview::WavePreview(sar::Pcm pcm, bool loops) : pcm_(std::move(pcm))
{
    if (pcm_.sample_rate == 0 || pcm_.channels.empty())
    {
        throw std::runtime_error("The wave has no sample rate or no channels.");
    }

    loops_ = loops && pcm_.loop && pcm_.loop_start < pcm_.loop_end;
}

int WavePreview::SampleRate() const
{
    return static_cast<int>(pcm_.sample_rate);
}

std::size_t WavePreview::Render(float* out, std::size_t frames)
{
    // A preview that loops goes back to the loop's start at the end. Any other ends there.
    const std::vector<int16_t>& left = pcm_.channels.front();
    const std::vector<int16_t>& right = pcm_.channels.size() > 1 ? pcm_.channels[1] : left;
    std::size_t done = 0;
    while (done < frames)
    {
        if (position_ >= pcm_.Length())
        {
            if (!loops_)
            {
                break;
            }
            position_ = pcm_.loop_start;
        }

        const std::size_t count = std::min<std::size_t>(frames - done, pcm_.Length() - position_);
        for (std::size_t i = 0; i < count; i++)
        {
            out[(done + i) * 2] = static_cast<float>(left[position_ + i]) * kSampleScale;
            out[(done + i) * 2 + 1] = static_cast<float>(right[position_ + i]) * kSampleScale;
        }
        position_ += count;
        done += count;
    }

    return done;
}

ModelPreview::ModelPreview(std::shared_ptr<const sar::SoundArchive> archive, uint64_t longest_ms, Mixing mixing)
    : archive_(std::move(archive)),
      wii_u_(archive_->ReadOptions().flavor == sar::Flavor::kCafe),
      ctr_(archive_->ReadOptions().flavor == sar::Flavor::kCtr),
      mixing_(mixing),
      longest_ms_(longest_ms),
      frame_((wii_u_ ? cafe::ArchiveModel::kFrameSamples
              : ctr_ ? (mixing == Mixing::kEnhanced ? CtrModel::kEnhancedFrameSamples : CtrModel::kFrameSamples)
                     : atk::ArchiveModel::kFrameSamples) *
             2),
      frame_read_(frame_.size())
{
}

ModelPreview::ModelPreview(std::shared_ptr<const ntdlib::rsar::SoundArchive> archive, ntdlib::SndBuild build,
                           uint64_t longest_ms)
    : wii_archive_(std::move(archive)),
      build_(build),
      mixing_(Mixing::kStrict),
      longest_ms_(longest_ms),
      frame_(static_cast<std::size_t>(ntdlib::rvl::ax::kSamplesPerFrame) * 2),
      frame_read_(frame_.size())
{
}

std::unique_ptr<atk::ArchiveModel> ModelPreview::NewSwitchModel() const
{
    atk::EngineOptions options;
    options.enhanced = mixing_ == Mixing::kEnhanced;

    return std::make_unique<atk::ArchiveModel>(*archive_, options);
}

std::unique_ptr<cafe::ArchiveModel> ModelPreview::NewWiiUModel() const
{
    cafe::EngineOptions options;
    options.enhanced = mixing_ == Mixing::kEnhanced;

    return std::make_unique<cafe::ArchiveModel>(*archive_, options);
}

std::unique_ptr<rvl::ArchiveModel> ModelPreview::NewWiiModel() const
{
    auto model = std::make_unique<rvl::ArchiveModel>();
    model->build = build_;
    model->StartWithRenderer(*wii_archive_);

    return model;
}

uint64_t ModelPreview::LongestFrames() const
{
    return FramesIn(longest_ms_, FrameLength());
}

FrameMs ModelPreview::FrameLength() const
{
    return wii_archive_ ? kWiiFrameMs : FrameMsOf(archive_->ReadOptions().flavor);
}

int ModelPreview::SampleRate() const
{
    if (wii_archive_)
    {
        return ntdlib::rvl::ax::kSampleRate;
    }
    if (ctr_)
    {
        return mixing_ == Mixing::kEnhanced ? CtrModel::kEnhancedSampleRate : CtrModel::kSampleRate;
    }

    return wii_u_ ? cafe::ArchiveModel::kSampleRate : atk::ArchiveModel::kSampleRate;
}

std::size_t ModelPreview::Render(float* out, std::size_t frames)
{
    std::size_t done = 0;
    while (done < frames)
    {
        if (frame_read_ == frame_.size() && !NextFrame())
        {
            break;
        }

        const std::size_t count = std::min((frames - done) * 2, frame_.size() - frame_read_);
        std::copy_n(frame_.data() + frame_read_, count, out + done * 2);
        frame_read_ += count;
        done += count / 2;
    }

    return done;
}

bool ModelPreview::NextFrame()
{
    if (!started_ && !ended_)
    {
        started_ = true;
        ended_ = !StartModel();
    }
    const auto run = [this](auto& model)
    {
        return RunFrame(*model);
    };
    if (ended_ || !std::visit(run, model_))
    {
        ended_ = true;
        return false;
    }

    frames_++;

    // The last frame of a preview that runs to the limit fades out.
    const auto output = [this](const auto& model)
    {
        CopyFrame(*model);
    };
    std::visit(output, model_);
    ended_ = frames_ == LongestFrames();
    for (std::size_t i = 0; ended_ && i < frame_.size(); i++)
    {
        const float through = static_cast<float>(i / 2) / static_cast<float>(frame_.size() / 2);
        frame_[i] *= 1.0f - through;
    }
    frame_read_ = 0;

    return true;
}

template <class ArchiveModel>
bool ModelPreview::RunFrame(ArchiveModel& model)
{
    auto& engine = *model.engine;
    BeforeFrame(engine);

    // The preview ends once nothing sounds. Damaged data in the archive makes the model fail, and ends it too.
    if (!engine.IsBusy())
    {
        return false;
    }

    try
    {
        engine.RunFrame();
    }
    catch (const std::exception&)
    {
        return false;
    }

    return true;
}

bool ModelPreview::RunFrame(rvl::ArchiveModel& model)
{
    // The model of AX's microcode refuses a command list that it lacks a command for. AX's stereo mode never sends one.
    rvl::Engine& engine = *model.engine;
    BeforeFrame(engine);
    if (!engine.IsBusy())
    {
        return false;
    }

    wii_output_.clear();
    try
    {
        return model.RunFrame(wii_output_);
    }
    catch (const std::exception&)
    {
        return false;
    }
}

template <class ArchiveModel>
void ModelPreview::CopyFrame(const ArchiveModel& model)
{
    CopyOutput(model, frame_);
}

void ModelPreview::CopyFrame(const rvl::ArchiveModel&)
{
    for (std::size_t i = 0; i < frame_.size(); i++)
    {
        frame_[i] = i < wii_output_.size() ? static_cast<float>(wii_output_[i]) * kSampleScale : 0.0f;
    }
}

bool ModelPreview::RunFrame(CtrModel& model)
{
    // The DSP's output comes out after the engine's frames. The preview ends once the output can't sound any more.
    BeforeFrame(model.Engine());
    if (!model.IsBusy())
    {
        return false;
    }

    try
    {
        return model.RunFrame();
    }
    catch (const std::exception&)
    {
        return false;
    }
}

NotePreview::NotePreview(std::shared_ptr<const sar::SoundArchive> archive, std::size_t bank, uint32_t program,
                         Mixing mixing)
    : NotePreview(archive, {sar::ItemId(sar::kBankItem, static_cast<uint32_t>(bank))}, 0, program, -1, kVelocity,
                  nullptr, mixing)
{
    const std::span<const uint8_t> file = ItemFile(*archive_, {ItemKind::kBank, static_cast<uint32_t>(bank), 0});
    const sar::Bank parsed = sar::Bank::Parse(file, ItemOptions(*archive_));
    if (program < parsed.instruments.size() && parsed.instruments[program])
    {
        key_ = KeyOf(*parsed.instruments[program]);
    }
}

NotePreview::NotePreview(std::shared_ptr<const sar::SoundArchive> archive, std::vector<uint32_t> banks, int slot,
                         uint32_t program, int key, int velocity, std::shared_ptr<const std::atomic<bool>> release,
                         Mixing mixing)
    : ModelPreview(std::move(archive), kLongestMs, mixing),
      banks_(std::move(banks)),
      slot_(slot),
      program_(program),
      key_(key),
      velocity_(velocity),
      release_(std::move(release))
{
}

NotePreview::NotePreview(std::shared_ptr<const ntdlib::rsar::SoundArchive> archive, std::size_t bank, uint32_t program,
                         const std::optional<ntdlib::SndBuild>& build)
    : NotePreview(archive, std::vector<uint32_t>{static_cast<uint32_t>(bank)}, 0, program, -1, kVelocity, nullptr,
                  build)
{
    const std::span<const uint8_t> file = ItemFile(*wii_archive_, {ItemKind::kBank, static_cast<uint32_t>(bank), 0});
    const ntdlib::rsar::Bank parsed = ntdlib::rsar::Bank::Parse(file);
    key_ = KeyOf(parsed, program);
}

NotePreview::NotePreview(std::shared_ptr<const ntdlib::rsar::SoundArchive> archive, std::vector<uint32_t> banks,
                         int slot, uint32_t program, int key, int velocity,
                         std::shared_ptr<const std::atomic<bool>> release, const std::optional<ntdlib::SndBuild>& build)
    : ModelPreview(archive, build.value_or(ntdlib::sequence_export::BuildOfRsarVersion(archive->Version())),
                   kLongestMs),
      banks_(std::move(banks)),
      slot_(slot),
      program_(program),
      key_(key),
      velocity_(velocity),
      release_(std::move(release))
{
}

int NotePreview::KeyOf(const ntdlib::rsar::Bank& bank, uint32_t program)
{
    for (int distance = 0; distance <= 127; distance++)
    {
        for (const int key : {kKey - distance, kKey + distance})
        {
            if (key >= 0 && key <= 127 && bank.Find(program, key, kVelocity) != nullptr)
            {
                return key;
            }
        }
    }

    return -1;
}

int NotePreview::KeyOf(const sar::Instrument& instrument)
{
    for (int distance = 0; distance <= 127; distance++)
    {
        for (const int key : {kKey - distance, kKey + distance})
        {
            if (key >= 0 && key <= 127 && instrument.Find(key, kVelocity) != nullptr)
            {
                return key;
            }
        }
    }

    return -1;
}

uint64_t NotePreview::HoldFrames() const
{
    return FramesIn(kHoldMs, FrameLength());
}

void NotePreview::BeforeFrame(atk::Engine& engine)
{
    LetGo(engine);
}

void NotePreview::BeforeFrame(cafe::Engine& engine)
{
    LetGo(engine);
}

void NotePreview::BeforeFrame(ntdlib::ctr::snd::Engine& engine)
{
    LetGo(engine);
}

void NotePreview::BeforeFrame(rvl::Engine& engine)
{
    LetGo(engine);
}

template <class Engine>
void NotePreview::LetGo(Engine& engine)
{
    // After the hold, or once the note is let go, each channel gets a note-off. A note that ignores note-off plays on
    // to its end.
    const bool let_go = release_ ? release_->load(std::memory_order_acquire) : Frames() >= HoldFrames();
    if (let_go && !released_)
    {
        released_ = true;
        auto& channels = engine.Channels();
        const std::vector<ntdlib::nwsnd::ChannelId> places(channels.InUse().begin(), channels.InUse().end());
        for (const ntdlib::nwsnd::ChannelId place : places)
        {
            channels.At(place).NoteOff(engine);
        }
    }
}

bool NotePreview::StartModel()
{
    // The note starts before the model's first frame. A sequence's first note likewise starts in the frame that plays
    // it.
    if (key_ < 0)
    {
        return false;
    }

    try
    {
        if (wii_archive_)
        {
            std::unique_ptr<rvl::ArchiveModel> model = NewWiiModel();
            rvl::NoteOnInfo info;
            info.prg_no = program_;
            info.key = key_;
            info.velocity = velocity_;
            const bool on = model->engine->NoteOn(banks_, slot_, info) != nullptr;
            model_ = std::move(model);

            return on;
        }

        if (ctr_)
        {
            auto model = std::make_unique<CtrModel>(mixing_ == Mixing::kEnhanced);
            if (!model->Start(*archive_))
            {
                return false;
            }
            ntdlib::ctr::snd::NoteOnInfo info;
            info.prg_no = program_;
            info.key = key_;
            info.velocity = velocity_;
            const bool on = model->Engine().NoteOn(banks_, slot_, info) != nullptr;
            model_ = std::move(model);

            return on;
        }

        if (wii_u_)
        {
            std::unique_ptr<cafe::ArchiveModel> model = NewWiiUModel();
            cafe::NoteOnInfo info;
            info.prg_no = program_;
            info.key = key_;
            info.velocity = velocity_;
            const bool on = model->engine->NoteOn(banks_, slot_, info) != nullptr;
            model_ = std::move(model);

            return on;
        }

        std::unique_ptr<atk::ArchiveModel> model = NewSwitchModel();
        atk::NoteOnInfo info;
        info.prg_no = program_;
        info.key = key_;
        info.velocity = velocity_;
        info.receiver = &model->sub_mix;
        const bool on = model->engine->NoteOn(banks_, slot_, info) != nullptr;
        model_ = std::move(model);

        return on;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

SoundPreview::SoundPreview(std::shared_ptr<const sar::SoundArchive> archive, uint32_t sound,
                           std::vector<uint8_t> stream, Mixing mixing)
    : ModelPreview(std::move(archive), kLongestMs, mixing), sound_(sound), stream_(std::move(stream))
{
    if (sound >= archive_->Sounds().size())
    {
        throw std::runtime_error("The archive lacks sound " + std::to_string(sound) + ".");
    }

    info_ = archive_->Sounds()[sound];
    if (info_.type == sar::SoundType::kWaveSound)
    {
        // The model plays note 0 of the sound. The wave has to be there. The model has to be able to read it.
        const WaveSoundNote note = WaveSoundNoteOf(*archive_, sound);
        DecodeItemWave(*archive_, note.wave_archive, note.wave);
    }
    else if (info_.type == sar::SoundType::kStream)
    {
        CheckStream();
    }
    else
    {
        throw std::runtime_error(info_.name + " is neither a wave sound nor a stream sound.");
    }
}

SoundPreview::SoundPreview(std::shared_ptr<const sar::SoundArchive> archive, sar::SoundInfo info,
                           std::vector<uint8_t> stream, Mixing mixing)
    : ModelPreview(std::move(archive), kLongestMs, mixing), info_(std::move(info)), stream_(std::move(stream))
{
    info_.type = sar::SoundType::kStream;
    CheckStream();
}

SoundPreview::SoundPreview(std::shared_ptr<const ntdlib::rsar::SoundArchive> archive, uint32_t sound,
                           const std::optional<ntdlib::SndBuild>& build)
    : ModelPreview(archive, build.value_or(ntdlib::sequence_export::BuildOfRsarVersion(archive->Version())),
                   kLongestMs),
      sound_(sound)
{
    if (sound >= wii_archive_->Sounds().size())
    {
        throw std::runtime_error("The archive lacks sound " + std::to_string(sound) + ".");
    }
    const ntdlib::rsar::SoundInfo& info = wii_archive_->Sounds()[sound];
    if (info.type == ntdlib::rsar::SoundType::kStream)
    {
        throw std::runtime_error("The file holds no stream of " + info.name + ".");
    }
    if (info.type != ntdlib::rsar::SoundType::kWaveSound)
    {
        throw std::runtime_error(info.name + " is neither a wave sound nor a stream sound.");
    }

    // The model plays the wave of the sound's first note. The wave has to be there. The model has to be able to read
    // it.
    const WaveSoundNote note = WaveSoundNoteOf(*wii_archive_, sound);
    DecodeItemWave(*wii_archive_, note.wave_archive, note.wave);
}

SoundPreview::SoundPreview(std::shared_ptr<const ntdlib::rsar::SoundArchive> archive, uint32_t sound,
                           std::vector<uint8_t> stream, const std::optional<ntdlib::SndBuild>& build)
    : ModelPreview(archive, build.value_or(ntdlib::sequence_export::BuildOfRsarVersion(archive->Version())),
                   kLongestMs),
      sound_(sound),
      stream_(std::move(stream))
{
    if (sound >= wii_archive_->Sounds().size())
    {
        throw std::runtime_error("The archive lacks sound " + std::to_string(sound) + ".");
    }
    CheckWiiStream(wii_archive_->Sounds()[sound]);
}

SoundPreview::SoundPreview(std::shared_ptr<const ntdlib::rsar::SoundArchive> archive, ntdlib::rsar::SoundInfo info,
                           std::vector<uint8_t> stream, const std::optional<ntdlib::SndBuild>& build)
    : ModelPreview(archive, build.value_or(ntdlib::sequence_export::BuildOfRsarVersion(archive->Version())),
                   kLongestMs),
      stream_(std::move(stream)),
      wii_info_(std::move(info))
{
    wii_info_->type = ntdlib::rsar::SoundType::kStream;
    CheckWiiStream(*wii_info_);
}

void SoundPreview::CheckWiiStream(const ntdlib::rsar::SoundInfo& info) const
{
    if (info.type != ntdlib::rsar::SoundType::kStream)
    {
        throw std::runtime_error(info.name + " isn't a stream sound.");
    }

    // The model reads the stream as rsar::StreamFile reads it, and plays the tracks that the build gives the sound.
    ntdlib::rsar::StreamFile file;
    try
    {
        file = ntdlib::rsar::StreamFile::Read(stream_);
    }
    catch (const std::exception& error)
    {
        throw std::runtime_error("Hokuto can't read the stream of " + info.name + ". " + error.what());
    }
    if (!ntdlib::rvl::snd::StrmPlayer::TracksOf(build_, info, wii_archive_->Version(), file))
    {
        throw std::runtime_error(info.name +
                                 " plays no track of its stream: its entry names a track or a channel that "
                                 "the stream lacks.");
    }
}

void SoundPreview::CheckStream() const
{
    // The model reads the stream as sar::StreamFile reads it. A stream of the other console plays nowhere.
    sar::StreamFile file;
    try
    {
        file = sar::StreamFile::Read(stream_);
    }
    catch (const std::exception& error)
    {
        throw std::runtime_error("Hokuto can't read the stream of " + info_.name + ". " + error.what());
    }
    if (file.flavor != archive_->ReadOptions().flavor)
    {
        throw std::runtime_error("The stream of " + info_.name + " is a stream of another console.");
    }
    if (info_.stream.valid_tracks == 0)
    {
        throw std::runtime_error(info_.name + " plays no track when it starts.");
    }
}

bool SoundPreview::StartModel()
{
    // The sound starts in the model's second frame. A game's sound thread starts it there. On the 3DS, it starts in the
    // first. So do the 3DS's sequences.
    try
    {
        if (wii_archive_)
        {
            std::unique_ptr<rvl::ArchiveModel> model = NewWiiModel();
            const bool stream = wii_info_ || wii_archive_->Sounds()[sound_].type == ntdlib::rsar::SoundType::kStream;
            const bool on = wii_info_ ? model->engine->StartStreamSound(*wii_info_, stream_) != nullptr
                            : stream  ? model->engine->StartStreamSound(sound_, stream_) != nullptr
                                      : model->engine->StartWaveSound(sound_) != nullptr;
            model_ = std::move(model);

            return on;
        }

        if (ctr_)
        {
            auto model = std::make_unique<CtrModel>(mixing_ == Mixing::kEnhanced);
            const bool on = model->Start(*archive_) && StartSound(model->Engine());
            model_ = std::move(model);

            return on;
        }

        if (wii_u_)
        {
            std::unique_ptr<cafe::ArchiveModel> model = NewWiiUModel();
            const bool on = StartSound(*model->engine);
            model_ = std::move(model);

            return on;
        }

        std::unique_ptr<atk::ArchiveModel> model = NewSwitchModel();
        const bool on = StartSound(*model->engine);
        model_ = std::move(model);

        return on;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

template <class Engine>
bool SoundPreview::StartSound(Engine& engine)
{
    if (info_.type == sar::SoundType::kWaveSound)
    {
        return engine.StartWaveSound(sound_) != nullptr;
    }

    return engine.StartStreamSound(info_, stream_) != nullptr;
}

} // namespace hokuto::song
