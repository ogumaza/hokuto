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

#include "ntdlib/rsar/bank_file.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/wave_file.h"
#include "ntdlib/rsar/wsd_file.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/wave_file.h"
#include "ntdlib/sar/wave_sound_edit.h"
#include "ntdlib/wave/adpcm.h"
#include "ntdlib/wave/encode.h"
#include "xsflib/bytes.h"

#include "edit/files.h"
#include "format/file.h"
#include "format/kinds.h"
#include "song/items.h"

namespace hokuto::edit
{
namespace
{

namespace sar = ntdlib::sar;
namespace wave = ntdlib::wave;

constexpr std::size_t kFrameBytes = 8;

// The bytes of each sample of a PCM encoding.
std::size_t SampleBytes(sar::WaveEncoding encoding)
{
    return encoding == sar::WaveEncoding::kPcm8 ? 1 : 2;
}

// A PCM16 sample as the nearest PCM8 sample.
int8_t ToPcm8(int16_t sample)
{
    return static_cast<int8_t>(std::clamp((sample + 128) >> 8, -128, 127));
}

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
                made.samples[i] = static_cast<uint8_t>(ToPcm8(channel[i]));
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

// The models of the waves of wave archive file `file_id` in `from`, but for wave `wave`.
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
        throw std::invalid_argument("The wave holds " + std::to_string(wave.channels.size()) +
                                    (wave.channels.size() == 1 ? " channel" : " channels") + ", not 2.");
    }
}

// Throws std::invalid_argument with a message for the user for a model that format::WaveModelProblem refuses. A game's
// wave can loop from its end to its end. A model can't hold that loop, and the message says how to change it.
void RequireWaveModel(const format::WaveModel& model)
{
    const std::optional<std::string> problem = format::WaveModelProblem(model);
    if (!problem)
    {
        return;
    }

    const bool empty_loop = model.loop && model.loop_start >= model.loop_end;
    throw std::invalid_argument("The wave " + *problem + "." +
                                (empty_loop ? " Turn the loop off, or move its start before its end, first." : ""));
}

} // namespace

sar::WaveFile PresentWave(const sar::SoundArchive& archive, uint32_t wave_archive, uint32_t wave)
{
    try
    {
        sar::WaveFile present =
            sar::WaveFile::Read(song::ItemFile(archive, {song::ItemKind::kWave, wave_archive, wave}));
        if (present.channels.empty())
        {
            throw std::invalid_argument("It holds no channels.");
        }

        return present;
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Hokuto can't read the wave. ") + format::AsSentence(error.what()));
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
        throw std::invalid_argument("The wave holds " + std::to_string(HeldSamples(wave)) + " samples, not " +
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
        double signal = 0.0;
        double noise = 0.0;
        for (std::size_t i = 0; i < decoded.size(); ++i)
        {
            const double sample = mono[0][i];
            const double error = sample - decoded[i];
            signal += sample * sample;
            noise += error * error;
        }
        if (noise > 0.0 && signal > 0.0)
        {
            made.signal_to_noise = 10.0 * std::log10(signal / noise);
        }
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
            throw std::invalid_argument("The wave holds " + std::to_string(before.length) + " samples.");
        }
        if (adpcm && model.loop && model.loop_start % kDspAdpcmFrame != 0)
        {
            throw std::invalid_argument("A loop in DSP-ADPCM starts at a frame of 14 samples.");
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
                throw std::invalid_argument("The wave holds " + std::to_string(before.length) + " samples.");
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
        throw std::invalid_argument(std::string("The wave can't be written. ") + format::AsSentence(error.what()));
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
            throw std::invalid_argument("The wave sound file holds no note for the sound.");
        }

        fields.values = read.sounds[place.index];
        const uint32_t entry = fields.values.notes[0].wave_id_index;
        if (entry >= read.waves.size())
        {
            throw std::invalid_argument("The sound's note plays no wave of the file's table.");
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
        throw std::invalid_argument(std::string("Hokuto can't read the wave sound file. ") +
                                    format::AsSentence(error.what()));
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
        throw std::invalid_argument(std::string("The wave sound file can't take the values. ") +
                                    format::AsSentence(error.what()));
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Hokuto can't read the wave sound file. ") +
                                    format::AsSentence(error.what()));
    }

    return ReplaceFile(file, place.file_id, bytes);
}

