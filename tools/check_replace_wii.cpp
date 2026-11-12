// SPDX-License-Identifier: MIT

// The checks of hokuto_check_replace on archives of the Wii: sequences, files and waves.
//
// The Wii's part of the check. An archive of the Wii imports with the game's build of NW4R SND, from the main.dol of
// the extracted disc that holds the archive, or else with the build that the archive's version gives. Its sequences
// read and play by the rules of that build.

#include "check_replace.h"

namespace hokuto::tools::replace
{

namespace
{

// What the Wii's part counted.
struct WiiTotals
{
    std::size_t sounds = 0;        // sequence sounds that took their files again
    std::size_t crossed = 0;       // sequence sounds that took another sound's file
    std::size_t failed = 0;        // sequence sounds that fail
    std::size_t in_bytes = 0;      // sequence sounds whose files stay in the archive's bytes
    std::size_t unlabelled = 0;    // sounds that start past their file's first command, where it has no label
    std::size_t untracked = 0;     // sounds that open tracks that they don't allocate
    std::size_t loops_moved = 0;   // sounds whose loop the performer found a whole number of passes away
    std::size_t other_lengths = 0; // sounds whose values decide another length for the new notes
    std::size_t files = 0;         // files of banks, wave archives and wave sounds that took their bytes again
    std::size_t files_crossed = 0;
    std::size_t files_failed = 0;
    std::size_t copies = 0;       // copies of files that took new bytes
    std::size_t other_form = 0;   // next files left out: they keep their waves otherwise
    std::size_t without_next = 0; // files without a next file of their kind and form
    std::size_t waves = 0;        // waves that came back from their wave files and their WAV files
    std::size_t wave_archives = 0;
    std::size_t wave_archives_failed = 0;
    std::size_t waves_wide = 0;    // waves of more than two channels: a WAV file of Export… doesn't hold them
    std::size_t loops_forward = 0; // DSP-ADPCM waves whose loops moved forward to a frame
    std::size_t empty_loops = 0;   // waves whose loops hold no samples: a WAV file can't hold such a loop
    std::vector<double> ratios;    // the ratios of signal to noise of the waves in DSP-ADPCM, in decibels
    std::size_t exact = 0;         // waves that came back exactly in DSP-ADPCM
    std::map<std::string, std::size_t> refused; // Replace…'s refusals, by their reasons
};

} // namespace

std::vector<std::vector<uint8_t>> WiiCopies(std::span<const uint8_t> bytes, const ntdlib::rsar::SoundArchive& archive,
                                            uint32_t id, bool wave_data)
{
    std::vector<std::vector<uint8_t>> copies;
    for (const ntdlib::rsar::SoundArchive::Group& group : archive.Groups())
    {
        for (const ntdlib::rsar::SoundArchive::Group::Item& item : group.items)
        {
            const std::size_t start = wave_data ? std::size_t{group.wave_data_offset} + item.wave_data_offset
                                                : std::size_t{group.offset} + item.offset;
            const std::size_t size = wave_data ? item.wave_data_size : item.size;
            if (item.file_id == id && group.internal && start <= bytes.size() && size <= bytes.size() - start)
            {
                copies.emplace_back(bytes.begin() + static_cast<std::ptrdiff_t>(start),
                                    bytes.begin() + static_cast<std::ptrdiff_t>(start + size));
            }
        }
    }

    return copies;
}

namespace
{

// The IDs of the files that the groups of the Wii's `archive` hold, each once, in order.
std::vector<uint32_t> WiiFileIds(const ntdlib::rsar::SoundArchive& archive)
{
    std::set<uint32_t> ids;
    for (const ntdlib::rsar::SoundArchive::Group& group : archive.Groups())
    {
        for (const ntdlib::rsar::SoundArchive::Group::Item& item : group.items)
        {
            ids.insert(item.file_id);
        }
    }

    return {ids.begin(), ids.end()};
}

} // namespace

std::string OtherWiiFilesProblem(std::span<const uint8_t> built_bytes, const ntdlib::rsar::SoundArchive& built,
                                 std::span<const uint8_t> after_bytes, const ntdlib::rsar::SoundArchive& after,
                                 uint32_t changed, bool file, bool wave_data)
{
    for (const uint32_t id : WiiFileIds(built))
    {
        for (const bool data : {false, true})
        {
            const bool may_differ = id == changed && (data ? wave_data : file);
            if (!may_differ && WiiCopies(built_bytes, built, id, data) != WiiCopies(after_bytes, after, id, data))
            {
                return "changes " + std::string(data ? "the wave data of " : "") + "file " + std::to_string(id);
            }
        }
    }

    return {};
}

Source WiiSourceOf(std::span<const uint8_t> bytes, uint32_t start_offset, const ntdlib::SndBuild& build)
{
    Source source{edit::ReadIncoming(bytes, song::Console{.engine = hokuto::format::Engine::kWii, .wii = build}),
                  std::nullopt};
    source.start = edit::ItemAtOffset(
        edit::Offsets(source.incoming.sequence, song::Console{.engine = hokuto::format::Engine::kWii, .wii = build}),
        start_offset);

    return source;
}

bool Untracked(const Source& source, const ntdlib::rsar::SoundInfo& info)
{
    return (edit::TracksOpened(source.incoming.sequence, *source.start) & ~info.allocate_track_flags) != 0;
}

song::Sequence WiiPlayed(const ntdlib::rsar::SoundArchive& archive, uint32_t sound, const ntdlib::SndBuild& build)
{
    return Placeless(song::ReadSequence(archive, sound, {}, {}, build));
}

bool StartsAtLabel(const ntdlib::rsar::SoundArchive& archive, uint32_t sound)
{
    const ntdlib::rsar::SoundInfo& info = archive.Sounds()[sound];
    const ntdlib::rsar::SequenceFile file = ntdlib::rsar::SequenceFile::Read(archive.FileData(info.file_id));
    return std::ranges::any_of(file.labels, [&info](const auto& label) { return label.offset == info.start_offset; });
}

namespace
{

// What is wrong when sound `sound` of the Wii's `file` takes its sequence file again from `same`, or nothing.
std::string WiiSameProblem(const format::File& file, std::span<const std::optional<edit::SoundPlace>> places,
                           uint32_t sound, const Source& same)
{
    if (!same.start)
    {
        return "starts where no command of its file starts";
    }

    const std::vector<edit::StartingSound> starts = edit::SoundStarts(file, places, same.incoming.sequence);
    if (std::ranges::find(starts, edit::StartingSound{sound, *same.start}) == starts.end())
    {
        return "isn't offered its start in its file";
    }
    const uint32_t tracks = edit::TracksToAdd(file, places, sound, same.incoming.sequence, *same.start);
    if (edit::ReplaceSequence(file, places, sound, same.incoming.sequence, *same.start, {}, "Check", tracks) != file)
    {
        return "doesn't come back from its file";
    }

    return {};
}

// What is wrong when sound `sound` of the Wii's `file` takes sound `other`'s sequence file, from `source`, and bank, or
// nothing. `built` is the archive that `file` builds, read from `built_bytes`. `before` holds what the performer plays
// of each sound there. A sound whose loop the performer finds a whole number of passes away counts in `totals`. So does
// a sound that plays the notes with another length: its values, such as a volume of 0, determine when its last release
// falls silent.
std::string WiiCrossProblem(const format::File& file, std::span<const std::optional<edit::SoundPlace>> places,
                            const ntdlib::rsar::SoundArchive& built, std::span<const uint8_t> built_bytes,
                            const ntdlib::SndBuild& build, uint32_t sound, uint32_t other, const Source& source,
                            std::map<uint32_t, song::Sequence>& before, WiiTotals& totals)
{
    // The replacement, with the other sound's bank for each sound that plays the new commands. A sound of the Wii plays
    // one bank.
    const uint32_t bank = built.Sounds()[other].bank;
    const std::vector<uint32_t> banks = {bank < built.Banks().size() ? sar::ItemId(sar::kBankItem, bank)
                                                                     : sar::kNoItem};
    const uint32_t tracks = edit::TracksToAdd(file, places, sound, source.incoming.sequence, *source.start);
    const format::File replaced =
        edit::ReplaceSequence(file, places, sound, source.incoming.sequence, *source.start, {}, "Check", tracks, banks);
    std::vector<uint32_t> playing = edit::SharingStart(places, sound);
    playing.push_back(sound);
    hokuto::tools::CheckSaved(replaced, true);
    const std::vector<uint8_t> after_bytes = format::ExportArchive(replaced);
    const WiiArchive after = song::LoadWiiArchive(after_bytes, "after.brsar", ntdlib::rvl::snd::kRsarOptions);

    // The sounds that play the new commands play the other sound's notes. The file's other sounds play as before.
    const auto played_before = [&](uint32_t which) -> const song::Sequence&
    {
        auto found = before.find(which);
        if (found == before.end())
        {
            found = before.emplace(which, WiiPlayed(built, which, build)).first;
        }

        return found->second;
    };
    for (const uint32_t player : playing)
    {
        song::Sequence played = WiiPlayed(*after, player, build);
        if (played != played_before(other))
        {
            const uint32_t length = std::exchange(played.length, played_before(other).length);
            if (played == played_before(other))
            {
                totals.other_lengths++;
                continue;
            }
            played.length = length;
            if (!SameUpToLoop(played_before(other), played))
            {
                return "doesn't play " + built.Sounds()[other].name + "'s notes from its file";
            }
            totals.loops_moved++;
        }
    }
    for (const uint32_t sharing : edit::SharingFile(places, sound))
    {
        if (std::ranges::find(playing, sharing) == playing.end() &&
            WiiPlayed(*after, sharing, build) != played_before(sharing))
        {
            return "changes how " + built.Sounds()[sharing].name + " plays";
        }
    }

    // Every copy of the sound's file holds the new commands. The other files keep their bytes in every copy, and the
    // other sounds their starts.
    const uint32_t file_id = built.Sounds()[sound].file_id;
    const std::vector<std::vector<uint8_t>> copies = WiiCopies(after_bytes, *after, file_id, false);
    const std::span<const uint8_t> first = after->FileData(file_id);
    for (const std::vector<uint8_t>& copy : copies)
    {
        if (!std::ranges::equal(copy, first))
        {
            return "leaves a copy of its file with other bytes";
        }
    }
    if (copies.size() != WiiCopies(built_bytes, built, file_id, false).size())
    {
        return "changes the copies of its file";
    }
    std::string problem = OtherWiiFilesProblem(built_bytes, built, after_bytes, *after, file_id, true, false);
    for (std::size_t i = 0; i < built.Sounds().size() && problem.empty(); i++)
    {
        const bool plays = std::ranges::find(playing, static_cast<uint32_t>(i)) != playing.end();
        if (!plays && after->Sounds()[i].start_offset != built.Sounds()[i].start_offset)
        {
            problem = "moves the start of " + built.Sounds()[i].name;
        }
    }

    return problem;
}

// The banks' files, the wave sound files and the RWARs of the wave data of the Wii's `archive`, each file once, in that
// order. A wave archive's item is its file's ID.
std::vector<FileItem> WiiFileItemsOf(const ntdlib::rsar::SoundArchive& archive)
{
    std::vector<FileItem> items;
    std::set<uint32_t> seen;
    for (uint32_t i = 0; i < archive.Banks().size(); i++)
    {
        const uint32_t id = archive.Banks()[i].file_id;
        if (!archive.FileData(id).empty() && seen.insert(id).second)
        {
            items.push_back({edit::FileKind::kBank, i, id});
        }
    }
    for (const song::WaveSoundSet& set : song::WaveSoundSets(archive))
    {
        if (!set.sounds.empty() && !archive.FileData(set.file_id).empty() && seen.insert(set.file_id).second)
        {
            items.push_back({edit::FileKind::kWaveSoundFile, set.sounds.front(), set.file_id});
        }
    }
    for (const song::WiiWaveArchive& wave_archive : song::WaveArchivesOf(archive))
    {
        if (wave_archive.rwar && !archive.WaveData(wave_archive.file_id).empty())
        {
            items.push_back({edit::FileKind::kWaveArchive, wave_archive.file_id, wave_archive.file_id});
        }
    }

    return items;
}

// How the file of `item` of the Wii's `archive` keeps its waves: true in a WAVE block, false in an RWAR, and nothing
// for a wave archive or a file that ntdlib can't read.
std::optional<bool> WaveBlockOf(const ntdlib::rsar::SoundArchive& archive, const FileItem& item)
{
    try
    {
        const std::span<const uint8_t> bytes = archive.FileData(item.file_id);
        if (item.kind == edit::FileKind::kBank)
        {
            return ntdlib::rsar::Bank::Parse(bytes, ntdlib::rvl::snd::kRsarOptions).HasWaveBlock();
        }
        if (item.kind == edit::FileKind::kWaveSoundFile)
        {
            return ntdlib::rsar::WaveSoundFile::Parse(bytes, ntdlib::rvl::snd::kRsarOptions).HasWaveBlock();
        }
    }
    catch (const std::exception&)
    {
        // A file that ntdlib can't read gives no answer.
    }

    return std::nullopt;
}

} // namespace

std::vector<uint8_t> PaddedForWii(std::span<const uint8_t> bytes)
{
    std::vector<uint8_t> padded(bytes.begin(), bytes.end());
    padded.resize((padded.size() + 31) / 32 * 32, 0);

    return padded;
}

namespace
{

// What is wrong when the file of `item` of the Wii's `file` takes `bytes` through Replace…'s edit, or nothing. `built`
// is the archive that `file` builds, read from `built_bytes`. With `same`, the bytes are the file's, and the Hokuto
// file has to come back unchanged. Otherwise every copy of the file, or of its wave data for a wave archive, has to
// hold the new bytes, and the other files, the other part of the file and the sounds' starts have to stay. A refusal
// counts in `totals` by its reason.
std::string WiiFileProblem(const format::File& file, const ntdlib::rsar::SoundArchive& built,
                           std::span<const uint8_t> built_bytes, const FileItem& item, std::span<const uint8_t> bytes,
                           bool same, WiiTotals& totals)
{
    // The file as the Replace window reads it, and what it changes.
    const edit::IncomingFile incoming = edit::ReadWiiIncomingFile(bytes, item.kind);
    edit::ChangesOf(file, built, item.item, incoming);
    std::optional<format::File> replaced;
    try
    {
        replaced = edit::ReplaceWiiFile(file, built, item.item, incoming);
    }
    catch (const std::invalid_argument& error)
    {
        totals.refused[error.what()]++;
        return {};
    }
    if (same)
    {
        return *replaced == file ? std::string() : "doesn't come back from its file";
    }

    // The copies take the new bytes, padded as the tools pad them.
    hokuto::tools::CheckSaved(*replaced, true);
    const std::vector<uint8_t> after_bytes = format::ExportArchive(*replaced);
    const WiiArchive after = song::LoadWiiArchive(after_bytes, "after.brsar", ntdlib::rvl::snd::kRsarOptions);
    const bool wave_data = item.kind == edit::FileKind::kWaveArchive;
    const std::vector<std::vector<uint8_t>> copies = WiiCopies(after_bytes, *after, item.file_id, wave_data);
    const std::vector<uint8_t> padded = PaddedForWii(incoming.bytes);
    if (copies.size() != WiiCopies(built_bytes, built, item.file_id, wave_data).size())
    {
        return "changes the number of copies";
    }
    for (const std::vector<uint8_t>& copy : copies)
    {
        if (copy != padded)
        {
            return "leaves a copy without the new bytes";
        }
    }
    totals.copies += copies.size();
    std::string problem =
        OtherWiiFilesProblem(built_bytes, built, after_bytes, *after, item.file_id, !wave_data, wave_data);
    for (std::size_t i = 0; i < built.Sounds().size() && problem.empty(); i++)
    {
        if (after->Sounds()[i].start_offset != built.Sounds()[i].start_offset)
        {
            problem = "moves the start of " + built.Sounds()[i].name;
        }
    }

    return problem;
}

// Each file of a bank, a wave sound file and a wave archive of the Wii's `file` takes its bytes again, then the next
// file of its kind that keeps its waves in the same way (WiiFileProblem).
void CheckWiiFiles(const format::File& file, const ntdlib::rsar::SoundArchive& built,
                   std::span<const uint8_t> built_bytes, const std::string& name, WiiTotals& totals)
{
    const std::vector<FileItem> items = WiiFileItemsOf(built);

    const auto bytes_of = [&built](const FileItem& item)
    {
        return item.kind == edit::FileKind::kWaveArchive ? built.WaveData(item.file_id) : built.FileData(item.file_id);
    };
    for (std::size_t i = 0; i < items.size(); i++)
    {
        const FileItem& item = items[i];
        std::string problem;
        try
        {
            problem = WiiFileProblem(file, built, built_bytes, item, bytes_of(item), true, totals);
            totals.files++;

            // The next file of the same kind, kept in the same way.
            const std::optional<bool> block = WaveBlockOf(built, item);
            std::optional<std::size_t> next;
            for (std::size_t step = 1; step < items.size() && problem.empty() && !next; step++)
            {
                const FileItem& other = items[(i + step) % items.size()];
                if (other.kind != item.kind || other.file_id == item.file_id)
                {
                    continue;
                }
                if (WaveBlockOf(built, other) != block)
                {
                    totals.other_form++;
                    continue;
                }
                next = (i + step) % items.size();
            }
            if (problem.empty() && next)
            {
                problem = WiiFileProblem(file, built, built_bytes, item, bytes_of(items[*next]), false, totals);
                totals.files_crossed++;
            }
            else if (problem.empty())
            {
                totals.without_next++;
            }
        }
        catch (const std::exception& error)
        {
            problem = std::string("fails: ") + error.what();
        }

        if (!problem.empty())
        {
            const std::string label = item.kind == edit::FileKind::kWaveArchive
                                          ? "the wave data of file " + std::to_string(item.file_id)
                                          : "file " + std::to_string(item.file_id);
            std::printf("%s: %s %s\n", name.c_str(), label.c_str(), problem.c_str());
            totals.files_failed++;
        }
    }
}

// What is wrong when wave `wave` of the wave data of file `id` of the Wii's `file` goes back in its place, or nothing.
// `built` is the archive that `file` builds, and `present` holds the wave data's waves. The wave's file has to give the
// Hokuto file back. Its WAV file from Export… then goes in as 16-bit PCM and as DSP-ADPCM. The new wave has to hold the
// wave's rate, channels and loop, and the other waves their values. In 16-bit PCM, it has to decode to the wave's
// samples. In DSP-ADPCM, its loop may move forward to the next frame. A wave of more than two channels counts apart:
// the WAV file holds two. Throws a Refusal when Replace… refuses the wave.
std::string WiiWaveProblem(const format::File& file, const ntdlib::rsar::SoundArchive& built, uint32_t id,
                           uint32_t wave, const std::vector<ntdlib::rsar::WaveFile>& present, WiiTotals& totals)
{
    const ntdlib::rsar::WaveFile& old = present[wave];
    const std::string label = "wave " + std::to_string(wave);
    const edit::IncomingWave same = edit::ReadIncomingWiiWaveFile(old.Write());
    if (Refusable([&] { return edit::WithIncomingWiiWave(file, built, id, wave, same); }) != file)
    {
        return label + " doesn't come back from its wave file";
    }
    totals.waves++;
    if (old.channels.size() > 2)
    {
        totals.waves_wide++;
        return {};
    }

    const std::vector<uint8_t> wav = song::WaveAsWav(built, id, wave);
    const std::vector<std::vector<int16_t>> samples = edit::DecodedSamples(old, old.loop_end);
    for (const sar::WaveEncoding encoding : {sar::WaveEncoding::kPcm16, sar::WaveEncoding::kDspAdpcm})
    {
        const edit::IncomingWave incoming = edit::ReadIncomingWiiWave(wav, old.version, encoding);
        const format::File replaced =
            Refusable([&] { return edit::WithIncomingWiiWave(file, built, id, wave, incoming); });
        hokuto::tools::CheckSaved(replaced, true);
        const WiiArchive after =
            song::LoadWiiArchive(format::ExportArchive(replaced), "after.brsar", ntdlib::rvl::snd::kRsarOptions);
        const std::vector<ntdlib::rsar::WaveFile> waves = edit::WavesOf(*after, id);
        if (waves.size() != present.size())
        {
            return label + " changes the number of waves";
        }
        for (std::size_t j = 0; j < waves.size(); j++)
        {
            if (j != wave && waves[j] != present[j])
            {
                return label + " changes wave " + std::to_string(j);
            }
        }

        // A WAV file's loop holds a sample at least. A wave whose loop holds none comes back without the loop.
        const ntdlib::rsar::WaveFile& now = waves[wave];
        const bool empty_loop = old.loop && old.loop_start >= old.loop_end;
        if (now.sample_rate != old.sample_rate || now.channels.size() != old.channels.size() ||
            now.loop != (old.loop && !empty_loop) || now.version != old.version)
        {
            return label + " comes back from its WAV file with other values: " + std::to_string(old.sample_rate) +
                   " Hz to " + std::to_string(now.sample_rate) + " Hz, " + std::to_string(old.channels.size()) +
                   " channels to " + std::to_string(now.channels.size()) + ", loop " + (old.loop ? "on" : "off") +
                   " to " + (now.loop ? "on" : "off") + ", version " + std::to_string(old.version) + " to " +
                   std::to_string(now.version);
        }
        if (encoding == sar::WaveEncoding::kPcm16)
        {
            if ((!empty_loop && now.loop_start != old.loop_start) || now.loop_end != old.loop_end ||
                edit::DecodedSamples(now, old.loop_end) != samples)
            {
                return label + " decodes otherwise from its WAV file";
            }
            totals.empty_loops += empty_loop ? 1 : 0;
            continue;
        }

        // DSP-ADPCM: a loop starts on a frame of 14 samples.
        const bool forward = now.loop_start != old.loop_start;
        if (now.loop && (now.loop_start < old.loop_start || now.loop_start - old.loop_start >= 14))
        {
            return label + " comes back from its WAV file in DSP-ADPCM with its loop elsewhere";
        }
        totals.loops_forward += now.loop && forward ? 1 : 0;
        if (incoming.signal_to_noise)
        {
            totals.ratios.push_back(*incoming.signal_to_noise);
        }
        else
        {
            totals.exact++;
        }
    }

    return {};
}

// Each wave of each wave data of the Wii's `file` goes back in its place (WiiWaveProblem). `built` is the archive that
// `file` builds. Wave data that Replace… can't read counts in `totals` by its reason.
void CheckWiiWaves(const format::File& file, const ntdlib::rsar::SoundArchive& built, const std::string& name,
                   WiiTotals& totals)
{
    for (const song::WiiWaveArchive& wave_archive : song::WaveArchivesOf(built))
    {
        std::vector<ntdlib::rsar::WaveFile> present;
        try
        {
            present = edit::WavesOf(built, wave_archive.file_id);
        }
        catch (const std::invalid_argument& error)
        {
            totals.refused[error.what()]++;
            continue;
        }
        std::string problem;
        bool refused = false;
        for (uint32_t wave = 0; wave < present.size() && problem.empty() && !refused; wave++)
        {
            try
            {
                problem = WiiWaveProblem(file, built, wave_archive.file_id, wave, present, totals);
            }
            catch (const Refusal& error)
            {
                // Replace… refuses the wave data, such as wave data past the end of an archive that is cut short.
                totals.refused[error.what()]++;
                refused = true;
            }
            catch (const std::exception& error)
            {
                problem = "wave " + std::to_string(wave) + " fails: " + error.what();
            }
        }
        totals.wave_archives += refused ? 0 : 1;
        if (!problem.empty())
        {
            std::printf("%s: %s %s\n", name.c_str(), wave_archive.name.c_str(), problem.c_str());
            totals.wave_archives_failed++;
        }
    }
}

// Prints the summary of the sequences of the Wii's archive `name` with `sequences`, and of its files with `files`.
void PrintWiiSequencesAndFiles(const std::string& name, const WiiTotals& totals, bool sequences, bool files)
{
    if (sequences)
    {
        std::printf(
            "%s: %zu sequence sounds took their files again. %zu took another sound's file. %zu fail. %zu sounds' "
            "files stay in the archive's bytes. %zu sounds start past the first command, where their files have no "
            "label, and Replace… offers their starts. %zu sounds open tracks that they don't allocate. With another "
            "sound's file, ntdlib's performer found the loop of %zu sounds a whole number of passes away. The values "
            "of %zu sounds, such as a volume of 0, decide another length for the other sound's notes.\n",
            name.c_str(), totals.sounds, totals.crossed, totals.failed, totals.in_bytes, totals.unlabelled,
            totals.untracked, totals.loops_moved, totals.other_lengths);
    }
    if (files)
    {
        std::printf(
            "%s: %zu files of banks, wave archives and wave sounds took their bytes again, and %zu took another "
            "file's, in %zu copies. %zu fail. %zu times a next file kept its waves otherwise, and %zu files had no "
            "next file of their kind.\n",
            name.c_str(), totals.files, totals.files_crossed, totals.copies, totals.files_failed, totals.other_form,
            totals.without_next);
    }
}

// Checks sequence sound `i` of `built`, the archive that `file` builds, with its file (WiiSameProblem), then with the
// next sequence sound's file (WiiCrossProblem). `places` lists where each sound is in `file`, `built_bytes` are the
// bytes of `built` and `build` its build of NW4R SND. `before` keeps the sounds' sequences as they played before.
// Prints a problem of archive `name`, and counts the sound in `totals`.
void CheckWiiSequenceSound(const format::File& file, std::span<const std::optional<edit::SoundPlace>> places,
                           const WiiArchive& built, std::span<const uint8_t> built_bytes, const ntdlib::SndBuild& build,
                           std::size_t i, std::map<uint32_t, song::Sequence>& before, const std::string& name,
                           WiiTotals& totals)
{
    const auto sound = static_cast<uint32_t>(i);
    const ntdlib::rsar::SoundInfo& info = built->Sounds()[i];
    if (info.type != ntdlib::rsar::SoundType::kSequence || built->FileData(info.file_id).empty())
    {
        return;
    }
    if (!places[i])
    {
        totals.in_bytes++;
        return;
    }

    std::string problem;
    try
    {
        const Source same = WiiSourceOf(built->FileData(info.file_id), info.start_offset, build);
        totals.unlabelled += info.start_offset != 0 && !StartsAtLabel(*built, sound) ? 1 : 0;
        const bool untracked = same.start && Untracked(same, info);
        totals.untracked += untracked ? 1 : 0;
        problem = WiiSameProblem(file, places, sound, same);

        // The next sequence sound with another file. A sound passes its turn on when it opens tracks that it
        // doesn't allocate.
        for (std::size_t step = 1; step < places.size() && problem.empty() && !untracked; step++)
        {
            const auto other = static_cast<uint32_t>((i + step) % places.size());
            if (!places[other] || places[other]->sequence == places[i]->sequence)
            {
                continue;
            }

            const ntdlib::rsar::SoundInfo& other_info = built->Sounds()[other];
            const Source source = WiiSourceOf(built->FileData(other_info.file_id), other_info.start_offset, build);
            if (!source.start || Untracked(source, other_info))
            {
                continue;
            }

            problem = WiiCrossProblem(file, places, *built, built_bytes, build, sound, other, source, before, totals);
            totals.crossed++;
            break;
        }
    }
    catch (const std::exception& error)
    {
        problem = std::string("fails: ") + error.what();
    }

    if (!problem.empty())
    {
        std::printf("%s: %s %s\n", name.c_str(), info.name.c_str(), problem.c_str());
        totals.failed++;
    }
    totals.sounds++;
}

// Prints the summary of the checks of the Wii in archive `name`: the sequences and files, the waves and the refusals.
// `files_only`, `sequences_only` and `waves_only` are the options that the checks ran with.
void PrintWiiSummary(const std::string& name, const WiiTotals& totals, bool files_only, bool sequences_only,
                     bool waves_only)
{
    PrintWiiSequencesAndFiles(name, totals, !files_only && !waves_only, !sequences_only && !waves_only);
    const std::vector<double>& ratios = totals.ratios;
    const double least = ratios.empty() ? 0.0 : *std::ranges::min_element(ratios);
    if (!sequences_only && !files_only)
    {
        std::printf(
            "%s: %zu waves of %zu wave archives came back from their wave files and their WAV files. %zu wave "
            "archives fail. %zu waves hold more than two channels. As DSP-ADPCM, %zu waves came back exactly, and %zu "
            "with a signal-to-noise ratio of at least %.1f dB. %zu loops moved forward to a frame. %zu waves' loops "
            "hold no samples, and they came back from their WAV files without them.\n",
            name.c_str(), totals.waves, totals.wave_archives, totals.wave_archives_failed, totals.waves_wide,
            totals.exact, ratios.size(), least, totals.loops_forward, totals.empty_loops);
    }
    for (const auto& [reason, count] : totals.refused)
    {
        std::printf("%s: refused %zu times: %s\n", name.c_str(), count, reason.c_str());
    }
}

} // namespace

int CheckWii(const std::vector<std::string>& arguments, const std::filesystem::path& path,
             const std::vector<uint8_t>& bytes)
{
    const bool files_only = arguments[1] == "--files";
    const bool sequences_only = arguments[1] == "--sequences";
    const bool waves_only = arguments[1] == "--waves";
    const bool imports = arguments[1] == "--new" || arguments[1] == "--banks" || arguments[1] == "--wave-sounds" ||
                         arguments[1] == "--new-waves";
    const bool donor = arguments.size() == 4 && (arguments[1] == "--new" || arguments[1] == "--wave-sounds" ||
                                                 arguments[1] == "--new-waves" || arguments[1] == "--files");
    if ((arguments.size() != 2 && !files_only && !sequences_only && !waves_only && !imports) ||
        (arguments.size() == 4 && !donor))
    {
        std::fprintf(stderr,
                     "hokuto_check_replace takes only --sequences, --files, --waves, --new, --banks, "
                     "--wave-sounds, --new-waves and --streams for an archive of the Wii, and a donor only after "
                     "--new, --wave-sounds, --new-waves, --files and --streams\n");
        return 2;
    }

    // The file, the archive that it builds, and where each sound plays in the file.
    const std::string name = xsflib::Utf8(path.filename());
    std::optional<format::File> imported;
    std::vector<uint8_t> built_bytes;
    WiiArchive built;
    std::vector<std::optional<edit::SoundPlace>> places;
    ntdlib::SndBuild build;
    try
    {
        const std::optional<ntdlib::SndBuild> game = hokuto::tools::GameBuildOf(path);
        imported = format::ImportWiiArchive(bytes, game);
        imported = hokuto::tools::CheckedForm(std::move(*imported), bytes);
        build = format::BuildOfWiiArchive(*imported->native);
        built_bytes = format::ExportArchive(*imported);
        built = song::LoadWiiArchive(built_bytes, name, ntdlib::rvl::snd::kRsarOptions);
        places = edit::SoundPlaces(*built, format::SequencesOfFiles(*imported, built_bytes, *built),
                                   format::StartFieldsOfSounds(*imported, built_bytes));
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
        return 2;
    }
    const format::File& file = *imported;
    if (donor)
    {
        return CheckWiiDonor(file, built, built_bytes, build, name, arguments[1], xsflib::PathFromUtf8(arguments[3]));
    }
    if (imports)
    {
        return CheckWiiImports(file, built, built_bytes, build, name, arguments[1]);
    }

    // Each sequence sound with its file (WiiSameProblem), then with the next sequence sound's file (WiiCrossProblem).
    WiiTotals totals;
    std::map<uint32_t, song::Sequence> before;
    for (std::size_t i = 0; i < built->Sounds().size() && !files_only && !waves_only; i++)
    {
        CheckWiiSequenceSound(file, places, built, built_bytes, build, i, before, name, totals);
    }

    if (!sequences_only && !waves_only)
    {
        CheckWiiFiles(file, *built, built_bytes, name, totals);
    }
    if (!sequences_only && !files_only)
    {
        CheckWiiWaves(file, *built, name, totals);
    }

    PrintWiiSummary(name, totals, files_only, sequences_only, waves_only);

    return totals.failed > 0 || totals.files_failed > 0 || totals.wave_archives_failed > 0 ? 1 : 0;
}

} // namespace hokuto::tools::replace
