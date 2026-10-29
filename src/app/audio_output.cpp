// SPDX-License-Identifier: MIT

#include "app/audio_output.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>

namespace hokuto::app
{
namespace
{

// The ring's frames are two floats: the left sample, then the right.
constexpr int kFrameBytes = static_cast<int>(2 * sizeof(float));

// The most frames that Feed copies at once.
constexpr std::size_t kBlockFrames = 4096;

// The silence that a new device and a change of the source's rate put before the frames. SDL3's resampler looks a few
// frames ahead (under 8 at Hokuto's rates).
constexpr std::size_t kLookAheadFrames = 64;
constexpr std::array<float, 2 * kLookAheadFrames> kSilence{};

// The playback device called `name`, or 0 if SDL3 doesn't list one.
SDL_AudioDeviceID FindDevice(const std::string& name)
{
    int count = 0;
    SDL_AudioDeviceID* devices = SDL_GetAudioPlaybackDevices(&count);
    SDL_AudioDeviceID found = 0;
    for (int i = 0; devices != nullptr && i < count; i++)
    {
        const char* device_name = SDL_GetAudioDeviceName(devices[i]);
        if (device_name != nullptr && name == device_name)
        {
            found = devices[i];
            break;
        }
    }
    SDL_free(devices);

    return found;
}

// A stream on `device` that takes frames of `spec` and opens the device at the spec's rate, with a buffer of
// `buffer_frames` if it's above 0. SDL3 takes the buffer size from a hint. The hint goes back as it was. Returns null
// if the device didn't open.
SDL_AudioStream* OpenStream(SDL_AudioDeviceID device, const SDL_AudioSpec& spec, int buffer_frames,
                            SDL_AudioStreamCallback feed, void* user_data)
{
    if (buffer_frames <= 0)
    {
        return SDL_OpenAudioDeviceStream(device, &spec, feed, user_data);
    }

    std::optional<std::string> hint;
    if (const char* given = SDL_GetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES))
    {
        hint = given;
    }
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, std::to_string(buffer_frames).c_str());
    SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(device, &spec, feed, user_data);
    if (hint)
    {
        SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, hint->c_str());
    }
    else
    {
        SDL_ResetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES);
    }

    return stream;
}

} // namespace

AudioOutput::AudioOutput(audio::FrameRing& ring, audio::FrameRing& auditions, int source_rate)
    : ring_(ring),
      auditions_(auditions),
      source_rate_(source_rate),
      block_(kBlockFrames * 2),
      audition_block_(kBlockFrames * 2)
{
}

AudioOutput::~AudioOutput()
{
    Close();
}

std::vector<std::string> AudioOutput::DeviceNames()
{
    std::vector<std::string> names;
    int count = 0;
    SDL_AudioDeviceID* devices = SDL_GetAudioPlaybackDevices(&count);
    for (int i = 0; devices != nullptr && i < count; i++)
    {
        if (const char* name = SDL_GetAudioDeviceName(devices[i]))
        {
            names.emplace_back(name);
        }
    }
    SDL_free(devices);

    return names;
}

bool AudioOutput::Open(const std::string& name)
{
    Close();
    chosen_ = name;

    // A chosen device that isn't there falls back on the default device.
    const SDL_AudioDeviceID chosen = name.empty() ? 0 : FindDevice(name);
    const SDL_AudioDeviceID device = chosen != 0 ? chosen : SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;

    // After the first opening, the device opens at the first's rate and buffer size.
    const SDL_AudioSpec asked = {
        .format = SDL_AUDIO_F32, .channels = 2, .freq = first_rate_ > 0 ? first_rate_ : source_rate_};
    stream_ = OpenStream(device, asked, first_buffer_frames_, &AudioOutput::Feed, this);
    if (stream_ == nullptr)
    {
        error_ = SDL_GetError();
        open_failed_ = true;
        return false;
    }

    // The stream takes the rings' frames at their rate. SDL3 opens the device paused, and Feed doesn't run yet.
    if (asked.freq != source_rate_)
    {
        const SDL_AudioSpec source = {.format = SDL_AUDIO_F32, .channels = 2, .freq = source_rate_};
        SDL_SetAudioStreamFormat(stream_, &source, nullptr);
    }

    // The format that SDL3 and the system settled on.
    SDL_AudioSpec spec{};
    if (!SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream_), &spec, &device_buffer_frames_))
    {
        spec.freq = 0;
        device_buffer_frames_ = 0;
    }
    device_rate_ = spec.freq;
    opened_ = chosen != 0 ? name : std::string();
    error_.clear();
    if (first_rate_ == 0)
    {
        first_rate_ = device_rate_;
        first_buffer_frames_ = device_buffer_frames_;
    }

    // SDL3 opens the device paused. A new stream's first request can come back a few frames short. A change of the
    // source's rate does the same (SetSourceRate). Silence covers it.
    SDL_PutAudioStreamData(stream_, kSilence.data(), static_cast<int>(sizeof(kSilence)));
    SDL_ResumeAudioStreamDevice(stream_);

    return true;
}