namespace
{

namespace rsar = ntdlib::rsar;

// The values of the Wii's encodings in the other consoles' enumeration. The two give the three encodings the same
// values.
sar::WaveEncoding SarEncoding(rsar::WaveEncoding encoding)
{
    return static_cast<sar::WaveEncoding>(encoding);
}

rsar::WaveEncoding RsarEncoding(sar::WaveEncoding encoding)
{
    return static_cast<rsar::WaveEncoding>(encoding);
}

// The file of the Wii's archive whose wave data holds a wave archive in ntdlib's model of it: a bank or a wave sound
// file.
struct WaveHolder
{
    std::vector<rsar::WaveFile>& Waves()
    {
        return bank ? bank->waves : wave_sound_file->waves;
    }

    std::optional<rsar::BankFile> bank;
    std::optional<rsar::WsdFile> wave_sound_file;
};

WaveHolder HolderOf(const rsar::SoundArchive& archive, uint32_t file_id)
{
    const auto of_bank = [file_id](const rsar::BankInfo& bank)
    {
        return bank.file_id == file_id;
    };
    const auto of_wave_sound = [file_id](const rsar::SoundInfo& sound)
    {
        return sound.type == rsar::SoundType::kWaveSound && sound.file_id == file_id;
    };
    const bool bank = std::ranges::any_of(archive.Banks(), of_bank);
    if (!bank && !std::ranges::any_of(archive.Sounds(), of_wave_sound))
    {
        throw std::invalid_argument("File " + std::to_string(file_id) +
                                    " of the archive is neither a bank nor a wave sound file.");
    }
    const std::optional<rsar::FileData> data = archive.File(file_id);
    if (!data)
    {
        throw std::invalid_argument(bank ? "The archive lacks the bank's file or its wave data."
                                         : "The archive lacks the wave sound file or its wave data.");
    }

    WaveHolder holder;
    try
    {
        if (bank)
        {
            holder.bank = rsar::BankFile::Read(data->file, data->wave_data);
        }
        else
        {
            holder.wave_sound_file = rsar::WsdFile::Read(data->file, data->wave_data);
        }
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(
            std::string(bank ? "Hokuto can't read the bank. " : "Hokuto can't read the wave sound file. ") +
            format::AsSentence(error.what()));
    }

    return holder;
}

// The file `file_id` of `archive`, a bank or a wave sound file, read with `wave_data` in place of its wave data.
// Nothing if it doesn't read that way.
std::optional<WaveHolder> HolderWith(const rsar::SoundArchive& archive, uint32_t file_id,
                                     std::span<const uint8_t> wave_data)
{
    const bool bank =
        std::ranges::any_of(archive.Banks(), [file_id](const rsar::BankInfo& info) { return info.file_id == file_id; });
    const std::optional<rsar::FileData> data = archive.File(file_id);
    if (!data)
    {
        return std::nullopt;
    }

    WaveHolder holder;
    try
    {
        if (bank)
        {
            holder.bank = rsar::BankFile::Read(data->file, wave_data);
        }
        else
        {
            holder.wave_sound_file = rsar::WsdFile::Read(data->file, wave_data);
        }
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }

    return holder;
}

// `file` after every group that holds file `file_id` holds the file and the wave data that `holder` lays out.
format::File WithHolder(const format::File& file, uint32_t file_id, const WaveHolder& holder)
{
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> wave_data;
    try
    {
        if (holder.bank)
        {
            rsar::BankFile::Files files = holder.bank->Write();
            bytes = std::move(files.file);
            wave_data = std::move(files.wave_data);
        }
        else
        {
            rsar::WsdFile::Files files = holder.wave_sound_file->Write();
            bytes = std::move(files.file);
            wave_data = std::move(files.wave_data);
        }
    }
    catch (const std::invalid_argument& error)
    {
        throw std::invalid_argument(
            std::string(holder.bank ? "The bank can't be written. " : "The wave sound file can't be written. ") +
            format::AsSentence(error.what()));
    }

    return ReplaceFile(file, file_id, bytes, std::span<const uint8_t>(wave_data));
}

// `channels` in `encoding`, for a wave of the Wii. A DSP-ADPCM channel keeps the decoder's state at `loop_start`. Each
// channel keeps the volumes of the channel of `present` at its place.
std::vector<rsar::WaveFile::Channel> Encoded(const std::vector<std::vector<int16_t>>& channels,
                                             sar::WaveEncoding encoding, uint32_t loop_start,
                                             const std::vector<rsar::WaveFile::Channel>& present)
{
    std::vector<rsar::WaveFile::Channel> encoded;
    for (std::size_t c = 0; c < channels.size(); ++c)
    {
        const std::vector<int16_t>& channel = channels[c];
        rsar::WaveFile::Channel& made = encoded.emplace_back();
        if (c < present.size())
        {
            made.volumes = present[c].volumes;
        }
        if (encoding == sar::WaveEncoding::kDspAdpcm)
        {
            const wave::DspAdpcmChannel adpcm = wave::EncodeDspAdpcm(channel, loop_start, wave::DspAdpcmSum::kWide);
            rsar::DspAdpcmInfo info;
            info.coefs = adpcm.coefs;
            info.pred_scale = adpcm.pred_scale;
            info.loop_pred_scale = adpcm.loop_pred_scale;
            info.loop_yn1 = adpcm.loop_yn1;
            info.loop_yn2 = adpcm.loop_yn2;
            made.samples = adpcm.data;
            made.adpcm = info;
            continue;
        }

        // PCM. The Wii's PCM16 is big-endian.
        made.samples.resize(channel.size() * SampleBytes(encoding));
        for (std::size_t i = 0; i < channel.size(); ++i)
        {
            if (encoding == sar::WaveEncoding::kPcm8)
            {
                made.samples[i] = static_cast<uint8_t>(ToPcm8(channel[i]));
            }
            else
            {
                xsflib::StoreBe16(&made.samples[i * 2], static_cast<uint16_t>(channel[i]));
            }
        }
    }

    return encoded;
}

// Throws std::invalid_argument with a message for the user unless `waves` hold wave `wave` with channels.
void RequireWave(const std::vector<rsar::WaveFile>& waves, uint32_t wave)
{
    if (wave >= waves.size())
    {
        throw std::invalid_argument("The wave archive holds " + std::to_string(waves.size()) +
                                    (waves.size() == 1 ? " wave." : " waves."));
    }
    if (waves[wave].channels.empty())
    {
        throw std::invalid_argument("Hokuto can't read the wave. It holds no channels.");
    }
}

// The models of `from`'s waves of file `file_id` whose waves stay the same from `before` to `after`.
void KeepSameModels(const format::File& from, uint32_t file_id, const std::vector<rsar::WaveFile>& before,
                    const std::vector<rsar::WaveFile>& after, format::File& to)
{
    for (const auto& [place, model] : from.waves)
    {
        if (place.file == file_id && place.index < before.size() && place.index < after.size() &&
            before[place.index] == after[place.index])
        {
            to.waves.emplace(place, model);
        }
    }
}

} // namespace

