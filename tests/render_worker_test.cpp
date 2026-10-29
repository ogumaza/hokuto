// SPDX-License-Identifier: MIT

// Tests rendering ahead (src/audio/render_worker.h). A reader stands in for the audio device and takes frames in blocks
// of random sizes. The frames that reach it must be the renderer's output, whole and in order, whatever the block
// sizes. After Stop, or after Play with a new renderer, no frame of the old renderer may reach it. SetAhead changes how
// many frames the worker keeps in the ring.

#include "audio/render_worker.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <thread>
#include <vector>

#include "xsflib/testing/check.h"

#include "audio/frame_ring.h"
#include "audio/renderer.h"

using hokuto::audio::FrameRing;
using hokuto::audio::Renderer;
using hokuto::audio::RenderWorker;
using Clock = std::chrono::steady_clock;

namespace
{

constexpr int kRate = 48'000;

// Long enough for the worker to take up a Play or Stop, even on a busy machine.
constexpr std::chrono::milliseconds kTakeUp{200};

// Frames numbered from `first`, `length` of them. A frame's left sample is its number, and its right sample the negated
// number. Floats hold whole numbers up to 2^24 exactly.
class CountingRenderer final : public Renderer
{
public:
    CountingRenderer(uint32_t first, uint32_t length) : first_(first), length_(length)
    {
    }

    int SampleRate() const override
    {
        return kRate;
    }

    std::size_t Render(float* out, std::size_t frames) override
    {
        const std::size_t n = std::min<std::size_t>(frames, length_ - position_);
        for (std::size_t i = 0; i < n; i++)
        {
            const auto number = static_cast<float>(first_ + position_ + i);
            out[i * 2] = number;
            out[i * 2 + 1] = -number;
        }
        position_ += static_cast<uint32_t>(n);

        return n;
    }

private:
    const uint32_t first_;
    const uint32_t length_;
    uint32_t position_ = 0;
};

// Stands in for the audio device: takes frames from a ring in blocks of random sizes, at about real time.
class Reader
{
public:
    explicit Reader(FrameRing& ring) : ring_(ring), random_(3)
    {
    }