void AudioOutput::Close()
{
    // SDL_DestroyAudioStream closes the device too. Once it returns, Feed doesn't run again.
    if (stream_ != nullptr)
    {
        SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
    }
    opened_.clear();
    device_rate_ = 0;
    device_buffer_frames_ = 0;
    suspended_ = false;
    open_failed_ = false;
}

bool AudioOutput::IsOpen() const
{
    return stream_ != nullptr || suspended_;
}

void AudioOutput::Suspend()
{
    // Once SDL_DestroyAudioStream returns, Feed doesn't run again. Closing the stream's device closes the physical
    // device too: SDL3 keeps a physical device open while a logical device on it is open, even a paused one.
    if (stream_ == nullptr)
    {
        return;
    }

    SDL_DestroyAudioStream(stream_);
    stream_ = nullptr;
    suspended_ = true;
}

bool AudioOutput::Resume()
{
    if (!suspended_)
    {
        return stream_ != nullptr;
    }

    return Open(chosen_);
}

bool AudioOutput::Suspended() const
{
    return suspended_;
}

void AudioOutput::DevicesChanged()
{
    // After an opening that failed, no stream follows the system's changes. The change may have brought a device.
    if (open_failed_)
    {
        Open(chosen_);
        return;
    }

    // On the default device, SDL3 follows the system's changes. Resume opens the device that's there then.
    if (chosen_.empty() || suspended_)
    {
        return;
    }

    // Reopen when the chosen device is back, or when the device that plays is gone.
    const bool chosen_listed = FindDevice(chosen_) != 0;
    const bool chosen_plays = stream_ != nullptr && !opened_.empty();
    if (chosen_listed != chosen_plays)
    {
        Open(chosen_);
    }
}

void AudioOutput::SetSourceRate(int rate)
{
    if (rate == source_rate_)
    {
        return;
    }

    source_rate_ = rate;
    if (stream_ != nullptr)
    {
        // Where SDL3's resampler starts after the change, the device's next request comes back a few frames short. The
        // device then plays zeros in their place: a click in the first notes. Silence at the new rate covers the
        // resampler's look-ahead. The stream's lock keeps the device's thread from asking for frames between the change
        // and the silence.
        const SDL_AudioSpec source = {.format = SDL_AUDIO_F32, .channels = 2, .freq = rate};
        SDL_LockAudioStream(stream_);
        SDL_SetAudioStreamFormat(stream_, &source, nullptr);
        SDL_PutAudioStreamData(stream_, kSilence.data(), static_cast<int>(sizeof(kSilence)));
        SDL_UnlockAudioStream(stream_);
    }
}

const std::string& AudioOutput::Chosen() const
{
    return chosen_;
}

int AudioOutput::DeviceRate() const
{
    return device_rate_;
}

int AudioOutput::DeviceBufferFrames() const
{
    return device_buffer_frames_;
}

const std::string& AudioOutput::Error() const
{
    return error_;
}

void AudioOutput::SetGain(float gain)
{
    gain_.store(gain, std::memory_order_relaxed);
}

std::array<float, 2> AudioOutput::TakePeaks()
{
    return {peaks_[0].exchange(0.0f, std::memory_order_relaxed), peaks_[1].exchange(0.0f, std::memory_order_relaxed)};
}

void SDLCALL AudioOutput::Feed(void* user_data, SDL_AudioStream* stream, int additional, int /*total*/)
{
    AudioOutput& output = *static_cast<AudioOutput*>(user_data);

    // `additional` counts bytes of the rings' format. Round it up to whole frames.
    auto frames = static_cast<std::size_t>((std::max(additional, 0) + kFrameBytes - 1) / kFrameBytes);
    const float gain = output.gain_.load(std::memory_order_relaxed);
    std::array<float, 2> peaks = {0.0f, 0.0f};

    // Each ring's frames go into its block. Where the ring runs out, the block is silent.
    const auto take =
        [](audio::FrameRing& ring, std::vector<float>& block, audio::Declicker& declicker, std::size_t count)
    {
        const std::size_t read = ring.Read(block.data(), count);
        std::fill_n(block.data() + read * 2, (count - read) * 2, 0.0f);
        declicker.Process(block.data(), count, read, ring.Skipped());
    };

    while (frames > 0)
    {
        const std::size_t count = std::min(frames, kBlockFrames);
        take(output.ring_, output.block_, output.declicker_, count);
        take(output.auditions_, output.audition_block_, output.audition_declicker_, count);

        // The auditions over the ring's frames, the gain, and the peaks for the output's meter.
        for (std::size_t i = 0; i < count * 2; i++)
        {
            float& sample = output.block_[i];
            sample = (sample + output.audition_block_[i]) * gain;
            peaks[i % 2] = std::max(peaks[i % 2], std::abs(sample));
        }

        SDL_PutAudioStreamData(stream, output.block_.data(), static_cast<int>(count) * kFrameBytes);
        frames -= count;
    }

    // A peak that the meter hasn't taken yet stays unless this one is higher. A race with TakePeaks can only lose a
    // peak. The meter falls slowly and hides the loss.
    for (std::size_t channel = 0; channel < 2; channel++)
    {
        if (peaks[channel] > output.peaks_[channel].load(std::memory_order_relaxed))
        {
            output.peaks_[channel].store(peaks[channel], std::memory_order_relaxed);
        }
    }
}

} // namespace hokuto::app