std::vector<rsar::WaveFile> WavesOf(const rsar::SoundArchive& archive, uint32_t file_id)
{
    return HolderOf(archive, file_id).Waves();
}

rsar::WaveFile PresentWave(const rsar::SoundArchive& archive, uint32_t file_id, uint32_t wave)
{
    std::vector<rsar::WaveFile> waves = WavesOf(archive, file_id);
    RequireWave(waves, wave);

    return std::move(waves[wave]);
}

uint32_t HeldSamples(const rsar::WaveFile& wave)
{
    std::size_t held = SIZE_MAX;
    for (const rsar::WaveFile::Channel& channel : wave.channels)
    {
        const std::size_t samples = wave.encoding == rsar::WaveEncoding::kDspAdpcm
                                        ? channel.samples.size() / kFrameBytes * kDspAdpcmFrame
                                        : channel.samples.size() / SampleBytes(SarEncoding(wave.encoding));
        held = std::min(held, samples);
    }

    return wave.channels.empty() ? 0 : static_cast<uint32_t>(std::min<std::size_t>(held, UINT32_MAX));
}

std::vector<std::vector<int16_t>> DecodedSamples(const rsar::WaveFile& wave, uint32_t count)
{
    if (count > HeldSamples(wave))
    {
        throw std::invalid_argument("The wave holds " + std::to_string(HeldSamples(wave)) + " samples, not " +
                                    std::to_string(count) + ".");
    }

    std::vector<std::vector<int16_t>> channels;
    for (const rsar::WaveFile::Channel& channel : wave.channels)
    {
        std::vector<int16_t>& decoded = channels.emplace_back();
        if (wave.encoding == rsar::WaveEncoding::kDspAdpcm)
        {
            const rsar::DspAdpcmInfo info = channel.adpcm.value_or(rsar::DspAdpcmInfo());
            decoded =
                wave::DecodeDspAdpcm(channel.samples, count, info.coefs, info.yn1, info.yn2, wave::DspAdpcmSum::kWide);
            continue;
        }

        decoded.resize(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            decoded[i] = wave.encoding == rsar::WaveEncoding::kPcm8
                             ? static_cast<int16_t>(static_cast<int8_t>(channel.samples[i]) * 256)
                             : static_cast<int16_t>(xsflib::LoadBe16(&channel.samples[i * 2]));
        }
    }

    return channels;
}

