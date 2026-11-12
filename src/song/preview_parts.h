// SPDX-License-Identifier: MIT

// What the previews of song/preview.h share. Only the preview files of src/song include it.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "song/pcm.h"
#include "song/preview.h"

namespace hokuto::song::preview_parts
{

// The key nearest NotePreview::kKey for which `plays` is true, the lower one first at the same distance, or -1 when
// none is.
template <typename Plays>
int NearestKey(Plays plays)
{
    for (int distance = 0; distance <= 127; distance++)
    {
        for (const int key : {NotePreview::kKey - distance, NotePreview::kKey + distance})
        {
            if (key >= 0 && key <= 127 && plays(key))
            {
                return key;
            }
        }
    }

    return -1;
}

// Sets `frame` to `samples` as floats.
inline void SetFrame(std::vector<float>& frame, std::span<const int16_t> samples)
{
    frame.resize(samples.size());
    for (std::size_t i = 0; i < samples.size(); i++)
    {
        frame[i] = static_cast<float>(samples[i]) * kSampleScale;
    }
}

// Fades `frame`, interleaved stereo, from its level at the start to silence at the end.
inline void FadeOut(std::vector<float>& frame)
{
    for (std::size_t i = 0; i < frame.size(); i++)
    {
        const float through = static_cast<float>(i / 2) / static_cast<float>(frame.size() / 2);
        frame[i] *= 1.0f - through;
    }
}

// Copies up to `frames` stereo frames into `out` from `frame`, the samples of the model's current frame, from
// `read` on. `next` runs the next frame of the model into `frame` and sets `read` to 0, and returns false at the end.
// Returns the frames copied.
template <typename Next>
std::size_t RenderFrames(float* out, std::size_t frames, const std::vector<float>& frame, std::size_t& read, Next next)
{
    std::size_t done = 0;
    while (done < frames)
    {
        if (read == frame.size() && !next())
        {
            break;
        }

        const std::size_t count = std::min((frames - done) * 2, frame.size() - read);
        std::copy_n(frame.data() + read, count, out + done * 2);
        read += count;
        done += count / 2;
    }

    return done;
}

} // namespace hokuto::song::preview_parts
