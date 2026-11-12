// SPDX-License-Identifier: MIT

// The helpers that the checks of hokuto_check_banks share (check_banks.h).

#include "check_banks.h"

namespace hokuto::tools::banks
{

void Problem(Totals& totals, const std::string& where, const std::string& what)
{
    std::printf("%s: %s\n", where.c_str(), what.c_str());
    totals.problems++;
}

std::vector<float> Rendered(song::NotePreview& preview)
{
    const auto frames = static_cast<std::size_t>(kNoteSeconds * preview.SampleRate());
    std::vector<float> samples(frames * 2);
    std::size_t done = 0;
    while (done < frames)
    {
        const std::size_t block = std::min<std::size_t>(1024, frames - done);
        const std::size_t rendered = preview.Render(samples.data() + done * 2, block);
        done += rendered;
        if (rendered < block)
        {
            break;
        }
    }
    samples.resize(done * 2);

    return samples;
}

float Peak(std::span<const float> samples)
{
    float peak = 0.0F;
    for (const float sample : samples)
    {
        peak = std::max(peak, std::abs(sample));
    }

    return peak;
}

double Level(std::span<const float> samples)
{
    double sum = 0.0;
    for (const float sample : samples)
    {
        sum += static_cast<double>(sample) * sample;
    }

    return sum > 0.0 ? 10.0 * std::log10(sum / static_cast<double>(samples.size())) : -INFINITY;
}

double SignalToNoise(const std::vector<std::vector<int16_t>>& expected, const std::vector<std::vector<int16_t>>& heard,
                     uint32_t length)
{
    double signal = 0.0;
    double noise = 0.0;
    for (std::size_t c = 0; c < expected.size() && c < heard.size(); ++c)
    {
        for (uint32_t i = 0; i < length && i < expected[c].size() && i < heard[c].size(); ++i)
        {
            const double difference = static_cast<double>(heard[c][i]) - expected[c][i];
            signal += static_cast<double>(expected[c][i]) * expected[c][i];
            noise += difference * difference;
        }
    }

    return noise > 0.0 ? 10.0 * std::log10(signal / noise) : INFINITY;
}

bool SameStart(const std::vector<std::vector<int16_t>>& a, const std::vector<std::vector<int16_t>>& b, uint32_t length)
{
    if (a.size() != b.size())
    {
        return false;
    }

    for (std::size_t c = 0; c < a.size(); ++c)
    {
        if (a[c].size() < length || b[c].size() < length ||
            !std::equal(a[c].begin(), a[c].begin() + length, b[c].begin()))
        {
            return false;
        }
    }

    return true;
}

edit::StreamValues LoopedStreamValues(const edit::StreamValues& values)
{
    const uint32_t block = sar::StreamBlockSamples(values.encoding);
    edit::StreamValues looped = values;
    looped.rate = std::max<uint32_t>(static_cast<uint32_t>(values.rate * kShare), 1);
    looped.loop = true;
    looped.loop_start = (values.length / 2 + block - 1) / block * block;
    if (looped.loop_start >= values.length)
    {
        looped.loop_start = 0;
    }

    return looped;
}

format::WaveModel LoopedWaveModel(const format::WaveModel& model)
{
    const bool adpcm = model.encoding == sar::WaveEncoding::kDspAdpcm;
    format::WaveModel looped = model;
    looped.rate = std::max<uint32_t>(static_cast<uint32_t>(model.rate * kShare), 1);
    looped.loop = true;
    looped.loop_end = static_cast<uint32_t>(model.length * kShare);
    looped.loop_start = std::min(Nearest(looped.loop_end / 2.0, adpcm ? edit::kDspAdpcmFrame : 1), looped.loop_end - 1);
    looped.loop_start -= adpcm ? looped.loop_start % edit::kDspAdpcmFrame : 0;

    return looped;
}

sar::WaveEncoding OtherEncoding(sar::WaveEncoding encoding)
{
    return encoding == sar::WaveEncoding::kDspAdpcm ? sar::WaveEncoding::kPcm16 : sar::WaveEncoding::kDspAdpcm;
}

bool KeepsValues(const ntdlib::wave::Pcm& before, const ntdlib::wave::Pcm& after, uint32_t slack)
{
    if (after.sample_rate != before.sample_rate || after.channels.size() != before.channels.size() ||
        after.loop != before.loop || after.loop_start < before.loop_start)
    {
        return false;
    }

    const uint32_t moved = after.loop_start - before.loop_start;
    return moved < slack && after.loop_end == before.loop_end + moved;
}

LoopDecoding LoopDecodingOf(sar::Flavor flavor)
{
    return {flavor == sar::Flavor::kCtr ? ntdlib::wave::DspAdpcmSum::kWide : ntdlib::wave::DspAdpcmSum::kWrap,
            flavor == sar::Flavor::kCafe};
}

sar::DspAdpcmInfo InfoOf(const sar::DspAdpcmInfo& info)
{
    return info;
}

sar::DspAdpcmInfo InfoOf(const rsar::DspAdpcmInfo& info)
{
    return {info.coefs, info.pred_scale, info.yn1, info.yn2, info.loop_pred_scale, info.loop_yn1, info.loop_yn2};
}

bool LaterPassesMatch(const std::vector<AdpcmChannel>& channels, const std::vector<std::vector<int16_t>>& first,
                      uint32_t loop_start, uint32_t loop_end, const LoopDecoding& decoding)
{
    for (std::size_t c = 0; c < channels.size() && c < first.size(); ++c)
    {
        if (!channels[c].second)
        {
            return false;
        }

        const std::vector<int16_t> later =
            LaterPass(channels[c].first, *channels[c].second, loop_start, loop_end, decoding);
        if (!std::equal(later.begin(), later.end(), first[c].begin() + loop_start))
        {
            return false;
        }
    }

    return true;
}

sar::BankFile Halved(sar::BankFile bank)
{
    for (std::optional<sar::BankFile::Instrument>& instrument : bank.instruments)
    {
        if (!instrument)
        {
            continue;
        }

        for (sar::BankFile::KeyRegion& keys : instrument->key_regions)
        {
            for (sar::BankFile::Region& region : keys.regions)
            {
                region.values.volume = static_cast<uint8_t>((region.values.volume + 1) / 2);
            }
        }
    }

    return bank;
}

std::pair<sar::BankFile, std::vector<std::pair<uint32_t, uint32_t>>> WithCopies(sar::BankFile bank)
{
    std::vector<std::pair<uint32_t, uint32_t>> copies;
    const auto programs = static_cast<uint32_t>(bank.instruments.size());
    for (uint32_t program = 0; program < programs; ++program)
    {
        if (bank.instruments[program])
        {
            const auto copy = static_cast<uint32_t>(bank.instruments.size());
            bank = edit::WithInstrumentCopy(std::move(bank), program, copy);
            copies.emplace_back(program, copy);
        }
    }

    return {std::move(bank), copies};
}

uint32_t Nearest(double target, uint32_t step)
{
    return static_cast<uint32_t>(std::lround(target / step)) * step;
}

bool SameFields(const edit::WaveSoundFields& a, const edit::WaveSoundFields& b)
{
    return a.values.pan == b.values.pan && a.values.span == b.values.span && a.values.pitch == b.values.pitch &&
           a.values.main_send == b.values.main_send && a.values.aux_sends == b.values.aux_sends &&
           a.values.low_pass == b.values.low_pass && a.values.biquad_type == b.values.biquad_type &&
           a.values.biquad_value == b.values.biquad_value && a.values.adshr == b.values.adshr && a.wave == b.wave;
}

uint8_t Moved(uint8_t value, int step)
{
    return static_cast<uint8_t>((value + step) % 128);
}

} // namespace hokuto::tools::banks