ntdlib::wave::Pcm WavePcm(const rsar::WaveFile& wave)
{
    // The wave's length counts its samples, looping or not.
    ntdlib::wave::Pcm pcm;
    pcm.sample_rate = wave.sample_rate;
    pcm.loop_end = std::min(wave.loop_end, HeldSamples(wave));
    pcm.loop = wave.loop && wave.loop_start < pcm.loop_end;
    pcm.loop_start = pcm.loop ? wave.loop_start : 0;
    pcm.channels = DecodedSamples(wave, pcm.loop_end);
    pcm.channels.resize(std::min<std::size_t>(pcm.channels.size(), 2));

    return pcm;
}

format::WaveModel WaveModelOf(const format::File& file, const rsar::SoundArchive& archive, uint32_t file_id,
                              uint32_t wave)
{
    if (const auto edited = file.waves.find({file_id, wave}); edited != file.waves.end())
    {
        return *edited->second;
    }

    const rsar::WaveFile present = PresentWave(archive, file_id, wave);
    format::WaveModel model;
    model.encoding = SarEncoding(present.encoding);
    model.rate = present.sample_rate;
    model.loop = present.loop;
    model.loop_start = present.loop ? present.loop_start : 0;
    model.loop_end = present.loop_end;
    model.length = present.loop_end;

    return model;
}

