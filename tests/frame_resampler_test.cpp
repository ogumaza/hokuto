// SPDX-License-Identifier: MIT

// Tests the resampler of frames (src/song/frame_resampler.h), between the frames of the 3DS's two mixing modes: 235
// stereo samples at about 48,070 Hz, and 160 at about 32,728 Hz. A sine of 1 kHz goes through both ways. Past the
// frames whose kernel reaches back before the first input, each output has to lie on the sine at its place, less the
// delay. The sinc's response is flat there: the outputs lie within 1e-5 of full scale of the sine's. A constant goes
// through at its level. After the last frame, silence follows the inputs out. A renderer at one mode's rate plays at
// the other's through a ResampledRenderer: its frames have to be the resampler's, in blocks of any length, with the
// resampler's delay after the renderer's end.

#include "song/frame_resampler.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "xsflib/testing/check.h"

#include "audio/renderer.h"
#include "song/ctr_model.h"

namespace
{

using hokuto::song::CtrModel;
using hokuto::song::FrameResampler;
using hokuto::song::ResampledRenderer;

constexpr std::size_t kStrictFrame = 160;
constexpr std::size_t kEnhancedFrame = 235;
constexpr double kStrictRate = 268111856.0 / 8192.0;
constexpr double kFrequency = 1000.0;
constexpr double kAmplitude = 0.5;
constexpr std::size_t kFrames = 100;

// The frames before the kernel takes only inputs that have come: its whole width, and one more frame.
std::size_t SettlingFrames(const FrameResampler& resampler)
{
    return 2 * resampler.Delay() / resampler.From() + 2;
}

// The sine at input sample `place`, at `rate` inputs a second.
double Sine(double place, double rate)
{
    return kAmplitude * std::sin(2.0 * std::numbers::pi * kFrequency * place / rate);
}

// The largest distance of the outputs from the sine, from frames of `from` stereo samples to frames of `to`.
double SineError(std::size_t from, std::size_t to)
{
    const double rate = kStrictRate * static_cast<double>(from) / static_cast<double>(kStrictFrame);
    FrameResampler resampler(from, to);
    std::vector<float> in(2 * from);
    std::vector<float> out(2 * to);
    double error = 0.0;
    for (std::size_t frame = 0; frame < kFrames; frame++)
    {
        for (std::size_t i = 0; i < from; i++)
        {
            const auto sample = static_cast<float>(Sine(static_cast<double>(frame * from + i), rate));
            in[2 * i] = sample;
            in[2 * i + 1] = -sample;
        }
        resampler.Process(in, out);
        if (frame < SettlingFrames(resampler))
        {
            continue;
        }

        // Output j lies j * from / to inputs into the frame, in 65536ths of a sample, less the delay.
        for (std::size_t j = 0; j < to; j++)
        {
            const uint64_t place = (static_cast<uint64_t>(j) * from << 16) / to;
            const double at = static_cast<double>(frame * from) + static_cast<double>(place) / 65536.0 -
                              static_cast<double>(resampler.Delay());
            const double wanted = Sine(at, rate);
            error = std::fmax(error, std::fabs(out[2 * j] - wanted));
            error = std::fmax(error, std::fabs(out[2 * j + 1] + wanted));
        }
    }

    return error;
}

// A sine goes from each mode's frames to the other's, and the outputs lie within 1e-5 of the sine.
void TestSines()
{
    for (const auto& [from, to] : {std::pair{kEnhancedFrame, kStrictFrame}, std::pair{kStrictFrame, kEnhancedFrame}})
    {
        const double error = SineError(from, to);
        XSFLIB_CHECK(error < 1e-5);
    }
}

// A constant comes out at its level. The weights of each place sum to 1. Silence follows the last inputs out.
void TestConstant()
{
    FrameResampler resampler(kEnhancedFrame, kStrictFrame);
    const std::vector<float> kInput(2 * kEnhancedFrame, 0.25f);
    std::vector<float> out(2 * kStrictFrame);
    double error = 0.0;
    for (std::size_t frame = 0; frame < 10; frame++)
    {
        resampler.Process(kInput, out);
        for (std::size_t i = 0; frame >= SettlingFrames(resampler) && i < out.size(); i++)
        {
            error = std::fmax(error, std::fabs(out[i] - 0.25));
        }
    }
    XSFLIB_CHECK(error < 1e-6);

    for (std::size_t frame = 0; frame < SettlingFrames(resampler); frame++)
    {
        resampler.Process({}, out);
    }
    bool silent = true;
    for (const float sample : out)
    {
        silent = silent && sample == 0.0f;
    }
    XSFLIB_CHECK(silent);
}

// A sine at `rate` that ends after `length` stereo samples.
class SineRenderer final : public hokuto::audio::Renderer
{
public:
    SineRenderer(int rate, std::size_t length) : rate_(rate), length_(length)
    {
    }

