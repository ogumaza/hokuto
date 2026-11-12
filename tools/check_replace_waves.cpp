// SPDX-License-Identifier: MIT

// The checks of waves of hokuto_check_replace.

#include "check_replace.h"
#include "later_passes.h"

namespace hokuto::tools::replace
{

namespace
{

// How hokuto_check_replace decodes a loop's later passes on `flavor`, with sums that wrap. The Switch reads the
// predictor and scale from the frame's header when the loop starts at a frame's start. The Wii U takes them from the
// context there too.
LoopDecoding ReplaceLoopDecoding(sar::Flavor flavor)
{
    return {ntdlib::wave::DspAdpcmSum::kWrap, flavor != sar::Flavor::kNx};
}

// Counts how the later passes of a looped DSP-ADPCM wave, `wave`, differ from its first pass, `pcm`. A WAV file holds
// the first pass, and a wave from it plays the first pass's samples in every pass.
void CountLaterPasses(std::span<const uint8_t> wave, const sar::Pcm& pcm, sar::Flavor flavor, Totals& totals)
{
    const sar::WaveFile parsed = sar::WaveFile::Read(wave);
    if (parsed.encoding != sar::WaveEncoding::kDspAdpcm || !pcm.loop)
    {
        return;
    }

    int peak = 0;
    uint32_t last = 0;
    for (std::size_t c = 0; c < pcm.channels.size(); c++)
    {
        const std::vector<int16_t> later = LaterPass(parsed.channels[c].samples, *parsed.channels[c].adpcm,
                                                     pcm.loop_start, pcm.loop_end, ReplaceLoopDecoding(flavor));
        for (std::size_t i = 0; i < later.size(); i++)
        {
            const int difference = std::abs(later[i] - pcm.channels[c][pcm.loop_start + i]);
            if (difference > 0)
            {
                peak = std::max(peak, difference);
                last = std::max(last, static_cast<uint32_t>(i + 1));
            }
        }
    }
    if (peak > 0)
    {
        totals.waves_later++;
        totals.later_peak = std::max(totals.later_peak, peak);
        totals.later_ms = std::max(totals.later_ms, 1000.0 * last / pcm.sample_rate);
    }
}

// The lowest ratio of signal to noise that a wave may take when it comes back from its WAV file as DSP-ADPCM, in
// decibels. The full runs of 2026-10-20 gave 21.1 dB at the lowest for --waves and 22.2 dB for --new-waves.
constexpr double kLeastAdpcmRatio = 20.0;

} // namespace

std::string DspAdpcmProblem(const sar::SoundArchive& built, const FileItem& item, const sar::WaveArchiveFile& present,
                            std::optional<uint32_t> wave, std::span<const uint8_t> wav, const sar::Pcm& pcm,
                            Totals& totals)
{
    const auto start = std::chrono::steady_clock::now();
    const edit::IncomingWave incoming =
        edit::ReadIncomingWave(wav, built, item.item, wave, sar::WaveEncoding::kDspAdpcm);
    totals.adpcm_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const uint32_t moved = pcm.loop ? (14 - pcm.loop_start % 14) % 14 : 0;
    if (incoming.encoding != sar::WaveEncoding::kDspAdpcm || incoming.loop_moved != moved ||
        incoming.frames != pcm.loop_end || incoming.loop_start != pcm.loop_start)
    {
        return "doesn't come back from its WAV file as DSP-ADPCM";
    }

    // The samples that the new wave plays up to its loop's end.
    std::vector<std::vector<int16_t>> played = pcm.channels;
    for (std::vector<int16_t>& channel : played)
    {
        for (std::size_t i = pcm.loop_start; i < pcm.loop_start + moved; i++)
        {
            const int16_t sample = channel[i];
            channel.push_back(sample);
        }
    }
    const sar::Pcm back = sar::DecodeWave(incoming.bytes, song::ItemOptions(built));
    if (back.sample_rate != pcm.sample_rate || back.loop != pcm.loop || back.loop_start != pcm.loop_start + moved ||
        back.loop_end != pcm.loop_end + moved || back.channels.size() != played.size())
    {
        return "comes back from its WAV file as DSP-ADPCM with another rate, loop or length";
    }

    // The Replace window's ratio of signal to noise over every channel.
    double signal = 0.0;
    double noise = 0.0;
    for (std::size_t c = 0; c < played.size(); c++)
    {
        if (back.channels[c].size() != played[c].size())
        {
            return "comes back from its WAV file as DSP-ADPCM with another length";
        }

        for (std::size_t i = 0; i < played[c].size(); i++)
        {
            const double sample = played[c][i];
            const double error = sample - back.channels[c][i];
            signal += sample * sample;
            noise += error * error;
        }
    }
    const std::optional<double> ratio =
        noise > 0.0 ? std::optional<double>(10.0 * std::log10(signal / noise)) : std::nullopt;
    if (ratio.has_value() != incoming.signal_to_noise.has_value() ||
        (ratio && std::abs(*ratio - *incoming.signal_to_noise) > 1e-6))
    {
        return "as DSP-ADPCM has another ratio of signal to noise than the Replace window shows";
    }
    if (ratio && *ratio < kLeastAdpcmRatio)
    {
        return "comes back from its WAV file as DSP-ADPCM with a ratio of signal to noise below " +
               std::to_string(static_cast<int>(kLeastAdpcmRatio)) + " dB";
    }

    // Each later pass of the loop starts from the loop's context and plays the first pass's samples.
    const sar::WaveFile parsed = sar::WaveFile::Read(incoming.bytes);
    for (std::size_t c = 0; c < back.channels.size() && back.loop; c++)
    {
        const std::vector<int16_t> later =
            LaterPass(parsed.channels[c].samples, *parsed.channels[c].adpcm, back.loop_start, back.loop_end,
                      ReplaceLoopDecoding(built.ReadOptions().flavor));
        if (!std::equal(later.begin(), later.end(), back.channels[c].begin() + back.loop_start))
        {
            return "as DSP-ADPCM plays its loop's later passes otherwise";
        }
    }

    // The new wave archive keeps the other waves' bytes.
    const std::size_t place = wave.value_or(present.waves.size());
    const sar::WaveArchiveFile one =
        sar::WaveArchiveFile::Read(edit::WaveArchiveWith(built, item.item, wave, incoming.bytes));
    if (one.waves.size() != std::max(present.waves.size(), place + 1))
    {
        return "as DSP-ADPCM changes the number of waves";
    }
    for (std::size_t other = 0; other < one.waves.size(); other++)
    {
        if (one.waves[other] != (other == place ? incoming.bytes : present.waves[other]))
        {
            return "as DSP-ADPCM leaves wave " + std::to_string(other) + " with other bytes";
        }
    }

    if (ratio)
    {
        totals.adpcm_ratios.push_back(*ratio);
    }
    else
    {
        totals.adpcm_exact++;
    }
    totals.adpcm_moved += moved > 0 ? 1 : 0;
    totals.adpcm_samples += back.loop_end * back.channels.size();

    return {};
}

namespace
{

// What is wrong when wave `wave` of wave archive `item` comes back from its WAV file `wav` at half its rate, or from
// the WAV file without its loop. `pcm` holds the WAV file's samples, and `incoming` is the WAV file's wave as 16-bit
// PCM. At half the rate, the new wave has to hold the samples that audio::Resample gives each channel, with the loop's
// start at its place at the new rate. A wave at 8,000 Hz or less keeps its rate. Without its loop, the WAV file takes
// the present wave's loop that the Replace window offers. The new wave then has to hold the bytes that the WAV file
// with the loop gives.
std::string ResampledProblem(const sar::SoundArchive& built, const FileItem& item, uint32_t wave,
                             const edit::IncomingWave& incoming, std::span<const uint8_t> wav, const sar::Pcm& pcm,
                             Totals& totals)
{
    if (pcm.loop)
    {
        seqlib::Pcm unlooped;
        unlooped.channels = pcm.channels;
        const edit::IncomingWave offered =
            edit::ReadIncomingWave(seqlib::WavFile(unlooped, pcm.sample_rate), built, item.item, wave,
                                   sar::WaveEncoding::kPcm16, edit::WaveLoop{pcm.loop_start, pcm.loop_end});
        if (offered.bytes != incoming.bytes || !offered.loop_from_present)
        {
            return "doesn't come back from its WAV file without a loop, with the present wave's loop";
        }
        totals.loops_offered++;
    }
    if (pcm.sample_rate <= 8000)
    {
        totals.resampled_low++;
        return {};
    }

    const uint32_t rate = pcm.sample_rate / 2;
    const auto start = std::chrono::steady_clock::now();
    const edit::IncomingWave lower =
        edit::ReadIncomingWave(wav, built, item.item, wave, sar::WaveEncoding::kPcm16, std::nullopt, rate);
    totals.resampled_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const sar::Pcm back = sar::DecodeWave(lower.bytes, song::ItemOptions(built));
    const std::size_t loop_start = pcm.loop ? audio::ResampledPlace(pcm.loop_start, pcm.sample_rate, rate) : 0;
    if (lower.rate != rate || lower.wav_rate != pcm.sample_rate || lower.loop_start != loop_start ||
        back.sample_rate != rate || back.loop != pcm.loop || (pcm.loop && back.loop_start != loop_start) ||
        back.loop_end != lower.frames || back.channels.size() != pcm.channels.size())
    {
        return "doesn't come back from its WAV file at half its rate";
    }
    for (std::size_t c = 0; c < pcm.channels.size(); c++)
    {
        const std::vector<int16_t> expected =
            audio::Resample(pcm.channels[c], pcm.sample_rate, rate,
                            pcm.loop ? std::optional<std::size_t>(pcm.loop_start) : std::nullopt);
        if (back.channels[c] != expected)
        {
            return "comes back from its WAV file at half its rate with other samples";
        }
    }

    totals.resampled++;
    totals.resampled_samples += back.loop_end * back.channels.size();

    return {};
}

// What is wrong when wave `wave` of wave archive `item`, whose waves are `present`, goes out as a WAV file and comes
// back in its place, or nothing. `pcm` are the samples of the wave. Puts the wave from its WAV file as 16-bit PCM in
// `all`, and counts the wave in `totals`.
std::string WaveProblem(const sar::SoundArchive& built, const FileItem& item, const sar::WaveArchiveFile& present,
                        uint32_t wave, const sar::Pcm& pcm, sar::WaveArchiveFile& all, Totals& totals)
{
    // The Replace window reads the WAV file and the present wave.
    const std::string label = "wave " + std::to_string(wave);
    const std::vector<uint8_t> wav = song::WaveAsWav(built, item.item, wave);
    const edit::IncomingWave incoming = edit::ReadIncomingWave(wav, built, item.item, wave, sar::WaveEncoding::kPcm16);
    const edit::WaveChanges changes = edit::ChangesOfWave(built, item.item, wave);
    if (incoming.channels != pcm.channels.size() || incoming.rate != pcm.sample_rate || incoming.loop != pcm.loop ||
        incoming.loop_start != (pcm.loop ? pcm.loop_start : 0) || incoming.frames != pcm.loop_end ||
        incoming.after_loop != 0 || incoming.dropped != 0 || incoming.bits != 16 || incoming.floating)
    {
        return label + " doesn't come back from its WAV file";
    }
    if (changes.rate != pcm.sample_rate || changes.channels < pcm.channels.size() ||
        changes.size != present.waves[wave].size())
    {
        return label + " isn't the wave that the Replace window shows";
    }

    // The new wave archive holds the new wave in the wave's place, and the other waves' bytes.
    const sar::WaveArchiveFile one =
        sar::WaveArchiveFile::Read(edit::WaveArchiveWith(built, item.item, wave, incoming.bytes));
    if (one.waves.size() != present.waves.size())
    {
        return label + " changes the number of waves";
    }
    for (std::size_t other = 0; other < one.waves.size(); other++)
    {
        if (one.waves[other] != (other == wave ? incoming.bytes : present.waves[other]))
        {
            return label + " leaves wave " + std::to_string(other) + " with other bytes";
        }
    }

    // The new wave plays the old one's samples. As DSP-ADPCM, it plays them closely.
    if (sar::DecodeWave(incoming.bytes, song::ItemOptions(built)) != pcm)
    {
        return label + " plays other samples from its WAV file";
    }
    const std::string adpcm = DspAdpcmProblem(built, item, present, wave, wav, pcm, totals);
    if (!adpcm.empty())
    {
        return label + " " + adpcm;
    }
    const std::string resampled = ResampledProblem(built, item, wave, incoming, wav, pcm, totals);
    if (!resampled.empty())
    {
        return label + " " + resampled;
    }

    // The wave's file goes back in its place as it is. The wave archive then holds the same waves.
    const sar::Flavor flavor = built.ReadOptions().flavor;
    const song::Item wave_item{song::ItemKind::kWave, item.item, wave};
    const std::span<const uint8_t> wave_file = song::ItemFile(built, wave_item);
    const edit::IncomingWave same = edit::ReadIncomingWaveFile(wave_file, built, item.item, wave);
    const std::vector<uint8_t> again = edit::WaveArchiveWith(built, item.item, wave, same.bytes);
    if (!std::ranges::equal(same.bytes, wave_file) || same.converted ||
        sar::WaveArchiveFile::Read(again).waves != present.waves)
    {
        return label + " doesn't go back in as it is from its wave file";
    }
    totals.waves_rewritten += !std::ranges::equal(again, built.FileData(item.file_id)) ? 1 : 0;

    // The wave's file for each other console goes back in converted, with the wave's version and its samples.
    const uint32_t version = HeaderVersion(present.waves[wave], flavor);
    const sar::Options options = song::ItemOptions(built);
    for (const sar::Flavor console : song::OtherConsoles(flavor))
    {
        const std::vector<uint8_t> other = song::OtherConsoleFile(built, wave_item, console);
        const edit::IncomingWave converted = edit::ReadIncomingWaveFile(other, built, item.item, wave);
        if (!converted.converted || converted.version != version || HeaderVersion(converted.bytes, flavor) != version ||
            sar::DecodeWave(converted.bytes, options) != sar::DecodeWave(present.waves[wave], options))
        {
            return label + " doesn't come back from its wave file for the " + song::ConsoleName(console);
        }
        totals.waves_other_bytes += converted.bytes != present.waves[wave] ? 1 : 0;
    }

    totals.waves++;
    totals.waves_wide += changes.channels > 2 ? 1 : 0;
    totals.waves_loop_lost += changes.loop != incoming.loop ? 1 : 0;
    CountLaterPasses(present.waves[wave], pcm, built.ReadOptions().flavor, totals);
    all.waves[wave] = incoming.bytes;

    return {};
}

// What is wrong when each wave of wave archive `item` goes out as a WAV file and comes back in its place. The WAV file
// is the one that the browser's Export… writes, and the edit is Replace…'s. Each wave goes in alone first, as 16-bit
// PCM and as DSP-ADPCM. The wave archive with every wave from its WAV file as 16-bit PCM then takes the old one's
// place. `built` is the archive that `file` builds, with `groups`.
std::string WaveArchiveProblem(const format::File& file, const sar::SoundArchive& built, std::span<const Group> groups,
                               const FileItem& item, Totals& totals)
{
    const sar::WaveArchiveFile present = sar::WaveArchiveFile::Read(built.FileData(item.file_id));
    sar::WaveArchiveFile all = present;
    std::vector<sar::Pcm> decoded;
    for (uint32_t wave = 0; wave < present.waves.size(); wave++)
    {
        decoded.push_back(song::DecodeItemWave(built, item.item, wave));
        if (std::string problem = WaveProblem(built, item, present, wave, decoded.back(), all, totals);
            !problem.empty())
        {
            return problem;
        }
    }

    // Every wave from its WAV file at once. The archive has to play the same samples.
    Archive after;
    const std::string problem = FileProblem(file, built, groups, item, all.Write(), after);
    if (!problem.empty())
    {
        return "with every wave from its WAV file " + problem;
    }
    for (uint32_t wave = 0; wave < present.waves.size(); wave++)
    {
        if (song::DecodeItemWave(*after, item.item, wave) != decoded[wave])
        {
            return "with every wave from its WAV file plays other samples in wave " + std::to_string(wave);
        }
    }

    return {};
}

} // namespace

void CheckWaves(const format::File& file, const sar::SoundArchive& built, std::span<const Group> groups,
                const std::string& name, Totals& totals)
{
    for (const FileItem& item : FileItemsOf(built))
    {
        if (item.kind != edit::FileKind::kWaveArchive)
        {
            continue;
        }
        if (Outside(built, item.file_id) || SharedFile(built, item.file_id))
        {
            totals.wave_archives_out++;
            continue;
        }

        totals.wave_archives_beside += Beside(built, item.file_id) ? 1 : 0;

        std::string problem;
        try
        {
            problem = WaveArchiveProblem(file, built, groups, item, totals);
        }
        catch (const std::exception& error)
        {
            problem = std::string("fails: ") + error.what();
        }

        if (!problem.empty())
        {
            std::printf("%s: %s %s\n", name.c_str(), NameOf(built, item).c_str(), problem.c_str());
            totals.wave_archives_failed++;
        }
        totals.wave_archives++;
    }
}

void PrintDspAdpcm(const std::string& name, const Totals& totals, bool half_rates)
{
    const std::vector<double>& ratios = totals.adpcm_ratios;
    const auto between = [&ratios](double low, double high)
    {
        return std::count_if(ratios.begin(), ratios.end(), [&](double ratio) { return ratio >= low && ratio < high; });
    };

    const double least = ratios.empty() ? 0.0 : *std::min_element(ratios.begin(), ratios.end());
    std::printf(
        "%s: As DSP-ADPCM, %zu waves came back exactly, and %zu with a signal-to-noise ratio of at least %.1f dB: %td "
        "under 20 dB, %td from 20 to 30 dB, %td from 30 to 40 dB and %td from 40 dB. %zu loops moved forward to a "
        "frame. The WAV files took %.1f s to read as DSP-ADPCM, for %zu samples.\n",
        name.c_str(), totals.adpcm_exact, ratios.size(), least, between(-1e9, 20.0), between(20.0, 30.0),
        between(30.0, 40.0), between(40.0, 1e9), totals.adpcm_moved, totals.adpcm_seconds, totals.adpcm_samples);
    if (!half_rates)
    {
        return;
    }

    std::printf(
        "%s: %zu waves came back from their WAV files at half their rates, with %zu samples in all channels. "
        "Their WAV files took %.1f s to read. %zu waves at 8,000 Hz or less kept their rates. %zu looped waves "
        "came back from their WAV files without loops, with the present waves' loops.\n",
        name.c_str(), totals.resampled, totals.resampled_samples, totals.resampled_seconds, totals.resampled_low,
        totals.loops_offered);
}

} // namespace hokuto::tools::replace
