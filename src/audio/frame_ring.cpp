// SPDX-License-Identifier: MIT

#include "audio/frame_ring.h"

#include <algorithm>

namespace hokuto::audio
{

FrameRing::FrameRing(std::size_t capacity) : samples_(capacity * 2), capacity_(capacity)
{
}

std::size_t FrameRing::Capacity() const
{
    return capacity_;
}

std::size_t FrameRing::Space() const
{
    // The reader's release store of read_ means that it has finished with the frames before that position.
    const uint64_t written = write_.load(std::memory_order_relaxed);
    const uint64_t read = read_.load(std::memory_order_acquire);
    return capacity_ - static_cast<std::size_t>(written - read);
}

std::size_t FrameRing::Write(const float* frames, std::size_t count)
{
    const uint64_t written = write_.load(std::memory_order_relaxed);
    const std::size_t n = std::min(count, Space());

    // Copy in two parts where the frames wrap around the end of the ring.
    const std::size_t offset = static_cast<std::size_t>(written % capacity_);
    const std::size_t first = std::min(n, capacity_ - offset);
    std::copy_n(frames, first * 2, samples_.data() + offset * 2);
    std::copy_n(frames + first * 2, (n - first) * 2, samples_.data());

    // The release store publishes the samples to the reader.
    write_.store(written + n, std::memory_order_release);

    return n;
}

void FrameRing::DiscardWritten()
{
    discard_.store(write_.load(std::memory_order_relaxed), std::memory_order_release);
}

uint64_t FrameRing::ReadStart() const
{
    // The writer stores discard_ after write_ has reached that position. A reader that sees the new discard_ then sees
    // a write_ at least as far.
    const uint64_t read = read_.load(std::memory_order_relaxed);
    const uint64_t discard = discard_.load(std::memory_order_acquire);
    return std::max(read, discard);
}

std::size_t FrameRing::Available() const
{
    const uint64_t start = ReadStart();
    const uint64_t written = write_.load(std::memory_order_acquire);
    return static_cast<std::size_t>(written - start);
}

std::size_t FrameRing::Read(float* out, std::size_t count)
{
    const uint64_t start = ReadStart();
    const uint64_t written = write_.load(std::memory_order_acquire);
    skipped_ = start > read_.load(std::memory_order_relaxed);
    const std::size_t n = static_cast<std::size_t>(std::min<uint64_t>(count, written - start));

    // Copy in two parts where the frames wrap around the end of the ring.
    const std::size_t offset = static_cast<std::size_t>(start % capacity_);
    const std::size_t first = std::min(n, capacity_ - offset);
    std::copy_n(samples_.data() + offset * 2, first * 2, out);
    std::copy_n(samples_.data(), (n - first) * 2, out + first * 2);

    // The release store hands the frames' space back to the writer.
    read_.store(start + n, std::memory_order_release);

    return n;
}

bool FrameRing::Skipped() const
{
    return skipped_;
}

} // namespace hokuto::audio
