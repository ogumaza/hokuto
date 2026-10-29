// SPDX-License-Identifier: MIT

#include "song/player.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <optional>
#include <span>
#include <utility>

namespace hokuto::song
{
namespace
{

// The scale of a meter's peak in 16-bit samples: full scale becomes 1.
constexpr float kSampleScale = 1.0f / 32768.0f;

// How far float `i` of a frame of `floats` floats lies through the frame, from 0 to just under 1. The fades follow it
// in a straight line.
float Through(std::size_t i, std::size_t floats)
{
    return static_cast<float>(i / 2) / static_cast<float>(floats / 2);
}

// The position in the arrangement at the model's position `ticks`. A looped sequence goes round its loop, and its
// position is a whole tick count. Past the end of a sequence without a loop, the position goes on.
double ArrangementPosition(const Sequence& sequence, double ticks)
{
    if (sequence.loop)
    {
        return PositionOfTicks(sequence, static_cast<uint64_t>(ticks));
    }

    return ticks;
}

} // namespace

PlaybackClock::PlaybackClock(uint64_t frame_samples) : frame_samples_(std::max<uint64_t>(frame_samples, 1))
{
    SetBusVolumes(kFullVolumes);
}

void PlaybackClock::Mark(uint64_t frame, double ticks, const Peaks& peaks, const Variables& variables)
{
    marks_[frame % kMarks].store(ticks, std::memory_order_relaxed);
    std::array<std::atomic<float>, kMeters * 2>& kept = peaks_[frame % kMarks];
    for (std::size_t i = 0; i < kept.size(); i++)
    {
        const std::size_t meter = i / 2;
        const float peak = meter < SequenceModel::kTracks ? peaks.tracks[meter][i % 2]
                                                          : peaks.buses[meter - SequenceModel::kTracks][i % 2];
        kept[i].store(peak, std::memory_order_relaxed);
    }

    std::array<std::atomic<int16_t>, kGameVariables>& values = variables_[frame % kMarks];
    for (std::size_t i = 0; i < values.size(); i++)
    {
        values[i].store(variables[i], std::memory_order_relaxed);
    }

    marked_.store(frame + 1, std::memory_order_release);
}

void PlaybackClock::SetWritten(uint64_t samples)
{
    written_.store(samples, std::memory_order_release);
}

void PlaybackClock::SetEnded()
{
    ended_.store(true, std::memory_order_release);
}

uint64_t PlaybackClock::Written() const
{
    return written_.load(std::memory_order_acquire);
}

bool PlaybackClock::Ended() const
{
    return ended_.load(std::memory_order_acquire);
}

std::optional<double> PlaybackClock::PositionAt(uint64_t sample) const
{
    const uint64_t marked = marked_.load(std::memory_order_acquire);
    const uint64_t frame = sample / frame_samples_;
    if (sample >= Written() || frame >= marked)
    {
        return std::nullopt;
    }

    // The position moves on from this frame's mark to the next one's. Across the end of the loop, it stays at this
    // frame's mark.
    const double here = marks_[frame % kMarks].load(std::memory_order_relaxed);
    if (frame + 1 >= marked)
    {
        return here;
    }

    const double next = marks_[(frame + 1) % kMarks].load(std::memory_order_relaxed);
    if (next < here)
    {
        return here;
    }

    const double part = static_cast<double>(sample % frame_samples_) / static_cast<double>(frame_samples_);
    return here + (next - here) * part;
}

PlaybackClock::Peaks PlaybackClock::PeaksBetween(uint64_t from, uint64_t to) const
{
    Peaks peaks;
    if (to <= from)
    {
        return peaks;
    }

    // The player marks the next frame in the place of the oldest one in the ring. The frames after that one are safe to
    // read.
    const uint64_t marked = marked_.load(std::memory_order_acquire);
    const uint64_t oldest = marked >= kMarks ? marked - kMarks + 1 : 0;
    const uint64_t last = std::min((to - 1) / frame_samples_ + 1, marked);
    for (uint64_t frame = std::max(from / frame_samples_, oldest); frame < last; frame++)
    {
        const std::array<std::atomic<float>, kMeters * 2>& kept = peaks_[frame % kMarks];
        for (std::size_t i = 0; i < kept.size(); i++)
        {
            const std::size_t meter = i / 2;
            float& peak = meter < SequenceModel::kTracks ? peaks.tracks[meter][i % 2]
                                                         : peaks.buses[meter - SequenceModel::kTracks][i % 2];
            peak = std::max(peak, kept[i].load(std::memory_order_relaxed));
        }
    }

    return peaks;
}

std::optional<PlaybackClock::Variables> PlaybackClock::VariablesAt(uint64_t sample) const
{
    // As in PeaksBetween, the frames after the oldest one in the ring are safe to read.
    const uint64_t marked = marked_.load(std::memory_order_acquire);
    const uint64_t oldest = marked >= kMarks ? marked - kMarks + 1 : 0;
    const uint64_t frame = sample / frame_samples_;
    if (frame >= marked || frame < oldest)
    {
        return std::nullopt;
    }

    Variables variables{};
    const std::array<std::atomic<int16_t>, kGameVariables>& kept = variables_[frame % kMarks];
    for (std::size_t i = 0; i < variables.size(); i++)
    {
        variables[i] = kept[i].load(std::memory_order_relaxed);
    }

    return variables;
}

void PlaybackClock::SetCycle(const CycleRange& cycle)
{
    locators_.store(static_cast<uint64_t>(cycle.start) << 32 | cycle.end, std::memory_order_relaxed);
    cycle_on_.store(cycle.on, std::memory_order_relaxed);
}

PlaybackClock::CycleRange PlaybackClock::Cycle() const
{
    const uint64_t locators = locators_.load(std::memory_order_relaxed);
    return {cycle_on_.load(std::memory_order_relaxed), static_cast<uint32_t>(locators >> 32),
            static_cast<uint32_t>(locators)};
}

void PlaybackClock::SetMutes(uint32_t tracks)
{
    mutes_.store(tracks, std::memory_order_relaxed);
}

uint32_t PlaybackClock::Mutes() const
{
    return mutes_.load(std::memory_order_relaxed);
}

void PlaybackClock::SetBusVolumes(const AuxVolumes& volumes)
{
    for (std::size_t bus = 0; bus < kAuxBuses; bus++)
    {
        bus_volumes_[bus].store(volumes[bus], std::memory_order_relaxed);
    }
}

AuxVolumes PlaybackClock::BusVolumes() const
{
    AuxVolumes volumes{};
    for (std::size_t bus = 0; bus < kAuxBuses; bus++)
    {
        volumes[bus] = bus_volumes_[bus].load(std::memory_order_relaxed);
    }

    return volumes;
}

void PlaybackClock::SetModelFrames(uint64_t frames)
{
    model_frames_.store(frames, std::memory_order_relaxed);
}

uint64_t PlaybackClock::ModelFrames() const
{
    return model_frames_.load(std::memory_order_relaxed);
}

void PlaybackClock::SetModelTicks(double ticks)
{
    model_ticks_.store(ticks, std::memory_order_relaxed);
}

double PlaybackClock::ModelTicks() const
{
    return model_ticks_.load(std::memory_order_relaxed);
}

void PlaybackClock::Offer(std::shared_ptr<SequenceModel> model, bool by_ticks)
{
    // A model that the player hasn't taken goes after the lock is released.
    Offered replaced;
    {
        std::lock_guard lock(offer_mutex_);
        replaced = std::exchange(offer_, Offered{std::move(model), by_ticks});
        offered_.store(offer_.model != nullptr, std::memory_order_release);
    }
}

PlaybackClock::Offered PlaybackClock::TakeOffer()
{
    if (!offered_.load(std::memory_order_acquire))
    {
        return {};
    }

    std::lock_guard lock(offer_mutex_);
    offered_.store(false, std::memory_order_relaxed);

    return std::exchange(offer_, Offered{});
}

SequencePlayer::SequencePlayer(std::shared_ptr<SequenceModel> model, uint32_t start,
                               std::shared_ptr<PlaybackClock> clock)
    : model_(std::move(model)),
      clock_(std::move(clock)),
      sample_rate_(model_->SampleRate()),
      start_(start),
      frame_(model_->FrameSamples() * 2),
      fading_(frame_.size()),
      frame_read_(frame_.size())
{
}

int SequencePlayer::SampleRate() const
{
    return sample_rate_;
}

std::size_t SequencePlayer::Render(float* out, std::size_t frames)
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

