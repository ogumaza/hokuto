// SPDX-License-Identifier: MIT

// The renders of the conformance suite (tests/conformance/README.md). Each sequence sound of a Hokuto file plays on the
// model of the file's console, with the file's effects, the aux buses' volumes and the sound's mixing mode, from the
// model's first frame. The suite gives the SHA-256 of each render. The conformance test and the tool that makes the
// suite's files share this header.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/rvl/snd/rsar_options.h"

#include "edit/effects.h"
#include "format/archive.h"
#include "format/file.h"
#include "song/archive.h"
#include "song/sequence.h"
#include "song/sequence_model.h"

namespace hokuto::conformance
{

// The SHA-256 of `bytes` in lower-case hexadecimal. FIPS 180-4 defines the hash.
inline std::string Sha256(std::span<const uint8_t> bytes)
{
    constexpr std::array<uint32_t, 64> kRounds = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    };
    std::array<uint32_t, 8> hash = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

    const auto rotate = [](uint32_t x, int n)
    {
        return (x >> n) | (x << (32 - n));
    };

    // The message, a one bit, zeros up to 8 bytes short of a block, and the message's length in bits, big-endian.
    std::vector<uint8_t> message(bytes.begin(), bytes.end());
    const uint64_t bits = static_cast<uint64_t>(bytes.size()) * 8;
    message.push_back(0x80);
    while (message.size() % 64 != 56)
    {
        message.push_back(0);
    }
    for (int shift = 56; shift >= 0; shift -= 8)
    {
        message.push_back(static_cast<uint8_t>(bits >> shift));
    }

    for (std::size_t block = 0; block < message.size(); block += 64)
    {
        std::array<uint32_t, 64> w{};
        for (std::size_t t = 0; t < 16; t++)
        {
            const uint8_t* word = &message[block + 4 * t];
            w[t] = (uint32_t{word[0]} << 24) | (uint32_t{word[1]} << 16) | (uint32_t{word[2]} << 8) | word[3];
        }
        for (std::size_t t = 16; t < 64; t++)
        {
            const uint32_t s0 = rotate(w[t - 15], 7) ^ rotate(w[t - 15], 18) ^ (w[t - 15] >> 3);
            const uint32_t s1 = rotate(w[t - 2], 17) ^ rotate(w[t - 2], 19) ^ (w[t - 2] >> 10);
            w[t] = w[t - 16] + s0 + w[t - 7] + s1;
        }

        std::array<uint32_t, 8> v = hash;
        for (std::size_t t = 0; t < 64; t++)
        {
            const uint32_t sum1 = rotate(v[4], 6) ^ rotate(v[4], 11) ^ rotate(v[4], 25);
            const uint32_t choose = (v[4] & v[5]) ^ (~v[4] & v[6]);
            const uint32_t t1 = v[7] + sum1 + choose + kRounds[t] + w[t];
            const uint32_t sum0 = rotate(v[0], 2) ^ rotate(v[0], 13) ^ rotate(v[0], 22);
            const uint32_t majority = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
            const uint32_t t2 = sum0 + majority;
            v = {t1 + t2, v[0], v[1], v[2], v[3] + t1, v[4], v[5], v[6]};
        }
        for (std::size_t i = 0; i < 8; i++)
        {
            hash[i] += v[i];
        }
    }

    std::string text;
    for (const uint32_t word : hash)
    {
        char digits[9];
        std::snprintf(digits, sizeof(digits), "%08x", static_cast<unsigned>(word));
        text += digits;
    }

    return text;
}

// The render of sequence sound `sound` of `file` for `seconds`, or until the output ends: interleaved stereo in 16
// bits, as little-endian bytes. In strict mixing, the samples are the model's. In enhanced mixing, they're the model's
// samples in floating point, times 32,768, rounded to the nearest whole number and held to 16 bits. The model starts
// with the sound's variables at their starting values. `effects` takes the place of the file's effects where it's
// given. Throws the exceptions of the archive's reading.
inline std::vector<uint8_t> Render(const format::File& file, uint32_t sound, double seconds,
                                   const song::AuxEffects* effects = nullptr)
{
    if (!file.native)
    {
        throw std::invalid_argument("the file holds no sound archive");
    }

    // A render runs once from the start and never seeks. A snapshot would only take memory.
    constexpr uint64_t kNoSnapshots = std::numeric_limits<uint32_t>::max();
    const song::AuxEffects played = effects != nullptr ? *effects : edit::ModelEffects(file.aux_effects);
    std::unique_ptr<song::SequenceModel> model;
    const format::Mixing mixing = format::MixingOf(file, sound);
    if (file.native->wii)
    {
        const std::shared_ptr<const ntdlib::rsar::SoundArchive> archive =
            song::LoadWiiArchive(format::ExportArchive(file), "conformance", ntdlib::rvl::snd::kRsarOptions);
        auto sequence = std::make_shared<const song::Sequence>(
            song::ReadSequence(*archive, sound, {}, {}, format::BuildOfWiiArchive(*file.native)));
        model = std::make_unique<song::SequenceModel>(archive, std::move(sequence), played, kNoSnapshots);
    }
    else
    {
        const std::shared_ptr<const ntdlib::sar::SoundArchive> archive =
            song::LoadArchive(format::ExportArchive(file), "conformance", format::ExportGroupFiles(file));
        auto sequence = std::make_shared<const song::Sequence>(song::ReadSequence(*archive, sound));
        model = std::make_unique<song::SequenceModel>(archive, std::move(sequence), played, edit::ModelMixing(mixing),
                                                      kNoSnapshots);
    }
    model->SetBusVolumes(file.aux_volumes);

    const auto frames =
        static_cast<uint64_t>(std::ceil(seconds * model->SampleRate() / static_cast<double>(model->FrameSamples())));
    std::vector<uint8_t> render;
    const auto put = [&render](int sample)
    {
        const auto bits = static_cast<uint16_t>(static_cast<int16_t>(sample));
        render.push_back(static_cast<uint8_t>(bits & 0xFF));
        render.push_back(static_cast<uint8_t>(bits >> 8));
    };
    for (bool plays = frames > 0 && model->SeekFrame(0); plays; plays = model->RunFrame())
    {
        if (mixing == format::Mixing::kEnhanced && !file.native->wii)
        {
            for (const float sample : model->FloatOutput())
            {
                put(static_cast<int>(std::clamp(std::lround(static_cast<double>(sample) * 32768.0), -32768L, 32767L)));
            }
        }
        else
        {
            for (const int16_t sample : model->Output())
            {
                put(sample);
            }
        }
        if (model->Frames() >= frames)
        {
            break;
        }
    }

    return render;
}

} // namespace hokuto::conformance
