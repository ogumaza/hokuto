// SPDX-License-Identifier: MIT

// The renders of the conformance suite (tests/conformance/README.md). Each sequence sound of a Hokuto file plays on the
// model of the file's console, with the file's effects, the aux buses' volumes and the sound's mixing mode, from the
// model's first frame. The suite gives the SHA-256 of each render. The conformance test and the tool that makes the
// suite's files share this header.

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/rvl/snd/rsar_options.h"
#include "xsflib/sha256.h"

#include "edit/effects.h"
#include "format/archive.h"
#include "format/file.h"
#include "song/archive.h"
#include "song/pcm.h"
#include "song/sequence.h"
#include "song/sequence_model.h"

namespace hokuto::conformance
{

// The SHA-256 of `bytes` in lower-case hexadecimal. FIPS 180-4 defines the hash.
inline std::string Sha256(std::span<const uint8_t> bytes)
{
    return xsflib::Sha256::Hex(xsflib::Sha256::Of(bytes));
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
    if (file.native->engine == format::Engine::kWii)
    {
        const std::shared_ptr<const ntdlib::rsar::SoundArchive> archive =
            song::LoadWiiArchive(format::ExportArchive(file), "conformance", ntdlib::rvl::snd::kRsarOptions);
        auto sequence = std::make_shared<const song::Sequence>(
            song::ReadSequence(*archive, sound, {}, {}, format::BuildOfWiiArchive(*file.native)));
        model = std::make_unique<song::SequenceModel>(
            archive, std::move(sequence), song::ModelOptions{.effects = played, .snapshot_interval = kNoSnapshots});
    }
    else
    {
        const std::shared_ptr<const ntdlib::sar::SoundArchive> archive =
            song::LoadArchive(format::ExportArchive(file), "conformance", format::ExportGroupFiles(file));
        auto sequence = std::make_shared<const song::Sequence>(song::ReadSequence(*archive, sound));
        model = std::make_unique<song::SequenceModel>(
            archive, std::move(sequence),
            song::ModelOptions{
                .effects = played, .mixing = edit::ModelMixing(mixing), .snapshot_interval = kNoSnapshots});
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
        if (mixing == format::Mixing::kEnhanced && file.native->engine != format::Engine::kWii)
        {
            for (const float sample : model->FloatOutput())
            {
                put(song::ToPcm16(sample));
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
