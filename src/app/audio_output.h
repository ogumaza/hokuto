// SPDX-License-Identifier: MIT

// The audio device, through SDL3: WASAPI on Windows and Core Audio on the Mac. SDL3 calls Feed on its audio thread
// whenever the device takes frames. Feed copies them from a FrameRing, and pads with silence when the ring runs out. A
// second ring holds auditioned notes. Its frames sound over the first ring's frames. A Declicker for each ring smooths
// the joins where a stop or a seek discards frames. SDL3 converts the frames from the rings' rate to the device's rate.
//
// The user chooses the device by name. An empty name means the system's default device. Playback then moves to whatever
// device becomes the default, such as headphones when they're plugged in.
//
// An open device keeps the system awake, even while it plays silence. Suspend closes the device after a stretch of
// silence, and Resume opens it again before anything plays.
//
// SDL3 opens a device at the rate that the first opening asks for, and at 44.1 kHz at least. The rings' rate can be the
// 3DS's 32,728 Hz or the Wii's 32 kHz. The device would then open at 44.1 kHz, and the system would resample SDL3's
// output again. Every opening after the first therefore asks for the rate and the buffer size of the first.

#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <atomic>
#include <string>
#include <vector>

#include "audio/declicker.h"
#include "audio/frame_ring.h"

namespace hokuto::app
{

class AudioOutput
{
public:
    // Plays the frames in `ring`, with the frames in `auditions` over them, at first at `source_rate`. The two rings
    // hold frames at the same rate.
    AudioOutput(audio::FrameRing& ring, audio::FrameRing& auditions, int source_rate);

    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    ~AudioOutput();

    // The names of the playback devices that SDL3 lists now.
    static std::vector<std::string> DeviceNames();

    // Opens the device called `name`, or the system's default device for an empty name or a device that isn't there.
    // The first opening asks for the rings' rate. Each later one asks for the rate and buffer size that the first
    // settled on. Returns false if no device opened. Error() then says why. A change of the devices tries again
    // (DevicesChanged). The device's first frames are silence. SetSourceRate puts the same silence first.
    bool Open(const std::string& name);
    void Close();

    // Whether a device is open or suspended.
    bool IsOpen() const;

    // Suspend closes the open device, and keeps the chosen device's name, rate and buffer size. Resume opens the device
    // again: the chosen device, or the system's default device if the chosen one is gone. The frames in the rings wait
    // for it. Resume returns false if no device opened. Error() then says why.
    void Suspend();
    bool Resume();
    bool Suspended() const;

    // Reopens the chosen device after a device was added or removed. The chosen device takes over again once it's back.
    // If it's gone, the system's default device plays. A suspended device stays closed until Resume. After an opening
    // that failed, at the start or in Resume, each change tries to open the chosen device again.
    void DevicesChanged();

    // Sets the rate of the frames in the rings.
    void SetSourceRate(int rate);

    // The chosen device's name, empty for the system's default device.
    const std::string& Chosen() const;

    // The open device's rate and buffer size in frames. SDL3 and the system settled them.
    int DeviceRate() const;
    int DeviceBufferFrames() const;

    const std::string& Error() const;

    // Sets the output's gain as a factor. Feed applies it.
    void SetGain(float gain);

    // The highest absolute sample of each channel since the last call, left then right. The peaks include the gain.
    std::array<float, 2> TakePeaks();

private:
    static void SDLCALL Feed(void* user_data, SDL_AudioStream* stream, int additional, int total);

    audio::FrameRing& ring_;
    audio::FrameRing& auditions_;
    int source_rate_;
    std::string chosen_;
    std::string opened_; // the device that plays: chosen_, or empty for the default device
    SDL_AudioStream* stream_ = nullptr;
    bool suspended_ = false;
    bool open_failed_ = false; // an opening failed, and no Close came after it
    int device_rate_ = 0;
    int device_buffer_frames_ = 0;

    // The rate and buffer size that the first opening settled on, or 0 before it.
    int first_rate_ = 0;
    int first_buffer_frames_ = 0;
    std::string error_;

    // Feed's blocks, allocated here so that Feed never allocates, and a declicker for each ring.
    std::vector<float> block_;
    std::vector<float> audition_block_;
    audio::Declicker declicker_;
    audio::Declicker audition_declicker_;

    // Shared with Feed on SDL3's audio thread.
    std::atomic<float> gain_{1.0f};
    std::array<std::atomic<float>, 2> peaks_{};
};

} // namespace hokuto::app
