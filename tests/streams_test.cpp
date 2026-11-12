// SPDX-License-Identifier: MIT

// Tests the streams beside an archive (src/song/streams.h), the replacement of a stream sound's stream
// (src/edit/streams.h) and new stream sounds (src/edit/new_items.h). A small archive of the Switch, and the same
// archive of the Wii U, name three stream files outside the archive: one in the folder stream, one at a path with a
// backslash, and one that leads out of the archive's folder. Two stream sounds play the first, one with a stereo track
// and one with two mono tracks. A third stream sound plays the second file and a fourth the third. ntdlib's MakeStream
// makes the streams from a ramp and a sine. The import reads them from a temporary folder. The new stream sounds go
// into the same archive laid out as NintendoWare's tools lay it out, with a wave sound after the stream sounds. A mono
// WAV file in place of an AAC stream takes the channels of the sound's entry.

#include "streams_fixtures.h"

namespace
{

// The paths that the file table gives, the files that leave the archive's folder, and the names of the files.
void TestPaths()
{
    const auto archive =
        hokuto::song::LoadArchive(ArchiveOf(sar::Flavor::kNx), "test.bfsar", std::vector<std::vector<uint8_t>>{});
    XSFLIB_CHECK(hokuto::song::StreamPath(*archive, kSongFile) == std::filesystem::path("stream/STRM_SONG.bfstm"));
    XSFLIB_CHECK(hokuto::song::StreamPath(*archive, kBackslashFile) == std::filesystem::path("stream/STRM_BACK.bfstm"));
    XSFLIB_CHECK(!hokuto::song::StreamPath(*archive, kOutsideFile));
    XSFLIB_CHECK(hokuto::song::StreamFileOf(*archive, kMonoSound) == kSongFile);
    XSFLIB_CHECK(hokuto::song::StreamFileName(*archive, kStereoSound) == "STRM_SONG.bfstm");
    XSFLIB_CHECK(hokuto::song::StreamFileName(*archive, kOutsideSound) == "STRM_OUT.bfstm");
    XSFLIB_CHECK(hokuto::song::ItemFileName(*archive, {hokuto::song::ItemKind::kStream, kBackslashSound}) ==
                 "STRM_BACK.bfstm");
    XSFLIB_CHECK(hokuto::song::ItemFormat(hokuto::song::ItemKind::kStream, sar::Flavor::kCafe) == "BFSTM");

    // An export finds the paths again, and refuses a file without one.
    const std::vector<uint32_t> kFiles = {kSongFile, kBackslashFile};
    XSFLIB_CHECK(hokuto::song::StreamPaths(*archive, kFiles) ==
                 std::vector<std::filesystem::path>({"stream/STRM_SONG.bfstm", "stream/STRM_BACK.bfstm"}));
    const std::vector<uint32_t> kOutside = {kOutsideFile};
    XSFLIB_CHECK(ErrorOf<std::runtime_error>([&] { hokuto::song::StreamPaths(*archive, kOutside); }) ==
                 "The file table has no path inside the archive folder for stream file 2.");
}

// The import reads the streams of the archive's console beside the archive. The file keeps them in both forms.
void TestImport(sar::Flavor flavor)
{
    const TemporaryFolder folder;
    const std::vector<uint8_t> bytes = ArchiveOf(flavor);
    const std::vector<uint8_t> stream = StreamOf(flavor);
    const sar::Flavor other = flavor == sar::Flavor::kNx ? sar::Flavor::kCafe : sar::Flavor::kNx;
    XSFLIB_CHECK(!hokuto::song::WriteStream(folder.Path() / "stream" / "STRM_SONG.bfstm", stream));
    XSFLIB_CHECK(!hokuto::song::WriteStream(folder.Path() / "stream" / "STRM_BACK.bfstm", StreamOf(other)));
    XSFLIB_CHECK(!hokuto::song::WriteStream(folder.Path() / "STRM_OUT.bfstm", stream));
    const std::filesystem::path path = folder.Path() / "test.bfsar";
    XSFLIB_CHECK(!xsflib::WriteFile(path, bytes));

    // The other console's stream and the one outside the folder stay out.
    const auto archive = hokuto::song::LoadArchive(bytes, "test.bfsar", path);
    const std::map<uint32_t, std::vector<uint8_t>> streams = hokuto::song::ReadStreams(*archive, path);
    XSFLIB_CHECK(streams.size() == 1 && streams.at(kSongFile) == stream);

    // A stream of the console with neither byte order mark stays out too.
    std::vector<uint8_t> unmarked = stream;
    unmarked[4] = 0;
    unmarked[5] = 0;
    XSFLIB_CHECK(!hokuto::song::WriteStream(folder.Path() / "stream" / "STRM_BACK.bfstm", unmarked));
    XSFLIB_CHECK(hokuto::song::ReadStreams(*archive, path).size() == 1);
    XSFLIB_CHECK(!hokuto::song::BigEndianMark(unmarked));
    XSFLIB_CHECK(hokuto::song::BigEndianMark(stream) == (flavor == sar::Flavor::kCafe));

    const File file = hokuto::format::ImportArchive(bytes, flavor, {}, streams);
    XSFLIB_CHECK(file.native->streams.size() == 1 && *file.native->streams.at(kSongFile).bytes == stream &&
                 file.native->streams.at(kSongFile).from_game);
    XSFLIB_CHECK(hokuto::format::Read(hokuto::format::Write(file)) == file);
    XSFLIB_CHECK(hokuto::format::ReadText(hokuto::format::WriteText(file)) == file);
    XSFLIB_CHECK(hokuto::format::ExportArchive(file) == bytes);
}

// A stream as a WAV file, and as a file of the other console.
void TestConversions(sar::Flavor flavor)
{
    const std::vector<uint8_t> stream = StreamOf(flavor, sar::WaveEncoding::kPcm16);
    const sar::Pcm decoded = hokuto::song::DecodeStreamFile(stream);
    const std::vector<std::vector<int16_t>> channels = Channels();
    SEQLIB_REQUIRE(decoded.channels.size() == 2 && decoded.channels[1].size() >= channels[1].size());
    XSFLIB_CHECK(decoded.sample_rate == kRate && decoded.loop);
    XSFLIB_CHECK(std::equal(channels[1].begin(), channels[1].end(), decoded.channels[1].begin()));

    // The WAV file holds every channel and the loop.
    const seqlib::WavContents wav = seqlib::ReadWav(hokuto::song::StreamAsWav(stream));
    XSFLIB_CHECK(wav.rate == kRate && wav.pcm.channels == decoded.channels && wav.pcm.loop &&
                 wav.pcm.loop_start == decoded.loop_start && wav.pcm.loop_end == decoded.loop_end);

    // The other console's file holds the same samples, and comes back with the same bytes.
    const sar::Flavor other = hokuto::song::OtherConsole(flavor);
    const std::vector<uint8_t> converted = hokuto::song::ConvertStreamFile(stream, other, kVersion);
    XSFLIB_CHECK(sar::StreamFile::Read(converted).flavor == other);
    XSFLIB_CHECK(hokuto::song::DecodeStreamFile(converted).channels == decoded.channels);
    XSFLIB_CHECK(hokuto::song::ConvertStreamFile(converted, flavor, kVersion) == stream);

    // In 3.0.0 too. A stream for the Wii U takes 3.0.0, and one for the Switch 4.0.0.
    const std::vector<uint8_t> old = hokuto::song::ConvertStreamFile(stream, other, kOldVersion);
    XSFLIB_CHECK(sar::StreamFile::Read(old).flavor == other && sar::StreamFile::Read(old).version == kOldVersion);
    XSFLIB_CHECK(hokuto::song::DecodeStreamFile(old).channels == decoded.channels);
    const hokuto::song::FileVersions versions = hokuto::song::ExportVersions(hokuto::song::ItemKind::kStream, other);
    XSFLIB_CHECK(versions.file == (other == sar::Flavor::kCafe ? kOldVersion : kVersion));
    XSFLIB_CHECK(hokuto::song::ConvertItemFile(stream, hokuto::song::ItemKind::kStream, flavor, other, versions) ==
                 hokuto::song::ConvertStreamFile(stream, other, versions.file));

    // The 3DS's file of 2.3.1.0 lays the stream out as 4.0.0 does, and comes back with the same bytes. A stream for the
    // 3DS takes 2.0.0.0, the oldest, with the same samples.
    const std::vector<uint8_t> ctr = hokuto::song::ConvertStreamFile(stream, sar::Flavor::kCtr, kCtrNewestVersion);
    XSFLIB_CHECK(sar::StreamFile::Read(ctr).flavor == sar::Flavor::kCtr);
    XSFLIB_CHECK(hokuto::song::DecodeStreamFile(ctr).channels == decoded.channels);
    XSFLIB_CHECK(hokuto::song::ConvertStreamFile(ctr, flavor, kVersion) == stream);
    const hokuto::song::FileVersions ctr_versions =
        hokuto::song::ExportVersions(hokuto::song::ItemKind::kStream, sar::Flavor::kCtr);
    XSFLIB_CHECK(ctr_versions.file == kCtrOldestVersion);
    const std::vector<uint8_t> oldest =
        hokuto::song::ConvertItemFile(stream, hokuto::song::ItemKind::kStream, flavor, sar::Flavor::kCtr, ctr_versions);
    XSFLIB_CHECK(sar::StreamFile::Read(oldest).version == kCtrOldestVersion);
    XSFLIB_CHECK(hokuto::song::DecodeStreamFile(oldest).channels == decoded.channels);
    XSFLIB_CHECK(ErrorOf<std::runtime_error>(
                     [&]
                     {
                         hokuto::song::ConvertItemFile(stream, hokuto::song::ItemKind::kStream, sar::Flavor::kCtr,
                                                       flavor, versions);
                     }) == "It isn't a 3DS BCSTM file.");
    XSFLIB_CHECK(ErrorOf<std::runtime_error>([&] { hokuto::song::ConvertStreamFile(stream, other, 0x00020000); })
                     .starts_with("Can't convert the stream. "));
}

// The output of `ArchiveModel`, the model of a console, for `frames` frames, with stream sound `sound` of `archive`
// started on it with `stream`, as floats.
template <class ArchiveModel>
std::vector<float> RenderModel(const sar::SoundArchive& archive, uint32_t sound, const std::vector<uint8_t>& stream,
                               std::size_t frames)
{
    ArchiveModel model(archive);
    model.engine->StartStreamSound(sound, stream);
    std::vector<float> samples;
    for (std::size_t frame = 0; frame < frames; frame++)
    {
        model.engine->RunFrame();
        std::span<const int16_t> output;
        if constexpr (std::is_same_v<ArchiveModel, ntdlib::cafe::snd::ArchiveModel>)
        {
            output = model.system.Output();
        }
        else
        {
            output = model.renderer.DeviceOutput();
        }
        for (const int16_t sample : output)
        {
            samples.push_back(static_cast<float>(sample) / 32768.0f);
        }
    }

    return samples;
}

// The browser plays a stream sound on the console's model as ntdlib's player of stream sounds plays it. The preview's
// output is the model's, sample for sample. The stereo track at the centre plays its first channel on the left and its
// second on the right. The two mono tracks at the left and the right play the same. A sound's info plays its stream as
// a stream sound, whatever kind the info gives. The stream plays at the sound's pitch of 1.5, faster than its rate. A
// stream of the other console, bytes that aren't a stream file, and a sound without a track that plays throw with a
// message.
void TestStreamPreview(sar::Flavor flavor)
{
    const std::vector<uint8_t> stream = StreamOf(flavor, sar::WaveEncoding::kPcm16);
    const auto archive =
        hokuto::song::LoadArchive(ArchiveOf(flavor), "test.bfsar", std::vector<std::vector<uint8_t>>{});
    hokuto::song::SoundPreview stereo(archive, kStereoSound, stream);
    hokuto::song::SoundPreview mono(archive, kMonoSound, stream);
    const bool wii_u = flavor == sar::Flavor::kCafe;
    const std::size_t frame = wii_u ? 144 : 240;
    constexpr std::size_t kFrames = 400;

    const std::vector<float> stereo_samples = RenderPreview(stereo, kFrames * frame);
    const std::vector<float> mono_samples = RenderPreview(mono, kFrames * frame);
    const std::vector<float> model =
        wii_u ? RenderModel<ntdlib::cafe::snd::ArchiveModel>(*archive, kStereoSound, stream, kFrames)
              : RenderModel<ntdlib::nx::atk::ArchiveModel>(*archive, kStereoSound, stream, kFrames);

    XSFLIB_CHECK(stereo.SampleRate() == 48000);
    XSFLIB_CHECK(stereo_samples.size() == kFrames * frame * 2 && stereo_samples == model);
    XSFLIB_CHECK(mono_samples == stereo_samples);
    XSFLIB_CHECK(std::any_of(stereo_samples.begin(), stereo_samples.end(), [](float s) { return s != 0.0f; }));

    // The left side follows channel 0, the rising ramp of Channels(). The right side follows channel 1, the sine. The
    // sound's pan curve is the square root. At the sides, that curve sets a channel to a gain of 1 on its side and 0 on
    // the other. Each step spans three samples. On the Wii U, AX renders at 32 kHz, and an FIR upsampler of three
    // phases makes 48 kHz from that output. Each phase makes one sample of every three. The gains of the phases differ
    // by up to 0.1 %. Between neighbouring samples, that difference outweighs the ramp's rise high on the ramp. Over
    // three samples, nearly every step on the left rises. Only the ramp's wrap every 4,000 samples of the stream falls.
    // On the right, the steps go up for half of each period of the sine and down for the other half.
    constexpr std::size_t kStep = 3;
    std::size_t steps = 0;
    std::size_t left_up = 0;
    std::size_t right_up = 0;
    for (std::size_t i = 50 * frame; i < 350 * frame && 2 * i + 1 < stereo_samples.size(); i++)
    {
        ++steps;
        left_up += stereo_samples[2 * i] >= stereo_samples[2 * (i - kStep)] ? 1 : 0;
        right_up += stereo_samples[2 * i + 1] >= stereo_samples[2 * (i - kStep) + 1] ? 1 : 0;
    }
    SEQLIB_REQUIRE(steps > 0);
    XSFLIB_CHECK(left_up * 10 > steps * 9);
    XSFLIB_CHECK(right_up * 10 < steps * 7);

    sar::SoundInfo other_kind = archive->Sounds()[kStereoSound];
    other_kind.type = sar::SoundType::kWaveSound;
    hokuto::song::SoundPreview from_info(archive, other_kind, stream);
    XSFLIB_CHECK(RenderPreview(from_info, kFrames * frame) == stereo_samples);

    // At a pitch of 1.5, the 40,000 samples at 32 kHz last 0.83 s, and the loop goes on from sample 1,000.
    sar::SoundInfo once = archive->Sounds()[kStereoSound];
    const std::vector<uint8_t> unlooped =
        sar::MakeStream(flavor, 0x00040000, sar::WaveEncoding::kPcm16, kRate, Channels(), std::nullopt).Write();
    hokuto::song::SoundPreview short_one(archive, once, unlooped);
    const std::size_t played = RenderPreview(short_one, 48000 * 2).size() / 2;
    XSFLIB_CHECK(played > 40000 * 48000 / (kRate * 3 / 2) && played < 40000 * 48000 / (kRate * 3 / 2) + 10 * frame);

    const sar::Flavor other = wii_u ? sar::Flavor::kNx : sar::Flavor::kCafe;
    XSFLIB_CHECK(ErrorOf<std::runtime_error>(
                     [&] { const hokuto::song::SoundPreview preview(archive, kStereoSound, StreamOf(other)); }) ==
                 "The stream of STRM_SONG is for another console.");
    XSFLIB_CHECK(ErrorOf<std::runtime_error>([&] { const hokuto::song::SoundPreview preview(archive, kStereoSound); })
                     .starts_with("Can't read the stream of STRM_SONG. "));
    sar::SoundInfo silent = once;
    silent.stream.valid_tracks = 0;
    XSFLIB_CHECK(
        ErrorOf<std::runtime_error>([&] { const hokuto::song::SoundPreview preview(archive, silent, stream); }) ==
        "STRM_SONG has no tracks enabled at the start.");
}

// Stream files and WAV files in the place of a stream sound's stream.
void TestReplacement(sar::Flavor flavor)
{
    const std::vector<uint8_t> bytes = ArchiveOf(flavor);
    const std::vector<uint8_t> stream = StreamOf(flavor);
    const File file = hokuto::format::ImportArchive(bytes, flavor, {}, {{kSongFile, stream}});
    const auto archive = hokuto::song::LoadArchive(bytes, "test.bfsar", std::vector<std::vector<uint8_t>>{});

    // The sound's stream, the other sound that plays it, and the tracks.
    const StreamChanges changes = hokuto::edit::ChangesOfStream(file, *archive, kStereoSound);
    XSFLIB_CHECK(changes.file == kSongFile && changes.present && changes.from_game && changes.channels == 2 &&
                 changes.rate == kRate && changes.loop && changes.length > kLength && changes.size == stream.size());
    XSFLIB_CHECK(changes.sounds == std::vector<uint32_t>({kMonoSound}) && changes.table_channels == 2);
    XSFLIB_CHECK(changes.tracks == std::vector<std::vector<uint8_t>>({{0, 1}}));
    const StreamChanges missing = hokuto::edit::ChangesOfStream(file, *archive, kBackslashSound);
    XSFLIB_CHECK(!missing.present && missing.file == kBackslashFile);
    XSFLIB_CHECK(
        ErrorOf<std::invalid_argument>([&] { hokuto::edit::ChangesOfStream(file, *archive, kOutsideSound); }) ==
        "Only streams in separate files beside the archive can be replaced.");

    // The same stream keeps the game's mark. Another one takes Hokuto's, and the stream that the file lacked does too.
    XSFLIB_CHECK(hokuto::edit::ReplaceStream(file, kSongFile, stream) == file);
    const File replaced = hokuto::edit::ReplaceStream(file, kSongFile, StreamOf(flavor, sar::WaveEncoding::kPcm16));
    XSFLIB_CHECK(!replaced.native->streams.at(kSongFile).from_game);
    const File added = hokuto::edit::ReplaceStream(file, kBackslashFile, stream);
    XSFLIB_CHECK(added.native->streams.size() == 2 && !added.native->streams.at(kBackslashFile).from_game);
    XSFLIB_CHECK(hokuto::format::ExportArchive(replaced) == bytes);

    // A stream file of the archive's console goes in as it is. One of the other console goes in converted.
    const IncomingStream same = hokuto::edit::ReadIncomingStreamFile(stream, flavor, kVersion);
    XSFLIB_CHECK(same.bytes == stream && !same.converted && same.channels == 2 && same.rate == kRate && same.loop);
    const sar::Flavor other = flavor == sar::Flavor::kNx ? sar::Flavor::kCafe : sar::Flavor::kNx;
    const IncomingStream converted = hokuto::edit::ReadIncomingStreamFile(StreamOf(other), flavor, kVersion);
    XSFLIB_CHECK(converted.bytes == stream && converted.converted && converted.converted->flavor == other);
    std::vector<uint8_t> longer = stream;
    longer.push_back(0);
    XSFLIB_CHECK(hokuto::edit::ReadIncomingStreamFile(longer, flavor, kVersion).bytes == stream);
    XSFLIB_CHECK(
        ErrorOf<std::invalid_argument>([&] { hokuto::edit::ReadIncomingStreamFile(bytes, flavor, kVersion); }) ==
        "It's a sound archive of the " + hokuto::song::ConsoleName(flavor) + ", not a stream file.");

    // A WAV file with a loop from sample 1000 to 30000. The samples after the loop's end stay out. The loop's start
    // moves to the block after it, and the stream keeps the WAV file's samples in 16-bit PCM.
    seqlib::Pcm pcm;
    pcm.channels = Channels();
    pcm.loop = true;
    pcm.loop_start = 1000;
    pcm.loop_end = 30000;
    const std::vector<uint8_t> wav = seqlib::WavFile(pcm, kRate);
    const IncomingStream exact = hokuto::edit::ReadIncomingStreamWav(wav, flavor, kVersion, sar::WaveEncoding::kPcm16);
    XSFLIB_CHECK(exact.frames == 30000 && exact.after_loop == kLength - 30000 && exact.loop &&
                 exact.loop_start == 1000 && exact.loop_moved == 4096 - 1000 && !exact.signal_to_noise);
    const sar::Pcm decoded = hokuto::song::DecodeStreamFile(exact.bytes);
    XSFLIB_CHECK(decoded.loop_start == 4096 && decoded.loop_end == 30000 + 4096 - 1000);
    SEQLIB_REQUIRE(decoded.channels.size() > 1 && decoded.channels[0].size() >= 30000 + 4096 - 1000 &&
                   decoded.channels[1].size() >= 30000);
    XSFLIB_CHECK(std::equal(pcm.channels[1].begin(), pcm.channels[1].begin() + 30000, decoded.channels[1].begin()));
    XSFLIB_CHECK(std::equal(pcm.channels[0].begin() + 1000, pcm.channels[0].begin() + 4096,
                            decoded.channels[0].begin() + 30000));

    // In DSP-ADPCM, the loop starts at a block of 14,336 samples, and the ratio of signal to noise is high.
    const IncomingStream adpcm = hokuto::edit::ReadIncomingStreamWav(wav, flavor, kVersion);
    XSFLIB_CHECK(adpcm.encoding == sar::WaveEncoding::kDspAdpcm && adpcm.loop_moved == 14336 - 1000);
    XSFLIB_CHECK(adpcm.signal_to_noise && *adpcm.signal_to_noise > 20.0);
    XSFLIB_CHECK(hokuto::edit::IsStreamFile(adpcm.bytes) && !hokuto::edit::IsStreamFile(wav));
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::ReadIncomingStreamWav(stream, flavor, kVersion); })
                     .starts_with("Can't read it as a WAV file"));
}

