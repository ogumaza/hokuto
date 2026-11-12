// SPDX-License-Identifier: MIT

// The rounds Waves, Encodings and New waves of hokuto_check_ds_replace.

#include "check_ds_replace.h"

namespace hokuto::tools::ds_replace
{

namespace
{

// Puts the wave file of wave `wave` of `wave_archive`, file `id`, back into the game's file. The wave file has to
// contain `bytes`, the bytes of the wave as the file stores them, and the file has to keep its bytes.
void PutBackWaveFile(const Game& game, uint32_t wave_archive, uint32_t id, uint32_t wave,
                     const std::vector<uint8_t>& bytes, const std::string& where, Totals& totals)
{
    const sdat::Archive& tables = game.archive->archive;
    try
    {
        const std::vector<uint8_t> swav = song::DsItemBytes(*game.archive, {song::ItemKind::kWave, wave_archive, wave});
        const hokuto::edit::IncomingWave incoming = hokuto::edit::ReadIncomingDsWaveFile(swav);
        if (incoming.bytes != bytes)
        {
            Fail(game, "waves", where + ": the wave file holds other bytes", totals);
        }
        else
        {
            const File back =
                hokuto::edit::WithDsWaveBytes(game.file, *game.archive, wave_archive, wave, incoming.bytes);
            const std::shared_ptr<const song::DsArchive> after =
                song::LoadDsArchive(hokuto::format::ExportArchive(back), game.archive->build);
            if (!std::ranges::equal(after->archive.File(id), tables.File(id)))
            {
                Fail(game, "waves", where + " comes back with other bytes", totals);
            }
            else
            {
                totals.wave_files_back++;
            }
        }
    }
    catch (const std::exception& error)
    {
        Fail(game, "waves", where + ": " + error.what(), totals);
    }
}

// Puts the WAV file of wave `wave` of `wave_archive`, `present` with its `samples`, into the game's file as 16-bit PCM
// and in the wave's encoding.
void PutWav(const Game& game, uint32_t wave_archive, uint32_t wave, const sdat::Wave& present,
            const std::vector<int16_t>& samples, const std::string& where, Totals& totals)
{
    std::vector<uint8_t> wav;
    try
    {
        wav = song::PcmAsWav(song::DsWavePcm(*game.archive, wave_archive, wave), where);
    }
    catch (const std::exception&)
    {
        totals.wavs_apart++;
        return;
    }
    try
    {
        const hokuto::edit::IncomingWave wide = hokuto::edit::ReadIncomingDsWave(wav, sar::WaveEncoding::kPcm16);
        const sdat::Wave made = sdat::Wave::Parse(xsflib::Reader(wide.bytes), 0, wide.bytes.size());
        const std::vector<int16_t> decoded = sdat::DecodeWave(made);
        const bool loops = present.loop == 1 && present.LoopStartSample() < samples.size();
        const std::size_t kept = std::min(samples.size(), decoded.size());
        if (!std::ranges::equal(std::span(samples).first(kept), std::span(decoded).first(kept)) ||
            decoded.size() < samples.size() || (made.loop == 1) != loops ||
            (loops && made.LoopStartSample() != present.LoopStartSample()))
        {
            Fail(game, "waves", where + " takes other samples from its WAV file", totals);
        }

        const hokuto::edit::IncomingWave same = hokuto::edit::ReadIncomingDsWave(wav, ToSar(present.encoding));
        const sdat::Wave held = sdat::Wave::Parse(xsflib::Reader(same.bytes), 0, same.bytes.size());
        const std::vector<int16_t> encoded = sdat::DecodeWave(held);
        const std::optional<double> ratio = Ratio(samples, encoded);
        const bool agrees = ratio.has_value() == same.signal_to_noise.has_value() &&
                            (!ratio || std::abs(*ratio - *same.signal_to_noise) < 1e-6);
        if (!agrees)
        {
            Fail(game, "waves", where + " gives another signal-to-noise ratio", totals);
        }
        if (ratio)
        {
            totals.lowest_ratio = std::min(totals.lowest_ratio, *ratio);
        }
        else
        {
            totals.exact++;
        }
        totals.wavs++;
    }
    catch (const std::invalid_argument& error)
    {
        std::printf("%s: waves: %s stays apart: %s\n", game.name.c_str(), where.c_str(), error.what());
        totals.wavs_apart++;
    }
}

// Converts the first wave of `wave_archive`, file `id`, to each other encoding through the edit of the properties.
// `parsed` is the wave archive, `samples` the first wave's samples and `name` the wave archive's name. `laid_out` says
// whether the file stores the waves as Hokuto lays them out.
void CheckOtherEncodings(const Game& game, uint32_t wave_archive, uint32_t id, const sdat::WaveArchive& parsed,
                         const std::vector<int16_t>& samples, const std::string& name, bool laid_out, Totals& totals)
{
    const sdat::Archive& tables = game.archive->archive;
    const sdat::Wave& first = parsed.waves[0];
    for (const sdat::WaveEncoding encoding :
         {sdat::WaveEncoding::kPcm8, sdat::WaveEncoding::kPcm16, sdat::WaveEncoding::kImaAdpcm})
    {
        if (encoding == first.encoding)
        {
            continue;
        }
        hokuto::edit::DsWaveValues values;
        values.rate = song::DsWaveRate(first);
        values.loop = first.loop == 1 && first.LoopStartSample() < samples.size();
        values.loop_start = values.loop ? first.LoopStartSample() : 0;
        values.encoding = encoding;

        // The loop's start moves forward to a word of the new encoding. The edit refuses a start past the words of its
        // field. The first word of an IMA-ADPCM wave contains the state of the decoder.
        const uint32_t step = sdat::SamplesPerWord(encoding);
        const uint32_t start = (values.loop_start + step - 1) / step * step;
        const uint32_t header_words = encoding == sdat::WaveEncoding::kImaAdpcm ? 1 : 0;
        const bool far_loop = values.loop && header_words + start / step > kMostLoopWords;
        try
        {
            const File edited = hokuto::edit::WithDsWave(game.file, *game.archive, wave_archive, 0, values);
            const std::shared_ptr<const song::DsArchive> after =
                song::LoadDsArchive(hokuto::format::ExportArchive(edited), game.archive->build);
            const sdat::WaveArchive now = sdat::WaveArchive::Parse(after->archive.File(id));
            const sdat::Wave& made = now.waves.at(0);
            if (made.encoding != encoding || made.sample_rate != first.sample_rate || made.timer != first.timer ||
                (made.loop == 1) != values.loop || (values.loop && made.LoopStartSample() != start))
            {
                Fail(game, "encodings", name + ": the new wave holds other values", totals);
            }
            if (encoding == sdat::WaveEncoding::kPcm16)
            {
                const std::vector<int16_t> decoded = sdat::DecodeWave(made);
                if (decoded.size() < samples.size() ||
                    !std::ranges::equal(std::span(decoded).first(samples.size()), samples))
                {
                    Fail(game, "encodings", name + ": 16-bit PCM holds other samples", totals);
                }
            }

            // The other waves keep their bytes.
            if (now.waves.size() != parsed.waves.size())
            {
                Fail(game, "encodings", name + " holds " + std::to_string(now.waves.size()) + " waves", totals);
            }
            for (std::size_t wave = 1; wave < std::min(now.waves.size(), parsed.waves.size()); wave++)
            {
                if (sdat::WaveBytes(now.waves[wave]) != sdat::WaveBytes(parsed.waves[wave]))
                {
                    Fail(game, "encodings", name + ": wave " + std::to_string(wave) + " changed", totals);
                }
            }
            totals.encodings++;
        }
        catch (const std::invalid_argument& error)
        {
            if (laid_out && !far_loop && !SharesBytes(tables, id) && !game.shares_file)
            {
                Fail(game, "encodings", name + ": " + error.what(), totals);
                continue;
            }

            if (laid_out)
            {
                std::printf("%s: encodings: %s stays apart: %s\n", game.name.c_str(), name.c_str(), error.what());
            }
            totals.encodings_apart++;
        }
    }
}

// Puts `first`, the bytes of the first wave of `wave_archive`, file `id`, in again after the archive's waves in
// `with_new_waves`, and records the wave count of `parsed` in `old_counts`.
void ReinsertFirstWave(const Game& game, uint32_t wave_archive, uint32_t id, const sdat::WaveArchive& parsed,
                       const std::vector<uint8_t>& first, const std::string& name, File& with_new_waves,
                       std::map<uint32_t, std::size_t>& old_counts, Totals& totals)
{
    const sdat::Archive& tables = game.archive->archive;
    try
    {
        with_new_waves = hokuto::edit::WithDsWaveBytes(with_new_waves, *game.archive, wave_archive,
                                                       static_cast<uint32_t>(parsed.waves.size()), first);
        old_counts[wave_archive] = parsed.waves.size();
        totals.new_waves++;
    }
    catch (const std::invalid_argument& error)
    {
        if (game.shares_file || SharesBytes(tables, id))
        {
            std::printf("%s: new waves: %s stays apart: %s\n", game.name.c_str(), name.c_str(), error.what());
            totals.new_waves_apart++;
        }
        else
        {
            Fail(game, "new waves", name + ": " + error.what(), totals);
        }
    }
}

// Checks `with_new_waves`, the game's file with a new wave at the end of each wave archive of `old_counts`. Each keeps
// its waves, and the new wave contains the bytes of the first.
void CheckNewWaves(const Game& game, const File& with_new_waves, const std::map<uint32_t, std::size_t>& old_counts,
                   Totals& totals)
{
    const sdat::Archive& tables = game.archive->archive;
    const std::shared_ptr<const song::DsArchive> after =
        song::LoadDsArchive(hokuto::format::ExportArchive(with_new_waves), game.archive->build);
    for (const auto& [wave_archive, count] : old_counts)
    {
        const uint32_t id = tables.wave_archives[wave_archive]->file_id & sdat::kFileIdBits;
        const sdat::WaveArchive before = sdat::WaveArchive::Parse(tables.File(id));
        const sdat::WaveArchive now = sdat::WaveArchive::Parse(after->archive.File(id));
        const std::string name = song::DsWaveArchiveName(*game.archive, wave_archive);
        if (now.waves.size() != count + 1)
        {
            Fail(game, "new waves", name + " holds " + std::to_string(now.waves.size()) + " waves", totals);
            continue;
        }
        const std::vector<std::vector<uint8_t>> old_waves = WavesOf(before, tables.File(id));
        const std::vector<std::vector<uint8_t>> new_waves = WavesOf(now, after->archive.File(id));
        for (std::size_t wave = 0; wave < count; wave++)
        {
            if (new_waves[wave] != old_waves[wave])
            {
                Fail(game, "new waves", name + ": wave " + std::to_string(wave) + " changed", totals);
            }
        }
        if (new_waves[count] != old_waves[0])
        {
            Fail(game, "new waves", name + ": the new wave holds other bytes", totals);
        }
    }
}

} // namespace

void Waves(const Game& game, uint32_t every, Totals& totals)
{
    const sdat::Archive& tables = game.archive->archive;
    File with_new_waves = game.file;
    std::map<uint32_t, std::size_t> old_counts;
    for (const auto& [wave_archive, id] : WaveArchivesOf(game))
    {
        const std::string name = song::DsWaveArchiveName(*game.archive, wave_archive);
        sdat::WaveArchive parsed;
        try
        {
            parsed = sdat::WaveArchive::Parse(tables.File(id));
        }
        catch (const std::exception&)
        {
            totals.unread_wave_archives++;
            continue;
        }

        const std::vector<std::vector<uint8_t>> waves = WavesOf(parsed, tables.File(id));
        const bool laid_out = std::ranges::equal(sdat::WaveArchiveFile(waves), tables.File(id));
        if (!laid_out)
        {
            totals.laid_otherwise++;
        }

        for (uint32_t wave = 0; wave < parsed.waves.size(); wave++)
        {
            const sdat::Wave& present = parsed.waves[wave];
            const std::string where = name + ", wave " + std::to_string(wave);
            totals.waves++;
            std::vector<int16_t> samples;
            try
            {
                samples = sdat::DecodeWave(present);
            }
            catch (const std::exception&)
            {
                totals.wavs_apart++;
                continue;
            }

            // The wave file is imported again. The file keeps its bytes, with the bytes of the wave as the file stores
            // them. A wave file contains the parameters and samples of the wave (sdat::WaveBytes). A wave whose bytes
            // in the file differ from those counts apart.
            const bool wave_file = laid_out && wave % every == 0;
            if (wave_file && sdat::WaveBytes(present) != waves[wave])
            {
                totals.wave_files_apart++;
            }
            else if (wave_file)
            {
                PutBackWaveFile(game, wave_archive, id, wave, waves[wave], where, totals);
            }

            // The WAV file goes in as 16-bit PCM, and in the wave's encoding. A wave without samples has no WAV file.
            if (wave % every != 0 || samples.empty() || present.sample_rate == 0)
            {
                continue;
            }
            PutWav(game, wave_archive, wave, present, samples, where, totals);
        }

        // The first wave is converted to each other encoding through the edit of the properties.
        if (!parsed.waves.empty() && parsed.waves[0].Samples() > 0)
        {
            const sdat::Wave& first = parsed.waves[0];
            std::vector<int16_t> samples;
            try
            {
                samples = sdat::DecodeWave(first);
            }
            catch (const std::exception&)
            {
                totals.encodings_apart++;
                continue;
            }
            CheckOtherEncodings(game, wave_archive, id, parsed, samples, name, laid_out, totals);

            // The first wave is added again after the waves. An archive that shares its file with other data refuses
            // the growth, and so does a wave archive whose file shares bytes with another file. Both count apart.
            if (laid_out)
            {
                ReinsertFirstWave(game, wave_archive, id, parsed, waves[0], name, with_new_waves, old_counts, totals);
            }
        }
    }

    if (old_counts.empty())
    {
        return;
    }
    CheckNewWaves(game, with_new_waves, old_counts, totals);
}

} // namespace hokuto::tools::ds_replace
