// SPDX-License-Identifier: MIT

// The audio output: the test tone, the previews and auditions, the rest of the device after a silence, and the meters.

#include <imgui_impl_sdl3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <initializer_list>
#include <utility>

#include "app/app.h"
#include "app/app_internal.h"
#include "app/widgets.h"
#include "audio/test_tone.h"
#include "song/frame_resampler.h"

namespace hokuto::app
{
namespace
{

// After a peak, a meter falls back this many dB a second. Below -80 dB, a meter shows silence.
constexpr float kMeterFall = 24.0f;
constexpr float kMeterFloor = -80.0f;

// The audio device rests after 30 s without sound (App::RestOutput). The timer that wakes the program for the rest
// comes 0.1 s after the rest is due. The times are in SDL3's nanoseconds.
constexpr uint64_t kOutputRest = 30'000'000'000;
constexpr uint64_t kOutputTimerSlack = 100'000'000;

// Moves a stereo meter on by a frame. The meter jumps up to the frame's peaks, amplitudes where 1 is full scale.
// Otherwise it falls back by `fall` dB. `peak` keeps the highest level that the meter reaches.
void FollowPeaks(const std::array<float, 2>& peaks, float fall, std::array<float, 2>& meter, float& peak)
{
    for (std::size_t i = 0; i < peaks.size(); i++)
    {
        const float level = std::max(DecibelsOfGain(peaks[i]), meter[i] - fall);
        meter[i] = level < kMeterFloor ? kSilence : level;
    }
    peak = std::max({peak, meter[0], meter[1]});
}

// The auditions' lead for a device of `rate` that takes `frames` at a time.
std::chrono::milliseconds AuditionLead(int rate, int frames)
{
    if (rate <= 0 || frames <= 0)
    {
        return kLeastAuditionLead;
    }

    const int64_t lead = (2 * static_cast<int64_t>(frames) * 1000 + rate - 1) / rate;
    return std::max(std::chrono::milliseconds(lead), kLeastAuditionLead);
}

} // namespace

void App::PlayTestTone()
{
    PlayAlone(std::make_unique<audio::TestTone>(kDefaultRate));
}

void App::PlayAlone(std::unique_ptr<audio::Renderer> renderer)
{
    // The renderer takes the place of the sequence's playback, of a preview and of an audition.
    if (transport_.Playing())
    {
        transport_.Stop();
    }
    StopPlayback();
    ForgetPreviews();
    audition_worker_.Stop();
    output_.SetSourceRate(renderer->SampleRate());
    output_.Resume();
    worker_.Play(std::move(renderer));
}

void App::PlayReplacePreview(std::unique_ptr<audio::Renderer> renderer)
{
    PlayAlone(std::move(renderer));
    replace_.previewing = true;
}

void App::StopPreview()
{
    // A preview plays on the worker while no playback follows the transport.
    if (!clock_ && worker_.Playing())
    {
        worker_.Stop();
    }
    ForgetPreviews();
}

void App::ForgetPreviews()
{
    browser_state_.playing.reset();
    wave_state_.playing = false;
    replace_.previewing = false;
}

void App::PlayAudition(std::unique_ptr<audio::Renderer> renderer)
{
    // Playback goes on under the audition, at the same rate. A preview or the test tone stops. Its rate can differ. On
    // the 3DS, the two mixing modes differ in rate. An audition in one mode during playback in the other reaches the
    // playback's rate through a resampler.
    const bool playing = transport_.Playing();
    if (!playing && worker_.Playing())
    {
        worker_.Stop();
        ForgetPreviews();
    }
    if (playing)
    {
        renderer = song::AtRate(std::move(renderer), playback_rate_);
    }
    audition_rate_ = renderer->SampleRate();
    if (!playing)
    {
        output_.SetSourceRate(audition_rate_);
    }
    output_.Resume();
    audition_worker_.Play(std::move(renderer));
}

void App::UpdateOutput()
{
    // The Stereo Out fader sets the output's gain. The auditions' lead follows the device.
    output_.SetGain(GainOfDecibels(master_.volume));
    audition_worker_.SetAhead(AuditionLead(output_.DeviceRate(), output_.DeviceBufferFrames()));
    RestOutput();
}

void App::RestOutput()
{
    // Something sounds while a worker renders or a ring holds frames. A start resumes the device before it renders.
    // Anything else that fills a ring resumes it here.
    const uint64_t now = SDL_GetTicksNS();
    const bool sounding =
        worker_.Playing() || audition_worker_.Playing() || ring_.Available() > 0 || audition_ring_.Available() > 0;
    if (sounding)
    {
        output_sounded_ = now;
        output_.Resume();
        return;
    }
    if (output_.Suspended() || !output_.IsOpen())
    {
        return;
    }

    // The rest. Once SDL3 waits for events, the timer's event brings the program back here when the rest is due.
    const uint64_t due = output_sounded_ + kOutputRest;
    if (now >= due)
    {
        output_.Suspend();
    }
    else if (output_timer_ < due)
    {
        output_timer_ = due;
        const auto wake = [](void* event_type, SDL_TimerID, Uint64) -> Uint64
        {
            SDL_Event event{};
            event.type = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(event_type));
            SDL_PushEvent(&event);

            return 0;
        };
        SDL_AddTimerNS(due - now + kOutputTimerSlack, wake, reinterpret_cast<void*>(uintptr_t{wake_event_}));
    }
}

void App::UpdateMeters()
{
    // The output's meter takes the samples that the device has taken since the last frame.
    const float fall = kMeterFall * ImGui::GetIO().DeltaTime;
    FollowPeaks(output_.TakePeaks(), fall, master_.meter, master_.peak);
    if (!song_)
    {
        return;
    }

    // The meters of the tracks and the FX channels take the samples that the device has played since the last frame.
    song::PlaybackClock::Peaks peaks;
    if (clock_)
    {
        const uint64_t heard = HeardSamples();
        peaks = clock_->PeaksBetween(metered_, heard);
        metered_ = std::max(metered_, heard);
    }

    // A track's number gives its meter in the model. The made-up song of --preview has no model.
    for (std::size_t i = 0; i < song_->tracks.size(); i++)
    {
        TrackView& track = song_->tracks[i];
        const bool modelled = sequence_ && i < sequence_->tracks.size();
        std::array<float, 2> track_peaks{};
        const uint32_t played = modelled ? PlayedTracks(sequence_->tracks[i].number) : 0;
        for (std::size_t number = 0; number < peaks.tracks.size(); number++)
        {
            if ((played >> number & 1U) != 0)
            {
                track_peaks[0] = std::max(track_peaks[0], peaks.tracks[number][0]);
                track_peaks[1] = std::max(track_peaks[1], peaks.tracks[number][1]);
            }
        }
        FollowPeaks(track_peaks, fall, track.meter, track.peak);
    }
    for (std::size_t channel = 0; channel < kFxChannels; channel++)
    {
        FollowPeaks(peaks.buses[channel], fall, fx_.meters[channel], fx_.peaks[channel]);
    }
}

} // namespace hokuto::app