// The version of the streams that go into an archive. On the Wii U, a stream takes 3.0.0 unless one of the game's
// streams in the file holds 4.0.0 or a later version. A stream that the file holds as Hokuto's doesn't count. A stream
// file of the Wii U of a later version goes in written again, with the same samples, and one of an earlier version as
// it is. On the Switch, a stream takes 4.0.0, and a stream file of the Switch goes in as it is.
void TestVersions(sar::Flavor flavor)
{
    const bool wii_u = flavor == sar::Flavor::kCafe;
    const uint32_t chosen = wii_u ? kOldVersion : kVersion;
    const std::vector<uint8_t> bytes = ArchiveOf(flavor);
    const std::vector<uint8_t> stream = StreamOf(flavor);
    const std::vector<uint8_t> old = StreamOf(flavor, sar::WaveEncoding::kDspAdpcm, kOldVersion);
    const File game = hokuto::format::ImportArchive(bytes, flavor, {}, {{kSongFile, stream}});
    const File old_game = hokuto::format::ImportArchive(bytes, flavor, {}, {{kSongFile, old}});
    const File bare = hokuto::format::ImportArchive(bytes, flavor, {}, {});
    XSFLIB_CHECK(hokuto::edit::IncomingStreamVersion(game, flavor) == kVersion);
    XSFLIB_CHECK(hokuto::edit::IncomingStreamVersion(old_game, flavor) == chosen);
    XSFLIB_CHECK(hokuto::edit::IncomingStreamVersion(bare, flavor) == chosen);
    XSFLIB_CHECK(hokuto::edit::IncomingStreamVersion(hokuto::edit::ReplaceStream(bare, kSongFile, stream), flavor) ==
                 chosen);

    // A stream file of the archive's console.
    const IncomingStream same = hokuto::edit::ReadIncomingStreamFile(stream, flavor, kOldVersion);
    if (wii_u)
    {
        XSFLIB_CHECK(same.bytes == old && same.version == kOldVersion && same.converted);
        XSFLIB_CHECK(same.converted && same.converted->flavor == flavor && same.converted->version == kVersion &&
                     same.converted->size == stream.size());
    }
    else
    {
        XSFLIB_CHECK(same.bytes == stream && same.version == kVersion && !same.converted);
    }
    const IncomingStream earlier = hokuto::edit::ReadIncomingStreamFile(old, flavor, kVersion);
    XSFLIB_CHECK(earlier.bytes == old && earlier.version == kOldVersion && !earlier.converted);

    // A stream file of the other console, and a WAV file.
    const sar::Flavor other = hokuto::song::OtherConsole(flavor);
    const IncomingStream converted = hokuto::edit::ReadIncomingStreamFile(StreamOf(other), flavor, kOldVersion);
    XSFLIB_CHECK(converted.bytes == old && converted.version == kOldVersion && converted.converted &&
                 converted.converted->flavor == other && converted.converted->version == kVersion);
    const std::vector<uint8_t> wav = WavOf(2);
    const IncomingStream from_wav =
        hokuto::edit::ReadIncomingStreamWav(wav, flavor, kOldVersion, sar::WaveEncoding::kPcm16);
    const IncomingStream newer = hokuto::edit::ReadIncomingStreamWav(wav, flavor, kVersion, sar::WaveEncoding::kPcm16);
    XSFLIB_CHECK(from_wav.version == kOldVersion && sar::StreamFile::Read(from_wav.bytes).version == kOldVersion);
    XSFLIB_CHECK(hokuto::song::DecodeStreamFile(from_wav.bytes).channels ==
                 hokuto::song::DecodeStreamFile(newer.bytes).channels);
}

