// SPDX-License-Identifier: MIT

// The mode --streams of hokuto_check_replace: the streams of the Switch, the Wii U and the 3DS.

#include "ntdlib/sar/archive_model.h"

#include "check_replace.h"

namespace hokuto::tools::replace
{

bool PlaysSame(const sar::Pcm& a, const sar::Pcm& b)
{
    if (a.channels.size() != b.channels.size() || a.sample_rate != b.sample_rate || a.loop != b.loop ||
        b.loop_start < a.loop_start)
    {
        return false;
    }

    const uint32_t moved = a.loop ? b.loop_start - a.loop_start : 0;
    if (b.Length() != a.Length() + moved)
    {
        return false;
    }
    for (std::size_t c = 0; c < a.channels.size(); c++)
    {
        const std::vector<int16_t>& old = a.channels[c];
        const std::vector<int16_t>& now = b.channels[c];
        if (old.size() < a.Length() || now.size() < b.Length())
        {
            return false;
        }
        for (uint32_t i = 0; i < b.Length(); i++)
        {
            if (now[i] != (i < a.Length() ? old[i] : old[a.loop_start + (i - a.Length())]))
            {
                return false;
            }
        }
    }

    return true;
}

namespace
{

// Whether two sounds hold the same settings that a new sound copies from a model of any kind.
bool SameSettings(const sar::SoundInfo& a, const sar::SoundInfo& b)
{
    return a.volume == b.volume && a.pan_mode == b.pan_mode && a.pan_curve == b.pan_curve &&
           a.player_id == b.player_id && a.player_priority == b.player_priority &&
           a.actor_player_id == b.actor_player_id;
}

// What is wrong with `added`, the version of `base` whose archive gains stream sound `name`, a copy of sound `model`
// that plays `made` from a new file, or nothing. `built` is the archive that `base` builds.
std::string NewStreamProblem(const format::File& base, const sar::SoundArchive& built, const format::File& added,
                             uint32_t model, const std::string& name, const edit::NewStream& made)
{
    const std::vector<uint8_t> bytes = format::ExportArchive(added);
    const Archive after = song::LoadArchive(bytes, "after.bfsar", format::ExportGroupFiles(added));
    const auto sound = static_cast<uint32_t>(built.Sounds().size());
    const auto file_id = static_cast<uint32_t>(built.FileCount());
    if (after->Sounds().size() != sound + 1 || after->FileCount() != file_id + 1)
    {
        return "doesn't give the archive one sound and one file more";
    }

    // The new sound, its file and its stream. An export writes the stream at the new path.
    const sar::SoundInfo& info = after->Sounds()[sound];
    const sar::SoundInfo& copied = built.Sounds()[model];
    if (info.name != name || info.file_id != file_id || info.type != sar::SoundType::kStream ||
        !SameSettings(info, copied) || info.stream != made.details ||
        (copied.type == sar::SoundType::kStream && !SameValues(info, copied)))
    {
        return "gives the new sound other values than the model's and the stream's";
    }
    const std::vector<uint32_t> files = {file_id};
    if (after->ExternalPath(file_id) != made.path || song::StreamPaths(*after, files).front() != made.path)
    {
        return "gives the new file another path";
    }
    const auto found = added.native->streams.find(file_id);
    if (found == added.native->streams.end() || *found->second.bytes != made.bytes || found->second.from_game ||
        added.native->streams.size() != base.native->streams.size() + 1)
    {
        return "doesn't keep the new stream as Hokuto's";
    }
    for (const auto& [id, stream] : base.native->streams)
    {
        const auto kept = added.native->streams.find(id);
        if (kept == added.native->streams.end() || kept->second != stream)
        {
            return "changes stream file " + std::to_string(id);
        }
    }

    // The other items and files.
    for (uint32_t i = 0; i < sound; i++)
    {
        const sar::SoundInfo& old = built.Sounds()[i];
        const sar::SoundInfo& now = after->Sounds()[i];
        if (now.name != old.name || now.file_id != old.file_id || now.start_offset != old.start_offset ||
            now.allocate_track_flags != old.allocate_track_flags || !SameValues(now, old) || now.stream != old.stream)
        {
            return "changes " + old.name;
        }
    }
    for (uint32_t id = 0; id < built.FileCount(); id++)
    {
        if (!std::ranges::equal(built.FileData(id), after->FileData(id)) ||
            built.ExternalPath(id) != after->ExternalPath(id))
        {
            return "changes file " + std::to_string(id);
        }
    }
    if (NamesOf(after->Banks()) != NamesOf(built.Banks()) ||
        NamesOf(after->WaveArchives()) != NamesOf(built.WaveArchives()) ||
        NamesOf(after->Players()) != NamesOf(built.Players()) || NamesOf(after->Groups()) != NamesOf(built.Groups()))
    {
        return "changes the names of other items";
    }

    return {};
}

// What is wrong when stream file `bytes` of version `present`, a file of `flavor`, is imported, written again in the
// oldest version that the edit writes, or nothing. `version` is the version that the archive's streams take, and
// `decoded` are the samples of the stream. Counts the stream in `totals`.
std::string RewrittenProblem(std::span<const uint8_t> bytes, sar::Flavor flavor, uint32_t present, uint32_t version,
                             const sar::Pcm& decoded, Totals& totals)
{
    const bool ctr = flavor == sar::Flavor::kCtr;

    // On the Wii U, a stream of a later version than 3.0.0 goes in written again in 3.0.0, and on the 3DS one of a
    // later version than 2.0.0.0 in 2.0.0.0. The new stream then goes in as it is.
    const uint32_t oldest = flavor == sar::Flavor::kCafe ? 0x00030000 : ctr ? 0x02000000 : 0;
    if (oldest != 0 && present > oldest)
    {
        const std::string text = song::VersionText(flavor, oldest);
        const edit::IncomingStream old = edit::ReadIncomingStreamFile(bytes, flavor, oldest);
        if (!old.converted || old.converted->flavor != flavor || old.converted->version != present ||
            old.version != oldest || !PlaysSame(decoded, song::DecodeStreamFile(old.bytes)))
        {
            return "plays other samples written again in version " + text;
        }
        const edit::IncomingStream again = edit::ReadIncomingStreamFile(old.bytes, flavor, version);
        if (again.converted || again.bytes != old.bytes)
        {
            return "doesn't go in as it is in version " + text;
        }
        totals.st_written++;
    }

    return {};
}

// What is wrong when the WAV file of stream file `id` of `file`, whose bytes are `bytes`, is imported, or nothing.
// Stream sound `model` plays the stream, `built` is the archive that `file` builds and `archive` its bytes. `version`
// is the version that the archive's streams take, and `decoded` are the samples of the stream. Sets `pcm` to the stream
// that the WAV file contains, in 16-bit PCM, and counts the stream in `totals`.
std::string WavProblem(const format::File& file, const sar::SoundArchive& built, uint32_t id, uint32_t model,
                       const std::vector<uint8_t>& bytes, const std::vector<uint8_t>& archive, uint32_t version,
                       const sar::Pcm& decoded, edit::IncomingStream& pcm, Totals& totals)
{
    const sar::Flavor flavor = built.ReadOptions().flavor;

    // The WAV file in 16-bit PCM: the same samples, and the archive as it was. Where the sound plays the tracks of its
    // stream file, the new stream takes the present stream's.
    const edit::StreamChanges changes = edit::ChangesOfStream(file, built, model);
    const std::vector<uint8_t> wav = song::StreamAsWav(bytes);
    pcm = edit::WithSoundTracks(edit::ReadIncomingStreamWav(wav, flavor, version, sar::WaveEncoding::kPcm16), changes);
    if (!PlaysSame(decoded, song::DecodeStreamFile(pcm.bytes)))
    {
        return "plays other samples from its WAV file in 16-bit PCM";
    }
    if (changes.tracks_in_file && sar::StreamFile::Read(pcm.bytes).tracks != sar::StreamFile::Read(bytes).tracks)
    {
        return "doesn't keep the present stream's tracks from its WAV file";
    }
    totals.st_tracked += changes.tracks_in_file ? 1 : 0;
    totals.st_moved += pcm.loop_moved > 0 ? 1 : 0;
    const format::File replaced = edit::ReplaceStream(file, id, pcm.bytes);
    hokuto::tools::CheckSaved(replaced, true);
    if (format::ExportArchive(replaced) != archive || replaced.native->streams.at(id).from_game ||
        *replaced.native->streams.at(id).bytes != pcm.bytes)
    {
        return "doesn't keep the stream from its WAV file as Hokuto's";
    }

    // The WAV file in DSP-ADPCM.
    const edit::IncomingStream adpcm = edit::ReadIncomingStreamWav(wav, flavor, version);
    if (adpcm.signal_to_noise)
    {
        totals.st_ratios.push_back(*adpcm.signal_to_noise);
    }
    else
    {
        totals.st_exact++;
    }

    return {};
}

// What is wrong when a new sound that copies stream sound `model` is added with the stream file `bytes`, or nothing.
// `built` is the archive that `file` builds, `archive` its bytes and `version` the version that its streams take. With
// `second`, a second new sound is added with the stream `pcm`, and the file saves and opens again. Counts the sounds in
// `totals`.
std::string NewSoundsProblem(const format::File& file, const sar::SoundArchive& built, uint32_t model,
                             const std::vector<uint8_t>& bytes, const std::vector<uint8_t>& archive, uint32_t version,
                             const edit::IncomingStream& pcm, bool second, Totals& totals)
{
    const sar::Flavor flavor = built.ReadOptions().flavor;

    // A new sound that copies the sound, with the stream from its file. The edit refuses a new sound in an archive
    // without names, and the tool counts the stream apart.
    const std::string name = FreeTableName(*file.native->tables, "HOKUTO_NEW_STREAM");
    const edit::NewStream made =
        edit::MakeNewStream(built, model, name, edit::ReadIncomingStreamFile(bytes, flavor, version));
    if (!sar::TablesOf(archive, flavor).strings)
    {
        try
        {
            edit::AddStreamSound(file, model, name, made);
        }
        catch (const std::invalid_argument& error)
        {
            const std::string text = error.what();
            if (text == "The archive has no item names. Can't name a new sound in it.")
            {
                totals.st_nameless++;
                return {};
            }
            return "is refused: " + text;
        }
        return "takes a new sound in an archive without names";
    }
    const auto start = std::chrono::steady_clock::now();
    const format::File added = edit::AddStreamSound(file, model, name, made);
    totals.st_slowest =
        std::max(totals.st_slowest, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    if (std::string problem = NewStreamProblem(file, built, added, model, name, made); !problem.empty())
    {
        return problem;
    }
    totals.st_same_details += made.details == built.Sounds()[model].stream ? 1 : 0;
    if (!second)
    {
        return {};
    }

    // A second new sound from the WAV file, with a sound of another kind as its model where the archive holds one. The
    // file saves and opens again.
    const std::vector<uint8_t> added_bytes = format::ExportArchive(added);
    const Archive after = song::LoadArchive(added_bytes, "after.bfsar", format::ExportGroupFiles(added));
    const auto other = std::ranges::find_if(
        after->Sounds(), [](const sar::SoundInfo& sound) { return sound.type != sar::SoundType::kStream; });
    const auto second_model =
        other != after->Sounds().end() ? static_cast<uint32_t>(other - after->Sounds().begin()) : model;
    const std::string second_name = FreeTableName(*added.native->tables, "HOKUTO_NEW_STREAM");
    const edit::NewStream second_made = edit::MakeNewStream(*after, second_model, second_name, pcm);
    const format::File twice = edit::AddStreamSound(added, second_model, second_name, second_made);
    if (std::string problem = NewStreamProblem(added, *after, twice, second_model, second_name, second_made);
        !problem.empty())
    {
        return "after a first new sound, " + problem;
    }
    if (!hokuto::tools::SavesAndOpens(twice, true))
    {
        return "doesn't save and open again with two new sounds";
    }
    totals.st_pairs++;

    return {};
}

// What is wrong with stream file `id` of `file`, or nothing. Stream sound `model` plays the stream. `built` is the
// archive that `file` builds. With `second`, a second new sound goes in after the first, and the file saves and opens
// again.
std::string StreamProblem(const format::File& file, const sar::SoundArchive& built, uint32_t id, uint32_t model,
                          bool second, Totals& totals)
{
    const sar::Flavor flavor = built.ReadOptions().flavor;
    const std::vector<uint8_t>& bytes = *file.native->streams.at(id).bytes;
    const std::vector<uint8_t> archive = format::ExportArchive(file);

    // The stream itself, and its file for the other console. A stream of the version that the archive's streams take
    // comes back with the same bytes. A 3DS stream comes back with its samples: the Wii U's file drops the samples past
    // its end, and its original loop.
    if (edit::ReplaceStream(file, id, bytes) != file)
    {
        return "doesn't give the file back with its stream";
    }
    const bool ctr = flavor == sar::Flavor::kCtr;
    const uint32_t version = edit::IncomingStreamVersion(file, flavor);
    const uint32_t present = sar::StreamFile::Read(bytes).version;
    const sar::Pcm decoded = song::DecodeStreamFile(bytes);
    const sar::Flavor other_console = song::OtherConsole(flavor);
    const std::vector<uint8_t> other_file = song::ConvertStreamFile(
        bytes, other_console, song::ExportVersions(song::ItemKind::kStream, other_console).file);
    const edit::IncomingStream converted = edit::ReadIncomingStreamFile(other_file, flavor, version);
    const bool exact = present == version && !ctr;
    if (!converted.converted || converted.version != version ||
        (exact ? converted.bytes != bytes : !PlaysSame(decoded, song::DecodeStreamFile(converted.bytes))))
    {
        return "comes back from the other console with other bytes";
    }

    if (std::string problem = RewrittenProblem(bytes, flavor, present, version, decoded, totals); !problem.empty())
    {
        return problem;
    }

    edit::IncomingStream pcm;
    if (std::string problem = WavProblem(file, built, id, model, bytes, archive, version, decoded, pcm, totals);
        !problem.empty())
    {
        return problem;
    }

    return NewSoundsProblem(file, built, model, bytes, archive, version, pcm, second, totals);
}

// The ratio of signal to noise of the samples of `got` from `offset` on against `expected`'s, in decibels, over every
// channel up to `expected`'s end. Nothing where `got` holds other channels or too few samples, or `expected` none.
std::optional<double> SignalToNoise(const sar::Pcm& got, std::size_t offset, const sar::Pcm& expected)
{
    if (got.channels.size() != expected.channels.size() || expected.channels.empty())
    {
        return std::nullopt;
    }

    double signal = 0.0;
    double noise = 0.0;
    for (std::size_t channel = 0; channel < expected.channels.size(); channel++)
    {
        const std::vector<int16_t>& want = expected.channels[channel];
        const std::vector<int16_t>& have = got.channels[channel];
        if (have.size() < offset + want.size())
        {
            return std::nullopt;
        }
        for (std::size_t i = 0; i < want.size(); i++)
        {
            const double difference = static_cast<double>(have[offset + i]) - want[i];
            signal += static_cast<double>(want[i]) * want[i];
            noise += difference * difference;
        }
    }
    if (signal <= 0.0)
    {
        return std::nullopt;
    }

    return noise > 0.0 ? 10.0 * std::log10(signal / noise) : 1e9;
}

// The lowest ratio of signal to noise that an AAC stream encoded again from its WAV file or its file for the Wii U may
// take, in decibels, and how far above the ratios a sample before and after the encoder's delay it has to lie. A stream
// a sample off can still lie above the floor: the ratio has to peak at the delay.
constexpr double kLeastAacRatio = 15.0;
constexpr double kAacAlignment = 3.0;

// What is wrong with `replaced`, `model`'s file after Replace… of AAC stream `id`, or nothing. Its model has to be
// `model` with `bytes` in the stream's place, and `loop` in the entries of the sounds that play the stream.
std::string AacModelProblem(const format::Model& model, const format::File& replaced, uint32_t id,
                            const std::vector<uint8_t>& bytes, const std::optional<song::AacLoop>& loop)
{
    hokuto::tools::CheckSaved(replaced);
    format::Model after = format::ModelOf(replaced);
    const auto stream = after.streams.find(id);
    if (stream == after.streams.end() || stream->second != bytes)
    {
        return "doesn't hold the new stream";
    }
    if (const auto game = model.streams.find(id); game != model.streams.end())
    {
        stream->second = game->second;
    }
    else
    {
        after.streams.erase(stream);
    }
    if (after.archive.sounds.size() != model.archive.sounds.size())
    {
        return "changes the sound table";
    }
    for (std::size_t i = 0; i < after.archive.sounds.size(); i++)
    {
        sar::SoundModel& entry = after.archive.sounds[i];
        const sar::SoundModel& game = model.archive.sounds[i];
        if (entry.type != sar::SoundType::kStream || entry.file_id != id || !entry.stream_extension)
        {
            continue;
        }
        const sar::StreamExtension& extension = *entry.stream_extension;
        if (((extension.stream_type & 0x100) != 0) != loop.has_value() ||
            (loop && (extension.loop_start != loop->start || extension.loop_end != loop->end)))
        {
            return "gives " + entry.name + " another loop";
        }
        entry.stream_extension = game.stream_extension;
        entry.stream_options = game.stream_options;
    }

    return after == model ? std::string() : "changes other values of the archive";
}

// What is wrong with an encoded AAC stream `encoded` from samples `source`, or nothing: its samples have to come after
// the encoder's delay, and `loop` has to be the loop of `source` after the delay. Counts its ratio of signal to noise.
std::string EncodedAacProblem(const edit::IncomingAac& encoded, const sar::Pcm& source,
                              std::optional<song::AacLoop>& loop, Totals& totals)
{
    loop = source.loop ? std::optional(song::AacLoop{encoded.delay + source.loop_start,
                                                     encoded.delay + static_cast<uint32_t>(source.loop_end)})
                       : std::nullopt;
    if (!encoded.encoded || encoded.after_loop != 0 || source.channels.at(0).size() != source.loop_end ||
        encoded.length != encoded.delay + source.loop_end || encoded.loop.has_value() != loop.has_value() ||
        (loop && (encoded.loop->start != loop->start || encoded.loop->end != loop->end)))
    {
        return "gives another length or loop";
    }
    const sar::Pcm decoded = song::DecodeAdts(encoded.bytes);
    const std::optional<double> ratio = SignalToNoise(decoded, encoded.delay, source);
    const std::optional<double> before = SignalToNoise(decoded, encoded.delay - 1, source);
    const std::optional<double> after = SignalToNoise(decoded, encoded.delay + 1, source);
    if (!ratio || *ratio < kLeastAacRatio || encoded.delay == 0)
    {
        return "decodes to other samples";
    }
    if (*ratio < before.value_or(0.0) + kAacAlignment || *ratio < after.value_or(0.0) + kAacAlignment)
    {
        return "decodes to its samples at another delay";
    }
    totals.st_aac_ratios.push_back(*ratio);
    totals.st_aac_delay = encoded.delay;

    return {};
}

// What is wrong with Replace… of AAC stream `id` of `file`, or nothing (edit::ReplaceAacStream). Stream sound `sound`
// plays the stream. `built` is the archive that `file` builds. The stream's file goes in as it is, with the present
// loop, and the file in the model form has to hold the game's model. The stream's WAV file and its file for the Wii U
// from Export… go in encoded by the system's encoder. The new stream has to decode to the samples that went in, after
// the encoder's delay, and the entries of the sounds that play it have to give the loop after the delay. Every other
// value of the model has to stay.
std::string AacStreamProblem(const format::File& file, const sar::SoundArchive& built, uint32_t id, uint32_t sound,
                             Totals& totals)
{
    const std::optional<edit::AacChanges> changes = edit::ChangesOfAacStream(file, built, sound);
    const std::optional<song::AacStream> aac = song::AacStreamOf(built, sound);
    if (!changes || !aac || changes->file != id || !changes->present)
    {
        return "plays no AAC stream that the file holds";
    }
    const std::vector<uint8_t>& present = *file.native->streams.at(id).bytes;
    const format::Model model = format::ModelOf(file);
    const auto problem_of = [&model, id](const format::File& replaced, const std::vector<uint8_t>& bytes,
                                         const std::optional<song::AacLoop>& loop)
    {
        return AacModelProblem(model, replaced, id, bytes, loop);
    };

    // The stream's file, with the present loop.
    const edit::IncomingAac itself = edit::ReadIncomingAac(present, *changes);
    if (itself.encoded || itself.bytes != present || itself.present_loop != changes->loop.has_value())
    {
        return "doesn't take its file as it is";
    }
    if (std::string problem = problem_of(edit::ReplaceAacStream(file, *changes, itself), present, changes->loop);
        !problem.empty())
    {
        return "with its file, " + problem;
    }

    // The stream's WAV file and its file for the Wii U, encoded again. Each holds the stream's samples up to the end of
    // its loop, with the loop. The Wii U's file moves the loop's start forward to a block, and the samples between
    // repeat after the loop's end.
    const std::vector<uint8_t> wav = song::StreamAsWav(present, *aac);
    const std::vector<uint8_t> wii_u = song::ConvertStreamFile(
        present, sar::Flavor::kCafe, song::ExportVersions(song::ItemKind::kStream, sar::Flavor::kCafe).file, *aac);
    for (const auto& [what, bytes] : {std::pair{"its WAV file", &wav}, std::pair{"its file for the Wii U", &wii_u}})
    {
        const std::string from = std::string("from ") + what;
        const sar::Pcm source = bytes == &wav ? song::DecodeStreamFile(present, *aac) : song::DecodeStreamFile(*bytes);
        const edit::IncomingAac encoded = edit::ReadIncomingAac(*bytes, *changes);
        std::optional<song::AacLoop> loop;
        if (std::string problem = EncodedAacProblem(encoded, source, loop, totals); !problem.empty())
        {
            return from + ", " + problem;
        }
        if (std::string problem = problem_of(edit::ReplaceAacStream(file, *changes, encoded), encoded.bytes, loop);
            !problem.empty())
        {
            return from + ", " + problem;
        }
    }

    totals.st_aac_replaced++;

    return {};
}

// What is wrong with Replace… of the AAC stream of stream sound `sound` of `file`, or nothing. The file lacks the
// stream. `built` is the archive that `file` builds. `wav` is a WAV file of 1 or 2 channels at 44,100 or 48,000 Hz. The
// new stream has to be AAC, at 128 kbit/s, with the WAV file's rate and the 1 or 2 channels that the sound's entry
// gives, or else the WAV file's. A mono file fills both channels of a stereo stream, and a stereo file goes into a mono
// stream as the mean of its channels. The new stream has to decode to those samples after the encoder's delay, and the
// entries of the sounds that play it have to give the loop after the delay. Every other value of the model has to stay.
std::string MissingAacProblem(const format::File& file, const sar::SoundArchive& built, uint32_t sound,
                              const std::vector<uint8_t>& wav, Totals& totals)
{
    const std::optional<edit::AacChanges> changes = edit::ChangesOfAacStream(file, built, sound);
    if (!changes || changes->present || changes->bit_rate != 128000 || file.native->streams.contains(changes->file))
    {
        return "isn't an AAC stream that the file lacks";
    }
    const seqlib::WavContents contents = seqlib::ReadWav(wav);
    sar::Pcm source;
    source.channels = contents.pcm.channels;
    source.sample_rate = contents.rate;
    source.loop = contents.pcm.loop;
    source.loop_start = contents.pcm.loop_start;
    source.loop_end = contents.pcm.loop ? contents.pcm.loop_end : static_cast<uint32_t>(source.channels.at(0).size());
    for (std::vector<int16_t>& channel : source.channels)
    {
        channel.resize(source.loop_end);
    }
    const uint16_t entry = built.Sounds().at(sound).stream.channel_count;
    const std::size_t channels = entry == 1 || entry == 2 ? entry : source.channels.size();
    if (source.channels.size() == 1 && channels == 2)
    {
        source.channels.push_back(source.channels[0]);
    }
    else if (source.channels.size() == 2 && channels == 1)
    {
        for (std::size_t i = 0; i < source.channels[0].size(); i++)
        {
            source.channels[0][i] = static_cast<int16_t>((source.channels[0][i] + source.channels[1][i]) / 2);
        }
        source.channels.resize(1);
    }
    const edit::IncomingAac encoded = edit::ReadIncomingAac(wav, *changes);
    if (encoded.stream_channels != channels || encoded.stream_rate != source.sample_rate)
    {
        return "takes other channels or another rate from a WAV file";
    }
    std::optional<song::AacLoop> loop;
    if (std::string problem = EncodedAacProblem(encoded, source, loop, totals); !problem.empty())
    {
        return "from a WAV file, " + problem;
    }
    if (std::string problem = AacModelProblem(format::ModelOf(file), edit::ReplaceAacStream(file, *changes, encoded),
                                              changes->file, encoded.bytes, loop);
        !problem.empty())
    {
        return "from a WAV file, " + problem;
    }
    totals.st_aac_missing++;

    return {};
}

} // namespace

void CheckStreams(const format::File& file, const sar::SoundArchive& built, const std::string& name, Totals& totals)
{
    bool second = true;
    for (const auto& [id, stream] : file.native->streams)
    {
        const auto plays = [id](const sar::SoundInfo& sound)
        {
            return sound.type == sar::SoundType::kStream && sound.file_id == id;
        };
        const auto model = std::ranges::find_if(built.Sounds(), plays);
        if (model == built.Sounds().end())
        {
            totals.st_unplayed++;
            continue;
        }

        std::string problem;
        try
        {
            const auto sound = static_cast<uint32_t>(model - built.Sounds().begin());
            if (song::IsAdts(*stream.bytes))
            {
                problem = AacStreamProblem(file, built, id, sound, totals);
                totals.st_channels += song::DecodeAdts(*stream.bytes).channels.size();
            }
            else
            {
                problem = StreamProblem(file, built, id, sound, std::exchange(second, false), totals);
                totals.st_channels += sar::StreamFile::Read(*stream.bytes).channels.size();
            }
        }
        catch (const std::exception& error)
        {
            problem = std::string("fails: ") + error.what();
        }
        if (!problem.empty())
        {
            std::printf("%s: %s %s\n", name.c_str(), model->name.c_str(), problem.c_str());
            totals.st_failed++;
        }
        totals.st_streams++;
    }

    // The first stream sound whose entry marks an AAC stream that the file lacks takes the WAV file of the first AAC
    // stream that the file holds (MissingAacProblem).
    std::optional<std::vector<uint8_t>> wav;
    for (uint32_t sound = 0; sound < built.Sounds().size() && !wav; sound++)
    {
        const std::optional<uint32_t> id = song::StreamFileOf(built, sound);
        const std::optional<song::AacStream> aac = song::AacStreamOf(built, sound);
        const auto held = id ? file.native->streams.find(*id) : file.native->streams.end();
        if (aac && held != file.native->streams.end() && song::IsAdts(*held->second.bytes))
        {
            try
            {
                wav = song::StreamAsWav(*held->second.bytes, *aac);
            }
            catch (const std::exception& error)
            {
                std::printf("%s: the stream of %s doesn't go out as a WAV file: %s\n", name.c_str(),
                            built.Sounds()[sound].name.c_str(), error.what());
                totals.st_failed++;
                break;
            }
        }
    }
    for (uint32_t sound = 0; sound < built.Sounds().size() && wav; sound++)
    {
        const std::optional<uint32_t> id = song::StreamFileOf(built, sound);
        if (!id || file.native->streams.contains(*id) || !song::AacStreamOf(built, sound))
        {
            continue;
        }
        std::string problem;
        try
        {
            problem = MissingAacProblem(file, built, sound, *wav, totals);
        }
        catch (const std::exception& error)
        {
            problem = std::string("fails: ") + error.what();
        }
        if (!problem.empty())
        {
            std::printf("%s: %s without its stream %s\n", name.c_str(), built.Sounds()[sound].name.c_str(),
                        problem.c_str());
            totals.st_failed++;
        }
        break;
    }
}

namespace
{

// What is wrong with `bytes`, a stream file of a donor, as a new stream sound of the archive of `file` with sound
// `model` as its model, or nothing. `built` is the archive that `file` builds, and `archive` the archive that it
// exports. With `save`, the file with the new sound saves and opens again.
std::string DonorStreamProblem(const format::File& file, const sar::SoundArchive& built,
                               std::span<const uint8_t> archive, uint32_t model, const std::vector<uint8_t>& bytes,
                               bool save, Totals& totals)
{
    const sar::Flavor flavor = built.ReadOptions().flavor;
    const std::string name = FreeTableName(*file.native->tables, "HOKUTO_NEW_STREAM");
    const edit::IncomingStream incoming =
        edit::ReadIncomingStreamFile(bytes, flavor, edit::IncomingStreamVersion(file, flavor));

    // A 3DS archive whose layout of stream sounds is unknown, and an archive without names, refuse the new sound.
    const uint32_t version = built.Version();
    const bool unknown = flavor == sar::Flavor::kCtr && !sar::LaysOutStreamSounds(version);
    const bool nameless = !sar::TablesOf(archive, flavor).strings;
    if (unknown || nameless)
    {
        try
        {
            edit::AddStreamSound(file, model, name, edit::MakeNewStream(built, model, name, incoming));
        }
        catch (const std::invalid_argument& error)
        {
            const std::string text = error.what();
            if (unknown && text.starts_with("Hokuto can't add a stream sound to a 3DS archive of version"))
            {
                totals.st_unknown++;
                return {};
            }
            if (!unknown && text == "The archive has no item names. Can't name a new sound in it.")
            {
                totals.st_nameless++;
                return {};
            }
            return "is refused: " + text;
        }
        return unknown ? "takes a new sound in a 3DS archive whose layout of stream sounds is unknown"
                       : "takes a new sound in an archive without names";
    }

    // The new stream plays the donor's samples. Where the sound plays the tracks of its stream file, a stream without
    // tracks takes a track for each pair of its channels.
    const edit::NewStream made = edit::MakeNewStream(built, model, name, incoming);
    if (!PlaysSame(song::DecodeStreamFile(bytes), song::DecodeStreamFile(made.bytes)))
    {
        return "plays other samples than the donor's stream";
    }
    if (made.tracks_in_file != edit::TracksInFiles(built))
    {
        return "gives the stream its tracks otherwise than the archive's sounds play them";
    }
    if (made.tracks_in_file && !made.file_tracks)
    {
        const sar::StreamFile stream = sar::StreamFile::Read(made.bytes);
        if (!stream.tracks || stream.tracks->size() != (stream.channels.size() + 1) / 2)
        {
            return "doesn't take a track for each pair of its channels";
        }
        totals.st_paired++;
    }
    const auto start = std::chrono::steady_clock::now();
    const format::File added = edit::AddStreamSound(file, model, name, made);
    totals.st_slowest =
        std::max(totals.st_slowest, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    if (std::string problem = NewStreamProblem(file, built, added, model, name, made); !problem.empty())
    {
        return problem;
    }
    totals.st_same_details += made.details == built.Sounds()[model].stream ? 1 : 0;
    if (save && !hokuto::tools::SavesAndOpens(added, true))
    {
        return "doesn't save and open again with the new sound";
    }

    return {};
}

} // namespace

void CheckDonorStreams(const format::File& file, const sar::SoundArchive& built, const format::File& donor,
                       const std::string& name, Totals& totals)
{
    const auto first = [&built](bool stream) -> std::optional<uint32_t>
    {
        const auto of_kind = [stream](const sar::SoundInfo& sound)
        {
            return (sound.type == sar::SoundType::kStream) == stream;
        };
        const auto found = std::ranges::find_if(built.Sounds(), of_kind);
        if (found == built.Sounds().end())
        {
            return std::nullopt;
        }

        return static_cast<uint32_t>(found - built.Sounds().begin());
    };
    const std::optional<uint32_t> stream_model = first(true);
    const std::optional<uint32_t> other_model = first(false);
    if (!stream_model && !other_model)
    {
        return;
    }
    const std::vector<uint8_t> archive = format::ExportArchive(file);
    bool turn = false;
    bool save = true;
    for (const auto& [id, stream] : donor.native->streams)
    {
        if (song::IsAdts(*stream.bytes))
        {
            totals.st_aac++;
            continue;
        }

        const uint32_t model =
            std::exchange(turn, !turn) ? other_model.value_or(*stream_model) : stream_model.value_or(*other_model);
        std::string problem;
        try
        {
            problem =
                DonorStreamProblem(file, built, archive, model, *stream.bytes, std::exchange(save, false), totals);
            totals.st_channels += sar::StreamFile::Read(*stream.bytes).channels.size();
        }
        catch (const std::exception& error)
        {
            problem = std::string("fails: ") + error.what();
        }
        if (!problem.empty())
        {
            std::printf("%s: the donor's stream file %u %s\n", name.c_str(), id, problem.c_str());
            totals.st_failed++;
        }
        totals.st_streams++;
    }
}

} // namespace hokuto::tools::replace
