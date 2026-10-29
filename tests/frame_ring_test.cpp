// SPDX-License-Identifier: MIT

// Tests the ring buffer between the render worker and the audio device (src/audio/frame_ring.h). Frames come out in the
// order they went in, across the end of the ring and in blocks of any size. Discarded frames never come out. A writer
// and a reader on two threads check the memory ordering.

#include "audio/frame_ring.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <thread>
#include <vector>

#include "xsflib/testing/check.h"

using hokuto::audio::FrameRing;

namespace
{

// `count` frames numbered from `first`. A frame's left sample is its number, and its right sample the negated number.
// Floats hold whole numbers up to 2^24 exactly.
std::vector<float> Frames(uint32_t first, std::size_t count)
{
    std::vector<float> frames(count * 2);
    for (std::size_t i = 0; i < count; i++)
    {
        frames[i * 2] = static_cast<float>(first + i);
        frames[i * 2 + 1] = -static_cast<float>(first + i);
    }

    return frames;
}

// Whether the first `count` frames of `frames` are numbered from `first`.
bool Numbered(const float* frames, uint32_t first, std::size_t count)
{
    for (std::size_t i = 0; i < count; i++)
    {
        const auto number = static_cast<float>(first + i);
        if (frames[i * 2] != number || frames[i * 2 + 1] != -number)
        {
            return false;
        }
    }

    return true;
}

// A write and a read wrap around the end of the ring, and keep the frames in order.
void TestWrapAround()
{
    FrameRing ring(8);
    std::vector<float> out(16);

    const std::size_t written = ring.Write(Frames(0, 5).data(), 5);
    const std::size_t read = ring.Read(out.data(), 3);

    SEQLIB_CHECK_EQ(written, 5);
    SEQLIB_CHECK_EQ(read, 3);
    SEQLIB_CHECK(Numbered(out.data(), 0, 3));

    // This write wraps around the end of the ring. 6 of its 7 frames fit.
    const std::size_t wrapped = ring.Write(Frames(5, 7).data(), 7);

    SEQLIB_CHECK_EQ(wrapped, 6);
    SEQLIB_CHECK_EQ(ring.Space(), 0);
    SEQLIB_CHECK_EQ(ring.Available(), 8);

    const std::size_t rest = ring.Read(out.data(), 9);

    SEQLIB_CHECK_EQ(rest, 8);
    SEQLIB_CHECK(Numbered(out.data(), 3, 8));
    SEQLIB_CHECK_EQ(ring.Available(), 0);
    SEQLIB_CHECK_EQ(ring.Space(), 8);
}

// Frames written before DiscardWritten never reach the reader. Their space returns to the writer at the next Read.
void TestDiscard()
{
    FrameRing ring(8);
    std::vector<float> out(16);
    ring.Write(Frames(0, 6).data(), 6);
    ring.Read(out.data(), 1);

    ring.DiscardWritten();
    const std::size_t written = ring.Write(Frames(100, 4).data(), 4);
    const std::size_t space = ring.Space();
    const std::size_t available = ring.Available();
    const std::size_t read = ring.Read(out.data(), 8);

    SEQLIB_CHECK_EQ(written, 3);
    SEQLIB_CHECK_EQ(space, 0);
    SEQLIB_CHECK_EQ(available, 3);
    SEQLIB_CHECK_EQ(read, 3);
    SEQLIB_CHECK(Numbered(out.data(), 100, 3));
    SEQLIB_CHECK_EQ(ring.Space(), 8);
}

// Skipped says whether the last Read passed over discarded frames, and only for that Read. A discard after the reader
// has read every frame skips nothing.
void TestSkipped()
{
    FrameRing ring(8);
    std::vector<float> out(16);
    ring.Write(Frames(0, 6).data(), 6);
    ring.Read(out.data(), 1);
    const bool before_discard = ring.Skipped();
    ring.DiscardWritten();
    ring.Write(Frames(100, 2).data(), 2);

    ring.Read(out.data(), 8);
    const bool after_discard = ring.Skipped();
    ring.Write(Frames(102, 2).data(), 2);
    ring.Read(out.data(), 8);
    const bool next_read = ring.Skipped();
    ring.DiscardWritten();
    ring.Read(out.data(), 8);
    const bool nothing_left = ring.Skipped();

    SEQLIB_CHECK(!before_discard);
    SEQLIB_CHECK(after_discard);
    SEQLIB_CHECK(!next_read);
    SEQLIB_CHECK(!nothing_left);
}

// A writer and a reader on two threads, each with blocks of random sizes. Every frame arrives once, in order. On ARM, a
// missing acquire or release would let the reader see a frame's position before its samples.
void TestTwoThreads()
{
    constexpr uint32_t kFrames = 1'000'000;
    constexpr std::size_t kMaxBlock = 40;
    FrameRing ring(61);
    const auto write = [&ring]
    {
        std::minstd_rand random(1);
        std::uniform_int_distribution<std::size_t> size(1, kMaxBlock);
        uint32_t next = 0;
        while (next < kFrames)
        {
            const std::size_t count = std::min<std::size_t>(size(random), kFrames - next);
            const std::size_t written = ring.Write(Frames(next, count).data(), count);
            next += static_cast<uint32_t>(written);
            if (written == 0)
            {
                std::this_thread::yield();
            }
        }
    };
    std::thread writer(write);

    // Keep reading after a wrong frame. The writer would otherwise wait forever for space.
    std::minstd_rand random(2);
    std::uniform_int_distribution<std::size_t> size(1, kMaxBlock);
    std::vector<float> out(kMaxBlock * 2);
    uint32_t next = 0;
    bool ordered = true;
    while (next < kFrames)
    {
        const std::size_t read = ring.Read(out.data(), size(random));
        ordered = ordered && Numbered(out.data(), next, read);
        next += static_cast<uint32_t>(read);
        if (read == 0)
        {
            std::this_thread::yield();
        }
    }
    writer.join();

    SEQLIB_CHECK(ordered);
    SEQLIB_CHECK_EQ(ring.Available(), 0);
}

} // namespace

int main()
{
    TestWrapAround();
    TestDiscard();
    TestSkipped();
    TestTwoThreads();

    return xsflib::testing::Summary("frame_ring");
}
