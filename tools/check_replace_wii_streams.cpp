// SPDX-License-Identifier: MIT

// The streams of hokuto_check_replace on archives of the Wii.

#include "check_replace.h"

namespace hokuto::tools::replace
{

namespace
{

// Whether `info`, the entry of a stream sound of a Wii archive of `version`, names every channel and track of `stream`:
// the channels by their count from version 1.4 on, and by a bit for each before.
bool NamesWholeStream(const ntdlib::rsar::SoundInfo& info, uint16_t version, const ntdlib::rsar::StreamFile& stream)
{
    const auto channels = static_cast<uint32_t>(stream.channels.size());
    const uint32_t named = version >= 0x0104 ? channels : (1U << channels) - 1;
    return info.allocate_channels == named && info.allocate_track_flags == (1U << stream.tracks.size()) - 1;
}

// The first 2 s of what `preview` plays, as floats.
std::vector<float> WiiPreviewSamples(song::SoundPreview& preview)
{
    constexpr std::size_t kBlock = 256;
    const std::size_t frames = 2 * static_cast<std::size_t>(preview.SampleRate());
    std::vector<float> samples(frames * 2);
    std::size_t done = 0;
    while (done < frames)
    {
        const std::size_t rendered = preview.Render(samples.data() + done * 2, std::min(kBlock, frames - done));
        if (rendered == 0)
        {
            break;
        }
        done += rendered;
    }
    samples.resize(done * 2);

    return samples;
}

// Whether each later pass of the loop of `stream`, a stream of the Wii, decodes to the samples of its first pass in
// `decoded`. A channel in DSP-ADPCM decodes from the loop's predictor, scale and history, with the Wii's sums.
bool WiiLaterPassesMatch(const ntdlib::rsar::StreamFile& stream, const sar::Pcm& decoded)
{
    if (!stream.loop || stream.encoding != ntdlib::rsar::WaveEncoding::kDspAdpcm)
    {
        return true;
    }

    for (std::size_t c = 0; c < stream.channels.size(); c++)
    {
        const ntdlib::rsar::StreamFile::Channel& held = stream.channels[c];
        ntdlib::wave::EncodedWave wave;
        wave.encoding = ntdlib::wave::Encoding::kDspAdpcm;
        wave.sum = ntdlib::wave::DspAdpcmSum::kWide;
        wave.loop_end = stream.sample_count - stream.loop_start;
        ntdlib::wave::EncodedChannel& encoded = wave.channels.emplace_back();
        encoded.data = std::span(held.samples).subspan(stream.loop_start / 14 * 8);
        encoded.coefs = &held.adpcm->coefs;
        encoded.yn1 = held.adpcm->loop_yn1;
        encoded.yn2 = held.adpcm->loop_yn2;
        const std::vector<int16_t> pass = ntdlib::wave::DecodeWave(wave).channels.at(0);
        const std::vector<int16_t>& first = decoded.channels.at(c);
        if (first.size() < stream.sample_count || pass.size() < stream.sample_count - stream.loop_start ||
            !std::equal(first.begin() + stream.loop_start, first.begin() + stream.sample_count, pass.begin()))
        {
            return false;
        }
    }

    return true;
}

// What is wrong with `added`, the Wii's `file` after its archive gains stream sound `name`, a copy of sound `model`
// that plays `made` from a new file beside the archive, or nothing. `built` is the archive that `file` builds, and
// `before` its model. With `same_stream`, `made` holds the stream that the model plays.
std::string WiiNewStreamProblem(const format::File& file, const WiiArchive& built,
                                const ntdlib::rsar::ArchiveModel& before, const format::File& added, uint32_t model,
                                const std::string& name, const edit::NewStream& made, const ntdlib::SndBuild& build,
                                bool same_stream, WiiStreamTotals& totals)
{
    hokuto::tools::CheckSaved(added, true);
    totals.new_sounds++;
    const std::vector<uint8_t> after_bytes = format::ExportArchive(added);
    const WiiArchive after = song::LoadWiiArchive(after_bytes, "after.brsar", ntdlib::rvl::snd::kRsarOptions);
    const ntdlib::rsar::ArchiveModel now = ntdlib::rsar::ReadArchiveModel(after_bytes);
    const auto sound = static_cast<uint32_t>(before.sounds.size());
    const auto id = static_cast<uint32_t>(before.files.size());
    if (now.sounds.size() != sound + 1 || now.files.size() != id + 1 || after->Sounds().size() != sound + 1)
    {
        return "doesn't give the archive one sound and one file more";
    }

    // The new file lies beside the archive at the new path, with the stream's size. The file keeps the new stream as
    // Hokuto's, and the other streams as they were.
    const ntdlib::rsar::FileModel beside{.path = made.path, .path_size = static_cast<uint32_t>(made.bytes.size())};
    if (now.files[id] != beside || song::StreamPath(*after, id) != std::filesystem::path(made.path))
    {
        return "gives the new file another path or size";
    }
    const auto found = added.native->streams.find(id);
    if (found == added.native->streams.end() || *found->second.bytes != made.bytes || found->second.from_game ||
        added.native->streams.size() != file.native->streams.size() + 1)
    {
        return "doesn't keep the new stream as Hokuto's";
    }
    for (const auto& [kept_id, stream] : file.native->streams)
    {
        const auto kept = added.native->streams.find(kept_id);
        if (kept == added.native->streams.end() || kept->second != stream)
        {
            return "changes stream file " + std::to_string(kept_id);
        }
    }

    // The new sound takes the model's settings, and its entry names every channel and track of the stream.
    const ntdlib::rsar::SoundModel& copied = before.sounds[model];
    const ntdlib::rsar::StreamFile stream = ntdlib::rsar::StreamFile::Read(made.bytes);
    const auto channels = static_cast<uint32_t>(stream.channels.size());
    ntdlib::rsar::SoundModel expected;
    expected.name = name;
    expected.type = ntdlib::rsar::SoundType::kStream;
    expected.file_id = id;
    expected.player_id = copied.player_id;
    expected.volume = copied.volume;
    expected.player_priority = copied.player_priority;
    expected.remote_filter = copied.remote_filter;
    expected.user_params = copied.user_params;
    expected.pan_mode = copied.pan_mode;
    expected.pan_curve = copied.pan_curve;
    expected.actor_player_id = copied.actor_player_id;
    expected.settings_3d = copied.settings_3d;
    expected.allocate_channel_count = static_cast<uint16_t>(before.version >= 0x0104 ? channels : (1U << channels) - 1);
    expected.stream_track_flags = static_cast<uint16_t>((1U << stream.tracks.size()) - 1);
    if (now.sounds[sound] != expected)
    {
        return "gives the new sound other values than the model's and the stream's";
    }

    // The reader gives the entry that the Import window plays.
    const ntdlib::rsar::SoundInfo& read = after->Sounds()[sound];
    const ntdlib::rsar::SoundInfo& offered = *made.wii_sound;
    if (read.name != offered.name || read.file_id != offered.file_id || read.type != offered.type ||
        read.player_id != offered.player_id || read.volume != offered.volume ||
        read.player_priority != offered.player_priority || read.remote_filter != offered.remote_filter ||
        !std::ranges::equal(read.user_params, offered.user_params) || read.pan_mode != offered.pan_mode ||
        read.pan_curve != offered.pan_curve || read.allocate_channels != offered.allocate_channels ||
        read.allocate_track_flags != offered.allocate_track_flags)
    {
        return "reads otherwise than the entry that the Import window plays";
    }

    // The other items, files and groups stay, and so do the file's sequences. The new file joins no group.
    if (std::string problem = WiiOthersProblem(before, now, {}); !problem.empty())
    {
        return problem;
    }
    if (added.sequences != file.sequences)
    {
        return "changes the file's sequences";
    }

    // The Import window's Play gives the samples of the browser's Play after the import. Where the model's entry names
    // every channel and track of the stream that it plays, the model plays them too.
    song::SoundPreview offered_play(built, offered, made.bytes, build);
    song::SoundPreview after_play(after, sound, made.bytes, build);
    const std::vector<float> samples = WiiPreviewSamples(after_play);
    if (WiiPreviewSamples(offered_play) != samples)
    {
        return "plays otherwise than the Import window plays it";
    }
    totals.sounding += std::ranges::any_of(samples, [](float sample) { return sample != 0.0f; }) ? 1 : 0;
    const ntdlib::rsar::SoundInfo& model_info = built->Sounds()[model];
    if (same_stream && model_info.type == ntdlib::rsar::SoundType::kStream &&
        NamesWholeStream(model_info, built->Version(), stream))
    {
        song::SoundPreview model_play(built, model, made.bytes, build);
        if (WiiPreviewSamples(model_play) != samples)
        {
            return "plays otherwise than its model plays the stream";
        }
        totals.same_as_model++;
    }

    return {};
}

// What is wrong when the files that the browser's Export… writes of stream file `bytes` for the later consoles come
// back, or nothing. `present` is the stream, `changes` the changes of its sound and `decoded` its samples. Counts the
// files in `totals`.
std::string OtherConsoleFilesProblem(const std::vector<uint8_t>& bytes, const ntdlib::rsar::StreamFile& present,
                                     const edit::StreamChanges& changes, const sar::Pcm& decoded,
                                     WiiStreamTotals& totals)
{
    // Its files that the browser's Export… writes for the 3DS, the Wii U and the Switch. Each comes back with the
    // stream's samples and tracks.
    for (const sar::Flavor flavor : {sar::Flavor::kCtr, sar::Flavor::kCafe, sar::Flavor::kNx})
    {
        const std::string console = "the " + song::ConsoleName(flavor);
        std::vector<uint8_t> other;
        try
        {
            other = song::ConvertStreamFile(bytes, flavor, song::ExportVersions(song::ItemKind::kStream, flavor).file);
        }
        catch (const std::runtime_error& error)
        {
            totals.refused["a file for " + console + ": " + error.what()]++;
            continue;
        }
        const edit::IncomingStream back = edit::ReadIncomingWiiStreamFile(other, changes.wii_tracks);
        if (!back.converted || back.converted->flavor != flavor ||
            !PlaysSame(decoded, song::DecodeStreamFile(back.bytes)))
        {
            return "comes back from " + console + " with other samples";
        }
        if (ntdlib::rsar::StreamFile::Read(back.bytes).tracks != present.tracks)
        {
            return "comes back from " + console + " with other tracks";
        }
        totals.conversions++;
        totals.exact_conversions += back.bytes == bytes ? 1 : 0;
    }

    return {};
}

// What is wrong when `wav`, the WAV file of stream file `id` of `file`, is imported as 16-bit PCM, or nothing. `bytes`
// is the stream file, `present` the stream, `changes` the changes of its sound and `decoded` its samples. `built_bytes`
// are the bytes of the archive that `file` builds, and `before` its model where the tools laid it out. With `second`,
// the model form exports the same archive. Sets `pcm` to the stream from the WAV file, and counts it in `totals`.
std::string WavPcmProblem(const format::File& file, std::span<const uint8_t> built_bytes,
                          const std::optional<ntdlib::rsar::ArchiveModel>& before, uint32_t id,
                          const std::vector<uint8_t>& bytes, const ntdlib::rsar::StreamFile& present,
                          const edit::StreamChanges& changes, const sar::Pcm& decoded, const std::vector<uint8_t>& wav,
                          bool second, edit::IncomingStream& pcm, WiiStreamTotals& totals)
{
    // The WAV file in 16-bit PCM: the same samples and tracks. An export writes the new stream's size into the file
    // table, and nothing else changes. The stream itself then gives the archive back.
    pcm = edit::ReadIncomingWiiStreamWav(wav, sar::WaveEncoding::kPcm16, changes.wii_tracks);
    if (!PlaysSame(decoded, song::DecodeStreamFile(pcm.bytes)))
    {
        return "plays other samples from its WAV file in 16-bit PCM";
    }
    if (ntdlib::rsar::StreamFile::Read(pcm.bytes).tracks != present.tracks)
    {
        return "doesn't keep the present stream's tracks from its WAV file";
    }
    totals.moved += pcm.loop_moved > 0 ? 1 : 0;
    const format::File replaced = Refusable([&] { return edit::ReplaceStream(file, id, pcm.bytes); });
    hokuto::tools::CheckSaved(replaced, true);
    if (pcm.bytes == bytes)
    {
        totals.same_wav++;
        if (replaced != file)
        {
            return "doesn't give the file back with its stream from its WAV file";
        }
    }
    else
    {
        if (replaced.native->streams.at(id).from_game || *replaced.native->streams.at(id).bytes != pcm.bytes)
        {
            return "doesn't keep the stream from its WAV file as Hokuto's";
        }
        const std::vector<uint8_t> exported = format::ExportArchive(replaced);
        std::vector<std::size_t> differing;
        for (std::size_t i = 0; i < std::min(exported.size(), built_bytes.size()); i++)
        {
            if (exported[i] != built_bytes[i])
            {
                differing.push_back(i);
            }
        }
        if (exported.size() != built_bytes.size() || (!differing.empty() && differing.back() - differing.front() >= 4))
        {
            return "changes more of the archive than the stream's size in the file table";
        }
        if (before)
        {
            ntdlib::rsar::ArchiveModel expected = *before;
            expected.files.at(id).path_size = static_cast<uint32_t>(pcm.bytes.size());
            if (ntdlib::rsar::ReadArchiveModel(exported) != expected)
            {
                return "doesn't give the file table the new stream's size";
            }
        }
        if (!std::ranges::equal(
                format::ExportArchive(Refusable([&] { return edit::ReplaceStream(replaced, id, bytes); })),
                built_bytes))
        {
            return "doesn't give the archive back with the stream again";
        }
        if (second && before)
        {
            const format::File in_model = format::InModelForm(replaced);
            if (format::ExportArchive(in_model) != exported || !RoundTrips(in_model))
            {
                return "exports otherwise, or doesn't save and open again, in the model form";
            }
        }
    }

    return {};
}

// What is wrong when `wav`, the WAV file of stream `present`, is imported as DSP-ADPCM, or nothing. `changes` are the
// changes of its sound. Counts the stream in `totals`.
std::string WavAdpcmProblem(const std::vector<uint8_t>& wav, const ntdlib::rsar::StreamFile& present,
                            const edit::StreamChanges& changes, WiiStreamTotals& totals)
{
    // The WAV file in DSP-ADPCM. A loop starts at a block, and its later passes decode to its first.
    const edit::IncomingStream adpcm =
        edit::ReadIncomingWiiStreamWav(wav, sar::WaveEncoding::kDspAdpcm, changes.wii_tracks);
    const ntdlib::rsar::StreamFile adpcm_stream = ntdlib::rsar::StreamFile::Read(adpcm.bytes);
    if (adpcm_stream.tracks != present.tracks)
    {
        return "doesn't keep the present stream's tracks from its WAV file in DSP-ADPCM";
    }
    if (adpcm_stream.loop != present.loop ||
        (present.loop &&
         (adpcm_stream.loop_start < present.loop_start || adpcm_stream.loop_start % adpcm_stream.block_samples != 0)))
    {
        return "comes back from its WAV file in DSP-ADPCM with its loop elsewhere";
    }
    if (!WiiLaterPassesMatch(adpcm_stream, song::DecodeStreamFile(adpcm.bytes)))
    {
        return "decodes a later pass of its loop in DSP-ADPCM otherwise than its first";
    }
    if (adpcm.signal_to_noise)
    {
        totals.ratios.push_back(*adpcm.signal_to_noise);
    }
    else
    {
        totals.exact++;
    }

    return {};
}

// What is wrong when a new sound that copies stream sound `model` is added with stream file `id` of `file`, whose bytes
// are `bytes`, or nothing. `same` is the stream as the edit imports it, and `pcm` the stream from its WAV file for the
// second new sound, with `second`. `built` is the archive that `file` builds, `before` its model where the tools laid
// it out and `build` its build of NW4R SND. Counts the sounds in `totals`.
std::string NewStreamSoundsProblem(const format::File& file, const WiiArchive& built,
                                   const std::optional<ntdlib::rsar::ArchiveModel>& before,
                                   const ntdlib::SndBuild& build, uint32_t id, uint32_t model,
                                   const std::vector<uint8_t>& bytes, const edit::IncomingStream& same,
                                   const edit::IncomingStream& pcm, bool second, WiiStreamTotals& totals)
{
    // A new sound that copies the sound, with the stream from its file. An archive that the tools didn't lay out takes
    // none, and the tool counts the refusal.
    const std::string name = "HOKUTO_CHECK_STRM_" + std::to_string(id);
    const edit::NewStream made = Refusable([&] { return edit::MakeNewWiiStream(*built, model, name, same); });
    if (made.bytes != bytes || !made.file_tracks || !made.tracks_in_file || !made.wii_sound)
    {
        return "doesn't go in as it is as a new sound's stream";
    }
    if (!before)
    {
        Refusable([&] { return edit::AddWiiStreamSound(file, model, name, made); });
        return "takes a new sound in an archive that the tools didn't lay out";
    }
    const auto start = std::chrono::steady_clock::now();
    const format::File added = Refusable([&] { return edit::AddWiiStreamSound(file, model, name, made); });
    totals.slowest =
        std::max(totals.slowest, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    if (std::string problem = WiiNewStreamProblem(file, built, *before, added, model, name, made, build, true, totals);
        !problem.empty())
    {
        return problem;
    }
    if (!second)
    {
        return {};
    }

    // A second new sound from the WAV file, with a sound of another kind as its model where the archive holds one, in
    // both forms. A model at volume 0 would leave the new sound silent. The file saves and opens again.
    const std::vector<uint8_t> added_bytes = format::ExportArchive(added);
    const WiiArchive after = song::LoadWiiArchive(added_bytes, "after.brsar", ntdlib::rvl::snd::kRsarOptions);
    const ntdlib::rsar::ArchiveModel after_model = ntdlib::rsar::ReadArchiveModel(added_bytes);
    const auto audible_other = [](const ntdlib::rsar::SoundInfo& sound)
    {
        return sound.type != ntdlib::rsar::SoundType::kStream && sound.volume > 0;
    };
    const auto other = std::ranges::find_if(after->Sounds(), audible_other);
    const auto second_model =
        other != after->Sounds().end() ? static_cast<uint32_t>(other - after->Sounds().begin()) : model;
    const std::string second_name = name + "_2";
    const edit::NewStream second_made =
        Refusable([&] { return edit::MakeNewWiiStream(*after, second_model, second_name, pcm); });
    const format::File twice =
        Refusable([&] { return edit::AddWiiStreamSound(added, second_model, second_name, second_made); });
    if (std::string problem = WiiNewStreamProblem(added, after, after_model, twice, second_model, second_name,
                                                  second_made, build, false, totals);
        !problem.empty())
    {
        return "after a first new sound, " + problem;
    }
    const format::File in_model = format::InModelForm(added);
    const format::File twice_in_model =
        Refusable([&] { return edit::AddWiiStreamSound(in_model, second_model, second_name, second_made); });
    if (twice_in_model.form != in_model.form || format::ExportArchive(twice_in_model) != format::ExportArchive(twice))
    {
        return "adds a second new sound otherwise in the model form";
    }
    for (const format::File& form : {twice, twice_in_model})
    {
        if (!RoundTrips(form))
        {
            return "doesn't save and open again with two new sounds";
        }
    }
    totals.pairs++;

    return {};
}

} // namespace

std::string WiiStreamProblem(const format::File& file, const WiiArchive& built, std::span<const uint8_t> built_bytes,
                             const std::optional<ntdlib::rsar::ArchiveModel>& before, const ntdlib::SndBuild& build,
                             uint32_t id, uint32_t model, bool second, WiiStreamTotals& totals)
{
    const std::vector<uint8_t>& bytes = *file.native->streams.at(id).bytes;
    const ntdlib::rsar::StreamFile present = ntdlib::rsar::StreamFile::Read(bytes);
    const edit::StreamChanges changes = Refusable([&] { return edit::ChangesOfWiiStream(file, *built, model); });
    totals.channels += present.channels.size();

    // The stream itself.
    const edit::IncomingStream same = edit::ReadIncomingWiiStreamFile(bytes, changes.wii_tracks);
    if (same.converted || same.bytes != bytes ||
        Refusable([&] { return edit::ReplaceStream(file, id, same.bytes); }) != file)
    {
        return "doesn't give the file back with its stream";
    }

    const sar::Pcm decoded = song::DecodeStreamFile(bytes);
    if (std::string problem = OtherConsoleFilesProblem(bytes, present, changes, decoded, totals); !problem.empty())
    {
        return problem;
    }

    const std::vector<uint8_t> wav = song::StreamAsWav(bytes);
    edit::IncomingStream pcm;
    if (std::string problem =
            WavPcmProblem(file, built_bytes, before, id, bytes, present, changes, decoded, wav, second, pcm, totals);
        !problem.empty())
    {
        return problem;
    }
    if (std::string problem = WavAdpcmProblem(wav, present, changes, totals); !problem.empty())
    {
        return problem;
    }

    return NewStreamSoundsProblem(file, built, before, build, id, model, bytes, same, pcm, second, totals);
}

std::string WiiDonorStreamProblem(const format::File& file, const WiiArchive& built,
                                  const std::optional<ntdlib::rsar::ArchiveModel>& before,
                                  const ntdlib::SndBuild& build, uint32_t model, uint32_t donor_id,
                                  const std::vector<uint8_t>& bytes, bool save, WiiStreamTotals& totals)
{
    const std::string name = "HOKUTO_CHECK_DONOR_" + std::to_string(donor_id);
    const edit::IncomingStream incoming = Refusable([&] { return edit::ReadIncomingWiiStreamFile(bytes); });
    const edit::NewStream made = Refusable([&] { return edit::MakeNewWiiStream(*built, model, name, incoming); });
    const ntdlib::rsar::StreamFile stream = ntdlib::rsar::StreamFile::Read(made.bytes);
    totals.channels += stream.channels.size();
    if (!before)
    {
        Refusable([&] { return edit::AddWiiStreamSound(file, model, name, made); });
        return "takes a new sound in an archive that the tools didn't lay out";
    }

    // The new stream plays the donor's samples. A stream of a later console without tracks takes a track for each pair
    // of its channels.
    if (!PlaysSame(song::DecodeStreamFile(bytes), song::DecodeStreamFile(made.bytes)))
    {
        return "plays other samples than the donor's stream";
    }
    if (!made.file_tracks)
    {
        if (stream.tracks.size() != (stream.channels.size() + 1) / 2)
        {
            return "doesn't take a track for each pair of its channels";
        }
        totals.paired++;
    }
    const auto start = std::chrono::steady_clock::now();
    const format::File added = Refusable([&] { return edit::AddWiiStreamSound(file, model, name, made); });
    totals.slowest =
        std::max(totals.slowest, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    if (std::string problem = WiiNewStreamProblem(file, built, *before, added, model, name, made, build, false, totals);
        !problem.empty())
    {
        return problem;
    }
    if (save && !RoundTrips(added))
    {
        return "doesn't save and open again with the new sound";
    }

    return {};
}

namespace
{

// The streams beside the donor archive at `path`, of the Wii, the 3DS, the Wii U or the Switch, by their files' IDs.
std::map<uint32_t, std::vector<uint8_t>> DonorStreamsOf(const std::filesystem::path& path)
{
    std::vector<uint8_t> bytes;
    if (!xsflib::ReadFile(path, bytes))
    {
        throw std::runtime_error(xsflib::Utf8(path.filename()) + " can't be read");
    }
    if (IsWiiArchive(bytes))
    {
        const WiiArchive archive =
            song::LoadWiiArchive(bytes, xsflib::Utf8(path.filename()), ntdlib::rvl::snd::kRsarOptions);
        return song::ReadStreams(*archive, path);
    }

    return song::ReadStreams(*song::OpenArchive(path), path);
}

// Runs `check` of `item` in archive `name`, and counts the stream in `totals`.
// Each check counts the edits that Hokuto refuses by their messages, and fails on any other error.
template <typename Check>
void Attempt(const std::string& name, const std::string& item, const Check& check, WiiStreamTotals& totals)
{
    try
    {
        if (const std::string problem = check(); !problem.empty())
        {
            std::printf("%s: %s %s\n", name.c_str(), item.c_str(), problem.c_str());
            totals.failed++;
        }
    }
    catch (const Refusal& error)
    {
        totals.refused[error.what()]++;
    }
    catch (const std::exception& error)
    {
        std::printf("%s: %s fails: %s\n", name.c_str(), item.c_str(), error.what());
        totals.failed++;
    }
    totals.streams++;
}

// Prints the edits that Hokuto refused in archive `name`, by their messages.
void PrintRefused(const std::string& name, const WiiStreamTotals& totals)
{
    for (const auto& [reason, count] : totals.refused)
    {
        std::printf("%s: refused %zu times: %s\n", name.c_str(), count, reason.c_str());
    }
}

// The first sound of `built` above volume 0 that is a stream sound, with `stream`, or of another kind, without.
std::optional<uint32_t> FirstAudible(const WiiArchive& built, bool stream)
{
    const auto audible_of_kind = [stream](const ntdlib::rsar::SoundInfo& sound)
    {
        return (sound.type == ntdlib::rsar::SoundType::kStream) == stream && sound.volume > 0;
    };
    const auto found = std::ranges::find_if(built->Sounds(), audible_of_kind);
    if (found == built->Sounds().end())
    {
        return std::nullopt;
    }

    return static_cast<uint32_t>(found - built->Sounds().begin());
}

// The streams `donor` of the donor as new stream sounds of `file`, with the model `before` where the tools laid it out
// and the build `build`. `file` builds `built`. Prints the summary of archive `name`, and returns the exit code.
int RunDonorStreams(const format::File& file, const WiiArchive& built,
                    const std::optional<ntdlib::rsar::ArchiveModel>& before, const ntdlib::SndBuild& build,
                    const std::map<uint32_t, std::vector<uint8_t>>& donor, const std::string& name,
                    WiiStreamTotals& totals)
{
    const std::optional<uint32_t> stream_model = FirstAudible(built, true);
    const std::optional<uint32_t> other_model = FirstAudible(built, false);
    bool turn = false;
    bool save = true;
    for (const auto& [id, stream] : donor)
    {
        if (!stream_model && !other_model)
        {
            break;
        }

        const uint32_t model =
            std::exchange(turn, !turn) ? other_model.value_or(*stream_model) : stream_model.value_or(*other_model);
        Attempt(
            name, "the donor's stream file " + std::to_string(id),
            [&, id, model]
            {
                return WiiDonorStreamProblem(file, built, before, build, model, id, stream, std::exchange(save, false),
                                             totals);
            },
            totals);
    }

    std::printf(
        "%s: %zu streams of the donor, with %zu channels, went in as new stream sounds. %zu fail. %zu took a "
        "track for each pair of their channels. %zu of the %zu new sounds' renders hold sound. The "
        "slowest edit took %.2f s.\n",
        name.c_str(), totals.streams, totals.channels, totals.failed, totals.paired, totals.sounding, totals.new_sounds,
        totals.slowest);
    PrintRefused(name, totals);

    return totals.failed > 0 ? 1 : 0;
}

// Counts the entries of the stream sounds of `built`, the archive that `file` builds, whose streams the file contains
// and ntdlib reads, and those that name the whole stream.
void CountStreamEntries(const format::File& file, const WiiArchive& built, WiiStreamTotals& totals)
{
    for (const ntdlib::rsar::SoundInfo& sound : built->Sounds())
    {
        const auto stream = file.native->streams.find(sound.file_id);
        if (sound.type != ntdlib::rsar::SoundType::kStream || stream == file.native->streams.end())
        {
            continue;
        }
        try
        {
            const ntdlib::rsar::StreamFile read = ntdlib::rsar::StreamFile::Read(*stream->second.bytes);
            totals.entries++;
            totals.whole_entries += NamesWholeStream(sound, built->Version(), read) ? 1 : 0;
        }
        catch (const std::exception&)
        {
            continue;
        }
    }
}

// Checks each stream of `file` with the first stream sound of `built` that plays it (WiiStreamProblem), and prints a
// problem of archive `name`. `built_bytes` are the bytes of `built`, `before` its model where the tools laid it out and
// `build` its build of NW4R SND. Counts the streams in `totals`.
void CheckEachStream(const format::File& file, const WiiArchive& built, std::span<const uint8_t> built_bytes,
                     const std::optional<ntdlib::rsar::ArchiveModel>& before, const ntdlib::SndBuild& build,
                     const std::string& name, WiiStreamTotals& totals)
{
    bool second = true;
    for (const auto& [id, stream] : file.native->streams)
    {
        const auto plays = [id](const ntdlib::rsar::SoundInfo& sound)
        {
            return sound.type == ntdlib::rsar::SoundType::kStream && sound.file_id == id;
        };
        const auto model = std::ranges::find_if(built->Sounds(), plays);
        if (model == built->Sounds().end())
        {
            totals.unplayed++;
            continue;
        }
        const auto sound = static_cast<uint32_t>(model - built->Sounds().begin());
        Attempt(
            name, model->name,
            [&, id, sound]
            {
                return WiiStreamProblem(file, built, built_bytes, before, build, id, sound,
                                        std::exchange(second, false), totals);
            },
            totals);
    }
}

} // namespace

int CheckWiiStreams(const std::vector<std::string>& arguments, const std::filesystem::path& path,
                    const std::vector<uint8_t>& bytes)
{
    // The file with the streams beside the archive, the archive that it builds, and the archive's model.
    const std::string name = xsflib::Utf8(path.filename());
    std::optional<format::File> imported;
    std::vector<uint8_t> built_bytes;
    WiiArchive built;
    ntdlib::SndBuild build;
    std::map<uint32_t, std::vector<uint8_t>> donor;
    try
    {
        const std::optional<ntdlib::SndBuild> game = hokuto::tools::GameBuildOf(path);
        const WiiArchive opened = song::LoadWiiArchive(bytes, name, ntdlib::rvl::snd::kRsarOptions);
        imported = format::ImportWiiArchive(bytes, game, song::ReadStreams(*opened, path));
        imported = hokuto::tools::CheckedForm(std::move(*imported), bytes);
        build = format::BuildOfWiiArchive(*imported->native);
        built_bytes = format::ExportArchive(*imported);
        built = song::LoadWiiArchive(built_bytes, name, ntdlib::rvl::snd::kRsarOptions);
        if (arguments.size() == 4)
        {
            donor = DonorStreamsOf(xsflib::PathFromUtf8(arguments[3]));
        }
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
        return 2;
    }
    const format::File& file = *imported;
    std::optional<ntdlib::rsar::ArchiveModel> before;
    try
    {
        before = ntdlib::rsar::ReadArchiveModel(built_bytes);
    }
    catch (const std::exception&)
    {
        before.reset();
    }

    WiiStreamTotals totals;

    // With a donor, the donor's streams as new stream sounds. The model alternates between the archive's first stream
    // sound and its first sound of another kind. A model at volume 0 would leave the new sounds silent.
    if (arguments.size() == 4)
    {
        return RunDonorStreams(file, built, before, build, donor, name, totals);
    }

    // The entries of the stream sounds whose streams the file holds and ntdlib reads.
    CountStreamEntries(file, built, totals);

    // Each stream with the first stream sound that plays it.
    CheckEachStream(file, built, built_bytes, before, build, name, totals);

    const std::vector<double>& ratios = totals.ratios;
    const double least = ratios.empty() ? 0.0 : *std::ranges::min_element(ratios);
    std::printf(
        "%s: %zu streams beside the archive, with %zu channels, took their bytes again, came back from the 3DS, the "
        "Wii U and the Switch and from their WAV files, and went in as new stream sounds. %zu fail. %zu of the %zu "
        "conversions came back with the stream's bytes. %zu streams came back from their WAV files in 16-bit PCM "
        "with their bytes, and %zu loops moved forward to a block there. As DSP-ADPCM, %zu streams came back exactly, "
        "and %zu with a signal-to-noise ratio of at least %.1f dB. %zu of the %zu stream sounds whose streams the "
        "archive holds name every channel and track of the stream. %zu new sounds played as their models play the "
        "stream, and %zu of the %zu new sounds' renders hold sound. %zu second new sounds went in after a first. %zu "
        "streams beside the archive play in no sound. The slowest edit took %.2f s.\n",
        name.c_str(), totals.streams, totals.channels, totals.failed, totals.exact_conversions, totals.conversions,
        totals.same_wav, totals.moved, totals.exact, ratios.size(), least, totals.whole_entries, totals.entries,
        totals.same_as_model, totals.sounding, totals.new_sounds, totals.pairs, totals.unplayed, totals.slowest);
    PrintRefused(name, totals);

    return totals.failed > 0 ? 1 : 0;
}

} // namespace hokuto::tools::replace
