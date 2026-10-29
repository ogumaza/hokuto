// SPDX-License-Identifier: MIT

// Tests the declicker (src/audio/declicker.h). Frames that follow on pass unchanged. At a join, the output continues
// from its last frame and moves to the new frames in small steps: after a skip, where the ring runs out, and where
// frames start after silence. Each channel moves on its own.

#include "audio/declicker.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include "xsflib/testing/check.h"

using hokuto::audio::Declicker;

namespace
{

// The largest step between two frames at a join of size 1: the gap closes by 1 - e^(-1/48) a frame.
constexpr float kLargestStep = 0.021f;

// `count` frames of `left` and `right`.
std::vector<float> Constant(std::size_t count, float left, float right)
{
    std::vector<float> frames(count * 2);
    for (std::size_t i = 0; i < count; i++)
    {
        frames[i * 2] = left;
        frames[i * 2 + 1] = right;
    }

    return frames;
}

// The largest step between two frames of one channel of `frames`, starting from `previous`.
float LargestStep(const std::vector<float>& frames, std::size_t channel, float previous)
{
    float largest = 0.0f;
    for (std::size_t i = channel; i < frames.size(); i += 2)
    {
        largest = std::max(largest, std::abs(frames[i] - previous));
        previous = frames[i];
    }

    return largest;
}

// A declicker that has played a second of `left` and `right`. The join at the start has closed by then.
Declicker Settled(float left, float right)
{
    Declicker declicker;
    std::vector<float> frames = Constant(48000, left, right);
    declicker.Process(frames.data(), 48000, 48000, false);

    return declicker;
}

// After silence, the output starts from silence. Once the join has closed, frames pass unchanged.
void TestStart()
{
    Declicker declicker;
    std::vector<float> frames(4000);
    for (std::size_t i = 0; i < frames.size(); i++)
    {
        frames[i] = std::sin(static_cast<float>(i) * 0.01f);
    }
    const std::vector<float> input = frames;

    declicker.Process(frames.data(), 2000, 2000, false);

    std::size_t changed_late = 0;
    for (std::size_t i = 2000; i < frames.size(); i++)
    {
        changed_late += frames[i] != input[i] ? 1 : 0;
    }
    SEQLIB_CHECK(frames[0] == 0.0f && frames[1] == 0.0f);
    SEQLIB_CHECK(LargestStep(frames, 0, 0.0f) < kLargestStep);
    SEQLIB_CHECK_EQ(changed_late, 0);
}

// After a skip, the output continues from its last frame and moves to the new frames.
void TestSkip()
{
    Declicker declicker = Settled(0.5f, -0.25f);
    std::vector<float> frames = Constant(1000, -0.5f, 0.25f);

    declicker.Process(frames.data(), 1000, 1000, true);

    SEQLIB_CHECK(frames[0] == 0.5f && frames[1] == -0.25f);
    SEQLIB_CHECK(LargestStep(frames, 0, 0.5f) < kLargestStep);
    SEQLIB_CHECK(LargestStep(frames, 1, -0.25f) < kLargestStep / 2.0f);
    SEQLIB_CHECK(std::abs(frames[240 * 2] + 0.5f) < 0.01f);
    SEQLIB_CHECK(frames[999 * 2] == -0.5f && frames[999 * 2 + 1] == 0.25f);
}

// Where the ring runs out, the output falls from its last frame to silence.
void TestRunOut()
{
    Declicker declicker = Settled(0.5f, -0.25f);
    std::vector<float> frames = Constant(1000, 0.5f, -0.25f);
    std::fill(frames.begin() + 200, frames.end(), 0.0f);

    declicker.Process(frames.data(), 1000, 100, false);

    SEQLIB_CHECK(frames[99 * 2] == 0.5f && frames[100 * 2] == 0.5f);
    SEQLIB_CHECK(LargestStep(frames, 0, 0.5f) < kLargestStep / 2.0f);
    SEQLIB_CHECK(std::abs(frames[340 * 2]) < 0.005f);
    SEQLIB_CHECK(frames[999 * 2] == 0.0f && frames[999 * 2 + 1] == 0.0f);
}

} // namespace

int main()
{
    TestStart();
    TestSkip();
    TestRunOut();

    return xsflib::testing::Summary("declicker");
}