// New stream sounds from WAV files and stream files, with the model's settings and the file's tracks.
struct NewStreamsFixture
{
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> stream;
    File file;
    std::shared_ptr<const sar::SoundArchive> archive;
    IncomingStream three;
    NewStream made;
};

// The fixture of the tests of new stream sounds.
NewStreamsFixture MakeNewStreamsFixture(sar::Flavor flavor)
{
    const std::vector<uint8_t> bytes = ArchiveOf(flavor, true);
    const std::vector<uint8_t> stream = StreamOf(flavor);
    const File file = hokuto::format::ImportArchive(bytes, flavor, {}, {{kSongFile, stream}});
    const auto archive = hokuto::song::LoadArchive(bytes, "test.bfsar", std::vector<std::vector<uint8_t>>{});
    const IncomingStream three = hokuto::edit::ReadIncomingStreamWav(WavOf(3), flavor, kVersion);
    const NewStream made = hokuto::edit::MakeNewStream(*archive, kStereoSound, "STRM_NEW", three);

    return {bytes, stream, file, archive, three, made};
}

// A new stream sound from three channels copies its tracks and settings from the model.
void TestNewStreamSettings(sar::Flavor flavor)
{
    const NewStreamsFixture fixture = MakeNewStreamsFixture(flavor);
    const IncomingStream& three = fixture.three;
    const NewStream& made = fixture.made;

    // Three channels make a stereo track and a mono one. The first takes the settings of the model's track, and the
    // second those of a track that sets none.
    const sar::StreamSoundInfo& details = made.details;
    XSFLIB_CHECK(made.path == "stream/STRM_NEW.dspadpcm.bfstm" && made.bytes == three.bytes);
    XSFLIB_CHECK(details.channel_count == 3 && details.valid_tracks == 3 && details.tracks.size() == 2);
    XSFLIB_CHECK(details.tracks.size() == 2 && details.tracks[0].channels == std::vector<uint8_t>({0, 1}) &&
                 details.tracks[1].channels == std::vector<uint8_t>({2}));
    XSFLIB_CHECK(details.tracks.size() == 2 && details.tracks[0].volume == 100 && details.tracks[1].volume == 127 &&
                 details.tracks[1].pan == 64 && details.tracks[1].main_send == 127);
    XSFLIB_CHECK(details.pitch == 1.5f && details.main_send == 110);
}