    // Reads blocks for `duration`. Appends the frames' numbers to `numbers`.
    void ReadFor(Clock::duration duration, std::vector<float>& numbers)
    {
        std::uniform_int_distribution<std::size_t> size(1, 1024);
        std::vector<float> block(1024 * 2);
        const Clock::time_point end = Clock::now() + duration;
        while (Clock::now() < end)
        {
            const std::size_t read = ring_.Read(block.data(), size(random_));
            for (std::size_t i = 0; i < read; i++)
            {
                numbers.push_back(block[i * 2]);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

private:
    FrameRing& ring_;
    std::minstd_rand random_;
};

// Whether `numbers` counts up by one from `first`, from index `begin` on.
bool CountsFrom(const std::vector<float>& numbers, std::size_t begin, uint32_t first)
{
    for (std::size_t i = begin; i < numbers.size(); i++)
    {
        if (numbers[i] != static_cast<float>(first + (i - begin)))
        {
            return false;
        }
    }

    return true;
}

// Reads until the worker's first frames arrive, then for 50 ms more. A busy machine can take more than 50 ms to start
// the worker's thread. A test that read for 50 ms alone then got no frame before its next step.
void ReadFirstFrames(Reader& reader, std::vector<float>& numbers)
{
    const Clock::time_point give_up = Clock::now() + std::chrono::seconds(10);
    while (numbers.empty() && Clock::now() < give_up)
    {
        reader.ReadFor(std::chrono::milliseconds(10), numbers);
    }
    reader.ReadFor(std::chrono::milliseconds(50), numbers);
}

// A short renderer plays to its end. The reader receives every frame once, in order.
void TestWholeAndInOrder()
{
    constexpr uint32_t kLength = 20'000;
    FrameRing ring(16'384);
    RenderWorker worker(ring, std::chrono::milliseconds(100), 256);
    Reader reader(ring);
    std::vector<float> numbers;

    worker.Play(std::make_unique<CountingRenderer>(0, kLength));
    const Clock::time_point give_up = Clock::now() + std::chrono::seconds(10);
    while (numbers.size() < kLength && Clock::now() < give_up)
    {
        reader.ReadFor(std::chrono::milliseconds(10), numbers);
    }
    reader.ReadFor(std::chrono::milliseconds(50), numbers);

    SEQLIB_CHECK_EQ(numbers.size(), kLength);
    SEQLIB_CHECK(CountsFrom(numbers, 0, 0));
    SEQLIB_CHECK(!worker.Playing());
}

// After Stop, the reader receives no more frames. The reader pauses while the worker takes up the Stop. Frames that the
// worker failed to discard would then still be in the ring: up to 100 ms of them.
void TestStop()
{
    FrameRing ring(16'384);
    RenderWorker worker(ring, std::chrono::milliseconds(100), 256);
    Reader reader(ring);
    std::vector<float> before;
    worker.Play(std::make_unique<CountingRenderer>(0, 1'000'000));
    ReadFirstFrames(reader, before);

    worker.Stop();
    const bool playing = worker.Playing();
    std::this_thread::sleep_for(kTakeUp);
    std::vector<float> after;
    reader.ReadFor(std::chrono::milliseconds(50), after);

    SEQLIB_CHECK(!playing);
    SEQLIB_CHECK(!before.empty());
    SEQLIB_CHECK(CountsFrom(before, 0, 0));
    SEQLIB_CHECK_EQ(after.size(), 0);
}

// After Play with a new renderer, the reader receives the new renderer's frames from the first one. The reader pauses
// while the worker takes up the Play, as in TestStop.
void TestReplace()
{
    constexpr uint32_t kSecond = 10'000'000;
    FrameRing ring(16'384);
    RenderWorker worker(ring, std::chrono::milliseconds(100), 256);
    Reader reader(ring);
    std::vector<float> before;
    worker.Play(std::make_unique<CountingRenderer>(0, 1'000'000));
    ReadFirstFrames(reader, before);

    worker.Play(std::make_unique<CountingRenderer>(kSecond, 1'000'000));
    std::this_thread::sleep_for(kTakeUp);
    std::vector<float> after;
    reader.ReadFor(std::chrono::milliseconds(50), after);

    SEQLIB_CHECK(CountsFrom(before, 0, 0));
    SEQLIB_CHECK(!after.empty());
    SEQLIB_CHECK(CountsFrom(after, 0, kSecond));
}

// Play with a new renderer while the reader keeps reading. Once the new renderer's first frame arrives, no frame of the
// old one follows it.
void TestReplaceWhileReading()
{
    constexpr uint32_t kSecond = 10'000'000;
    FrameRing ring(16'384);
    RenderWorker worker(ring, std::chrono::milliseconds(100), 256);
    Reader reader(ring);
    std::vector<float> numbers;
    worker.Play(std::make_unique<CountingRenderer>(0, 1'000'000));
    ReadFirstFrames(reader, numbers);

    worker.Play(std::make_unique<CountingRenderer>(kSecond, 1'000'000));
    reader.ReadFor(std::chrono::milliseconds(200), numbers);

    const auto switched = std::find(numbers.begin(), numbers.end(), static_cast<float>(kSecond));
    SEQLIB_REQUIRE(switched != numbers.end());
    const auto index = static_cast<std::size_t>(switched - numbers.begin());
    SEQLIB_CHECK(index > 0);
    SEQLIB_CHECK(CountsFrom(std::vector<float>(numbers.begin(), switched), 0, 0));
    SEQLIB_CHECK(CountsFrom(numbers, index, kSecond));
}

// The worker keeps 100 ms in the ring at first: 4,800 frames at 48 kHz, less up to a block of 256. With 20 ms from
// SetAhead, it keeps 960 frames, less up to a block. The reader empties the ring between the two.
void TestSetAhead()
{
    FrameRing ring(16'384);
    RenderWorker worker(ring, std::chrono::milliseconds(100), 256);
    std::vector<float> taken(16'384 * 2);

    worker.Play(std::make_unique<CountingRenderer>(0, 1'000'000));
    std::this_thread::sleep_for(kTakeUp);
    const std::size_t at_first = ring.Available();
    worker.SetAhead(std::chrono::milliseconds(20));
    ring.Read(taken.data(), ring.Available());
    std::this_thread::sleep_for(kTakeUp);
    const std::size_t after = ring.Available();

    SEQLIB_CHECK(at_first > 4800 - 256 && at_first <= 4800);
    SEQLIB_CHECK(after > 960 - 256 && after <= 960);
}

} // namespace

int main()
{
    TestWholeAndInOrder();
    TestStop();
    TestReplace();
    TestReplaceWhileReading();
    TestSetAhead();

    return xsflib::testing::Summary("render_worker");
}
