// SPDX-License-Identifier: MIT

// A ring buffer of stereo frames for one writer thread and one reader thread. Neither side locks, waits or allocates.
// The reader can therefore be the audio device's callback. A frame is two floats: the left sample, then the right.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hokuto::audio
{

class FrameRing
{
public:
    // A ring that holds up to `capacity` frames. `capacity` is at least 1.
    explicit FrameRing(std::size_t capacity);

    FrameRing(const FrameRing&) = delete;
    FrameRing& operator=(const FrameRing&) = delete;

    std::size_t Capacity() const;

    // The writer's side. Space returns how many frames fit. Write copies up to `count` frames from `frames` and returns
    // how many it copied.
    std::size_t Space() const;
    std::size_t Write(const float* frames, std::size_t count);

    // The writer's side: makes the reader skip every frame written so far. The writer calls it before it writes audio
    // that replaces them, such as after a stop. Their space stays taken until the reader's next Read.
    void DiscardWritten();

    // The reader's side. Available returns how many frames are ready. Read copies up to `count` frames into `out` and
    // returns how many it copied. Skipped says whether the last Read passed over frames that DiscardWritten discarded.
    // The frames that it copied then don't follow on from the frames read before them.
    std::size_t Available() const;
    std::size_t Read(float* out, std::size_t count);
    bool Skipped() const;

private:
    // The cache line of Apple Silicon. x64's is 64 bytes. Keeping the two sides' counters apart stops each write from
    // evicting the line that the other thread reads.
    static constexpr std::size_t kCacheLine = 128;

    // The first frame that the reader may read: the read position, or the discard position if that's later.
    uint64_t ReadStart() const;

    std::vector<float> samples_;
    const std::size_t capacity_;

    // Frames counted from the start. The writer stores write_ and discard_. The reader stores read_, and skipped_ for
    // itself.
    alignas(kCacheLine) std::atomic<uint64_t> write_{0};
    std::atomic<uint64_t> discard_{0};
    alignas(kCacheLine) std::atomic<uint64_t> read_{0};
    bool skipped_ = false;
};

} // namespace hokuto::audio