// A new stream sound in the archive, and a second one after it.
void TestNewStreamSounds(sar::Flavor flavor)
{
    const NewStreamsFixture fixture = MakeNewStreamsFixture(flavor);
    const File& file = fixture.file;
    const std::shared_ptr<const sar::SoundArchive>& archive = fixture.archive;
    const IncomingStream& three = fixture.three;
    const NewStream& made = fixture.made;
    const sar::StreamSoundInfo& details = made.details;

    // The new sound copies the model's values, and plays the stream from its new file. The file keeps the stream as
    // Hokuto's, and the game's stream as the game's.
    const File added = hokuto::edit::AddStreamSound(file, kStereoSound, "STRM_NEW", made);
    const std::vector<uint8_t> exported = hokuto::format::ExportArchive(added);
    const auto read = hokuto::song::LoadArchive(exported, "test.bfsar", std::vector<std::vector<uint8_t>>{});
    const auto new_file = static_cast<uint32_t>(archive->FileCount());
    XSFLIB_CHECK(read->Sounds().size() == 6 && read->FileCount() == new_file + 1);
    if (read->Sounds().size() == 6)
    {
        const sar::SoundInfo& sound = read->Sounds()[5];
        XSFLIB_CHECK(sound.name == "STRM_NEW" && sound.type == sar::SoundType::kStream && sound.file_id == new_file);
        XSFLIB_CHECK(sound.stream == details && sound.volume == archive->Sounds()[kStereoSound].volume &&
                     sound.player_id == archive->Sounds()[kStereoSound].player_id);
        XSFLIB_CHECK(read->ExternalPath(new_file) == made.path && read->Sounds()[kWaveSound].name == "WSD_A");
    }
    XSFLIB_CHECK(added.native->streams.size() == 2 && *added.native->streams.at(new_file).bytes == three.bytes &&
                 !added.native->streams.at(new_file).from_game && added.native->streams.at(kSongFile).from_game);
    XSFLIB_CHECK(hokuto::format::Read(hokuto::format::Write(added)) == added);
    XSFLIB_CHECK(hokuto::format::ReadText(hokuto::format::WriteText(added)) == added);

    // A second new sound keeps the first one's stream. Its name takes a number in its path: the file table names the
    // path in another case. 16-bit PCM takes no encoding in the name.
    const IncomingStream pcm =
        hokuto::edit::ReadIncomingStreamWav(WavOf(2), flavor, kVersion, sar::WaveEncoding::kPcm16);
    const NewStream second = hokuto::edit::MakeNewStream(*read, kWaveSound, "strm_song", pcm);
    XSFLIB_CHECK(second.path == "stream/strm_song_2.bfstm");
    XSFLIB_CHECK(hokuto::edit::MakeNewStream(*read, kWaveSound, "STRM_BACK", pcm).path == "stream/STRM_BACK_2.bfstm");
    const File again = hokuto::edit::AddStreamSound(added, kWaveSound, "strm_song", second);
    XSFLIB_CHECK(again.native->streams.size() == 3 && *again.native->streams.at(new_file).bytes == three.bytes &&
                 *again.native->streams.at(new_file + 1).bytes == pcm.bytes);

    // A model of another kind gives its values, and the tracks take the settings of a track that sets none.
    const sar::StreamTrackInfo kDefaultTrack;
    XSFLIB_CHECK(second.details.tracks.size() == 1 && second.details.tracks[0].volume == kDefaultTrack.volume &&
                 second.details.pitch == 1.0f && second.details.main_send == 127);
    const auto twice = hokuto::song::LoadArchive(hokuto::format::ExportArchive(again), "test.bfsar",
                                                 std::vector<std::vector<uint8_t>>{});
    XSFLIB_CHECK(twice->Sounds().size() == 7 && twice->Sounds()[6].volume == 70 &&
                 twice->Sounds()[6].stream == second.details);
}