format::File WithWaveModel(const format::File& file, const rsar::SoundArchive& archive, uint32_t file_id, uint32_t wave,
                           format::WaveModel model)
{
    RequireWaveModel(model);

    WaveHolder holder = HolderOf(archive, file_id);
    std::vector<rsar::WaveFile>& waves = holder.Waves();
    RequireWave(waves, wave);
    const rsar::WaveFile& present = waves[wave];
    const format::WaveModel before = WaveModelOf(file, archive, file_id, wave);
    const bool adpcm = model.encoding == sar::WaveEncoding::kDspAdpcm;
    rsar::WaveFile made = present;
    made.sample_rate = model.rate;
    made.loop = model.loop;
    if (model.encoding == SarEncoding(present.encoding) && model.samples == before.samples)
    {
        // The wave keeps its encoded samples. A loop that stays keeps the decoder's state at its start. A loop that
        // moves in DSP-ADPCM starts again with the decoder's state at its start, on a frame.
        if (model.length != before.length)
        {
            throw std::invalid_argument("The wave holds " + std::to_string(before.length) + " samples.");
        }
        const bool moved = model.loop && (!present.loop || model.loop_start != present.loop_start);
        if (adpcm && moved && model.loop_start % kDspAdpcmFrame != 0)
        {
            throw std::invalid_argument("A loop in DSP-ADPCM starts at a frame of 14 samples.");
        }

        made.loop_start = model.loop ? model.loop_start : present.loop_start;
        made.loop_end = model.loop_end;
        for (rsar::WaveFile::Channel& channel : made.channels)
        {
            if (adpcm && moved && channel.adpcm)
            {
                const wave::DspAdpcmState state =
                    wave::DspAdpcmStateAt(channel.samples, model.loop_start, channel.adpcm->coefs, channel.adpcm->yn1,
                                          channel.adpcm->yn2, wave::DspAdpcmSum::kWide);
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
                throw std::invalid_argument("The wave holds " + std::to_string(before.length) + " samples.");
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

        made.encoding = RsarEncoding(model.encoding);
        made.loop_start = model.loop_start;
        made.loop_end = model.loop_end;
        made.channels = Encoded(model.samples, model.encoding, model.loop_start, present.channels);
    }

    // The file with the new wave, in every group that holds it. The other waves keep their bytes and their models.
    waves[wave] = std::move(made);
    format::File result = WithHolder(file, file_id, holder);
    KeepOtherModels(file, file_id, wave, result);
    result.waves[{file_id, wave}] = std::move(model);

    return result;
}

format::File WithWaves(const format::File& file, const rsar::SoundArchive& archive, uint32_t file_id,
                       std::vector<rsar::WaveFile> waves)
{
    // A WAVE block can describe other waves than the wave data holds, such as after Replace… of a bank of version 1.1
    // alone. The file then reads with the new waves, laid out as the tools lay them out. No model of a wave stays.
    WaveHolder holder;
    std::vector<rsar::WaveFile> before;
    try
    {
        holder = HolderOf(archive, file_id);
        before = holder.Waves();
    }
    catch (const std::invalid_argument&)
    {
        const std::optional<WaveHolder> read = HolderWith(archive, file_id, rsar::RawWaveData(waves));
        if (!read)
        {
            throw;
        }
        holder = *read;
    }
    holder.Waves() = std::move(waves);
    format::File result = WithHolder(file, file_id, holder);
    KeepSameModels(file, file_id, before, holder.Waves(), result);

    return result;
}

WaveSoundFields WaveSoundFieldsOf(const rsar::SoundArchive& archive, uint32_t sound)
{
    if (sound >= archive.Sounds().size() || archive.Sounds()[sound].type != rsar::SoundType::kWaveSound)
    {
        throw std::invalid_argument("The sound isn't a wave sound.");
    }

    const rsar::SoundInfo& info = archive.Sounds()[sound];
    const WaveHolder holder = HolderOf(archive, info.file_id);
    const rsar::WsdFile& file = *holder.wave_sound_file;
    if (info.sub_index < 0 || static_cast<std::size_t>(info.sub_index) >= file.sounds.size() ||
        file.sounds[static_cast<std::size_t>(info.sub_index)].notes.empty())
    {
        throw std::invalid_argument("The wave sound file holds no note for the sound.");
    }
    const rsar::WsdFile::WaveSound& read = file.sounds[static_cast<std::size_t>(info.sub_index)];
    const rsar::WsdFile::Note& note = read.notes.front();
    if (note.wave_index < 0 || static_cast<std::size_t>(note.wave_index) >= file.waves.size())
    {
        throw std::invalid_argument("The sound's note plays no wave of the file's wave data.");
    }

    // The sound's settings, and its first note's envelope, root key and wave.
    WaveSoundFields fields;
    fields.values.pan = read.pan;
    fields.values.span = read.surround_pan;
    fields.values.pitch = read.pitch;
    fields.values.main_send = read.main_send;
    fields.values.aux_sends = read.fx_sends;
    fields.values.adshr = {note.attack, note.decay, note.sustain, note.hold, note.release};
    sar::VelocityRegion& region = fields.values.notes.emplace_back();
    region.wave_id_index = static_cast<uint32_t>(note.wave_index);
    region.original_key = note.original_key;
    region.volume = note.volume;
    region.pan = note.pan;
    region.pitch = note.pitch;
    region.adshr = fields.values.adshr;
    fields.places = {.pan = true, .pitch = true, .filters = false, .sends = true, .aux_sends = 3, .envelope = true};
    fields.wave = {sar::ItemId(sar::kWaveArchiveItem, info.file_id), static_cast<uint32_t>(note.wave_index)};

    return fields;
}

format::File WithWaveSoundFields(const format::File& file, const rsar::SoundArchive& archive, uint32_t sound,
                                 const sar::WaveSoundFile::WaveSound& values, sar::WaveId wave)
{
    const WaveSoundFields present = WaveSoundFieldsOf(archive, sound);
    if (values.low_pass != present.values.low_pass || values.biquad_type != present.values.biquad_type ||
        values.biquad_value != present.values.biquad_value)
    {
        throw std::invalid_argument("A wave sound of the Wii has no filters.");
    }
    const rsar::SoundInfo& info = archive.Sounds()[sound];
    if (wave.wave_archive_item != sar::ItemId(sar::kWaveArchiveItem, info.file_id))
    {
        throw std::invalid_argument("A wave sound of the Wii plays a wave of its wave sound file's wave data.");
    }

    // The settings and the first note's envelope and wave. The waves stay, and keep their models.
    WaveHolder holder = HolderOf(archive, info.file_id);
    rsar::WsdFile& made = *holder.wave_sound_file;
    if (wave.index >= made.waves.size())
    {
        throw std::invalid_argument("The wave sound file's wave data holds " + std::to_string(made.waves.size()) +
                                    (made.waves.size() == 1 ? " wave." : " waves."));
    }
    rsar::WsdFile::WaveSound& target = made.sounds[static_cast<std::size_t>(info.sub_index)];
    target.pan = values.pan;
    target.surround_pan = values.span;
    target.pitch = values.pitch;
    target.main_send = values.main_send;
    target.fx_sends = values.aux_sends;
    rsar::WsdFile::Note& note = target.notes.front();
    note.attack = values.adshr.attack;
    note.decay = values.adshr.decay;
    note.sustain = values.adshr.sustain;
    note.hold = values.adshr.hold;
    note.release = values.adshr.release;
    note.wave_index = static_cast<int32_t>(wave.index);
    format::File result = WithHolder(file, info.file_id, holder);
    KeepOtherModels(file, info.file_id, std::nullopt, result);

    return result;
}

} // namespace hokuto::edit
