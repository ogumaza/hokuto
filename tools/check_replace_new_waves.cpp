// SPDX-License-Identifier: MIT

// The mode --new-waves of hokuto_check_replace: new waves.

#include "check_replace.h"

namespace hokuto::tools::replace
{

namespace
{

// The version that a new wave of wave archive `wave_archive` of `archive` takes where Hokuto makes or converts it: the
// version in the header of the wave archive's first wave, or else of the first wave of the archive's first wave archive
// with waves that ntdlib reads, or else the version of Export…'s waves.
uint32_t NewWaveVersion(const sar::SoundArchive& archive, uint32_t wave_archive)
{
    const sar::Flavor flavor = archive.ReadOptions().flavor;
    std::vector<uint32_t> order = {wave_archive};
    for (uint32_t i = 0; i < archive.WaveArchives().size(); i++)
    {
        order.push_back(i);
    }
    for (const uint32_t i : order)
    {
        try
        {
            const std::span<const uint8_t> file = archive.FileData(archive.WaveArchives()[i].file_id);
            const std::vector<std::span<const uint8_t>> waves =
                sar::WaveArchive::Parse(file, archive.ReadOptions()).waves;
            if (!waves.empty() && waves[0].size() >= 0x14)
            {
                return HeaderVersion(waves[0], flavor);
            }
        }
        catch (const std::exception&)
        {
            // The next wave archive gives the version.
        }
    }

    return song::ExportVersions(song::ItemKind::kWaveArchive, flavor).waves;
}

// The first wave of the archive's first wave archive with waves, or nothing.
std::optional<song::Item> FirstWave(const sar::SoundArchive& archive)
{
    for (uint32_t i = 0; i < archive.WaveArchives().size(); i++)
    {
        try
        {
            if (song::WaveCountOf(archive, i) > 0)
            {
                return song::Item{song::ItemKind::kWave, i, 0};
            }
        }
        catch (const std::exception&)
        {
            // Another wave archive gives the wave.
        }
    }

    return std::nullopt;
}

// What is wrong when `incoming`, a wave that the Import window read, goes in after the waves of wave archive `item`, or
// nothing. `present` is the wave archive's file. The new wave has to decode to `pcm`.
std::string AppendProblem(const sar::SoundArchive& built, const FileItem& item, const sar::WaveArchiveFile& present,
                          const edit::IncomingWave& incoming, const sar::Pcm& pcm)
{
    const sar::WaveArchiveFile now =
        sar::WaveArchiveFile::Read(edit::WaveArchiveWith(built, item.item, std::nullopt, incoming.bytes));
    if (now.waves.size() != present.waves.size() + 1 || now.waves.back() != incoming.bytes)
    {
        return "doesn't put the new wave after the others";
    }
    for (std::size_t i = 0; i < present.waves.size(); i++)
    {
        if (now.waves[i] != present.waves[i])
        {
            return "leaves wave " + std::to_string(i) + " with other bytes";
        }
    }
    if (sar::DecodeWave(incoming.bytes, song::ItemOptions(built)) != pcm)
    {
        return "plays other samples";
    }

    return {};
}

// What is wrong when a short WAV file goes in as the first wave of wave archive `item`, in an archive without waves, or
// nothing. The new wave has to take the version of Export…'s waves and decode to the WAV file's samples. The edit then
// goes through the file as in NewWavesProblem. `built` is the archive that `file` builds, with `groups`.
std::string WavelessProblem(const format::File& file, const sar::SoundArchive& built, std::span<const Group> groups,
                            const FileItem& item, Totals& totals)
{
    // The WAV file goes in as 16-bit PCM.
    const sar::Flavor flavor = built.ReadOptions().flavor;
    seqlib::Pcm pcm;
    pcm.channels = {{0, 1000, -1000, 2000, -2000, 3000, -3000, 4000}};
    const std::vector<uint8_t> wav = seqlib::WavFile(pcm, 32000);
    const edit::IncomingWave incoming =
        edit::ReadIncomingWave(wav, built, item.item, std::nullopt, sar::WaveEncoding::kPcm16);
    const uint32_t version = song::ExportVersions(song::ItemKind::kWaveArchive, flavor).waves;
    if (incoming.version != version || HeaderVersion(incoming.bytes, flavor) != version)
    {
        return "doesn't take the version of Export…'s waves from a WAV file";
    }

    // The Import window's edit.
    edit::IncomingFile waves;
    waves.kind = edit::FileKind::kWaveArchive;
    waves.bytes = edit::WaveArchiveWith(built, item.item, std::nullopt, incoming.bytes);
    edit::ChangesOf(file, built, item.item, waves);
    Archive after;
    const std::string problem =
        ReplacedProblem(built, groups, item, waves.bytes, edit::ReplaceFile(file, item.file_id, waves.bytes), after);
    if (!problem.empty())
    {
        return "with a new wave " + problem;
    }
    const sar::Pcm decoded = song::DecodeItemWave(*after, item.item, 0);
    if (song::WaveCountOf(*after, item.item) != 1 || decoded.sample_rate != 32000 || decoded.loop ||
        decoded.channels != pcm.channels)
    {
        return "with a new wave plays other samples there";
    }
    totals.nw_waveless++;

    return {};
}

// What is wrong when wave `source` is imported as a new wave after the waves `present` of wave archive `item`, or
// nothing. `version` is the version of the wave archive's waves, `options` the options of `built`, and `pcm` the
// samples of `source`. The wave goes in from its wave file, from its wave file for each other console and from its WAV
// file, and the `first` wave also from its WAV file as DSP-ADPCM (NewWavesProblem). Counts the wave in `totals`.
std::string AddedWaveProblem(const sar::SoundArchive& built, const FileItem& item, const sar::WaveArchiveFile& present,
                             uint32_t version, const sar::Options& options, const song::Item& source, bool first,
                             const sar::Pcm& pcm, Totals& totals)
{
    const sar::Flavor flavor = built.ReadOptions().flavor;

    // The wave's file as the browser's Export… writes it.
    const std::string label =
        source.index == item.item ? "wave " + std::to_string(source.wave) : song::ItemName(built, source);
    const std::span<const uint8_t> wave_file = song::ItemFile(built, source);
    const sar::Pcm decoded = sar::DecodeWave(wave_file, options);

    // The wave file goes in as it is, or with the version of the wave archive's waves when it holds a later one.
    const edit::IncomingWave same = edit::ReadIncomingWaveFile(wave_file, built, item.item);
    const bool later = HeaderVersion(wave_file, flavor) > version;
    if (later ? !same.converted || same.converted->flavor != flavor || same.version != version ||
                    HeaderVersion(same.bytes, flavor) != version
              : !std::ranges::equal(same.bytes, wave_file) || same.converted ||
                    same.version != HeaderVersion(wave_file, flavor))
    {
        return label + " doesn't go in as it is from its wave file";
    }
    totals.nw_written += later ? 1 : 0;
    if (same.rate != pcm.sample_rate || same.loop != pcm.loop || same.frames != pcm.loop_end ||
        same.loop_start != (pcm.loop ? pcm.loop_start : 0) ||
        same.channels != song::WaveChannels(built, source.index, source.wave))
    {
        return label + " isn't the wave that the Import window shows";
    }
    std::string problem = AppendProblem(built, item, present, same, decoded);
    if (!problem.empty())
    {
        return label + " from its wave file " + problem;
    }

    // The file for each other console goes in converted, with the version of the wave archive's waves.
    for (const sar::Flavor console : song::OtherConsoles(flavor))
    {
        const std::vector<uint8_t> other = song::OtherConsoleFile(built, source, console);
        const std::string name = song::ConsoleName(console);
        const edit::IncomingWave converted = edit::ReadIncomingWaveFile(other, built, item.item);
        if (!converted.converted || converted.converted->flavor != console ||
            converted.converted->version != HeaderVersion(other, console) ||
            converted.converted->size != other.size() || converted.version != version ||
            HeaderVersion(converted.bytes, flavor) != version)
        {
            return label + " doesn't go in converted from its wave file for the " + name;
        }
        problem = AppendProblem(built, item, present, converted, decoded);
        if (!problem.empty())
        {
            return label + " from its wave file for the " + name + " " + problem;
        }
        totals.nw_other_bytes += !std::ranges::equal(converted.bytes, wave_file) ? 1 : 0;
    }

    // The WAV file goes in as 16-bit PCM, with the version of the wave archive's waves.
    const std::vector<uint8_t> wav = song::WaveAsWav(built, source.index, source.wave);
    const edit::IncomingWave from_wav =
        edit::ReadIncomingWave(wav, built, item.item, std::nullopt, sar::WaveEncoding::kPcm16);
    if (from_wav.version != version || HeaderVersion(from_wav.bytes, flavor) != version)
    {
        return label + " doesn't take the version of the wave archive's waves from its WAV file";
    }
    problem = AppendProblem(built, item, present, from_wav, pcm);
    if (!problem.empty())
    {
        return label + " from its WAV file " + problem;
    }

    // The first wave also goes in from its WAV file as DSP-ADPCM.
    problem = first ? DspAdpcmProblem(built, item, present, std::nullopt, wav, pcm, totals) : std::string();
    if (!problem.empty())
    {
        return label + " " + problem;
    }

    totals.nw_waves++;
    totals.nw_other_versions += HeaderVersion(wave_file, flavor) != version ? 1 : 0;

    return {};
}

// What is wrong when the file of the first wave `first` is imported through the edit of the Import window, or nothing.
// `first_pcm` are the samples of that wave. Sets `once` to the file after the edit and `after` to the archive that it
// builds.
std::string ImportWindowProblem(const format::File& file, const sar::SoundArchive& built, std::span<const Group> groups,
                                const FileItem& item, const sar::WaveArchiveFile& present, const song::Item& first,
                                const sar::Pcm& first_pcm, format::File& once, Archive& after, Totals& totals)
{
    const auto start = std::chrono::steady_clock::now();
    const edit::IncomingWave incoming = edit::ReadIncomingWaveFile(song::ItemFile(built, first), built, item.item);
    edit::IncomingFile waves;
    waves.kind = edit::FileKind::kWaveArchive;
    waves.bytes = edit::WaveArchiveWith(built, item.item, std::nullopt, incoming.bytes);
    edit::ChangesOf(file, built, item.item, waves);
    once = edit::ReplaceFile(file, item.file_id, waves.bytes);
    const std::chrono::duration<double> took = std::chrono::steady_clock::now() - start;
    totals.nw_slowest = std::max(totals.nw_slowest, took.count());
    const std::string problem = ReplacedProblem(built, groups, item, waves.bytes, once, after);
    if (!problem.empty())
    {
        return "with a new wave " + problem;
    }
    const auto count = static_cast<uint32_t>(present.waves.size());
    if (song::WaveCountOf(*after, item.item) != count + 1 ||
        song::DecodeItemWave(*after, item.item, count) != first_pcm)
    {
        return "with a new wave plays other samples there";
    }

    return {};
}

// What is wrong when the WAV file of the first wave `first`, with the samples `first_pcm`, is imported as a second new
// wave after the first in `once`, or nothing. `once` builds `after`. `built` is the archive before the new waves.
// `count` is the number of waves before the new ones. Sets `paired` once that works, and counts the pair in `totals`.
std::string SecondWaveProblem(const sar::SoundArchive& built, const format::File& once, const Archive& after,
                              const FileItem& item, const song::Item& first, const sar::Pcm& first_pcm, uint32_t count,
                              bool& paired, Totals& totals)
{
    const edit::IncomingWave again = edit::ReadIncomingWave(song::WaveAsWav(built, first.index, first.wave), *after,
                                                            item.item, std::nullopt, sar::WaveEncoding::kPcm16);
    const format::File twice =
        edit::ReplaceFile(once, item.file_id, edit::WaveArchiveWith(*after, item.item, std::nullopt, again.bytes));
    const Archive twice_built =
        song::LoadArchive(format::ExportArchive(twice), "twice.bfsar", format::ExportGroupFiles(twice));
    if (again.version != NewWaveVersion(*after, item.item) || song::WaveCountOf(*twice_built, item.item) != count + 2 ||
        song::DecodeItemWave(*twice_built, item.item, count) != first_pcm ||
        song::DecodeItemWave(*twice_built, item.item, count + 1) != first_pcm)
    {
        return "doesn't take a second new wave after the first";
    }
    if (!hokuto::tools::SavesAndOpens(twice, true))
    {
        return "doesn't save and open again with two new waves";
    }
    paired = true;
    totals.nw_pairs++;

    return {};
}

// What is wrong when the waves of wave archive `item` go in again as new waves after its waves, or nothing. Each wave
// goes in from its wave file, from its wave file for each other console and from its WAV file as 16-bit PCM, through
// the reads of the Import window. The first also goes in from its WAV file as DSP-ADPCM (DspAdpcmProblem). A wave
// archive without waves takes the archive's first wave instead, or a WAV file in an archive without waves
// (WavelessProblem). The new wave archive has to hold the old waves' bytes and the new wave after them. The new wave
// has to decode to the samples of the wave that it came from. A wave that Hokuto makes or converts has to take the
// version of the wave archive's waves (NewWaveVersion). The first wave's file then goes in through the Import window's
// edit. The exported archive has to hold the new wave archive in every copy, and every other file has to keep its bytes
// (ReplacedProblem). Until `paired` is set, the first wave's WAV file also goes in as a second new wave after the
// first, and the file has to save and open again in both forms. `built` is the archive that `file` builds, with
// `groups`.
std::string NewWavesProblem(const format::File& file, const sar::SoundArchive& built, std::span<const Group> groups,
                            const FileItem& item, bool& paired, Totals& totals)
{
    // The wave archive, and the waves that go in.
    const sar::Options options = song::ItemOptions(built);
    const sar::WaveArchiveFile present = sar::WaveArchiveFile::Read(built.FileData(item.file_id));
    const uint32_t version = NewWaveVersion(built, item.item);
    std::vector<song::Item> sources;
    for (uint32_t wave = 0; wave < present.waves.size(); wave++)
    {
        sources.push_back({song::ItemKind::kWave, item.item, wave});
    }
    if (sources.empty())
    {
        const std::optional<song::Item> first = FirstWave(built);
        if (!first)
        {
            return WavelessProblem(file, built, groups, item, totals);
        }
        sources.push_back(*first);
        totals.nw_empty++;
    }

    std::vector<sar::Pcm> pcms;
    for (std::size_t k = 0; k < sources.size(); k++)
    {
        pcms.push_back(song::DecodeItemWave(built, sources[k].index, sources[k].wave));
        if (std::string problem =
                AddedWaveProblem(built, item, present, version, options, sources[k], k == 0, pcms.back(), totals);
            !problem.empty())
        {
            return problem;
        }
    }

    // The first wave's file through the Import window's edit: the window reads the file, makes the new wave archive and
    // finds what it changes. Import then makes the edit.
    format::File once;
    Archive after;
    if (std::string problem =
            ImportWindowProblem(file, built, groups, item, present, sources[0], pcms[0], once, after, totals);
        !problem.empty())
    {
        return problem;
    }
    if (paired)
    {
        return {};
    }

    // A second new wave after the first, from the first wave's WAV file. The file then saves and opens again.
    const auto count = static_cast<uint32_t>(present.waves.size());

    return SecondWaveProblem(built, once, after, item, sources[0], pcms[0], count, paired, totals);
}

} // namespace

void CheckNewWaves(const format::File& file, const sar::SoundArchive& built, std::span<const Group> groups,
                   const std::string& name, Totals& totals)
{
    bool paired = false;
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
            problem = NewWavesProblem(file, built, groups, item, paired, totals);
        }
        catch (const std::exception& error)
        {
            problem = std::string("fails: ") + error.what();
        }

        if (!problem.empty())
        {
            std::printf("%s: %s %s\n", name.c_str(), NameOf(built, item).c_str(), problem.c_str());
            totals.nw_failed++;
        }
        totals.nw_wave_archives++;
    }
}

} // namespace hokuto::tools::replace