// A new stream sound from a stream file with tracks.
void TestNewStreamFileTracks(sar::Flavor flavor)
{
    const NewStreamsFixture fixture = MakeNewStreamsFixture(flavor);
    const std::vector<uint8_t>& stream = fixture.stream;
    const std::shared_ptr<const sar::SoundArchive>& archive = fixture.archive;

    // A stream file's tracks give the sound's tracks, with their volumes and pans.
    sar::StreamFile tracked = sar::StreamFile::Read(stream);
    tracked.tracks = std::vector<sar::StreamFile::Track>{{.volume = 90, .pan = 0, .channels = {0}},
                                                         {.volume = 80, .pan = 127, .channels = {1}}};
    const IncomingStream from_file = hokuto::edit::ReadIncomingStreamFile(tracked.Write(), flavor, kVersion);
    const NewStream tracks = hokuto::edit::MakeNewStream(*archive, kStereoSound, "STRM_TRACKS", from_file);
    XSFLIB_CHECK(tracks.details.tracks.size() == 2 && tracks.details.tracks[0].volume == 90 &&
                 tracks.details.tracks[1].pan == 127 && tracks.details.tracks[1].channels == std::vector<uint8_t>({1}));
}

// The errors of new stream sounds.
void TestNewStreamErrors(sar::Flavor flavor)
{
    const NewStreamsFixture fixture = MakeNewStreamsFixture(flavor);
    const File& file = fixture.file;
    const std::shared_ptr<const sar::SoundArchive>& archive = fixture.archive;
    const NewStream& made = fixture.made;

    // A taken name, an empty one, a path that the file table names, a model past the table, more than 8 tracks, and a
    // file without the archive's tables.
    NewStream taken = made;
    taken.path = "stream/STRM_SONG.bfstm";
    File bare = file;
    bare.native.Edit([](hokuto::format::NativeArchive& native) { native.tables.reset(); });
    XSFLIB_CHECK(
        ErrorOf<std::invalid_argument>([&] { hokuto::edit::AddStreamSound(file, kStereoSound, "STRM_SONG", made); }) ==
        "The archive already has an item named STRM_SONG.");
    XSFLIB_CHECK(
        !ErrorOf<std::invalid_argument>([&] { hokuto::edit::AddStreamSound(file, kStereoSound, "", made); }).empty());
    XSFLIB_CHECK(
        ErrorOf<std::invalid_argument>([&] { hokuto::edit::AddStreamSound(file, kStereoSound, "STRM_X", taken); }) ==
        "The archive's file table already contains stream/STRM_SONG.bfstm.");
    XSFLIB_CHECK(
        !ErrorOf<std::invalid_argument>([&] { hokuto::edit::AddStreamSound(file, 9, "STRM_X", made); }).empty());
    XSFLIB_CHECK(
        !ErrorOf<std::invalid_argument>([&] { hokuto::edit::AddStreamSound(bare, 0, "STRM_X", made); }).empty());
    const IncomingStream seventeen =
        hokuto::edit::ReadIncomingStreamWav(WavOf(17), flavor, kVersion, sar::WaveEncoding::kPcm16);
    XSFLIB_CHECK(
        ErrorOf<std::invalid_argument>([&] { hokuto::edit::MakeNewStream(*archive, 0, "STRM_X", seventeen); }) ==
        "A stream sound can have at most 8 tracks. The stream has 17 channels.");
}

