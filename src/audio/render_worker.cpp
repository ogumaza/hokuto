// SPDX-License-Identifier: MIT

#include "audio/render_worker.h"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace hokuto::audio
{

RenderWorker::RenderWorker(FrameRing& ring, std::chrono::milliseconds ahead, std::size_t block)
    : ring_(ring),
      ahead_(ahead.count()),
      block_(std::clamp<std::size_t>(block, 1, ring.Capacity()) * 2),
      thread_([this] { Run(); })
{
}

RenderWorker::~RenderWorker()
{
    {
        std::lock_guard lock(mutex_);
        quit_ = true;
    }
    changed_.notify_one();

    thread_.join();
}

void RenderWorker::Play(std::unique_ptr<Renderer> renderer)
{
    // A renderer that the worker hasn't taken up yet is destroyed here, after the lock is released.
    std::unique_ptr<Renderer> replaced;
    {
        std::lock_guard lock(mutex_);
        replaced = std::exchange(next_, std::move(renderer));
        changing_ = true;
        playing_.store(next_ != nullptr, std::memory_order_relaxed);
    }
    changed_.notify_one();
}

void RenderWorker::Stop()
{
    Play(nullptr);
}

bool RenderWorker::Playing() const
{
    return playing_.load(std::memory_order_relaxed);
}

void RenderWorker::SetAhead(std::chrono::milliseconds ahead)
{
    ahead_.store(ahead.count(), std::memory_order_relaxed);
}

void RenderWorker::Run()
{
    std::unique_ptr<Renderer> renderer;
    for (;;)
    {
        // Wait for Play, Stop or the destructor. While a renderer plays, top up the ring every kInterval.
        std::unique_lock lock(mutex_);
        const auto woken = [this]
        {
            return changing_ || quit_;
        };
        if (renderer)
        {
            changed_.wait_for(lock, kInterval, woken);
        }
        else
        {
            changed_.wait(lock, woken);
        }
        if (quit_)
        {
            return;
        }

        // Take up the renderer from Play or Stop. The frames in the ring came from the one before it.
        std::unique_ptr<Renderer> finished;
        if (changing_)
        {
            finished = std::exchange(renderer, std::move(next_));
            changing_ = false;
            ring_.DiscardWritten();
        }
        lock.unlock();
        finished.reset();

        if (renderer && !Fill(*renderer))
        {
            renderer.reset();

            // A Play since the last wait has set playing_ for its renderer already.
            std::lock_guard relock(mutex_);
            if (!changing_)
            {
                playing_.store(false, std::memory_order_relaxed);
            }
        }
    }
}

bool RenderWorker::Fill(Renderer& renderer)
{
    // The frames to keep in the ring: `ahead_` at the renderer's rate, at least one block, and at most the ring's size.
    const std::size_t block = block_.size() / 2;
    const int64_t ahead_ms = std::max<int64_t>(ahead_.load(std::memory_order_relaxed), 0);
    const uint64_t ahead =
        static_cast<uint64_t>(std::max(renderer.SampleRate(), 0)) * static_cast<uint64_t>(ahead_ms) / 1000;
    const std::size_t target = std::min(std::max(static_cast<std::size_t>(ahead), block), ring_.Capacity());

    // The ring always has room for the block: the frames in it and the block add up to `target` at most.
    while (ring_.Capacity() - ring_.Space() + block <= target)
    {
        const std::size_t frames = renderer.Render(block_.data(), block);
        ring_.Write(block_.data(), frames);
        if (frames < block)
        {
            return false;
        }
    }

    return true;
}

} // namespace hokuto::audio
