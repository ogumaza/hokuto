// SPDX-License-Identifier: MIT

// Tests the clip (src/audio/clip.h). A clip plays its samples once, at its rate. The first and last 10 ms fade linearly
// from and to silence, and the samples between come out as they are. The sizes of the calls' blocks don't change the
// output. After the end, Render writes nothing. A clip shorter than 20 ms fades over half its length at each end.

#include "audio/clip.h"

#include <cstddef>
#include <vector>

#include "xsflib/testing/check.h"

using hokuto::audio::Clip;

namespace
{

constexpr int kRate = 48000;

// `frames` stereo frames whose left samples count up from 1 and whose right samples count down from -1.
std::vector<float> Ramp(std::size_t frames)
{
    std::vector<float> samples(frames * 2);
    for (std::size_t i = 0; i < frames; i++)
    {
        samples[i * 2] = static_cast<float>(i + 1);
        samples[i * 2 + 1] = -static_cast<float>(i + 1);
    }

    return samples;
}

// Everything that `clip` renders, in blocks of `block` frames.
std::vector<float> RenderAll(Clip& clip, std::size_t block)
{
    std::vector<float> out;
    std::vector<float> buffer(block * 2);
    for (std::size_t n = clip.Render(buffer.data(), block); n > 0; n = clip.Render(buffer.data(), block))
    {
        out.insert(out.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(n * 2));
    }

    return out;
}

// A clip of 0.1 s: 4,800 frames. The fades take 480 frames at each end. Frame k of the first fade plays at k / 480 of
// its level, and the last frame at 0.
void TestFades()
{
    Clip clip(Ramp(4800), kRate);
    const std::vector<float> out = RenderAll(clip, 256);
    float last[2] = {1.0f, 1.0f};

    SEQLIB_CHECK_EQ(clip.SampleRate(), kRate);
    SEQLIB_REQUIRE_EQ(out.size(), std::size_t{9600});
    SEQLIB_CHECK(out[0] == 0.0f);
    SEQLIB_CHECK(out[1] == 0.0f);
    SEQLIB_CHECK(out[240 * 2] == 241.0f * (240.0f / 480.0f));
    SEQLIB_CHECK(out[240 * 2 + 1] == -241.0f * (240.0f / 480.0f));
    SEQLIB_CHECK(out[480 * 2] == 481.0f);
    SEQLIB_CHECK(out[4319 * 2] == 4320.0f);
    SEQLIB_CHECK(out[4320 * 2] == 4321.0f * (479.0f / 480.0f));
    SEQLIB_CHECK(out[4799 * 2] == 0.0f);
    SEQLIB_CHECK_EQ(clip.Render(last, 1), std::size_t{0});
    SEQLIB_CHECK(last[0] == 1.0f);
}

// Blocks of 1, 7 and 4,800 frames give the same output.
void TestBlocks()
{
    Clip one(Ramp(4800), kRate);
    Clip seven(Ramp(4800), kRate);
    Clip whole(Ramp(4800), kRate);
    const std::vector<float> expected = RenderAll(whole, 4800);

    SEQLIB_CHECK(RenderAll(one, 1) == expected);
    SEQLIB_CHECK(RenderAll(seven, 7) == expected);
}

// A clip of 100 frames fades over 50 at each end. An empty clip renders nothing.
void TestShortClip()
{
    Clip clip(Ramp(100), kRate);
    Clip empty({}, kRate);
    const std::vector<float> out = RenderAll(clip, 64);
    float buffer[2] = {};

    SEQLIB_REQUIRE_EQ(out.size(), std::size_t{200});
    SEQLIB_CHECK(out[25 * 2] == 26.0f * (25.0f / 50.0f));
    SEQLIB_CHECK(out[50 * 2] == 51.0f * (49.0f / 50.0f));
    SEQLIB_CHECK(out[99 * 2] == 0.0f);
    SEQLIB_CHECK_EQ(empty.Render(buffer, 1), std::size_t{0});
}

} // namespace

int main()
{
    TestFades();
    TestBlocks();
    TestShortClip();

    return xsflib::testing::Summary("clip");
}