// A stream's values from its properties. A rate, and a loop from a block to the stream's end, keep the encoded samples.
// Other values make the stream again from its decoded samples.
void TestStreamValues(sar::Flavor flavor)
{
    // MakeStream moved the loop from sample 1000 to the next block, and the samples between follow the end.
    const std::vector<uint8_t> stream = StreamOf(flavor);
    const hokuto::edit::StreamValues values = hokuto::edit::StreamValuesOf(stream);
    const hokuto::edit::StreamValues kExpected{sar::WaveEncoding::kDspAdpcm, kRate, true, 14336, 53336};
    XSFLIB_CHECK(values == kExpected);
    const sar::StreamFile before = sar::StreamFile::Read(stream);
    hokuto::edit::StreamValues later = values;
    later.rate = 24000;
    later.loop_start = 28672;
    hokuto::edit::StreamValues shorter = values;
    shorter.length = 30000;
    shorter.loop_start = 1000;
    hokuto::edit::StreamValues pcm = values;
    pcm.encoding = sar::WaveEncoding::kPcm16;
    pcm.loop = false;
    pcm.loop_start = 0;

    const sar::StreamFile kept = sar::StreamFile::Read(hokuto::edit::StreamWithValues(stream, later));
    const sar::StreamFile made = sar::StreamFile::Read(hokuto::edit::StreamWithValues(stream, shorter));
    const sar::StreamFile decoded = sar::StreamFile::Read(hokuto::edit::StreamWithValues(stream, pcm));

    // The loop at a block keeps the samples, and starts again with the decoder's state there.
    XSFLIB_CHECK(kept.sample_rate == 24000 && kept.loop_start == 28672 && kept.sample_count == 53336);
    SEQLIB_REQUIRE(kept.channels.size() > 1 && before.channels.size() > 1 && kept.channels[1].adpcm &&
                   before.channels[1].adpcm);
    XSFLIB_CHECK(kept.channels[0].samples == before.channels[0].samples);
    const ntdlib::wave::DspAdpcmState state = ntdlib::wave::DspAdpcmStateAt(
        before.channels[1].samples, 28672, before.channels[1].adpcm->coefs, 0, 0, ntdlib::wave::DspAdpcmSum::kWrap);
    XSFLIB_CHECK(kept.channels[1].adpcm->loop_pred_scale == state.pred_scale &&
                 kept.channels[1].adpcm->loop_yn1 == state.yn1 && kept.channels[1].adpcm->loop_yn2 == state.yn2);

    // A shorter stream with a loop between blocks is made again: its loop moves to the next block, and the samples
    // between follow its end.
    XSFLIB_CHECK(made.loop && made.loop_start == 14336 && made.sample_count == 30000 + 13336);
    XSFLIB_CHECK(sar::DecodeStream(decoded).channels == sar::DecodeStream(before).channels && !decoded.loop);
    XSFLIB_CHECK(decoded.encoding == sar::WaveEncoding::kPcm16);

    hokuto::edit::StreamValues silent = values;
    silent.rate = 0;
    hokuto::edit::StreamValues longer = values;
    longer.length = 53337;
    hokuto::edit::StreamValues late = values;
    late.loop_start = 53336;
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::StreamWithValues(stream, silent); }) ==
                 "The sample rate must be above 0.");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::StreamWithValues(stream, longer); }) ==
                 "The length must be from 1 to 53336 samples.");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::StreamWithValues(stream, late); }) ==
                 "The loop must start before the end of the stream.");
}

