// SPDX-License-Identifier: MIT

#include "song/sequence_model.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <iterator>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/cafe/snd/engine.h"
#include "ntdlib/nx/atk/engine.h"
#include "ntdlib/sequence_export/consoles.h"

#include "song/console_model.h"
#include "song/ctr_model.h"
#include "song/driver.h"
#include "song/ds.h"
#include "song/dse.h"
#include "song/gba.h"
#include "song/pcm.h"

namespace hokuto::song
{
namespace
{

using console_model::CopyScaled;
using console_model::LeadFramesOf;

// A change's frame (ChangeFrame) counts the model's frames.
static_assert(CtrModel::kFrameSamples * 8192 * 1000 * FrameMsOf(ntdlib::sar::Flavor::kCtr).denominator ==
              FrameMsOf(ntdlib::sar::Flavor::kCtr).numerator * uint64_t{268111856});

// The 3DS's frames last as long in both modes. The enhanced rate is the DSP's rate times 235 / 160, rounded.
static_assert((uint64_t{268111856} * CtrModel::kEnhancedFrameSamples + 8192 * CtrModel::kFrameSamples / 2) /
                  (8192 * CtrModel::kFrameSamples) ==
              CtrModel::kEnhancedSampleRate);

// The stereo frames in each of the console's frames in `mixing`. Throws std::invalid_argument for a console that Hokuto
// doesn't play.
std::size_t FrameSamplesOf(ntdlib::sar::Flavor flavor, Mixing mixing)
{
    switch (flavor)
    {
    case ntdlib::sar::Flavor::kNx:
        return ntdlib::nx::atk::ArchiveModel::kFrameSamples;
    case ntdlib::sar::Flavor::kCafe:
        return ntdlib::cafe::snd::ArchiveModel::kFrameSamples;
    case ntdlib::sar::Flavor::kCtr:
        return mixing == Mixing::kEnhanced ? CtrModel::kEnhancedFrameSamples : CtrModel::kFrameSamples;
    default:
        throw std::invalid_argument("Hokuto plays only the sound archives of the Switch, the Wii U and the 3DS.");
    }
}

// The sample rate of the model of the console of `flavor` in `mixing`.
int SampleRateOf(ntdlib::sar::Flavor flavor, Mixing mixing)
{
    switch (flavor)
    {
    case ntdlib::sar::Flavor::kCafe:
        return ntdlib::cafe::snd::ArchiveModel::kSampleRate;
    case ntdlib::sar::Flavor::kCtr:
        return mixing == Mixing::kEnhanced ? CtrModel::kEnhancedSampleRate : CtrModel::kSampleRate;
    default:
        return ntdlib::nx::atk::ArchiveModel::kSampleRate;
    }
}

} // namespace

namespace console_model
{

std::array<int16_t, kGameVariables> GameVariables(const std::array<int16_t, 16>& local,
                                                  const std::array<int16_t, 16>& global)
{
    std::array<int16_t, kGameVariables> values{};
    values.fill(-1);
    std::copy(local.begin(), local.end(), values.begin());
    std::copy(global.begin(), global.end(), values.begin() + kFirstGlobal);

    return values;
}

bool QuietWithin(std::span<const int16_t> output, int level)
{
    return std::all_of(output.begin(), output.end(), [level](int16_t sample) { return std::abs(sample) <= level; });
}

// Copies `floats` to `out`, or else `samples` with the scale of a 16-bit sample, and zeros after them.
void CopyScaled(std::span<const float> floats, std::span<const int16_t> samples, std::span<float> out)
{
    if (!floats.empty())
    {
        const std::size_t count = std::min(floats.size(), out.size());
        std::copy_n(floats.begin(), count, out.begin());
        std::fill(out.begin() + static_cast<std::ptrdiff_t>(count), out.end(), 0.0f);
        return;
    }

    for (std::size_t i = 0; i < out.size(); i++)
    {
        out[i] = i < samples.size() ? static_cast<float>(samples[i]) * kSampleScale : 0.0f;
    }
}

FrameFacts FrameFactsOf(const Played& played, Mixing mixing)
{
    const bool enhanced = mixing == Mixing::kEnhanced;
    switch (played.Family())
    {
    case format::Family::kNintendoWare:
        {
            const ntdlib::sar::Flavor flavor = played.Sar()->ReadOptions().flavor;
            return {FrameMsOf(flavor), FrameSamplesOf(flavor, mixing), SampleRateOf(flavor, mixing)};
        }
    case format::Family::kNw4r:
        return {kWiiFrameMs, WiiFrameSamples(), WiiSampleRate()};
    case format::Family::kMp2k:
        return {GbaFrameMs(mixing), GbaFrameSamples(*played.Gba(), mixing),
                enhanced ? ntdlib::agb::kEnhancedRate : GbaRate(*played.Gba())};
    case format::Family::kGbaDriver:
        return {GbaFrameMs(mixing), DriverFrameSamples(*played.Driver(), mixing),
                enhanced ? ntdlib::agb::kEnhancedRate : played.Driver()->Rate()};
    case format::Family::kNitroComposer:
    case format::Family::kDse:
        return {kDsFrameMs, DsFrameSamples(mixing), DsSampleRate(mixing)};
    }

    throw std::logic_error("an unknown family of engines");
}

ModelOptions SupportedOptions(const Played& played, ModelOptions options)
{
    const format::Family family = played.Family();
    if (family != format::Family::kNintendoWare && family != format::Family::kNw4r)
    {
        options.effects = {};
    }

    if (family == format::Family::kNw4r)
    {
        options.mixing = Mixing::kStrict;
    }

    if (family != format::Family::kNitroComposer && family != format::Family::kGbaDriver)
    {
        options.dry = false;
    }

    return options;
}

std::unique_ptr<ConsoleModel> NewModel(const Played& played, const Sequence& sequence, const ModelOptions& options)
{
    switch (played.Family())
    {
    case format::Family::kNintendoWare:
        switch (played.Sar()->ReadOptions().flavor)
        {
        case ntdlib::sar::Flavor::kCafe:
            return NewWiiUModel(*played.Sar(), options.effects, options.mixing);
        case ntdlib::sar::Flavor::kCtr:
            return NewCtrModel(*played.Sar(), options.effects, options.mixing);
        default:
            return NewSwitchModel(*played.Sar(), options.effects, options.mixing);
        }
    case format::Family::kNw4r:
        return NewWiiModel(
            *played.Wii(),
            sequence.wii_build.value_or(ntdlib::sequence_export::BuildOfRsarVersion(played.Wii()->Version())),
            options.effects);
    case format::Family::kMp2k:
        return NewGbaModel(*played.Gba(), options.mixing, FrameFactsOf(played, options.mixing).frame_samples);
    case format::Family::kGbaDriver:
        return NewDriverModel(*played.Driver(), options.mixing, FrameFactsOf(played, options.mixing).frame_samples,
                              options.dry);
    case format::Family::kNitroComposer:
        return NewDsModel(*played.Ds(), options.mixing, options.dry);
    case format::Family::kDse:
        return NewDseModel(*played.Dse(), options.mixing);
    }

    throw std::logic_error("an unknown family of engines");
}

} // namespace console_model

void CopyOutput(const ntdlib::nx::atk::ArchiveModel& model, std::span<float> out)
{
    CopyScaled(model.renderer.DeviceOutputFloat(), model.renderer.DeviceOutput(), out);
}

void CopyOutput(const ntdlib::cafe::snd::ArchiveModel& model, std::span<float> out)
{
    CopyScaled(model.system.OutputFloat(), model.system.Output(), out);
}

void CopyOutput(const CtrModel& model, std::span<float> out)
{
    CopyScaled(model.FloatOutput(), model.Output(), out);
}

SequenceModel::SequenceModel(Played played, std::shared_ptr<const Sequence> sequence, ModelOptions options)
    : SequenceModel(played, std::move(sequence), console_model::SupportedOptions(played, options),
                    console_model::FrameFactsOf(played, options.mixing))
{
}

SequenceModel::SequenceModel(Played played, std::shared_ptr<const Sequence> sequence, ModelOptions options,
                             FrameFacts facts)
    : played_(std::move(played)),
      sequence_(std::move(sequence)),
      effects_(std::move(options.effects)),
      mixing_(options.mixing),
      dry_(options.dry),
      frame_ms_(facts.frame_ms),
      frame_samples_(facts.frame_samples),
      sample_rate_(facts.sample_rate),
      lead_(LeadFramesOf(effects_, sample_rate_, frame_samples_)),
      interval_(options.snapshot_interval > 0 ? options.snapshot_interval
                                              : 2 * static_cast<uint64_t>(sample_rate_) / frame_samples_)
{
}

SequenceModel::~SequenceModel() = default;

const Sequence& SequenceModel::GetSequence() const
{
    return *sequence_;
}

int SequenceModel::SampleRate() const
{
    return sample_rate_;
}

std::size_t SequenceModel::FrameSamples() const
{
    return frame_samples_;
}

double SequenceModel::TicksPerFrame(double ticks) const
{
    // The ticks in a minute over the frames in a minute. The 3DS's frames don't last a whole number of milliseconds.
    const double frames_per_minute =
        60000.0 * static_cast<double>(frame_ms_.denominator) / static_cast<double>(frame_ms_.numerator);
    const auto tick = static_cast<uint32_t>(std::clamp(ticks, 0.0, static_cast<double>(sequence_->length)));
    return TempoAt(*sequence_, tick) * sequence_->timebase / frames_per_minute;
}

bool SequenceModel::Seek(uint64_t ticks)
{
    if (!started_ && !Start())
    {
        return false;
    }
    if (!playing_)
    {
        return false;
    }

    // The latest snapshot at or before the target that lies before the lead's start, or has mixed since then, starts
    // the run. The first snapshot, at tick 0, always does. The model runs on from where it is when that's between the
    // snapshot and the target, and it lies before the lead's start or has mixed since then.
    const auto target = static_cast<double>(ticks);
    const double lead_start = LeadStartOfTick(target);
    const auto settles = [lead_start](double at, double mixed_from)
    {
        return at <= lead_start || mixed_from <= lead_start;
    };

    const Saved* from = &saved_.front();
    for (const Saved& saved : saved_)
    {
        if (static_cast<double>(saved.ticks) > target)
        {
            break;
        }

        if (settles(static_cast<double>(saved.ticks), saved.mixed_from_ticks))
        {
            from = &saved;
        }
    }
    if (failed_ || frame_ < from->frame || after_ > target || !settles(after_, mixed_from_ticks_))
    {
        Restore(*from);
    }

    // Close to the target, the run mixes throughout. Further away, it mixes from the lead on.
    const double reach = static_cast<double>(std::max(lead_, interval_)) * TicksPerFrame(target);
    const bool close = target - static_cast<double>(model_->TickCount()) <= reach;
    bool reached = false;
    for (;;)
    {
        mixes_ = close || InLeadOfTick(target);
        if (!Step())
        {
            break;
        }

        if (after_ > target)
        {
            reached = true;
            break;
        }

        if (model_->ClockStopped())
        {
            break;
        }
    }

    mixes_ = true;

    return reached;
}

bool SequenceModel::SeekFrame(uint64_t frame)
{
    if (!started_ && !Start())
    {
        return false;
    }
    if (!playing_)
    {
        return false;
    }

    // The latest snapshot at or before the frame that lies before the lead's start, or has mixed since then, starts the
    // run. The first snapshot always does. The model runs on from where it is when that's between the snapshot and the
    // frame, and it lies before the lead's start or has mixed since then. It stays where it is when its last frame is
    // the one.
    const uint64_t lead_start = LeadStart(frame);
    const auto settles = [lead_start](uint64_t at, uint64_t mixed_from)
    {
        return at <= lead_start || mixed_from <= lead_start;
    };

    const Saved* from = &saved_.front();
    for (const Saved& saved : saved_)
    {
        if (saved.frame > frame)
        {
            break;
        }

        if (settles(saved.frame, saved.mixed_from))
        {
            from = &saved;
        }
    }
    if (failed_ || frame_ < from->frame || frame_ > frame + 1 || !settles(frame_, mixed_from_))
    {
        Restore(*from);
    }

    // Close to the frame, the run mixes throughout. Further away, it mixes from the lead on.
    const bool close = frame + 1 - frame_ <= std::max(lead_, interval_);
    bool reached = true;
    while (frame_ <= frame)
    {
        mixes_ = close || InLead(frame);
        if (!Step())
        {
            reached = false;
            break;
        }
    }

    mixes_ = true;

    return reached;
}

bool SequenceModel::RunFrame()
{
    return playing_ && Step();
}

void SequenceModel::SetMixing(bool mixing)
{
    mixes_ = mixing;
}

uint64_t SequenceModel::LeadFrames() const
{
    return lead_;
}

bool SequenceModel::InLead(uint64_t frame) const
{
    return frame_ >= LeadStart(frame);
}

bool SequenceModel::InLeadOfTick(double ticks) const
{
    // A frame that starts less than two frames' ticks before the lead's start mixes. The first frame that mixes then
    // starts at or before the lead's start, unless the tempo more than doubles within a frame.
    const double at = model_ != nullptr ? static_cast<double>(model_->TickCount()) : 0.0;
    return at + 2.0 * TicksPerFrame(at) > LeadStartOfTick(ticks);
}

void SequenceModel::SetMutes(uint32_t tracks)
{
    if (tracks == mutes_)
    {
        return;
    }

    mutes_ = tracks;
    if (playing_)
    {
        ApplyMutes();
    }
}

void SequenceModel::SetBusVolumes(const AuxVolumes& volumes)
{
    if (volumes == bus_volumes_)
    {
        return;
    }

    bus_volumes_ = volumes;
    if (playing_)
    {
        ApplyBusVolumes();
    }
}

std::span<const int16_t> SequenceModel::Output() const
{
    if (!model_)
    {
        return {};
    }

    return model_->Output();
}

std::span<const float> SequenceModel::FloatOutput()
{
    if (!model_)
    {
        return {};
    }

    float_output_.resize(frame_samples_ * 2);
    model_->CopyOutput(float_output_);

    return float_output_;
}

double SequenceModel::Before() const
{
    return before_;
}

double SequenceModel::After() const
{
    return after_;
}

bool SequenceModel::ClockStopped() const
{
    return playing_ && model_->ClockStopped();
}

uint64_t SequenceModel::Frames() const
{
    return frame_;
}

std::array<int32_t, 2> SequenceModel::TrackPeaks(int track) const
{
    if (!model_)
    {
        return {0, 0};
    }

    return model_->TrackPeaks(track);
}

std::array<int32_t, 2> SequenceModel::BusPeaks(int bus) const
{
    if (!model_ || bus < 0 || bus >= static_cast<int>(kAuxBuses))
    {
        return {0, 0};
    }

    return model_->BusPeaks(bus);
}

std::size_t SequenceModel::SnapshotCount() const
{
    return saved_.size();
}

std::array<int16_t, kGameVariables> SequenceModel::Variables() const
{
    if (playing_)
    {
        return model_->Variables();
    }

    std::array<int16_t, kGameVariables> values{};
    values.fill(-1);
    for (const auto& [number, value] : sequence_->start_values)
    {
        if (number >= 0 && number < kGameVariables)
        {
            values[static_cast<std::size_t>(number)] = value;
        }
    }

    return values;
}

bool SequenceModel::Start()
{
    // The effects go on their buses before the sound starts. A game sets up its effects the same way. Damaged data in
    // the archive throws.
    started_ = true;
    try
    {
        model_ = console_model::NewModel(played_, *sequence_,
                                         ModelOptions{.effects = effects_, .mixing = mixing_, .dry = dry_});
        playing_ = model_->StartSequence(sequence_->sound);
        if (playing_)
        {
            model_->SetArrangement(*sequence_);
        }
    }
    catch (const std::exception& error)
    {
        playing_ = false;
        failure_ = error.what();
    }
    if (!playing_)
    {
        return false;
    }

    // The variables take their starting values before the sound's first frame. A game sets them through the sound's
    // handle the same way.
    for (const auto& [number, value] : sequence_->start_values)
    {
        model_->SetVariable(number, value);
    }

    ApplyMutes();
    ApplyBusVolumes();
    model_->MeterTracks();
    model_->MeterBuses();
    saved_.push_back({0, 0, 0, 0.0, model_->Save()});

    return true;
}

bool SequenceModel::Step()
{
    // The effects' tails sound on after the last channel stops. The model runs until a frame comes out silent. With an
    // effect that rings, a frame within ±8 counts as silent (ConsoleModel::Silent).
    if (failed_ || (!model_->IsBusy() && model_->Silent()))
    {
        return false;
    }

    // A snapshot comes before the frame's changes of the variables, and the changes before the frame's ticks. Once the
    // sequence has ended, its tick count stops. The position goes on at the tempo at the end, and the frames mix.
    SaveIfDue();
    ChangeVariables();
    const bool ended = model_->IsFinished();
    const bool mixes = mixes_ || ended;
    before_ = ended ? after_ : static_cast<double>(model_->TickCount());
    model_->SetMixing(mixes);
    try
    {
        model_->RunFrame();
    }
    catch (const std::exception& error)
    {
        failed_ = true;
        failure_ = error.what();
        return false;
    }

    after_ = ended ? before_ + TicksPerFrame(before_) : static_cast<double>(model_->TickCount());
    frame_++;
    if (!mixes)
    {
        mixed_from_ = frame_;
        mixed_from_ticks_ = after_;
    }

    return true;
}

void SequenceModel::SaveIfDue()
{
    // While the sequence plays, a snapshot in each stretch of interval_ frames of the first pass, the first time the
    // model runs it. A settled state takes the place of a snapshot that isn't.
    const uint64_t ticks = model_->TickCount();
    if (model_->IsFinished() || ticks >= sequence_->length)
    {
        return;
    }

    const auto later = std::upper_bound(saved_.begin(), saved_.end(), frame_,
                                        [](uint64_t frame, const Saved& saved) { return frame < saved.frame; });
    Saved& last = *std::prev(later);
    if (last.frame / interval_ == frame_ / interval_)
    {
        if (!Settled(last.frame, last.mixed_from) && Settled(frame_, mixed_from_))
        {
            last = {frame_, ticks, mixed_from_, mixed_from_ticks_, model_->Save()};
        }

        return;
    }

    saved_.insert(later, {frame_, ticks, mixed_from_, mixed_from_ticks_, model_->Save()});

    // Past the limit, the interval doubles, and each new stretch keeps its first snapshot.
    while (saved_.size() > kMaxSnapshots)
    {
        interval_ *= 2;
        std::vector<Saved> kept;
        for (Saved& saved : saved_)
        {
            if (kept.empty() || kept.back().frame / interval_ != saved.frame / interval_)
            {
                kept.push_back(std::move(saved));
            }
        }

        saved_ = std::move(kept);
    }
}

void SequenceModel::ChangeVariables()
{
    for (const VariableChange& change : sequence_->changes)
    {
        if (ChangeFrame(change.time, frame_ms_) == frame_)
        {
            model_->SetVariable(change.variable, change.value);
        }
    }
}

void SequenceModel::Restore(const Saved& saved)
{
    // A snapshot holds the volumes of the buses that it was taken with.
    model_->Restore(*saved.snapshot);
    ApplyBusVolumes();
    frame_ = saved.frame;
    mixed_from_ = saved.mixed_from;
    mixed_from_ticks_ = saved.mixed_from_ticks;
    failed_ = false;
    failure_.clear();
}

uint64_t SequenceModel::LeadStart(uint64_t frame) const
{
    return frame + 1 > lead_ ? frame + 1 - lead_ : 0;
}

double SequenceModel::LeadStartOfTick(double ticks) const
{
    return std::max(ticks - static_cast<double>(lead_) * TicksPerFrame(ticks), 0.0);
}

bool SequenceModel::Settled(uint64_t frame, uint64_t mixed_from) const
{
    return mixed_from == 0 || frame - mixed_from >= lead_;
}

void SequenceModel::ApplyMutes()
{
    for (int track = 0; track < kTracks; track++)
    {
        model_->MuteTrack(track, (mutes_ >> track & 1u) != 0);
    }
}

void SequenceModel::ApplyBusVolumes()
{
    for (std::size_t bus = 0; bus < kAuxBuses; bus++)
    {
        model_->SetBusVolume(static_cast<int>(bus), bus_volumes_[bus]);
    }
}

} // namespace hokuto::song
