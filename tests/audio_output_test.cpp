// SPDX-License-Identifier: MIT

// Tests the audio output's rest (src/app/audio_output.h). The output plays through SDL3's disk driver. The driver
// writes the device's frames to a file in place of a device, and no sound reaches a device. Suspend closes the device:
// the file stops growing. The chosen device's rate and buffer size stay. A sine that waits in the ring while the device
// rests plays once Resume opens the device again: after a short silence, whole and unchanged, without a click. Close
// ends the rest. After a rest at the rate of the 3DS or the Wii, and after a new choice of the device, the device opens
// again at the rate and buffer size of its first opening. SDL3 would open it at 44.1 kHz at the rings' rate. An opening
// that fails, at the start or after a rest, leaves no device. A change of the devices opens one once it can.

#include "app/audio_output.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <numbers>
#include <random>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "xsflib/files.h"
#include "xsflib/testing/check.h"

#include "audio/frame_ring.h"

using hokuto::app::AudioOutput;
using hokuto::audio::FrameRing;

namespace
{

constexpr int kRate = 48'000;

// The silence that the output puts before a new device's first frames.
constexpr std::size_t kLookAheadFrames = 64;

// Long enough for the disk driver to write a few buffers, even on a busy machine.
constexpr std::chrono::milliseconds kBuffers{200};

// A sine of 440 Hz for 0.1 s at kRate: 44 whole periods from a sample of 0. The right channel is half as loud.
std::vector<float> Sine()
{
    constexpr std::size_t kFrames = 4'800;
    std::vector<float> frames(kFrames * 2);
    for (std::size_t i = 0; i < kFrames; i++)
    {
        const auto value =
            static_cast<float>(std::sin(2.0 * std::numbers::pi * 440.0 * static_cast<double>(i) / kRate));
        frames[i * 2] = value;
        frames[i * 2 + 1] = value * 0.5f;
    }

    return frames;
}

// The size of the file at `path`, or 0 if there's none.
std::uintmax_t FileSize(const std::filesystem::path& path)
{
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    return error ? 0 : size;
}

// Points SDL3's disk driver at `file` for the next opening of a device.
void WriteTo(const std::filesystem::path& file)
{
    SDL_SetHint(SDL_HINT_AUDIO_DISK_OUTPUT_FILE, xsflib::Utf8(file).c_str());
}

// Suspend closes the device and Resume opens it again. The ring keeps its frames meanwhile, and the device then plays
// them. SDL's disk driver writes the device's output to `path`.
void TestSuspendAndResume(const std::filesystem::path& path)
{
    FrameRing ring(kRate);
    FrameRing auditions(kRate);
    AudioOutput output(ring, auditions, kRate);

    // An open device plays silence while the rings are empty.
    SEQLIB_CHECK(output.Open(""));
    SEQLIB_CHECK(output.IsOpen() && !output.Suspended());
    const int rate = output.DeviceRate();
    const int buffer = output.DeviceBufferFrames();
    SEQLIB_CHECK_EQ(rate, kRate);
    SEQLIB_CHECK(buffer > 0);
    std::this_thread::sleep_for(kBuffers);

    // Suspend closes the device. The output still counts as open, with the same rate and buffer size. A second Suspend
    // changes nothing.
    output.Suspend();
    output.Suspend();
    SEQLIB_CHECK(output.IsOpen() && output.Suspended());
    SEQLIB_CHECK(output.DeviceRate() == rate && output.DeviceBufferFrames() == buffer);
    const std::uintmax_t written = FileSize(path);
    std::this_thread::sleep_for(kBuffers);
    SEQLIB_CHECK(written > 0);
    SEQLIB_CHECK_EQ(FileSize(path), written);

    // The sine waits in the ring while the device rests. Resume opens the device again, and a second Resume changes
    // nothing. The device takes the whole sine.
    const std::vector<float> sine = Sine();
    const std::size_t frames = sine.size() / 2;
    SEQLIB_CHECK_EQ(ring.Write(sine.data(), frames), frames);
    std::this_thread::sleep_for(kBuffers);
    SEQLIB_CHECK_EQ(ring.Available(), frames);
    SEQLIB_CHECK(output.Resume());
    SEQLIB_CHECK(output.Resume());
    SEQLIB_CHECK(output.IsOpen() && !output.Suspended());
    const auto start = std::chrono::steady_clock::now();
    while (ring.Available() > 0 && std::chrono::steady_clock::now() - start < std::chrono::seconds(5))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    SEQLIB_CHECK_EQ(ring.Available(), std::size_t{0});
    std::this_thread::sleep_for(kBuffers);

    // Close ends the rest too. Resume then opens nothing.
    output.Close();
    SEQLIB_CHECK(!output.IsOpen() && !output.Suspended());
    SEQLIB_CHECK(!output.Resume());

    // The file of the device that Resume opened: the silence, then the sine sample for sample, then silence after a
    // fade from the sine's last samples. The sine starts at 0, and the declicker leaves its start unchanged.
    std::vector<uint8_t> bytes;
    SEQLIB_CHECK(xsflib::ReadFile(path, bytes));
    std::vector<float> out(bytes.size() / sizeof(float));
    std::memcpy(out.data(), bytes.data(), out.size() * sizeof(float));
    SEQLIB_CHECK(out.size() >= (kLookAheadFrames + frames) * 2);
    if (out.size() < (kLookAheadFrames + frames) * 2)
    {
        return;
    }
    std::size_t silent = 0;
    while (silent < kLookAheadFrames * 2 && out[silent] == 0.0f)
    {
        silent++;
    }
    SEQLIB_CHECK_EQ(silent, kLookAheadFrames * 2);
    std::size_t same = 0;
    while (same < sine.size() && out[kLookAheadFrames * 2 + same] == sine[same])
    {
        same++;
    }
    SEQLIB_CHECK_EQ(same, sine.size());
    float tail = 0.0f;
    for (std::size_t i = (kLookAheadFrames + frames) * 2; i < out.size(); i++)
    {
        tail = std::max(tail, std::abs(out[i]));
    }
    SEQLIB_CHECK(tail <= std::abs(sine[sine.size() - 2]));
    SEQLIB_CHECK(out.back() == 0.0f);
}

// A rest and a new opening of the device keep its rate and buffer size, after a change of the source's rate.
void TestRestAtOtherRates()
{
    FrameRing ring(kRate);
    FrameRing auditions(kRate);
    AudioOutput output(ring, auditions, kRate);
    SEQLIB_CHECK(output.Open(""));
    const int buffer = output.DeviceBufferFrames();
    SEQLIB_CHECK_EQ(output.DeviceRate(), kRate);

    // The 3DS's rate and the Wii's, each after a rest. A new opening of the device follows at the Wii's rate.
    for (const int rate : {32'728, 32'000})
    {
        output.SetSourceRate(rate);
        output.Suspend();
        SEQLIB_CHECK(output.Resume());
        SEQLIB_CHECK_EQ(output.DeviceRate(), kRate);
        SEQLIB_CHECK_EQ(output.DeviceBufferFrames(), buffer);
    }
    SEQLIB_CHECK(output.Open(""));
    SEQLIB_CHECK_EQ(output.DeviceRate(), kRate);
    SEQLIB_CHECK_EQ(output.DeviceBufferFrames(), buffer);
    output.Close();
}

// An opening that fails leaves no device. Each change of the devices then retries the opening. The device opens once it
// can. The disk driver fails to open while the folder of its file is missing. SDL3 reads the file's name at each
// opening.
void TestOpenAfterFailure(const std::filesystem::path& path)
{
    const std::filesystem::path missing = path.parent_path() / (path.stem().string() + "_missing") / "output.raw";
    FrameRing ring(kRate);
    FrameRing auditions(kRate);
    AudioOutput output(ring, auditions, kRate);

    // No device opens at the start. A change while the device still fails leaves it closed. The next change opens the
    // default device at the rings' rate.
    WriteTo(missing);
    SEQLIB_CHECK(!output.Open(""));
    SEQLIB_CHECK(!output.IsOpen() && !output.Error().empty());
    output.DevicesChanged();
    SEQLIB_CHECK(!output.IsOpen());
    WriteTo(path);
    output.DevicesChanged();
    SEQLIB_CHECK(output.IsOpen() && !output.Suspended());
    SEQLIB_CHECK_EQ(output.DeviceRate(), kRate);
    SEQLIB_CHECK(output.Error().empty());

    // A reopening after a rest that fails ends the same way.
    output.Suspend();
    WriteTo(missing);
    SEQLIB_CHECK(!output.Resume());
    SEQLIB_CHECK(!output.IsOpen());
    WriteTo(path);
    output.DevicesChanged();
    SEQLIB_CHECK(output.IsOpen() && !output.Suspended());

    // A chosen device that isn't there gives the default device. The output keeps the choice.
    WriteTo(missing);
    SEQLIB_CHECK(!output.Open("No such device"));
    WriteTo(path);
    output.DevicesChanged();
    SEQLIB_CHECK(output.IsOpen());
    SEQLIB_CHECK(output.Chosen() == "No such device");

    // After Close, a change opens nothing.
    output.Close();
    output.DevicesChanged();
    SEQLIB_CHECK(!output.IsOpen());
}

} // namespace

int main()
{
    // SDL3's disk driver writes the device's frames to a file in the system's temporary folder. The test sets the
    // driver before SDL3 starts, and no other driver opens.
    const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                       ("hokuto_audio_output_test_" + std::to_string(std::random_device{}()) + ".raw");

    // The disk driver announces itself in SDL3's log as a critical message. The test's output leaves the log out.
    SDL_SetLogOutputFunction([](void*, int, SDL_LogPriority, const char*) {}, nullptr);
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "disk");
    WriteTo(path);
    SEQLIB_CHECK(SDL_Init(SDL_INIT_AUDIO));
    TestSuspendAndResume(path);
    TestRestAtOtherRates();
    TestOpenAfterFailure(path);
    SDL_Quit();
    std::error_code error;
    std::filesystem::remove(path, error);

    return xsflib::testing::Summary("audio_output");
}