    written_ += done;
    clock_->SetWritten(written_);
    if (done < frames)
    {
        clock_->SetEnded();
    }

    return done;
}

SequencePlayer::CycleMode SequencePlayer::ModeOf(const PlaybackClock::CycleRange& cycle) const
{
    const std::optional<Loop>& loop = model_->GetSequence().loop;
    if (!cycle.on)
    {
        return CycleMode::kOff;
    }
    if (loop && cycle.start == loop->start && cycle.end == loop->end)
    {
        return CycleMode::kGameLoop;
    }

    return cycle.start < CycleEnd(cycle) ? CycleMode::kLocators : CycleMode::kOff;
}

uint32_t SequencePlayer::CycleEnd(const PlaybackClock::CycleRange& cycle) const
{
    const std::optional<Loop>& loop = model_->GetSequence().loop;
    return loop ? std::min(cycle.end, loop->end) : cycle.end;
}

bool SequencePlayer::NextFrame()
{
    if (ended_)
    {
        return false;
    }

    // The first frame plays the start. After the end of a pass of the cycle, the next one plays the left locator.
    const PlaybackClock::CycleRange cycle = clock_->Cycle();
    const CycleMode mode = ModeOf(cycle);
    const bool starting = !std::exchange(started_, true);
    const bool jumping = std::exchange(wrap_, false) && mode == CycleMode::kLocators;

    // A model that the program offers takes over. A frame that seeks takes it as it is, and so does silence. Any other
    // frame crossfades from the old model to the new one.
    if (PlaybackClock::Offered next = clock_->TakeOffer(); next.model)
    {
        if (!starting && !jumping && !silent_)
        {
            return Crossfade(std::move(next), cycle, mode);
        }

        model_ = std::move(next.model);
    }

    // The mutes and the buses' volumes that the program sets take effect in this frame.
    model_->SetMutes(clock_->Mutes());
    model_->SetBusVolumes(clock_->BusVolumes());
    if (starting)
    {
        return Begin(start_, start_ > 0, cycle, mode);
    }
    if (jumping)
    {
        return Begin(cycle.start, true, cycle, mode);
    }
    if (silent_)
    {
        return Silence(cycle, mode);
    }

    // A sequence without a loop ends once its last note has faded. Inside the cycle, silence follows up to the right
    // locator.
    if (!model_->RunFrame())
    {
        if (mode == CycleMode::kLocators && model_->After() < CycleEnd(cycle))
        {
            silent_ = true;
            silent_ticks_ = model_->After();
            return Silence(cycle, mode);
        }

        ended_ = true;
        return false;
    }

    Finish(model_->Before(), false, cycle, mode);

    return true;
}

