// SPDX-License-Identifier: MIT

// Playback of the shown sequence: the catch-up of a new model, starting and stopping, and what the playback clock
// reads: the position, the mutes and the volumes of the aux buses.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

#include "app/app.h"
#include "app/widgets.h"
#include "format/drivers.h"
#include "format/file.h"
#include "format/kinds.h"
#include "song/driver.h"

namespace hokuto::app
{
namespace
{

// Without a sequence, a frame moves the transport on by the frame's time, up to 0.1 s. The first frame after a wait for
// events comes long after the frame before it.
constexpr float kLongestStep = 0.1f;

// The highest tick that playback can start from or reach. Ticks count in 32 bits.
constexpr double kMostTicks = std::numeric_limits<uint32_t>::max();

// An aux bus's volume, a gain, in the decibels that an FX channel's fader shows, and back. A gain of 0 is silence.
float AuxGainOfDecibels(float level)
{
    return std::min(GainOfDecibels(level), format::kMaxAuxVolume);
}

} // namespace

void App::StartCatchUp()
{
    // A model of another engine runs in ticks, up to the sample that the device plays. While playback is stopped, it
    // runs through the first pass.
    if (PlayedEngine() != model_engine_)
    {
        engine_catch_up_ = PlayedEngine();
        const double behind =
            clock_ && playback_rate_ > 0 ? static_cast<double>(QueuedSamples()) / playback_rate_ : 0.0;
        catch_up_.Start(MakeModel(), clock_, true, behind, model_ != nullptr ? CatchUpScale(true) : 1.0);
        return;
    }

    // A model that plays the ticks at other times than the model that plays, such as after an edit of a tempo, takes
    // over at the player's position in ticks.
    engine_catch_up_.reset();
    const bool by_ticks = model_ != nullptr && !song::SameTiming(model_->GetSequence(), PlayedSequence());
    catch_up_.Start(MakeModel(), clock_, by_ticks, 0.0, model_ != nullptr ? CatchUpScale(false) : 1.0);
}

void App::CancelCatchUp()
{
    catch_up_.Cancel();
    engine_catch_up_.reset();
}

void App::UpdatePlayback()
{
    // Stop while stopped ends a preview: the browser's, a window's Play, an Export clip or the test tone.
    if (transport_.IdleStops() != idle_stops_)
    {
        idle_stops_ = transport_.IdleStops();
        StopPreview();
    }

    if (!sequence_)
    {
        transport_.Advance(std::min(ImGui::GetIO().DeltaTime, kLongestStep));
        return;
    }

    // A new model that has caught up with playback takes over at the player's next frame. One that has run while
    // playback was stopped plays the next playback.
    // A model of another engine takes over by a new playback, from about the sample that the device plays.
    if (catch_up_.Finished())
    {
        model_ = catch_up_.Take();
        if (const std::optional<format::Engine> engine = std::exchange(engine_catch_up_, std::nullopt))
        {
            model_engine_ = *engine;
            if (clock_ && transport_.Playing())
            {
                StartPlayback(ChangeOverTick());
            }
        }
        else if (clock_)
        {
            clock_->Offer(model_, catch_up_.ByTicks());
        }
    }

    // A drag in the ruler moves the position once the mouse is released.
    const bool scrubbing = arrangement_.ruler.MovingPosition();
    if (!transport_.Playing())
    {
        StopPlayback();
    }
    else if (!scrubbing && (!clock_ || transport_.Moves() != playback_moves_))
    {
        StartPlayback();
    }
    if (!clock_)
    {
        return;
    }

    clock_->SetCycle(CycleInTicks());
    clock_->SetMutes(MutedTracks());
    clock_->SetBusVolumes(BusVolumes());
    if (!scrubbing)
    {
        if (const std::optional<double> ticks = clock_->PositionAt(HeardSamples()))
        {
            transport_.Follow(song::ShownTickOf(*sequence_, model_->GetSequence(), *ticks) / sequence_->timebase);
        }
    }

    // Playback stops at the end of the sequence, once the device has taken the last frame.
    if (clock_->Ended() && ring_.Available() == 0)
    {
        transport_.Stop();
        StopPlayback();
        if (model_ && !model_->Failure().empty())
        {
            ShowMessage("Playback stopped. " + format::AsSentence(model_->Failure()));
        }
    }
}

void App::StartPlayback(std::optional<uint32_t> start)
{
    // A new model that's still running stops where it has got to, and this playback plays it.
    if (catch_up_.Pending())
    {
        model_ = catch_up_.Take();
        if (const std::optional<format::Engine> engine = std::exchange(engine_catch_up_, std::nullopt))
        {
            model_engine_ = *engine;
        }
    }

    // Playback starts from the position, even past the end of the sequence, or from `start`.
    const double ticks = start ? *start : std::clamp(PositionInModel(), 0.0, kMostTicks);
    clock_ = std::make_shared<song::PlaybackClock>(model_->FrameSamples());
    clock_->SetCycle(CycleInTicks());
    clock_->SetMutes(MutedTracks());
    clock_->SetBusVolumes(BusVolumes());
    playback_moves_ = transport_.Moves();
    metered_ = 0;
    browser_state_.playing.reset();
    wave_state_.playing = false;
    replace_.previewing = false;
    auto player = std::make_unique<song::SequencePlayer>(model_, static_cast<uint32_t>(ticks), clock_);
    playback_rate_ = player->SampleRate();
    if (audition_rate_ != playback_rate_)
    {
        audition_worker_.Stop();
    }
    output_.SetSourceRate(playback_rate_);
    output_.Resume();
    worker_.Play(std::move(player));
}

uint32_t App::ChangeOverTick() const
{
    // The position that the device plays, and the model's position in the arrangement. A looped sequence's model counts
    // its ticks on past the loop.
    const double heard = std::clamp(PositionInModel(), 0.0, kMostTicks);
    const double after = std::clamp(model_->After(), 0.0, kMostTicks);
    const song::Sequence& played = model_->GetSequence();
    const double at = played.loop ? song::PositionOfTicks(played, static_cast<uint64_t>(after)) : after;

    // Within a quarter of a second, playback goes on from the model's next tick. The model then runs on without a seek.
    // Further away, such as after the cycle went back, playback starts at the position.
    const double frames = 0.25 * model_->SampleRate() / static_cast<double>(model_->FrameSamples());
    const double nearby = frames * model_->TicksPerFrame(after);
    return static_cast<uint32_t>(std::abs(at - heard) <= nearby ? std::ceil(after) : heard);
}

double App::PositionInModel() const
{
    return song::PlayedTickOf(*sequence_, model_->GetSequence(), transport_.Position() * sequence_->timebase);
}

double App::CatchUpScale(bool other_engine) const
{
    // A catch-up on the same engine runs to the same ticks, unless a model plays a conversion for another engine.
    const song::Sequence& from = model_->GetSequence();
    const song::Sequence& to = PlayedSequence();
    if (!other_engine && !from.arrangement_ticks && !to.arrangement_ticks)
    {
        return 1.0;
    }

    // The count of the new model at the ticks of the old one: through the arrangement's ticks, pass by pass.
    const double counted = clock_ ? clock_->ModelTicks() : 0.0;
    if (counted <= 0.0)
    {
        return static_cast<double>(to.timebase) / std::max(from.timebase, 1);
    }

    return song::CountedTicksOn(*sequence_, from, to, counted) / counted;
}

uint64_t App::HeardSamples() const
{
    // The device plays the sample that comes before those in the ring and in the device's buffer.
    const uint64_t written = clock_->Written();
    const uint64_t queued = QueuedSamples();
    return written > queued ? written - queued : 0;
}

uint64_t App::QueuedSamples() const
{
    // The ring's frames are at the player's rate.
    uint64_t queued = ring_.Available();
    if (output_.DeviceRate() > 0)
    {
        queued += static_cast<uint64_t>(output_.DeviceBufferFrames()) * static_cast<uint64_t>(playback_rate_) /
                  static_cast<uint64_t>(output_.DeviceRate());
    }

    return queued;
}

song::PlaybackClock::CycleRange App::CycleInTicks() const
{
    // The transport counts quarter notes. A locator before the start counts as the start. The model that plays counts
    // its ticks: a conversion for another engine maps the arrangement's ticks (song::PlayedTickOf).
    const auto ticks = [this](double quarters)
    {
        const double shown = quarters * sequence_->timebase;
        const double played = model_ != nullptr ? song::PlayedTickOf(*sequence_, model_->GetSequence(), shown) : shown;
        return static_cast<uint32_t>(std::clamp(std::round(played), 0.0, kMostTicks));
    };
    return {transport_.Cycle(), ticks(transport_.CycleStart()), ticks(transport_.CycleEnd())};
}

uint32_t App::PlayedTracks(int number) const
{
    const auto index = static_cast<std::size_t>(number) & 31;
    return engine_tracks_ ? (*engine_tracks_)[index] : 1U << index;
}

uint32_t App::MutedTracks() const
{
    uint32_t tracks = 0;
    if (!song_)
    {
        return tracks;
    }

    for (std::size_t i = 0; i < song_->tracks.size() && i < sequence_->tracks.size(); i++)
    {
        if (!IsHeard(*song_, i))
        {
            tracks |= PlayedTracks(sequence_->tracks[i].number);
        }
    }

    return tracks;
}

song::AuxVolumes App::BusVolumes() const
{
    const format::File* document = Document();
    if (document == nullptr)
    {
        return song::kFullVolumes;
    }

    song::AuxVolumes volumes = document->aux_volumes;
    if (fx_.held)
    {
        volumes[fx_.held->channel] = AuxGainOfDecibels(fx_.volumes[fx_.held->channel]);
    }

    return volumes;
}

void App::FollowBusVolumes()
{
    const format::File* document = Document();
    for (std::size_t channel = 0; document != nullptr && channel < kFxChannels; channel++)
    {
        if (!fx_.held || fx_.held->channel != channel)
        {
            fx_.volumes[channel] = DecibelsOfGain(document->aux_volumes[channel]);
        }
    }
}

void App::ApplyFxMove()
{
    const std::optional<FxMove> move = std::exchange(fx_.moved, std::nullopt);
    const format::File* document = Document();
    if (!move || document == nullptr)
    {
        return;
    }

    // A move that ends where it started changes nothing.
    format::File file = *document;
    file.aux_volumes[move->channel] = AuxGainOfDecibels(move->to);
    if (file.aux_volumes == document->aux_volumes)
    {
        return;
    }

    // The Undo History names the edit after the channel: FX A / Volume.
    const std::string channel = FxName(move->channel);
    Do(channel + " / Volume", "from " + LevelText(move->from) + " dB to " + LevelText(move->to) + " dB",
       std::move(file));
}

void App::StopPlayback()
{
    if (clock_)
    {
        worker_.Stop();
        clock_.reset();
    }
}

} // namespace hokuto::app
