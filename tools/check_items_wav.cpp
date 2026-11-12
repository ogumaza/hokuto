// SPDX-License-Identifier: MIT

// The WAV files of hokuto_check_items: the read of a file and the check of its samples.

#include "check_items.h"

namespace hokuto::tools::items
{

uint16_t Le16(std::span<const uint8_t> bytes, std::size_t at)
{
    return static_cast<uint16_t>(bytes[at] | bytes[at + 1] << 8);
}

uint32_t Le32(std::span<const uint8_t> bytes, std::size_t at)
{
    return static_cast<uint32_t>(Le16(bytes, at)) | static_cast<uint32_t>(Le16(bytes, at + 2)) << 16;
}

std::optional<WavRead> ReadWav(std::span<const uint8_t> file)
{
    if (file.size() < 12 || std::string(file.begin(), file.begin() + 4) != "RIFF" ||
        std::string(file.begin() + 8, file.begin() + 12) != "WAVE" || Le32(file, 4) != file.size() - 8)
    {
        return std::nullopt;
    }

    // The chunks, each padded to an even size.
    WavRead wav;
    std::span<const uint8_t> data;
    bool format = false;
    for (std::size_t at = 12; at + 8 <= file.size();)
    {
        const std::string id(file.begin() + static_cast<std::ptrdiff_t>(at),
                             file.begin() + static_cast<std::ptrdiff_t>(at + 4));
        const uint32_t size = Le32(file, at + 4);
        if (at + 8 + size > file.size())
        {
            return std::nullopt;
        }
        const std::span<const uint8_t> body = file.subspan(at + 8, size);
        if (id == "fmt ")
        {
            const uint16_t tag = Le16(body, 0);
            const bool pcm = tag == 1 && size == 16;
            const bool extensible = tag == 0xFFFE && size == 40 && Le16(body, 16) == 22 && Le16(body, 18) == 16 &&
                                    Le16(body, 24) == 1 && Le16(body, 26) == 0;
            wav.channels = Le16(body, 2);
            wav.rate = Le32(body, 4);
            if ((!pcm && !extensible) || extensible != (wav.channels > 2) || Le16(body, 14) != 16 ||
                Le16(body, 12) != wav.channels * 2 || Le32(body, 8) != wav.rate * wav.channels * 2)
            {
                return std::nullopt;
            }
            format = true;
        }
        else if (id == "data")
        {
            data = body;
        }
        else if (id == "smpl")
        {
            if (size != 60 || Le32(body, 28) != 1 || Le32(body, 40) != 0 || Le32(body, 56) != 0)
            {
                return std::nullopt;
            }
            wav.loop = true;
            wav.loop_start = Le32(body, 44);
            wav.loop_last = Le32(body, 48);
        }
        at += 8 + size + (size & 1);
    }
    if (!format || wav.channels == 0 || data.size() % (wav.channels * 2u) != 0)
    {
        return std::nullopt;
    }

    // The samples, interleaved.
    const std::size_t frames = data.size() / (wav.channels * 2u);
    wav.samples.assign(wav.channels, std::vector<int16_t>(frames));
    for (std::size_t frame = 0; frame < frames; frame++)
    {
        for (std::size_t channel = 0; channel < wav.channels; channel++)
        {
            wav.samples[channel][frame] = static_cast<int16_t>(Le16(data, (frame * wav.channels + channel) * 2));
        }
    }

    return wav;
}

bool Holds(const WavRead& wav, const sar::Pcm& pcm, int tolerance)
{
    if (wav.channels != pcm.channels.size() || wav.rate != pcm.sample_rate ||
        wav.samples.size() != pcm.channels.size() || wav.loop != pcm.loop)
    {
        return false;
    }

    for (std::size_t channel = 0; channel < pcm.channels.size(); channel++)
    {
        const std::vector<int16_t>& ours = wav.samples[channel];
        const std::vector<int16_t>& theirs = pcm.channels[channel];
        if (ours.size() != theirs.size() ||
            !std::equal(ours.begin(), ours.end(), theirs.begin(),
                        [tolerance](int16_t a, int16_t b) { return std::abs(int{a} - int{b}) <= tolerance; }))
        {
            return false;
        }
    }

    return !pcm.loop || (wav.loop_start == pcm.loop_start && wav.loop_last + 1 == pcm.loop_end);
}

} // namespace hokuto::tools::items
