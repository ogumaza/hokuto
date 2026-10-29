// SPDX-License-Identifier: MIT

// Renders audio ahead of the audio device, on a thread of its own, into a FrameRing. The device's callback then only
// copies frames from the ring. A renderer that takes longer for one block than the device's buffer lasts doesn't cause
// a dropout. It only has to keep up on average.

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "audio/frame_ring.h"
#include "audio/renderer.h"

namespace hokuto::audio
{

class RenderWorker
{
public:
    // Keeps `ahead` of audio in `ring`, rendered `block` frames at a time. The ring must hold `ahead` at the highest
    // sample rate of any renderer.
    RenderWorker(FrameRing& ring, std::chrono::milliseconds ahead, std::size_t block);

    RenderWorker(const RenderWorker&) = delete;
    RenderWorker& operator=(const RenderWorker&) = delete;

    // Stops the worker and waits for it to exit.
    ~RenderWorker();

    // Discards the frames in the ring and renders `renderer` in their place. The worker owns the renderer until it
    // reaches the end of its audio, or until the next Play or Stop.
    void Play(std::unique_ptr<Renderer> renderer);

    // Discards the frames in the ring and stops rendering.
    void Stop();

    // Whether the worker renders: from Play until Stop, or until the renderer reaches the end of its audio. The device
    // may still play frames from the ring after that.
    bool Playing() const;

    // Keeps `ahead` of audio in the ring from the next top-up on. The ring must hold it at the highest sample rate of
    // any renderer.
    void SetAhead(std::chrono::milliseconds ahead);

private:
    // How often the worker tops up the ring. Devices take frames in buffers of a few milliseconds or more.
    static constexpr std::chrono::milliseconds kInterval{2};

    void Run();

    // Renders blocks until the ring holds `ahead_` of audio at the renderer's rate. Returns false once the renderer has
    // reached the end of its audio.
    bool Fill(Renderer& renderer);

    FrameRing& ring_;
    std::atomic<int64_t> ahead_; // in milliseconds
    std::vector<float> block_;

    // Play and Stop pass the next renderer to the worker under mutex_.
    std::mutex mutex_;
    std::condition_variable changed_;
    std::unique_ptr<Renderer> next_;
    bool changing_ = false;
    bool quit_ = false;

    std::atomic<bool> playing_{false};

    // Started last, once the members that it uses exist.
    std::thread thread_;
};

} // namespace hokuto::audio