bool SequencePlayer::Begin(uint32_t ticks, bool fade_in, const PlaybackClock::CycleRange& cycle, CycleMode mode)
{
    // The resampler's inputs before a seek belong to another place in the sequence.
    resampler_.reset();
    if (model_->Seek(ticks))
    {
        Finish(ticks, fade_in, cycle, mode);
        return true;
    }

    if (mode == CycleMode::kLocators && ticks < CycleEnd(cycle))
    {
        silent_ = true;
        silent_ticks_ = ticks;
        return Silence(cycle, mode);
    }

    ended_ = true;

    return false;
}

void SequencePlayer::Finish(double mark, bool fade_in, const PlaybackClock::CycleRange& cycle, CycleMode mode)
{
    // Inside the cycle, the pass ends with the frame that reaches the right locator. Outside it, a looped sequence's
    // output ends with the pass of its loop, unless the cycle goes round the loop.
    const Sequence& sequence = model_->GetSequence();
    const double before = model_->Before();
    const double after = model_->After();
    const double end = CycleEnd(cycle);
    bool fade_out = false;
    if (mode == CycleMode::kLocators && before < end)
    {
        fade_out = after >= end;
        wrap_ = fade_out;
    }
    else if (mode != CycleMode::kGameLoop && sequence.loop && sequence.loop->end > sequence.loop->start)
    {
        // The pass that the frame starts in, and its end. A looped sequence's tick count goes on for ever.
        const auto ticks = static_cast<uint64_t>(before);
        const uint64_t loop_end = sequence.loop->end;
        const uint64_t pass = loop_end - sequence.loop->start;
        const uint64_t pass_end = ticks < loop_end ? loop_end : loop_end + ((ticks - loop_end) / pass + 1) * pass;
        fade_out = after >= static_cast<double>(pass_end);
        ended_ = fade_out;
    }

    TakeFrame(ArrangementPosition(sequence, mark), fade_in, fade_out);
}