    int SampleRate() const override
    {
        return rate_;
    }

    std::size_t Render(float* out, std::size_t frames) override
    {
        const std::size_t count = std::min(frames, length_ - at_);
        for (std::size_t i = 0; i < count; i++)
        {
            const auto sample = static_cast<float>(Sine(static_cast<double>(at_ + i), rate_));
            out[2 * i] = sample;
            out[2 * i + 1] = -sample;
        }
        at_ += count;

        return count;
    }

private:
    int rate_;
    std::size_t length_;
    std::size_t at_ = 0;
};

// All of a renderer's output, in blocks of `block` stereo samples.
std::vector<float> RenderAll(hokuto::audio::Renderer& renderer, std::size_t block)
{
    std::vector<float> output;
    std::vector<float> samples(2 * block);
    for (std::size_t done = block; done == block;)
    {
        done = renderer.Render(samples.data(), block);
        output.insert(output.end(), samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(2 * done));
    }

    return output;
}

// The resampled renderer gives the resampler's frames of a sine. The last frame is short, and a frame of silence for
// the delay follows.
void TestRenderer()
{
    // 10 frames and a half of the sine at the enhanced mode's rate.
    constexpr std::size_t kLength = 10 * kEnhancedFrame + kEnhancedFrame / 2;
    const int kRate = CtrModel::kEnhancedSampleRate;

    // The resampler's frames of the sine: 11 frames, the last one short, then a frame of silence for the delay.
    FrameResampler resampler(kEnhancedFrame, kStrictFrame);
    SineRenderer sine(kRate, kLength);
    std::vector<float> expected;
    std::vector<float> in(2 * kEnhancedFrame);
    std::vector<float> out(2 * kStrictFrame);
    for (std::size_t frame = 0; frame < 12; frame++)
    {
        const std::size_t taken = sine.Render(in.data(), kEnhancedFrame);
        resampler.Process(std::span<const float>(in).first(2 * taken), out);
        expected.insert(expected.end(), out.begin(), out.end());
    }
    XSFLIB_CHECK(resampler.Delay() < kEnhancedFrame);

    for (const std::size_t block : {std::size_t{1}, std::size_t{37}, std::size_t{160}, std::size_t{4096}})
    {
        ResampledRenderer resampled(std::make_unique<SineRenderer>(kRate, kLength), kEnhancedFrame, kStrictFrame,
                                    CtrModel::kSampleRate);
        XSFLIB_CHECK(resampled.SampleRate() == CtrModel::kSampleRate);
        XSFLIB_CHECK(RenderAll(resampled, block) == expected);
        float sample = 0.0f;
        XSFLIB_CHECK(resampled.Render(&sample, 1) == 0);
    }

    // A renderer that ends at the end of a frame gives one frame of silence after it.
    ResampledRenderer whole(std::make_unique<SineRenderer>(CtrModel::kSampleRate, 4 * kStrictFrame), kStrictFrame,
                            kEnhancedFrame, kRate);
    XSFLIB_CHECK(RenderAll(whole, 100).size() == 2 * 5 * kEnhancedFrame);
}

// AtRate brings a renderer from one of the 3DS's rates to the other. A renderer at the rate, and one at another
// console's rate, come back as they are.
void TestAtRate()
{
    auto strict = std::make_unique<SineRenderer>(CtrModel::kSampleRate, kStrictFrame);
    const hokuto::audio::Renderer* const strict_address = strict.get();
    const auto same = hokuto::song::AtRate(std::move(strict), CtrModel::kSampleRate);
    XSFLIB_CHECK(same.get() == strict_address);

    auto other = std::make_unique<SineRenderer>(48000, kStrictFrame);
    const hokuto::audio::Renderer* const other_address = other.get();
    const auto kept = hokuto::song::AtRate(std::move(other), CtrModel::kSampleRate);
    XSFLIB_CHECK(kept.get() == other_address);

    const auto enhanced = hokuto::song::AtRate(std::make_unique<SineRenderer>(CtrModel::kSampleRate, kStrictFrame),
                                               CtrModel::kEnhancedSampleRate);
    XSFLIB_CHECK(dynamic_cast<const ResampledRenderer*>(enhanced.get()) != nullptr);
    XSFLIB_CHECK(enhanced->SampleRate() == CtrModel::kEnhancedSampleRate);
}

// A resampler from frames of no samples is refused.
void TestEmptyFrames()
{
    bool threw = false;
    try
    {
        FrameResampler resampler(0, kStrictFrame);
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }

    XSFLIB_CHECK(threw);
}

} // namespace

int main()
{
    TestSines();
    TestConstant();
    TestRenderer();
    TestAtRate();
    TestEmptyFrames();

    return xsflib::testing::Summary("frame_resampler");
}
