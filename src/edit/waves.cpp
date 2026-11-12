// SPDX-License-Identifier: MIT

#include "edit/waves.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/wave_file.h"
#include "ntdlib/sar/wave_sound_edit.h"
#include "ntdlib/wave/adpcm.h"
#include "ntdlib/wave/encode.h"
#include "ntdlib/wave/pcm.h"
#include "xsflib/bytes.h"

#include "edit/files.h"
#include "edit/files_internal.h"
#include "edit/waves_parts.h"
#include "format/file.h"
#include "format/kinds.h"
#include "song/items.h"

namespace hokuto::edit
{
namespace
{

namespace sar = ntdlib::sar;
namespace wave = ntdlib::wave;
using waves_parts::KeepOtherModels;
using waves_parts::kFrameBytes;
using waves_parts::RequireWaveModel;
using waves_parts::SampleBytes;

// `channels` in `encoding`, for a wave file of `flavor`. A DSP-ADPCM channel keeps the decoder's state at `loop_start`.
std::vector<sar::WaveFile::Channel> Encoded(const std::vector<std::vector<int16_t>>& channels,
                                            sar::WaveEncoding encoding, sar::Flavor flavor, uint32_t loop_start)
{
    std::vector<sar::WaveFile::Channel> encoded;
    for (const std::vector<int16_t>& channel : channels)
    {
        sar::WaveFile::Channel& made = encoded.emplace_back();
        if (encoding == sar::WaveEncoding::kDspAdpcm)
        {
            const wave::DspAdpcmChannel adpcm = wave::EncodeDspAdpcm(channel, loop_start, wave::DspAdpcmSum::kWrap);
            sar::DspAdpcmInfo info;
            info.coefs = adpcm.coefs;
            info.pred_scale = adpcm.pred_scale;
            info.loop_pred_scale = adpcm.loop_pred_scale;
            info.loop_yn1 = adpcm.loop_yn1;
            info.loop_yn2 = adpcm.loop_yn2;
            made.samples = adpcm.data;
            made.adpcm = info;
            continue;
        }

        // PCM, in the byte order of the console's files.
        made.samples.resize(channel.size() * SampleBytes(encoding));
        for (std::size_t i = 0; i < channel.size(); ++i)
        {
            if (encoding == sar::WaveEncoding::kPcm8)
            {
                made.samples[i] = wave::EncodePcm8(channel[i]);
            }
            else if (flavor == sar::Flavor::kCafe)
            {
                xsflib::StoreBe16(&made.samples[i * 2], static_cast<uint16_t>(channel[i]));
            }
            else
            {
                xsflib::StoreLe16(&made.samples[i * 2], static_cast<uint16_t>(channel[i]));
            }
        }
    }

    return encoded;
}

// The wave sound file of `sound` of `archive`, its file ID and the sound's index in it, or a message for the user.
struct WaveSoundPlace
{
    std::span<const uint8_t> bytes;
    uint32_t file_id = 0;
    uint32_t index = 0;
};

WaveSoundPlace WaveSoundPlaceOf(const sar::SoundArchive& archive, uint32_t sound)
{
    WaveSoundPlace place;
    place.file_id = FileIdOf(archive, FileKind::kWaveSoundFile, sound);
    place.index = archive.Sounds()[sound].wave_sound_index;
    try
    {
        place.bytes = song::ItemFile(archive, {song::ItemKind::kWaveSound, sound, 0});
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(error.what());
    }

    return place;
}

// Throws std::invalid_argument unless `wave` holds two channels.
void RequireTwoChannels(const sar::WaveFile& wave)
{
    if (wave.channels.size() != 2)
    {
        throw std::invalid_argument("The wave isn't stereo. It has " + std::to_string(wave.channels.size()) +
                                    (wave.channels.size() == 1 ? " channel." : " channels."));
    }
}

} // namespace

namespace waves_parts
{

std::size_t SampleBytes(sar::WaveEncoding encoding)
{
    return encoding == sar::WaveEncoding::kPcm8 ? 1 : 2;
}

void KeepOtherModels(const format::File& from, uint32_t file_id, std::optional<uint32_t> wave, format::File& to)
{
    for (const auto& [place, model] : from.waves)
    {
        if (place.file == file_id && place.index != wave)
        {
            to.waves.emplace(place, model);
        }
    }
}

void RequireWaveModel(const format::WaveModel& model)
{
    const std::optional<std::string> problem = format::WaveModelProblem(model);
    if (!problem)
    {
        return;
    }

    const bool empty_loop = model.loop && model.loop_start >= model.loop_end;
    throw std::invalid_argument("The wave " + *problem + "." +
                                (empty_loop ? " Turn off the loop, or move the loop start before the loop end." : ""));
}

} // namespace waves_parts

sar::WaveFile PresentWave(const sar::SoundArchive& archive, uint32_t wave_archive, uint32_t wave)
{
    try
    {
        sar::WaveFile present =
            sar::WaveFile::Read(song::ItemFile(archive, {song::ItemKind::kWave, wave_archive, wave}));
        if (present.channels.empty())
        {
            throw std::invalid_argument("It has no channels.");
        }

        return present;
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Can't read the wave. ") + format::AsSentence(error.what()));
    }
}

uint32_t HeldSamples(const sar::WaveFile& wave)
{
    std::size_t held = SIZE_MAX;
    for (const sar::WaveFile::Channel& channel : wave.channels)
    {
        const std::size_t samples = wave.encoding == sar::WaveEncoding::kDspAdpcm
                                        ? channel.samples.size() / kFrameBytes * kDspAdpcmFrame
                                        : channel.samples.size() / SampleBytes(wave.encoding);
        held = std::min(held, samples);
    }

    return wave.channels.empty() ? 0 : static_cast<uint32_t>(std::min<std::size_t>(held, UINT32_MAX));
}

std::vector<std::vector<int16_t>> DecodedSamples(const sar::WaveFile& wave, uint32_t count)
{
    if (count > HeldSamples(wave))
    {
        throw std::invalid_argument("The wave has only " + std::to_string(HeldSamples(wave)) + " samples, not " +
                                    std::to_string(count) + ".");
    }

    std::vector<std::vector<int16_t>> channels;
    for (const sar::WaveFile::Channel& channel : wave.channels)
    {
        std::vector<int16_t>& decoded = channels.emplace_back();
        if (wave.encoding == sar::WaveEncoding::kDspAdpcm)
        {
            const sar::DspAdpcmInfo info = channel.adpcm.value_or(sar::DspAdpcmInfo());
            decoded =
                wave::DecodeDspAdpcm(channel.samples, count, info.coefs, info.yn1, info.yn2, wave::DspAdpcmSum::kWrap);
            continue;
        }

        decoded.resize(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            if (wave.encoding == sar::WaveEncoding::kPcm8)
            {
                decoded[i] = static_cast<int16_t>(static_cast<int8_t>(channel.samples[i]) * 256);
            }
            else
            {
                const uint8_t* at = &channel.samples[i * 2];
                decoded[i] = static_cast<int16_t>(wave.flavor == sar::Flavor::kCafe ? xsflib::LoadBe16(at)
                                                                                    : xsflib::LoadLe16(at));
            }
        }
    }

    return channels;
}

MonoWave MonoWaveOf(const sar::WaveFile& wave, double gain)
{
    RequireTwoChannels(wave);

    const uint32_t length = std::min(wave.loop_end, HeldSamples(wave));
    const std::vector<std::vector<int16_t>> channels = DecodedSamples(wave, length);
    std::vector<std::vector<int16_t>> mono(1, std::vector<int16_t>(length));
    for (uint32_t i = 0; i < length; ++i)
    {
        const double mean = (static_cast<double>(channels[0][i]) + channels[1][i]) * 0.5 * gain;
        mono[0][i] = static_cast<int16_t>(std::clamp(std::lround(mean), -32768L, 32767L));
    }

    // A loop in DSP-ADPCM starts at a frame. The wave then ends at its loop's end.
    MonoWave made{wave, std::nullopt};
    const bool adpcm = wave.encoding == sar::WaveEncoding::kDspAdpcm;
    if (adpcm && wave.loop && wave.loop_start % kDspAdpcmFrame != 0)
    {
        made.wave.loop_start = wave::AlignDspAdpcmLoop(mono, wave.loop_start);
        made.wave.loop_end = static_cast<uint32_t>(mono[0].size());
    }

    made.wave.channels = Encoded(mono, wave.encoding, wave.flavor, made.wave.loop ? made.wave.loop_start : 0);
    if (adpcm)
    {
        const sar::DspAdpcmInfo& info = *made.wave.channels[0].adpcm;
        const std::vector<int16_t> decoded =
            wave::DecodeDspAdpcm(made.wave.channels[0].samples, static_cast<uint32_t>(mono[0].size()), info.coefs,
                                 info.yn1, info.yn2, wave::DspAdpcmSum::kWrap);
        NoiseSums sums;
        AddNoise(mono[0], decoded, sums);
        made.signal_to_noise = SignalToNoise(sums);
    }

    return made;
}

std::size_t MonoWaveBytes(const sar::WaveFile& wave)
{
    RequireTwoChannels(wave);

    // The samples of MonoWaveOf's channel. A loop in DSP-ADPCM that moves to the next frame repeats the samples between
    // after the loop's end.
    sar::WaveFile made = wave;
    std::size_t samples = std::min(wave.loop_end, HeldSamples(wave));
    const bool adpcm = wave.encoding == sar::WaveEncoding::kDspAdpcm;
    if (adpcm && wave.loop && wave.loop_start % kDspAdpcmFrame != 0)
    {
        const uint32_t aligned = (wave.loop_start + kDspAdpcmFrame - 1) / kDspAdpcmFrame * kDspAdpcmFrame;
        samples += aligned - wave.loop_start;
        made.loop_start = aligned;
        made.loop_end = static_cast<uint32_t>(samples);
    }

    const std::size_t bytes = adpcm ? wave::DspAdpcmBytes(samples) : samples * SampleBytes(wave.encoding);
    made.channels = {
        {std::vector<uint8_t>(bytes), adpcm ? std::optional<sar::DspAdpcmInfo>(sar::DspAdpcmInfo()) : std::nullopt}};

    return made.Write().size();
}

MonoLevel MonoLevelOf(const sar::WaveFile& wave)
{
    RequireTwoChannels(wave);

    const uint32_t length = std::min(wave.loop_end, HeldSamples(wave));
    const std::vector<std::vector<int16_t>> channels = DecodedSamples(wave, length);
    MonoLevel level;
    for (uint32_t i = 0; i < length; ++i)
    {
        const double left = channels[0][i];
        const double right = channels[1][i];
        level.left += left * left;
        level.right += right * right;
        level.both += left * right;
        level.peak = std::max(level.peak, static_cast<int>(std::abs(std::lround((left + right) * 0.5))));
    }

    if (length > 0)
    {
        level.left /= length;
        level.right /= length;
        level.both /= length;
    }

    return level;
}

format::WaveModel WaveModelOf(const format::File& file, const sar::SoundArchive& archive, uint32_t wave_archive,
                              uint32_t wave)
{
    const uint32_t file_id = FileIdOf(archive, FileKind::kWaveArchive, wave_archive);
    if (const auto edited = file.waves.find({file_id, wave}); edited != file.waves.end())
    {
        return *edited->second;
    }

    const sar::WaveFile present = PresentWave(archive, wave_archive, wave);
    format::WaveModel model;
    model.encoding = present.encoding;
    model.rate = present.sample_rate;
    model.loop = present.loop;
    model.loop_start = present.loop ? present.loop_start : 0;
    model.loop_end = present.loop_end;
    model.length = present.loop_end;

    return model;
}

format::File WithWaveModel(const format::File& file, const sar::SoundArchive& archive, uint32_t wave_archive,
                           uint32_t wave, format::WaveModel model)
{
    RequireWaveModel(model);

    const uint32_t file_id = FileIdOf(archive, FileKind::kWaveArchive, wave_archive);
    const sar::WaveFile present = PresentWave(archive, wave_archive, wave);
    const format::WaveModel before = WaveModelOf(file, archive, wave_archive, wave);
    const bool adpcm = model.encoding == sar::WaveEncoding::kDspAdpcm;
    sar::WaveFile made = present;
    made.sample_rate = model.rate;
    made.loop = model.loop;
    made.original_loop_start = model.loop_start;
    if (model.encoding == present.encoding && model.samples == before.samples)
    {
        // The wave keeps its encoded samples. A loop in DSP-ADPCM starts again with the decoder's state at its start.
        if (model.length != before.length)
        {
            throw std::invalid_argument("The wave length must stay at " + std::to_string(before.length) + " samples.");
        }
        if (adpcm && model.loop && model.loop_start % kDspAdpcmFrame != 0)
        {
            throw std::invalid_argument("In DSP-ADPCM, a loop must start on a frame of 14 samples.");
        }

        made.loop_start = model.loop_start;
        made.loop_end = model.loop_end;
        for (sar::WaveFile::Channel& channel : made.channels)
        {
            if (adpcm && model.loop && channel.adpcm)
            {
                const wave::DspAdpcmState state =
                    wave::DspAdpcmStateAt(channel.samples, model.loop_start, channel.adpcm->coefs, channel.adpcm->yn1,
                                          channel.adpcm->yn2, wave::DspAdpcmSum::kWrap);
                channel.adpcm->loop_pred_scale = state.pred_scale;
                channel.adpcm->loop_yn1 = state.yn1;
                channel.adpcm->loop_yn2 = state.yn2;
            }
        }
    }
    else
    {
        // The wave from PCM16 samples: the model's, or the wave's decoded. A loop in DSP-ADPCM moves to a frame, and
        // the wave then ends at its loop's end.
        if (model.samples.empty())
        {
            if (model.length != before.length)
            {
                throw std::invalid_argument("The wave length must stay at " + std::to_string(before.length) +
                                            " samples.");
            }

            model.samples = DecodedSamples(present, before.length);
        }

        if (adpcm && model.loop && model.loop_start % kDspAdpcmFrame != 0)
        {
            for (std::vector<int16_t>& channel : model.samples)
            {
                channel.resize(model.loop_end);
            }

            model.loop_start = wave::AlignDspAdpcmLoop(model.samples, model.loop_start);
            model.loop_end = static_cast<uint32_t>(model.samples[0].size());
            model.length = model.loop_end;
        }

        made.encoding = model.encoding;
        made.loop_start = model.loop_start;
        made.loop_end = model.loop_end;
        made.channels = Encoded(model.samples, model.encoding, present.flavor, model.loop_start);
    }

    // The wave archive with the new wave, in every copy. The other waves keep their bytes and their models.
    std::vector<uint8_t> wave_bytes;
    try
    {
        wave_bytes = made.Write();
    }
    catch (const std::invalid_argument& error)
    {
        throw std::invalid_argument(std::string("Can't write the wave. ") + format::AsSentence(error.what()));
    }

    format::File result = ReplaceFile(file, file_id, WaveArchiveWith(archive, wave_archive, wave, wave_bytes));
    KeepOtherModels(file, file_id, wave, result);
    result.waves[{file_id, wave}] = std::move(model);

    return result;
}

format::File WithIncomingWave(const format::File& file, const sar::SoundArchive& archive, uint32_t wave_archive,
                              std::optional<uint32_t> wave, const IncomingWave& incoming)
{
    if (!wave)
    {
        return WithNewWaves(file, archive, wave_archive, {incoming});
    }

    const uint32_t file_id = FileIdOf(archive, FileKind::kWaveArchive, wave_archive);
    format::File result = ReplaceFile(file, file_id, WaveArchiveWith(archive, wave_archive, wave, incoming.bytes));
    KeepOtherModels(file, file_id, wave, result);
    if (incoming.model)
    {
        result.waves[{file_id, *wave}] = *incoming.model;
    }

    return result;
}

format::File WithNewWaves(const format::File& file, const sar::SoundArchive& archive, uint32_t wave_archive,
                          const std::vector<IncomingWave>& incoming)
{
    const uint32_t file_id = FileIdOf(archive, FileKind::kWaveArchive, wave_archive);
    const auto first = static_cast<uint32_t>(song::WaveCountOf(archive, wave_archive));
    std::vector<std::vector<uint8_t>> wave_files;
    for (const IncomingWave& wave : incoming)
    {
        wave_files.push_back(wave.bytes);
    }

    format::File result = ReplaceFile(file, file_id, WaveArchiveWithNew(archive, wave_archive, wave_files));
    KeepOtherModels(file, file_id, std::nullopt, result);
    for (std::size_t k = 0; k < incoming.size(); ++k)
    {
        if (incoming[k].model)
        {
            result.waves[{file_id, first + static_cast<uint32_t>(k)}] = *incoming[k].model;
        }
    }

    return result;
}

WaveSoundFields WaveSoundFieldsOf(const sar::SoundArchive& archive, uint32_t sound)
{
    const WaveSoundPlace place = WaveSoundPlaceOf(archive, sound);
    const sar::Flavor flavor = archive.ReadOptions().flavor;
    WaveSoundFields fields;
    try
    {
        const sar::WaveSoundFile read = sar::WaveSoundFile::Parse(place.bytes, song::ItemOptions(archive));
        if (place.index >= read.sounds.size() || read.sounds[place.index].notes.empty())
        {
            throw std::invalid_argument("The wave sound file has no note for this sound.");
        }

        fields.values = read.sounds[place.index];
        const uint32_t entry = fields.values.notes[0].wave_id_index;
        if (entry >= read.waves.size())
        {
            throw std::invalid_argument(
                "The note of this sound doesn't refer to a wave in the wave table of the file.");
        }

        fields.wave = read.waves[entry];
        fields.places = sar::WaveSoundValuePlaces(place.bytes, flavor, place.index);
    }
    catch (const std::invalid_argument&)
    {
        throw;
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Can't read the wave sound file. ") + format::AsSentence(error.what()));
    }

    return fields;
}

format::File WithWaveSoundFields(const format::File& file, const sar::SoundArchive& archive, uint32_t sound,
                                 const sar::WaveSoundFile::WaveSound& values, sar::WaveId wave)
{
    const WaveSoundPlace place = WaveSoundPlaceOf(archive, sound);
    const sar::Flavor flavor = archive.ReadOptions().flavor;
    std::vector<uint8_t> bytes;
    try
    {
        bytes = sar::WithWaveSoundValues(place.bytes, flavor, place.index, values);
        bytes = sar::WithWaveSoundWave(bytes, flavor, place.index, 0, wave);
    }
    catch (const std::invalid_argument& error)
    {
        throw std::invalid_argument(std::string("Can't apply these values to the wave sound file. ") +
                                    format::AsSentence(error.what()));
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Can't read the wave sound file. ") + format::AsSentence(error.what()));
    }

    return ReplaceFile(file, place.file_id, bytes);
}

} // namespace hokuto::edit