bool SequencePlayer::Silence(const PlaybackClock::CycleRange& cycle, CycleMode mode)
{
    if (mode != CycleMode::kLocators)
    {
        ended_ = true;
        return false;
    }
    if (silent_ticks_ >= CycleEnd(cycle))
    {
        silent_ = false;
        return Begin(cycle.start, true, cycle, mode);
    }

    // The silence moves on at the tempo where it is.
    TakeSilence(silent_ticks_);
    silent_ticks_ += model_->TicksPerFrame(silent_ticks_);

    return true;
}

bool SequencePlayer::Crossfade(PlaybackClock::Offered next, const PlaybackClock::CycleRange& cycle, CycleMode mode)
{
    // The old model's next frame, faded out. The old model may have ended there.
    const uint64_t frame = model_->Frames();
    const double position = model_->After();
    model_->SetMutes(clock_->Mutes());
    model_->SetBusVolumes(clock_->BusVolumes());
    std::fill(fading_.begin(), fading_.end(), 0.0f);
    const bool faded = model_->RunFrame();
    const std::span<const float> samples = faded ? ModelFrame() : std::span<const float>();
    for (std::size_t i = 0; i < fading_.size() && i < samples.size(); i++)
    {
        fading_[i] = samples[i] * (1.0f - Through(i, fading_.size()));
    }

    // The new model's frame at the same place fades in: the same frame, or the frame that plays the same tick. If the
    // new model's output has ended before that frame, only the old model's fade sounds in it. Silence then follows
    // inside the cycle, and the output ends outside it. Without an old frame either, the output ends at once. The new
    // model's frames go through a resampler of their own when their length differs from the player's.
    model_ = std::move(next.model);
    resampler_.reset();
    model_->SetMutes(clock_->Mutes());
    model_->SetBusVolumes(clock_->BusVolumes());
    if (next.by_ticks ? model_->Seek(static_cast<uint64_t>(position)) : model_->SeekFrame(frame))
    {
        Finish(model_->Before(), true, cycle, mode);
    }
    else if (mode == CycleMode::kLocators && position < CycleEnd(cycle))
    {
        silent_ = true;
        silent_ticks_ = position;
        Silence(cycle, mode);
    }
    else if (faded)
    {
        TakeSilence(ArrangementPosition(model_->GetSequence(), position));
        ended_ = true;
    }
    else
    {
        ended_ = true;
        return false;
    }

    for (std::size_t i = 0; i < frame_.size(); i++)
    {
        frame_[i] += fading_[i];
    }

    return true;
}

std::span<const float> SequencePlayer::ModelFrame()
{
    const std::span<const float> samples = model_->FloatOutput();
    const std::size_t from = model_->FrameSamples();
    const std::size_t to = frame_.size() / 2;
    if (from == to)
    {
        return samples;
    }

    if (!resampler_)
    {
        resampler_.emplace(from, to);
    }
    resampled_.resize(frame_.size());
    resampler_->Process(samples, resampled_);

    return resampled_;
}

void SequencePlayer::TakeFrame(double position, bool fade_in, bool fade_out)
{
    const std::span<const float> samples = ModelFrame();
    for (std::size_t i = 0; i < frame_.size(); i++)
    {
        float sample = i < samples.size() ? samples[i] : 0.0f;
        if (fade_in)
        {
            sample *= Through(i, frame_.size());
        }
        if (fade_out)
        {
            sample *= 1.0f - Through(i, frame_.size());
        }
        frame_[i] = sample;
    }
    frame_read_ = 0;

    // The meters show a frame that fades in or out at its level before the fade.
    const auto scaled = [](const std::array<int32_t, 2>& peak)
    {
        return std::array<float, 2>{static_cast<float>(peak[0]) * kSampleScale,
                                    static_cast<float>(peak[1]) * kSampleScale};
    };

    PlaybackClock::Peaks peaks;
    for (int track = 0; track < SequenceModel::kTracks; track++)
    {
        peaks.tracks[static_cast<std::size_t>(track)] = scaled(model_->TrackPeaks(track));
    }
    for (std::size_t bus = 0; bus < kAuxBuses; bus++)
    {
        peaks.buses[bus] = scaled(model_->BusPeaks(static_cast<int>(bus)));
    }
    clock_->Mark(frames_, position, peaks, model_->Variables());
    clock_->SetModelFrames(model_->Frames());
    clock_->SetModelTicks(model_->After());
    frames_++;
}

void SequencePlayer::TakeSilence(double position)
{
    std::fill(frame_.begin(), frame_.end(), 0.0f);
    frame_read_ = 0;

    clock_->Mark(frames_, position, {}, model_->Variables());
    frames_++;
}

} // namespace hokuto::song