// A WAV file in place of an AAC stream that the file lacks takes the channels that the sound's entry gives. A mono file
// fills both channels of a stereo stream. It does the same in place of a present stereo stream. An entry of no channels
// leaves the file's. The WAV file's rate is 32 kHz. In place of a stream that the file lacks, the system's encoder
// encodes it at 48 kHz. In place of a present stream, the encoder takes the present stream's rate: 44.1 kHz here.
void TestAacChannels()
{
    hokuto::edit::AacChanges changes;
    changes.present = false;
    changes.channels = 2;
    changes.bit_rate = 128000;
    const hokuto::edit::IncomingAac stereo = hokuto::edit::ReadIncomingAac(WavOf(1), changes);
    const ntdlib::wave::Pcm both = hokuto::song::DecodeAdts(stereo.bytes);
    changes.channels = 0;
    const hokuto::edit::IncomingAac mono = hokuto::edit::ReadIncomingAac(WavOf(1), changes);
    changes.present = true;
    changes.channels = 2;
    changes.rate = 44100;
    const hokuto::edit::IncomingAac in_place = hokuto::edit::ReadIncomingAac(WavOf(1), changes);
    const ntdlib::wave::Pcm in_place_both = hokuto::song::DecodeAdts(in_place.bytes);

    XSFLIB_CHECK(stereo.encoded && stereo.channels == 1 && stereo.stream_channels == 2 && stereo.stream_rate == 48000);
    XSFLIB_CHECK(in_place.encoded && in_place.channels == 1 && in_place.stream_channels == 2 &&
                 in_place.stream_rate == 44100 && in_place_both.sample_rate == 44100);
    SEQLIB_REQUIRE_EQ(both.channels.size(), std::size_t{2});
    SEQLIB_REQUIRE_EQ(in_place_both.channels.size(), std::size_t{2});

    const auto level = [](const std::vector<int16_t>& channel)
    {
        double sum = 0.0;
        for (const int16_t sample : channel)
        {
            sum += static_cast<double>(sample) * sample;
        }

        return 10.0 * std::log10(sum / static_cast<double>(std::max<std::size_t>(channel.size(), 1)) + 1e-9);
    };
    XSFLIB_CHECK(level(both.channels[0]) > 40.0 && std::abs(level(both.channels[0]) - level(both.channels[1])) < 1.0);
    XSFLIB_CHECK(level(in_place_both.channels[0]) > 40.0 &&
                 std::abs(level(in_place_both.channels[0]) - level(in_place_both.channels[1])) < 1.0);
    XSFLIB_CHECK(mono.stream_channels == 1 && hokuto::song::DecodeAdts(mono.bytes).channels.size() == 1);
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("streams",
                                    []
                                    {
                                        TestPaths();
                                        TestAacChannels();
                                        for (const sar::Flavor flavor : {sar::Flavor::kNx, sar::Flavor::kCafe})
                                        {
                                            TestImport(flavor);
                                            TestConversions(flavor);
                                            TestStreamPreview(flavor);
                                            TestReplacement(flavor);
                                            TestVersions(flavor);
                                            TestNewStreamSettings(flavor);
                                            TestNewStreamSounds(flavor);
                                            TestNewStreamFileTracks(flavor);
                                            TestNewStreamErrors(flavor);
                                            TestStreamValues(flavor);
                                        }
                                    });
}
