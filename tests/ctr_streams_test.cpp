// SPDX-License-Identifier: MIT

// Tests the streams of the 3DS (src/song/streams.cpp, src/edit/streams.cpp and new_items.cpp). ntdlib lays out 3DS
// archives of four versions from its model. Each holds a stream sound. Before CSAR 2.3.0.0, its stream file gives its
// tracks.

#include "streams_fixtures.h"

namespace
{

// The versions of 3DS archives from which the table of sounds gives a stream sound's tracks, and from which it gives
// the sound's pitch and sends too.
constexpr uint32_t kCtrTracksVersion = 0x02030000;
constexpr uint32_t kCtrFieldsVersion = 0x02030200;

// A 3DS archive of `version`, CSAR 2.2.0.0 unless another is given, that ntdlib lays out from its model. Its stream
// sound plays stream/STRM_CTR.bcstm. Before 2.3.0.0, the sound's details hold the tracks that play and the count of
// channels alone: the stream file gives the tracks. From 2.3.0.0 on, the details give a stereo track at volume 100 and
// pan 40. From 2.3.2.0 on, they also give a pitch of 1.5 and a main send of 110.
std::vector<uint8_t> CtrArchive(uint32_t version = 0x02020000)
{
    sar::ArchiveModel model;
    model.flavor = sar::Flavor::kCtr;
    model.version = version;
    sar::SoundModel sound;
    sound.name = "STRM_CTR";
    sound.type = sar::SoundType::kStream;
    sound.file_id = 0;
    sound.stream.valid_tracks = 1;
    sound.stream.channel_count = 2;
    if (version >= kCtrTracksVersion)
    {
        sound.stream.tracks = {{.channels = {0, 1}, .volume = 100, .pan = 40}};
    }
    if (version >= kCtrFieldsVersion)
    {
        sound.stream.pitch = 1.5f;
        sound.stream.main_send = 110;
    }
    else
    {
        sound.stream_fields = sar::StreamFields::kNone;
    }
    model.sounds = {sound};
    model.players = {{.name = "PLAYER", .playable_sound_max = 1}};
    sar::FileModel stream_file;
    stream_file.place = sar::FilePlace::kPath;
    stream_file.path = "stream/STRM_CTR.bcstm";
    model.files = {stream_file};

    return sar::BuildArchive(model);
}

// The streams of a 3DS archive: the import, the version of a new stream, the tracks that its stream sound plays from
// the stream file, and a stream's values.
struct CtrStreamsFixture
{
    sar::StreamFile present;
    std::vector<uint8_t> stream;
    std::vector<uint8_t> newer;
    std::vector<uint8_t> bytes;
    File file;
    std::shared_ptr<const sar::SoundArchive> archive;
};

// The fixture of the tests of the 3DS's streams. It imports the archive with its stream, as the import reads it.
CtrStreamsFixture MakeCtrStreamsFixture()
{
    // The game's stream of 2.0.0.0, in DSP-ADPCM, with a stereo track at volume 100 and pan 40. Its loop moves from
    // sample 1000 to the second block.
    sar::StreamFile present =
        sar::MakeStream(sar::Flavor::kCtr, kCtrOldestVersion, sar::WaveEncoding::kDspAdpcm, kRate, Channels(), 1000);
    present.tracks = std::vector<sar::StreamFile::Track>{{.volume = 100, .pan = 40, .channels = {0, 1}}};
    const std::vector<uint8_t> stream = present.Write();
    const std::vector<uint8_t> newer =
        sar::MakeStream(sar::Flavor::kCtr, kCtrNewestVersion, sar::WaveEncoding::kDspAdpcm, kRate, Channels(), 1000)
            .Write();
    const std::vector<uint8_t> bytes = CtrArchive();
    const File file = hokuto::format::ImportArchive(bytes, sar::Flavor::kCtr, {}, {{0, stream}});
    const auto archive = hokuto::song::LoadArchive(bytes, "test.bcsar", std::vector<std::vector<uint8_t>>{});

    return {present, stream, newer, bytes, file, archive};
}

// The import of a 3DS archive with its stream.
void TestCtrImport()
{
    const CtrStreamsFixture fixture = MakeCtrStreamsFixture();
    const std::vector<uint8_t>& stream = fixture.stream;

    // The import reads the 3DS's stream beside the archive, and leaves a stream of another console out.
    const TemporaryFolder folder;
    const std::vector<uint8_t> bytes = CtrArchive();
    const std::filesystem::path path = folder.Path() / "test.bcsar";
    XSFLIB_CHECK(!xsflib::WriteFile(path, bytes));
    XSFLIB_CHECK(!hokuto::song::WriteStream(folder.Path() / "stream" / "STRM_CTR.bcstm", StreamOf(sar::Flavor::kNx)));
    const auto archive = hokuto::song::LoadArchive(bytes, "test.bcsar", path);
    XSFLIB_CHECK(hokuto::song::ReadStreams(*archive, path).empty());
    XSFLIB_CHECK(!hokuto::song::WriteStream(folder.Path() / "stream" / "STRM_CTR.bcstm", stream));
    const std::map<uint32_t, std::vector<uint8_t>> streams = hokuto::song::ReadStreams(*archive, path);
    XSFLIB_CHECK(streams.size() == 1 && streams.at(0) == stream);
    XSFLIB_CHECK(hokuto::song::StreamFileName(*archive, 0) == "STRM_CTR.bcstm");
    const File file = hokuto::format::ImportArchive(bytes, sar::Flavor::kCtr, {}, streams);
    XSFLIB_CHECK(hokuto::format::ExportArchive(file) == bytes);
    XSFLIB_CHECK(hokuto::format::Read(hokuto::format::Write(file)) == file);
    XSFLIB_CHECK(hokuto::format::ReadText(hokuto::format::WriteText(file)) == file);
}

// The version of a new stream, and the tracks that its stream sound plays.
void TestCtrIncomingStreams()
{
    const CtrStreamsFixture fixture = MakeCtrStreamsFixture();
    const sar::StreamFile& present = fixture.present;
    const std::vector<uint8_t>& stream = fixture.stream;
    const std::vector<uint8_t>& newer = fixture.newer;
    const std::vector<uint8_t>& bytes = fixture.bytes;
    const File& file = fixture.file;
    const std::shared_ptr<const sar::SoundArchive>& archive = fixture.archive;

    // A new stream takes the version of the game's streams, or the oldest without them. A stream of a later version
    // goes in written again, and one of the Switch converted.
    const File bare = hokuto::format::ImportArchive(bytes, sar::Flavor::kCtr, {}, {});
    const File newer_game = hokuto::format::ImportArchive(bytes, sar::Flavor::kCtr, {}, {{0, newer}});
    XSFLIB_CHECK(hokuto::edit::IncomingStreamVersion(file, sar::Flavor::kCtr) == kCtrOldestVersion);
    XSFLIB_CHECK(hokuto::edit::IncomingStreamVersion(bare, sar::Flavor::kCtr) == kCtrOldestVersion);
    XSFLIB_CHECK(hokuto::edit::IncomingStreamVersion(newer_game, sar::Flavor::kCtr) == kCtrNewestVersion);
    const IncomingStream later = hokuto::edit::ReadIncomingStreamFile(newer, sar::Flavor::kCtr, kCtrOldestVersion);
    XSFLIB_CHECK(later.version == kCtrOldestVersion && later.converted &&
                 later.converted->flavor == sar::Flavor::kCtr && later.converted->version == kCtrNewestVersion);
    XSFLIB_CHECK(hokuto::song::DecodeStreamFile(later.bytes).channels ==
                 hokuto::song::DecodeStreamFile(newer).channels);
    const IncomingStream earlier = hokuto::edit::ReadIncomingStreamFile(stream, sar::Flavor::kCtr, kCtrNewestVersion);
    XSFLIB_CHECK(earlier.bytes == stream && !earlier.converted);
    const IncomingStream from_switch =
        hokuto::edit::ReadIncomingStreamFile(StreamOf(sar::Flavor::kNx), sar::Flavor::kCtr, kCtrOldestVersion);
    XSFLIB_CHECK(from_switch.converted && from_switch.converted->flavor == sar::Flavor::kNx &&
                 sar::StreamFile::Read(from_switch.bytes).flavor == sar::Flavor::kCtr);

    // The sound plays the present stream's track. A new stream without tracks takes it. A mono stream takes a track of
    // its channel, and a stream with tracks keeps them.
    const StreamChanges changes = hokuto::edit::ChangesOfStream(file, *archive, 0);
    XSFLIB_CHECK(changes.tracks_in_file && changes.file_tracks == *present.tracks);
    XSFLIB_CHECK(changes.tracks == std::vector<std::vector<uint8_t>>({{0, 1}}) && changes.table_channels == 2);
    const IncomingStream wav = hokuto::edit::ReadIncomingStreamWav(WavOf(2), sar::Flavor::kCtr, kCtrOldestVersion);
    XSFLIB_CHECK(hokuto::edit::SoundTracksFor(wav, changes) == present.tracks);
    XSFLIB_CHECK(sar::StreamFile::Read(hokuto::edit::WithSoundTracks(wav, changes).bytes).tracks == present.tracks);
    const IncomingStream mono = hokuto::edit::ReadIncomingStreamWav(WavOf(1), sar::Flavor::kCtr, kCtrOldestVersion);
    const std::optional<std::vector<sar::StreamFile::Track>> mono_tracks = hokuto::edit::SoundTracksFor(mono, changes);
    XSFLIB_CHECK(mono_tracks && mono_tracks->size() == 1 && mono_tracks->front().channels == std::vector<uint8_t>({0}));
    XSFLIB_CHECK(!hokuto::edit::SoundTracksFor(earlier, changes));
    const StreamChanges without = hokuto::edit::ChangesOfStream(bare, *archive, 0);
    XSFLIB_CHECK(without.tracks_in_file && !without.present && without.tracks.empty());
    const std::optional<std::vector<sar::StreamFile::Track>> pairs = hokuto::edit::SoundTracksFor(wav, without);
    XSFLIB_CHECK(pairs && pairs->size() == 1 && pairs->front().channels == std::vector<uint8_t>({0, 1}) &&
                 pairs->front().volume == 127 && pairs->front().pan == 64);

    // From 2.3.0.0 on, the archive's table gives the tracks.
    const std::vector<uint8_t> tables = ArchiveOf(sar::Flavor::kCtr);
    const auto tables_archive = hokuto::song::LoadArchive(tables, "test.bcsar", std::vector<std::vector<uint8_t>>{});
    const File tables_file = hokuto::format::ImportArchive(tables, sar::Flavor::kCtr, {}, {{kSongFile, stream}});
    const StreamChanges table_changes = hokuto::edit::ChangesOfStream(tables_file, *tables_archive, kMonoSound);
    XSFLIB_CHECK(!table_changes.tracks_in_file &&
                 table_changes.tracks == std::vector<std::vector<uint8_t>>({{0}, {1}}));
    XSFLIB_CHECK(!hokuto::edit::SoundTracksFor(wav, table_changes));
}

// A 3DS stream's values from its properties.
void TestCtrStreamValues()
{
    const CtrStreamsFixture fixture = MakeCtrStreamsFixture();
    const sar::StreamFile& present = fixture.present;
    const std::vector<uint8_t>& stream = fixture.stream;

    // A rate keeps the samples, and so does a loop at a later block. A loop between blocks makes the stream again: the
    // loop moves to the block after the one that holds it, and the loop's first 128 samples follow the end. So does a
    // loop from the first block.
    const hokuto::edit::StreamValues values = hokuto::edit::StreamValuesOf(stream);
    XSFLIB_CHECK(values.loop && values.loop_start == 14336 && values.length == kLength + 14336 - 1000);
    hokuto::edit::StreamValues faster = values;
    faster.rate = 48000;
    const sar::StreamFile kept = sar::StreamFile::Read(hokuto::edit::StreamWithValues(stream, faster));
    SEQLIB_REQUIRE(!kept.channels.empty() && !sar::StreamFile::Read(stream).channels.empty());
    XSFLIB_CHECK(kept.sample_rate == 48000 &&
                 kept.channels[0].samples == sar::StreamFile::Read(stream).channels[0].samples &&
                 kept.samples_past_end == 128 && kept.tracks == present.tracks);
    hokuto::edit::StreamValues moved = values;
    moved.loop_start = 20000;
    const sar::StreamFile made = sar::StreamFile::Read(hokuto::edit::StreamWithValues(stream, moved));
    XSFLIB_CHECK(made.loop && made.loop_start == 28672 && made.sample_count == values.length + 28672 - 20000);
    XSFLIB_CHECK(made.samples_past_end == 128 && made.tracks == present.tracks);
    hokuto::edit::StreamValues at_block = values;
    at_block.loop_start = 28672;
    const sar::StreamFile block = sar::StreamFile::Read(hokuto::edit::StreamWithValues(stream, at_block));
    XSFLIB_CHECK(block.loop_start == 28672 && block.sample_count == values.length && block.samples_past_end == 128);
    SEQLIB_REQUIRE(block.channels.size() > 1 && sar::StreamFile::Read(stream).channels.size() > 1);
    XSFLIB_CHECK(block.channels[1].samples == sar::StreamFile::Read(stream).channels[1].samples);
    hokuto::edit::StreamValues first = values;
    first.loop_start = 0;
    const sar::StreamFile again = sar::StreamFile::Read(hokuto::edit::StreamWithValues(stream, first));
    XSFLIB_CHECK(again.loop_start == 14336 && again.sample_count == values.length + 14336);
}

// New stream sounds in 3DS archives of four versions.
struct CtrNewStreamsFixture
{
    std::shared_ptr<const sar::SoundArchive> archive;
    File file;
    bool in_file = false;
    IncomingStream three;
    NewStream made;
};

// The fixture of the tests of new stream sounds in a 3DS archive of `version`.
CtrNewStreamsFixture MakeCtrNewStreamsFixture(uint32_t version)
{
    sar::StreamFile present =
        sar::MakeStream(sar::Flavor::kCtr, kCtrOldestVersion, sar::WaveEncoding::kDspAdpcm, kRate, Channels(), 1000);
    present.tracks = std::vector<sar::StreamFile::Track>{{.volume = 100, .pan = 40, .channels = {0, 1}}};
    const std::vector<uint8_t> stream = present.Write();
    const std::vector<uint8_t> bytes = CtrArchive(version);
    const auto archive = hokuto::song::LoadArchive(bytes, "test.bcsar", std::vector<std::vector<uint8_t>>{});
    const File file = hokuto::format::ImportArchive(bytes, sar::Flavor::kCtr, {}, {{0, stream}});
    const bool in_file = version < kCtrTracksVersion;
    const IncomingStream three = hokuto::edit::ReadIncomingStreamWav(WavOf(3), sar::Flavor::kCtr, kCtrOldestVersion);
    const NewStream made = hokuto::edit::MakeNewStream(*archive, 0, "STRM_NEW", three);

    return {archive, file, in_file, three, made};
}

// Before 2.3.0.0, the sound plays the tracks of its stream file, and a stream without tracks takes those of the pairs
// of its channels. From 2.3.0.0 on, the archive's table gives the tracks, as on the Wii U and the Switch.
void TestCtrNewStreamDetails(uint32_t version)
{
    const CtrNewStreamsFixture fixture = MakeCtrNewStreamsFixture(version);
    const bool in_file = fixture.in_file;
    const IncomingStream& three = fixture.three;
    const NewStream& made = fixture.made;

    // Three channels make a stereo track and a mono one. Where the stream file holds the tracks, they take full volume
    // and the centre. Otherwise the first takes the settings of the model's track, and the second those of a track that
    // sets none.
    const sar::StreamSoundInfo& details = made.details;
    XSFLIB_CHECK(made.path == "stream/STRM_NEW.dspadpcm.bcstm" && made.tracks_in_file == in_file && !made.file_tracks);
    XSFLIB_CHECK(details.channel_count == 3 && details.valid_tracks == 3);
    const std::optional<std::vector<sar::StreamFile::Track>> tracks = sar::StreamFile::Read(made.bytes).tracks;
    if (in_file)
    {
        XSFLIB_CHECK(details.tracks.empty() && details.pitch == 1.0f && details.main_send == 127);
        XSFLIB_CHECK(tracks && tracks->size() == 2 && (*tracks)[0].channels == std::vector<uint8_t>({0, 1}) &&
                     (*tracks)[1].channels == std::vector<uint8_t>({2}) && (*tracks)[0].volume == 127 &&
                     (*tracks)[1].pan == 64);
        XSFLIB_CHECK(hokuto::song::DecodeStreamFile(made.bytes).channels ==
                     hokuto::song::DecodeStreamFile(three.bytes).channels);
    }
    else
    {
        XSFLIB_CHECK(made.bytes == three.bytes && !tracks);
        XSFLIB_CHECK(details.tracks.size() == 2 && details.tracks[0].volume == 100 && details.tracks[0].pan == 40 &&
                     details.tracks[1].volume == 127 && details.tracks[1].pan == 64);
        XSFLIB_CHECK(details.tracks.size() == 2 && details.tracks[0].channels == std::vector<uint8_t>({0, 1}) &&
                     details.tracks[1].channels == std::vector<uint8_t>({2}));
        XSFLIB_CHECK(version < kCtrFieldsVersion ? details.pitch == 1.0f && details.main_send == 127
                                                 : details.pitch == 1.5f && details.main_send == 110);
    }
}

// A new stream sound in a 3DS archive, and a second one from a stream file with tracks.
void TestCtrNewStreamSounds(uint32_t version)
{
    const CtrNewStreamsFixture fixture = MakeCtrNewStreamsFixture(version);
    const std::shared_ptr<const sar::SoundArchive>& archive = fixture.archive;
    const File& file = fixture.file;
    const bool in_file = fixture.in_file;
    const NewStream& made = fixture.made;
    const sar::StreamSoundInfo& details = made.details;

    // The new sound holds the details, and plays the stream from its new file. The file keeps the stream as Hokuto's,
    // and the game's stream as the game's.
    const File added = hokuto::edit::AddStreamSound(file, 0, "STRM_NEW", made);
    const std::vector<uint8_t> exported = hokuto::format::ExportArchive(added);
    const auto read = hokuto::song::LoadArchive(exported, "test.bcsar", std::vector<std::vector<uint8_t>>{});
    XSFLIB_CHECK(read->Sounds().size() == 2 && read->FileCount() == 2);
    if (read->Sounds().size() == 2)
    {
        const sar::SoundInfo& sound = read->Sounds()[1];
        XSFLIB_CHECK(sound.name == "STRM_NEW" && sound.type == sar::SoundType::kStream && sound.file_id == 1);
        XSFLIB_CHECK(sound.stream == details && read->ExternalPath(1) == made.path);
        XSFLIB_CHECK(read->Sounds()[0].stream == archive->Sounds()[0].stream);
    }
    XSFLIB_CHECK(added.native->streams.size() == 2 && *added.native->streams.at(1).bytes == made.bytes &&
                 !added.native->streams.at(1).from_game && added.native->streams.at(0).from_game);
    XSFLIB_CHECK(hokuto::format::Read(hokuto::format::Write(added)) == added);
    XSFLIB_CHECK(hokuto::format::ReadText(hokuto::format::WriteText(added)) == added);

    // A stream file's tracks go in as they are, and 16-bit PCM takes no encoding in the name.
    sar::StreamFile tracked = sar::StreamFile::Read(
        hokuto::edit::ReadIncomingStreamWav(WavOf(2), sar::Flavor::kCtr, kCtrOldestVersion, sar::WaveEncoding::kPcm16)
            .bytes);
    tracked.tracks = std::vector<sar::StreamFile::Track>{{.volume = 90, .pan = 0, .channels = {0}},
                                                         {.volume = 80, .pan = 127, .channels = {1}}};
    const IncomingStream from_file =
        hokuto::edit::ReadIncomingStreamFile(tracked.Write(), sar::Flavor::kCtr, kCtrOldestVersion);
    const NewStream kept = hokuto::edit::MakeNewStream(*read, 1, "STRM_TRACKS", from_file);
    XSFLIB_CHECK(kept.path == "stream/STRM_TRACKS.bcstm" && kept.file_tracks && kept.bytes == from_file.bytes);
    XSFLIB_CHECK(kept.details.valid_tracks == 3 &&
                 (in_file ? kept.details.tracks.empty()
                          : kept.details.tracks.size() == 2 && kept.details.tracks[0].volume == 90 &&
                                kept.details.tracks[1].pan == 127));
    const File twice = hokuto::edit::AddStreamSound(added, 1, "STRM_TRACKS", kept);
    const auto again = hokuto::song::LoadArchive(hokuto::format::ExportArchive(twice), "test.bcsar",
                                                 std::vector<std::vector<uint8_t>>{});
    XSFLIB_CHECK(again->Sounds().size() == 3 && again->Sounds()[2].stream == kept.details);
}

// The 3DS's player plays 4 tracks of 2 channels at most, and 8 channels.
void TestCtrNewStreamLimits()
{
    // More than 4 tracks, a track of more than 2 channels, and more than 8 channels.
    const auto archive = hokuto::song::LoadArchive(CtrArchive(), "test.bcsar", std::vector<std::vector<uint8_t>>{});
    const IncomingStream nine =
        hokuto::edit::ReadIncomingStreamWav(WavOf(9), sar::Flavor::kCtr, kCtrOldestVersion, sar::WaveEncoding::kPcm16);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::MakeNewStream(*archive, 0, "STRM_X", nine); }) ==
                 "A 3DS stream sound can have at most 4 tracks. The stream has 9 channels.");
    sar::StreamFile wide = sar::StreamFile::Read(nine.bytes);
    wide.tracks = std::vector<sar::StreamFile::Track>{{.channels = {0, 1, 2}}};
    const IncomingStream three_wide =
        hokuto::edit::ReadIncomingStreamFile(wide.Write(), sar::Flavor::kCtr, kCtrOldestVersion);
    XSFLIB_CHECK(
        ErrorOf<std::invalid_argument>([&] { hokuto::edit::MakeNewStream(*archive, 0, "STRM_X", three_wide); }) ==
        "A track of a 3DS stream sound can have at most 2 channels. Track 0 of the stream has 3.");
    wide.tracks = std::vector<sar::StreamFile::Track>{{.channels = {0, 1}}};
    const IncomingStream nine_wide =
        hokuto::edit::ReadIncomingStreamFile(wide.Write(), sar::Flavor::kCtr, kCtrOldestVersion);
    XSFLIB_CHECK(
        ErrorOf<std::invalid_argument>([&] { hokuto::edit::MakeNewStream(*archive, 0, "STRM_X", nine_wide); }) ==
        "A 3DS stream sound can have at most 8 channels. The stream has 9 channels in 1 track.");
}

} // namespace

int main()
{
    return xsflib::testing::RunTest(
        "ctr_streams",
        []
        {
            TestCtrImport();
            TestCtrIncomingStreams();
            TestCtrStreamValues();
            for (const uint32_t version : {kCtrOldestVersion, 0x02020000u, kCtrTracksVersion, kCtrFieldsVersion})
            {
                TestCtrNewStreamDetails(version);
                TestCtrNewStreamSounds(version);
            }
            TestCtrNewStreamLimits();
        });
}
