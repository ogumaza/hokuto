// SPDX-License-Identifier: MIT

// Tests the streams of the Wii (src/song/streams.cpp, src/edit/streams.cpp and wii_new_items.cpp). ntdlib lays out a
// Wii archive whose stream sounds play RSTM files beside it, built with ntdlib's builder of the Wii's streams. That
// archive's stream takes new values, replacements of the Wii, the Wii U and the Switch and of WAV files, and new stream
// sounds, in versions 1.1 and 1.4 of the archive.

#include "streams_fixtures.h"

namespace
{

// A stream of the Wii that holds Channels() in `encoding`, with a loop from the start of the second block. Its one
// track plays both channels at volume 100, left of the centre.
std::vector<uint8_t> WiiStreamOf(ntdlib::rsar::WaveEncoding encoding)
{
    ntdlib::testing::rsar_streams::StreamSpec spec;
    spec.encoding = encoding;
    spec.sample_rate = kRate;
    spec.loop_start = encoding == ntdlib::rsar::WaveEncoding::kPcm16 ? 4096 : 14336;
    spec.channels = Channels();
    spec.tracks = {{100, 40, {0, 1}}};

    return ntdlib::testing::rsar_streams::MakeStream(spec);
}

// A Wii archive of `version`, laid out as NW4R's tools lay one out (ntdlib's rsar::BuildArchive). Its file table names
// three stream files of `size` bytes beside it: one in the folder stream, one from the disc's root and one that leads
// out of the archive's folder. STRM_BGM, STRM_ROOT and STRM_OUT play them, at volume 100.
std::vector<uint8_t> WiiArchiveOf(uint32_t size, uint16_t version = 0x0104)
{
    namespace rsar = ntdlib::rsar;
    rsar::ArchiveModel model;
    model.version = version;
    model.players = {{"PLAYER_BGM", 1, 0}};
    model.files = {{.path = "stream/BGM.brstm", .path_size = size},
                   {.path = "/Sound/stream/ROOT.brstm", .path_size = size},
                   {.path = "../OUT.brstm", .path_size = size}};
    const std::array<const char*, 3> kNames = {"STRM_BGM", "STRM_ROOT", "STRM_OUT"};
    for (uint32_t i = 0; i < kNames.size(); i++)
    {
        rsar::SoundModel sound;
        sound.name = kNames[i];
        sound.type = rsar::SoundType::kStream;
        sound.file_id = i;
        sound.volume = 100;
        model.sounds.push_back(sound);
    }

    return rsar::BuildArchive(model);
}

// The output of ntdlib's model of the Wii run directly on `build` for `frames` frames, with stream sound `sound` of
// `archive` started on `stream`, as floats.
std::vector<float> WiiStreamOnModel(const ntdlib::rsar::SoundArchive& archive, uint32_t sound,
                                    const std::vector<uint8_t>& stream, const ntdlib::SndBuild& build,
                                    std::size_t frames)
{
    ntdlib::rvl::snd::ArchiveModel model;
    model.build = build;
    model.StartWithRenderer(archive);
    model.engine->StartStreamSound(sound, stream);
    std::vector<int16_t> output;
    for (std::size_t frame = 0; frame < frames; frame++)
    {
        model.RunFrame(output);
    }
    std::vector<float> samples;
    for (const int16_t sample : output)
    {
        samples.push_back(static_cast<float>(sample) / 32768.0f);
    }

    return samples;
}

// The streams beside a Wii archive. The import reads the stream in the archive's folder, and leaves out the one from
// the disc's root and the one outside the folder. The file keeps the stream in both forms, and in the model form as
// Hokuto's. The stream decodes as ntdlib decodes it: 16-bit PCM gives its samples back. It goes out as a WAV file, and
// as a file of each later console with the same samples. DSP-ADPCM keeps its decoded samples there too. The browser
// plays the stream sound as ntdlib's model of NW4R SND plays it run directly, on a build of 2007 and on one of 2008.
// The build of 2008 plays the track at the stream file's volume and pan.
void TestWiiStreams()
{
    namespace rsar = ntdlib::rsar;
    const TemporaryFolder folder;
    const std::vector<uint8_t> stream = WiiStreamOf(rsar::WaveEncoding::kPcm16);
    const std::vector<uint8_t> bytes = WiiArchiveOf(static_cast<uint32_t>(stream.size()));
    const std::filesystem::path path = folder.Path() / "Sound" / "test.brsar";
    XSFLIB_CHECK(!hokuto::song::WriteStream(folder.Path() / "Sound" / "stream" / "BGM.brstm", stream));
    XSFLIB_CHECK(!hokuto::song::WriteStream(folder.Path() / "OUT.brstm", stream));
    XSFLIB_CHECK(!xsflib::WriteFile(path, bytes));

    const auto archive = hokuto::song::LoadWiiArchive(bytes, "test.brsar", ntdlib::rvl::snd::kRsarOptions);
    XSFLIB_CHECK(hokuto::song::StreamPath(*archive, 0) == std::filesystem::path("stream/BGM.brstm"));
    XSFLIB_CHECK(!hokuto::song::StreamPath(*archive, 1));
    XSFLIB_CHECK(!hokuto::song::StreamPath(*archive, 2));
    XSFLIB_CHECK(hokuto::song::StreamFileOf(*archive, 0) == 0u);
    XSFLIB_CHECK(!hokuto::song::StreamFileOf(*archive, 1));
    XSFLIB_CHECK(hokuto::song::StreamFileName(*archive, 0) == "BGM.brstm");
    XSFLIB_CHECK(hokuto::song::StreamFileName(*archive, 2) == "STRM_OUT.brstm");
    const std::vector<uint32_t> kFiles = {0};
    XSFLIB_CHECK(hokuto::song::StreamPaths(*archive, kFiles) ==
                 std::vector<std::filesystem::path>({"stream/BGM.brstm"}));
    const std::map<uint32_t, std::vector<uint8_t>> streams = hokuto::song::ReadStreams(*archive, path);
    XSFLIB_CHECK(streams.size() == 1 && streams.at(0) == stream);

    const File file = hokuto::format::ImportWiiArchive(bytes, std::nullopt, streams);
    SEQLIB_REQUIRE(file.native && file.native->streams.size() == 1);
    XSFLIB_CHECK(*file.native->streams.at(0).bytes == stream && file.native->streams.at(0).from_game);
    XSFLIB_CHECK(hokuto::format::Read(hokuto::format::Write(file)) == file);
    XSFLIB_CHECK(hokuto::format::ReadText(hokuto::format::WriteText(file)) == file);
    XSFLIB_CHECK(hokuto::format::ExportArchive(file) == bytes);
    const File model = hokuto::format::InModelForm(file);
    SEQLIB_REQUIRE(model.native && model.native->streams.size() == 1);
    XSFLIB_CHECK(*model.native->streams.at(0).bytes == stream && !model.native->streams.at(0).from_game);
    XSFLIB_CHECK(hokuto::format::Read(hokuto::format::Write(model)) == model);
    XSFLIB_CHECK(hokuto::format::ReadText(hokuto::format::WriteText(model)) == model);

    const sar::Pcm decoded = hokuto::song::DecodeStreamFile(stream);
    XSFLIB_CHECK(decoded.channels == Channels() && decoded.sample_rate == kRate);
    XSFLIB_CHECK(decoded.loop && decoded.loop_start == 4096 && decoded.loop_end == kLength);
    seqlib::Pcm pcm;
    pcm.loop = true;
    pcm.loop_start = 4096;
    pcm.loop_end = kLength;
    pcm.channels = Channels();
    XSFLIB_CHECK(hokuto::song::StreamAsWav(stream) == seqlib::WavFile(pcm, kRate));
    const std::vector<uint8_t> adpcm = WiiStreamOf(rsar::WaveEncoding::kDspAdpcm);
    const sar::Pcm adpcm_decoded = hokuto::song::DecodeStreamFile(adpcm);
    for (const auto& [flavor, version] :
         {std::pair{sar::Flavor::kCafe, kOldVersion}, std::pair{sar::Flavor::kNx, kVersion},
          std::pair{sar::Flavor::kCtr, kCtrOldestVersion}})
    {
        const sar::Pcm back = hokuto::song::DecodeStreamFile(hokuto::song::ConvertStreamFile(stream, flavor, version));
        XSFLIB_CHECK(back.channels == decoded.channels && back.sample_rate == kRate && back.loop &&
                     back.loop_start == 4096 && back.loop_end == kLength);
        const sar::Pcm adpcm_back =
            hokuto::song::DecodeStreamFile(hokuto::song::ConvertStreamFile(adpcm, flavor, version));
        XSFLIB_CHECK(adpcm_back.channels == adpcm_decoded.channels && adpcm_back.loop_start == 14336);
    }

    const std::vector<float> played_2007 = WiiStreamOnModel(*archive, 0, stream, {20070608, false}, 300);
    const std::vector<float> played_2008 = WiiStreamOnModel(*archive, 0, stream, {20080308, false}, 300);
    SEQLIB_REQUIRE(played_2007.size() == 300 * 96 * 2 && played_2008.size() == played_2007.size());
    XSFLIB_CHECK(std::any_of(played_2007.begin(), played_2007.end(), [](float s) { return s != 0.0f; }));
    XSFLIB_CHECK(played_2008 != played_2007);
    hokuto::song::SoundPreview preview_2007(archive, 0, stream, ntdlib::SndBuild{20070608, false});
    hokuto::song::SoundPreview preview_2008(archive, 0, stream, ntdlib::SndBuild{20080308, false});
    XSFLIB_CHECK(preview_2007.SampleRate() == 32000);
    XSFLIB_CHECK(RenderPreview(preview_2007, 300 * 96) == played_2007);
    XSFLIB_CHECK(RenderPreview(preview_2008, 300 * 96) == played_2008);
    XSFLIB_CHECK(
        ErrorOf<std::runtime_error>(
            [&] { const hokuto::song::SoundPreview preview(archive, 0, std::vector<uint8_t>{1, 2, 3}, std::nullopt); })
            .starts_with("Can't read the stream of STRM_BGM. "));
    XSFLIB_CHECK(ErrorOf<std::runtime_error>([&] { const hokuto::song::SoundPreview preview(archive, 0); }) ==
                 "The file contains no stream for STRM_BGM.");
}

// The samples of `stream`'s channel `channel` in DSP-ADPCM from its loop's start to its end, decoded from the loop's
// predictor, scale and history with the Wii's sums: the samples of each later pass of the loop. Nothing when the stream
// lacks the channel, the channel's ADPCM state or the loop's first frame.
std::optional<std::vector<int16_t>> WiiLoopPass(const ntdlib::rsar::StreamFile& stream, std::size_t channel)
{
    if (channel >= stream.channels.size() || !stream.channels[channel].adpcm ||
        stream.channels[channel].samples.size() <= stream.loop_start / 14 * 8)
    {
        return std::nullopt;
    }

    const ntdlib::rsar::StreamFile::Channel& held = stream.channels[channel];
    ntdlib::wave::EncodedWave wave;
    wave.encoding = ntdlib::wave::Encoding::kDspAdpcm;
    wave.sum = ntdlib::wave::DspAdpcmSum::kWide;
    wave.loop_end = stream.sample_count - stream.loop_start;
    ntdlib::wave::EncodedChannel& encoded = wave.channels.emplace_back();
    encoded.data = std::span(held.samples).subspan(stream.loop_start / 14 * 8);
    encoded.coefs = &held.adpcm->coefs;
    encoded.yn1 = held.adpcm->loop_yn1;
    encoded.yn2 = held.adpcm->loop_yn2;

    return ntdlib::wave::DecodeWave(wave).channels.at(0);
}

// The properties of a Wii stream.
struct WiiStreamValuesFixture
{
    std::vector<uint8_t> pcm;
    ntdlib::rsar::StreamFile read;
    hokuto::edit::StreamValues values;
    std::vector<std::vector<int16_t>> channels;
};

// The fixture of the tests of a Wii stream's values.
WiiStreamValuesFixture MakeWiiStreamValuesFixture()
{
    namespace rsar = ntdlib::rsar;
    using hokuto::edit::StreamValues;
    const std::vector<uint8_t> pcm = WiiStreamOf(rsar::WaveEncoding::kPcm16);
    const rsar::StreamFile read = rsar::StreamFile::Read(pcm);
    const StreamValues values = hokuto::edit::StreamValuesOf(pcm);
    const std::vector<std::vector<int16_t>> channels = Channels();

    return {pcm, read, values, channels};
}

// A new rate, a loop from another block's start and no loop keep the samples and the tracks.
void TestWiiStreamKeptSamples()
{
    namespace rsar = ntdlib::rsar;
    using hokuto::edit::StreamValues;
    const WiiStreamValuesFixture fixture = MakeWiiStreamValuesFixture();
    const std::vector<uint8_t>& pcm = fixture.pcm;
    const rsar::StreamFile& read = fixture.read;
    const StreamValues& values = fixture.values;

    XSFLIB_CHECK(values.encoding == sar::WaveEncoding::kPcm16 && values.rate == kRate && values.loop &&
                 values.loop_start == 4096 && values.length == kLength);
    XSFLIB_CHECK(hokuto::edit::StreamWithValues(pcm, values) == pcm);

    StreamValues faster = values;
    faster.rate = 48000;
    const rsar::StreamFile fast = rsar::StreamFile::Read(hokuto::edit::StreamWithValues(pcm, faster));
    XSFLIB_CHECK(fast.sample_rate == 48000 && fast.loop_start == 4096 && fast.channels == read.channels &&
                 fast.tracks == read.tracks && fast.extended_tracks);
    StreamValues later = values;
    later.loop_start = 8192;
    const rsar::StreamFile looped = rsar::StreamFile::Read(hokuto::edit::StreamWithValues(pcm, later));
    XSFLIB_CHECK(looped.loop && looped.loop_start == 8192 && looped.channels == read.channels);
    StreamValues once = values;
    once.loop = false;
    once.loop_start = 0;
    const rsar::StreamFile unlooped = rsar::StreamFile::Read(hokuto::edit::StreamWithValues(pcm, once));
    XSFLIB_CHECK(!unlooped.loop && unlooped.channels == read.channels && unlooped.sample_count == kLength);
}

// A loop between blocks makes the stream again: the loop moves forward to the next block's start, and the samples
// between follow the loop's end. NW4R's tools lay a stream out that way.
void TestWiiStreamLoopBetweenBlocks()
{
    namespace rsar = ntdlib::rsar;
    using hokuto::edit::StreamValues;
    const WiiStreamValuesFixture fixture = MakeWiiStreamValuesFixture();
    const std::vector<uint8_t>& pcm = fixture.pcm;
    const rsar::StreamFile& read = fixture.read;
    const StreamValues& values = fixture.values;
    const std::vector<std::vector<int16_t>>& channels = fixture.channels;

    // A loop between blocks.
    StreamValues between = values;
    between.loop_start = 5000;
    const std::vector<uint8_t> moved_bytes = hokuto::edit::StreamWithValues(pcm, between);
    const rsar::StreamFile moved = rsar::StreamFile::Read(moved_bytes);
    XSFLIB_CHECK(moved.loop && moved.loop_start == 8192 && moved.sample_count == kLength + 8192 - 5000);
    XSFLIB_CHECK(moved.tracks == read.tracks && moved.extended_tracks);
    const sar::Pcm moved_pcm = hokuto::song::DecodeStreamFile(moved_bytes);
    SEQLIB_REQUIRE(moved_pcm.channels.size() == channels.size());
    for (std::size_t c = 0; c < channels.size(); c++)
    {
        SEQLIB_REQUIRE(moved_pcm.channels[c].size() >= kLength + 8192 - 5000 &&
                       moved_pcm.channels[c].size() >= channels[c].size());
        XSFLIB_CHECK(std::equal(channels[c].begin(), channels[c].end(), moved_pcm.channels[c].begin()));
        XSFLIB_CHECK(std::equal(channels[c].begin() + 5000, channels[c].begin() + 8192,
                                moved_pcm.channels[c].begin() + kLength));
    }
}

// In DSP-ADPCM, each channel's loop state holds the predictor, scale and history of the loop's start: each later pass
// decodes to the first pass's samples. A DSP-ADPCM stream without a loop holds the start's predictor and scale, and no
// history. An earlier end makes the stream again with fewer samples.
void TestWiiStreamAdpcmValues()
{
    namespace rsar = ntdlib::rsar;
    using hokuto::edit::StreamValues;
    const WiiStreamValuesFixture fixture = MakeWiiStreamValuesFixture();
    const std::vector<uint8_t>& pcm = fixture.pcm;
    const rsar::StreamFile& read = fixture.read;
    const StreamValues& values = fixture.values;
    const std::vector<std::vector<int16_t>>& channels = fixture.channels;

    // In DSP-ADPCM, from a block's start and without a loop.
    StreamValues adpcm_values = values;
    adpcm_values.encoding = sar::WaveEncoding::kDspAdpcm;
    adpcm_values.loop_start = 14336;
    const std::vector<uint8_t> adpcm_bytes = hokuto::edit::StreamWithValues(pcm, adpcm_values);
    const rsar::StreamFile adpcm = rsar::StreamFile::Read(adpcm_bytes);
    XSFLIB_CHECK(adpcm.encoding == rsar::WaveEncoding::kDspAdpcm && adpcm.loop_start == 14336 &&
                 adpcm.sample_count == kLength && adpcm.tracks == read.tracks);
    const sar::Pcm adpcm_pcm = hokuto::song::DecodeStreamFile(adpcm_bytes);
    for (std::size_t c = 0; c < channels.size(); c++)
    {
        const std::optional<std::vector<int16_t>> pass = WiiLoopPass(adpcm, c);
        SEQLIB_REQUIRE(pass && adpcm.channels[c].samples.size() > 14336 / 14 * 8 && c < adpcm_pcm.channels.size() &&
                       adpcm_pcm.channels[c].size() >= 14336 + pass->size());
        XSFLIB_CHECK(adpcm.channels[c].adpcm->loop_pred_scale == adpcm.channels[c].samples[14336 / 14 * 8]);
        XSFLIB_CHECK(std::equal(pass->begin(), pass->end(), adpcm_pcm.channels[c].begin() + 14336));
    }
    StreamValues adpcm_once = adpcm_values;
    adpcm_once.loop = false;
    adpcm_once.loop_start = 0;
    const rsar::StreamFile unlooped_adpcm =
        rsar::StreamFile::Read(hokuto::edit::StreamWithValues(adpcm_bytes, adpcm_once));
    XSFLIB_CHECK(!unlooped_adpcm.loop);
    for (std::size_t c = 0; c < channels.size(); c++)
    {
        SEQLIB_REQUIRE(c < unlooped_adpcm.channels.size() && unlooped_adpcm.channels[c].adpcm &&
                       c < adpcm.channels.size());
        const rsar::DspAdpcmInfo& info = *unlooped_adpcm.channels[c].adpcm;
        XSFLIB_CHECK(info.loop_pred_scale == info.pred_scale && info.loop_yn1 == 0 && info.loop_yn2 == 0);
        XSFLIB_CHECK(unlooped_adpcm.channels[c].samples == adpcm.channels[c].samples);
    }
    StreamValues shorter = values;
    shorter.length = 30000;
    const rsar::StreamFile cut = rsar::StreamFile::Read(hokuto::edit::StreamWithValues(pcm, shorter));
    XSFLIB_CHECK(cut.sample_count == 30000 && cut.loop_start == 4096);
}

// The errors for values that a Wii stream can't hold.
void TestWiiStreamValueErrors()
{
    using hokuto::edit::StreamValues;
    const WiiStreamValuesFixture fixture = MakeWiiStreamValuesFixture();
    const std::vector<uint8_t>& pcm = fixture.pcm;
    const StreamValues& values = fixture.values;

    // Values that a stream can't hold.
    StreamValues stopped = values;
    stopped.rate = 0;
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::StreamWithValues(pcm, stopped); }) ==
                 "The sample rate must be above 0.");
    StreamValues longer = values;
    longer.length = kLength + 1;
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::StreamWithValues(pcm, longer); }) ==
                 "The length must be from 1 to 40000 samples.");
    StreamValues past = values;
    past.loop_start = kLength;
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::StreamWithValues(pcm, past); }) ==
                 "The loop must start before the end of the stream.");
}

// The difference of `exported` from `bytes`, two archives of the same size: the offsets of the bytes that differ.
std::vector<std::size_t> Differences(const std::vector<uint8_t>& bytes, const std::vector<uint8_t>& exported)
{
    std::vector<std::size_t> offsets;
    for (std::size_t i = 0; i < std::min(bytes.size(), exported.size()); i++)
    {
        if (bytes[i] != exported[i])
        {
            offsets.push_back(i);
        }
    }

    return offsets;
}

// Replace… of a Wii stream sound's stream.
struct WiiReplacementFixture
{
    std::vector<uint8_t> stream;
    std::vector<uint8_t> bytes;
    File file;
    std::shared_ptr<const ntdlib::rsar::SoundArchive> archive;
    std::vector<ntdlib::rsar::StreamFile::Track> present;
    std::vector<uint8_t> adpcm;
};

// The fixture of the tests of Replace… in a Wii archive.
WiiReplacementFixture MakeWiiReplacementFixture()
{
    namespace rsar = ntdlib::rsar;
    const std::vector<uint8_t> stream = WiiStreamOf(rsar::WaveEncoding::kPcm16);
    const std::vector<uint8_t> bytes = WiiArchiveOf(static_cast<uint32_t>(stream.size()));
    const File file = hokuto::format::ImportWiiArchive(bytes, std::nullopt, {{0, stream}});
    const auto archive = hokuto::song::LoadWiiArchive(bytes, "test.brsar", ntdlib::rvl::snd::kRsarOptions);
    const std::vector<rsar::StreamFile::Track> kPresent = {{100, 40, {0, 1}}};
    const std::vector<uint8_t> adpcm = WiiStreamOf(rsar::WaveEncoding::kDspAdpcm);

    return {stream, bytes, file, archive, kPresent, adpcm};
}

// A stream file of the Wii goes in as it is, without the bytes past the size in its header. An export writes the new
// stream's size into the file table, and nothing else changes.
void TestWiiStreamFileReplacement()
{
    namespace rsar = ntdlib::rsar;
    const WiiReplacementFixture fixture = MakeWiiReplacementFixture();
    const std::vector<uint8_t>& stream = fixture.stream;
    const std::vector<uint8_t>& bytes = fixture.bytes;
    const File& file = fixture.file;
    const std::shared_ptr<const ntdlib::rsar::SoundArchive>& archive = fixture.archive;
    const std::vector<rsar::StreamFile::Track>& kPresent = fixture.present;
    const std::vector<uint8_t>& adpcm = fixture.adpcm;

    // The present stream, and what plays it. The entry of version 1.4 counts the channels.
    const StreamChanges changes = hokuto::edit::ChangesOfWiiStream(file, *archive, 0);
    XSFLIB_CHECK(changes.file == 0 && changes.sounds.empty() && changes.present && changes.tracks_in_file);
    XSFLIB_CHECK(changes.encoding == sar::WaveEncoding::kPcm16 && changes.channels == 2 && changes.rate == kRate &&
                 changes.loop && changes.loop_start == 4096 && changes.length == kLength && changes.from_game);
    XSFLIB_CHECK(changes.size == stream.size() && changes.table_channels == 2);
    XSFLIB_CHECK(changes.wii_tracks == kPresent);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::ChangesOfWiiStream(file, *archive, 1); }) ==
                 "Only streams in separate files beside the archive can be replaced.");

    // A stream file of the Wii, and the export.
    std::vector<uint8_t> padded = adpcm;
    padded.resize(padded.size() + 32, 0);
    const IncomingStream wii = hokuto::edit::ReadIncomingWiiStreamFile(padded, kPresent);
    XSFLIB_CHECK(wii.bytes == adpcm && wii.version == 0x0100 && !wii.converted && wii.channels == 2 &&
                 wii.rate == kRate && wii.loop && wii.loop_start == 14336 &&
                 wii.encoding == sar::WaveEncoding::kDspAdpcm);
    const File replaced = hokuto::edit::ReplaceStream(file, changes.file, wii.bytes);
    XSFLIB_CHECK(*replaced.native->streams.at(0).bytes == adpcm && !replaced.native->streams.at(0).from_game);
    const std::vector<uint8_t> exported = hokuto::format::ExportArchive(replaced);
    const rsar::ArchiveModel model = rsar::ReadArchiveModel(exported);
    SEQLIB_REQUIRE(model.files.size() > 2);
    XSFLIB_CHECK(exported.size() == bytes.size() && model.files[0].path_size == adpcm.size() &&
                 model.files[1].path_size == stream.size() && model.files[2].path_size == stream.size());
    const std::vector<std::size_t> differences = Differences(bytes, exported);
    XSFLIB_CHECK(!differences.empty() && differences.back() - differences.front() < 4);
    XSFLIB_CHECK(hokuto::format::Read(hokuto::format::Write(replaced)) == replaced);
    XSFLIB_CHECK(hokuto::format::ReadText(hokuto::format::WriteText(replaced)) == replaced);
    const File in_model = hokuto::format::InModelForm(replaced);
    XSFLIB_CHECK(hokuto::format::ExportArchive(in_model) == exported);
    XSFLIB_CHECK(hokuto::format::ExportArchive(hokuto::edit::ReplaceStream(replaced, changes.file, stream)) == bytes);
}

// A stream file of the Wii U goes in converted: the same samples in the Wii's blocks. A WAV file, or a stream without
// tracks, takes the present stream's tracks where its channels allow, and otherwise a track for each pair of channels.
void TestWiiReplacementTracks()
{
    namespace rsar = ntdlib::rsar;
    const WiiReplacementFixture fixture = MakeWiiReplacementFixture();
    const std::vector<rsar::StreamFile::Track>& kPresent = fixture.present;

    // A stream of the Wii U without tracks takes the present stream's track. One with tracks keeps them. A mono stream
    // takes a track of its one channel.
    const IncomingStream cafe =
        hokuto::edit::ReadIncomingWiiStreamFile(StreamOf(sar::Flavor::kCafe, sar::WaveEncoding::kPcm16), kPresent);
    XSFLIB_CHECK(cafe.converted && cafe.converted->flavor == sar::Flavor::kCafe && cafe.version == 0x0100);
    const rsar::StreamFile cafe_read = rsar::StreamFile::Read(cafe.bytes);
    XSFLIB_CHECK(cafe_read.tracks == kPresent && cafe_read.loop && cafe_read.loop_start == 4096);
    XSFLIB_CHECK(hokuto::song::DecodeStreamFile(cafe.bytes).channels ==
                 hokuto::song::DecodeStreamFile(StreamOf(sar::Flavor::kCafe, sar::WaveEncoding::kPcm16)).channels);
    sar::StreamFile tracked = sar::StreamFile::Read(StreamOf(sar::Flavor::kNx, sar::WaveEncoding::kPcm16));
    tracked.tracks = std::vector<sar::StreamFile::Track>{{.volume = 90, .pan = 20, .channels = {1}}};
    const IncomingStream switch_tracked = hokuto::edit::ReadIncomingWiiStreamFile(tracked.Write(), kPresent);
    XSFLIB_CHECK(rsar::StreamFile::Read(switch_tracked.bytes).tracks ==
                 std::vector<rsar::StreamFile::Track>({{90, 20, {1}}}));
    const std::vector<std::vector<int16_t>> mono = {Channels()[1]};
    const IncomingStream single = hokuto::edit::ReadIncomingWiiStreamFile(
        sar::MakeStream(sar::Flavor::kCafe, kVersion, sar::WaveEncoding::kPcm16, kRate, mono, std::nullopt).Write(),
        kPresent);
    XSFLIB_CHECK(rsar::StreamFile::Read(single.bytes).tracks == std::vector<rsar::StreamFile::Track>({{127, 64, {0}}}));
}

// A WAV file goes in as a stream that NW4R's tools would lay out, in 16-bit PCM or DSP-ADPCM. A stream that ntdlib
// can't read, such as one of another version, is refused with ntdlib's reason as a sentence. The browser plays the new
// stream as ntdlib's model run directly plays it.
void TestWiiWavReplacement()
{
    namespace rsar = ntdlib::rsar;
    const WiiReplacementFixture fixture = MakeWiiReplacementFixture();
    const std::vector<uint8_t>& bytes = fixture.bytes;
    const std::shared_ptr<const ntdlib::rsar::SoundArchive>& archive = fixture.archive;
    const std::vector<rsar::StreamFile::Track>& kPresent = fixture.present;
    const std::vector<uint8_t>& adpcm = fixture.adpcm;

    // A WAV file with a loop from sample 1000 to 30000, in 16-bit PCM and in DSP-ADPCM.
    seqlib::Pcm pcm;
    pcm.channels = Channels();
    pcm.loop = true;
    pcm.loop_start = 1000;
    pcm.loop_end = 30000;
    const std::vector<uint8_t> wav = seqlib::WavFile(pcm, kRate);
    const IncomingStream exact = hokuto::edit::ReadIncomingWiiStreamWav(wav, sar::WaveEncoding::kPcm16, kPresent);
    XSFLIB_CHECK(exact.frames == 30000 && exact.after_loop == kLength - 30000 && exact.loop &&
                 exact.loop_start == 1000 && exact.loop_moved == 4096 - 1000 && !exact.signal_to_noise);
    XSFLIB_CHECK(hokuto::edit::IsWiiStreamFile(exact.bytes) && !hokuto::edit::IsWiiStreamFile(wav) &&
                 !hokuto::edit::IsWiiStreamFile(StreamOf(sar::Flavor::kCafe)));
    const rsar::StreamFile exact_read = rsar::StreamFile::Read(exact.bytes);
    XSFLIB_CHECK(exact_read.tracks == kPresent && exact_read.loop_start == 4096 &&
                 exact_read.sample_count == 30000 + 4096 - 1000);
    const sar::Pcm decoded = hokuto::song::DecodeStreamFile(exact.bytes);
    SEQLIB_REQUIRE(decoded.channels.size() > 1 && decoded.channels[0].size() >= 30000 + 4096 - 1000 &&
                   decoded.channels[1].size() >= 30000);
    XSFLIB_CHECK(std::equal(pcm.channels[1].begin(), pcm.channels[1].begin() + 30000, decoded.channels[1].begin()));
    XSFLIB_CHECK(std::equal(pcm.channels[0].begin() + 1000, pcm.channels[0].begin() + 4096,
                            decoded.channels[0].begin() + 30000));
    const IncomingStream encoded = hokuto::edit::ReadIncomingWiiStreamWav(wav, sar::WaveEncoding::kDspAdpcm, kPresent);
    XSFLIB_CHECK(encoded.encoding == sar::WaveEncoding::kDspAdpcm && encoded.loop_moved == 14336 - 1000);
    XSFLIB_CHECK(encoded.signal_to_noise && *encoded.signal_to_noise > 20.0);
    const rsar::StreamFile encoded_read = rsar::StreamFile::Read(encoded.bytes);
    const sar::Pcm first = hokuto::song::DecodeStreamFile(encoded.bytes);
    SEQLIB_REQUIRE(first.channels.size() == 2);
    for (std::size_t c = 0; c < 2; c++)
    {
        const std::optional<std::vector<int16_t>> pass = WiiLoopPass(encoded_read, c);
        SEQLIB_REQUIRE(pass && first.channels[c].size() >= 14336 + pass->size());
        XSFLIB_CHECK(std::equal(pass->begin(), pass->end(), first.channels[c].begin() + 14336));
    }
    XSFLIB_CHECK(rsar::StreamFile::Read(
                     hokuto::edit::ReadIncomingWiiStreamWav(WavOf(1), sar::WaveEncoding::kPcm16, kPresent).bytes)
                     .tracks == std::vector<rsar::StreamFile::Track>({{127, 64, {0}}}));
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::ReadIncomingWiiStreamFile(bytes); }) ==
                 "It's a sound archive of the Wii, not a stream file.");

    // ntdlib's reason for a stream that it can't read follows as a sentence. The version at 0x06 becomes 1.1.
    std::vector<uint8_t> newer = adpcm;
    newer[0x07] = 0x01;
    const std::string unread = ErrorOf<std::invalid_argument>([&] { hokuto::edit::ReadIncomingWiiStreamFile(newer); });
    constexpr std::string_view kUnread = "Can't read the stream. ";
    XSFLIB_CHECK(unread.starts_with(kUnread) && unread.size() > kUnread.size() + 1 &&
                 std::isupper(static_cast<unsigned char>(unread[kUnread.size()])) && unread.ends_with('.'));

    // The browser plays the new stream as the model run directly plays it, on a build of 2008.
    const ntdlib::SndBuild kBuild{20080308, false};
    hokuto::song::SoundPreview preview(archive, 0, exact.bytes, kBuild);
    XSFLIB_CHECK(RenderPreview(preview, 300 * 96) == WiiStreamOnModel(*archive, 0, exact.bytes, kBuild, 300));
}

// File > Import > Stream… on a Wii file. The new sound copies the model's entry, and its entry names every channel and
// track of the stream: by a count from version 1.4 on, and by a bit for each before. Its stream goes beside the archive
// at stream/ and the name, with a number after a name that the file table names already. The file keeps the stream as
// Hokuto's in both forms, and an export writes the stream's size into the file table. The archive's other streams stay.
// The Import window plays the new sound as the model plays it from the new archive.
void TestWiiNewStreams()
{
    namespace rsar = ntdlib::rsar;
    const std::vector<uint8_t> stream = WiiStreamOf(rsar::WaveEncoding::kPcm16);
    const std::vector<uint8_t> adpcm = WiiStreamOf(rsar::WaveEncoding::kDspAdpcm);
    for (const uint16_t version : {uint16_t{0x0104}, uint16_t{0x0101}})
    {
        const std::vector<uint8_t> bytes = WiiArchiveOf(static_cast<uint32_t>(stream.size()), version);
        const File file = hokuto::format::ImportWiiArchive(bytes, std::nullopt, {{0, stream}});
        const auto archive = hokuto::song::LoadWiiArchive(bytes, "test.brsar", ntdlib::rvl::snd::kRsarOptions);
        const IncomingStream incoming = hokuto::edit::ReadIncomingWiiStreamFile(adpcm);
        const NewStream made = hokuto::edit::MakeNewWiiStream(*archive, 0, "STRM_NEW", incoming);
        XSFLIB_CHECK(made.path == "stream/STRM_NEW.brstm" && made.bytes == adpcm && made.file_tracks &&
                     made.tracks_in_file && made.wii_sound);
        const rsar::SoundInfo& entry = *made.wii_sound;
        XSFLIB_CHECK(entry.name == "STRM_NEW" && entry.type == rsar::SoundType::kStream && entry.file_id == 3 &&
                     entry.volume == 100 && entry.allocate_track_flags == 1);
        XSFLIB_CHECK(entry.allocate_channels == (version >= 0x0104 ? 2 : 3));
        XSFLIB_CHECK(hokuto::edit::MakeNewWiiStream(*archive, 0, "bgm", incoming).path == "stream/bgm_2.brstm");

        for (const File& form : {file, hokuto::format::InModelForm(file)})
        {
            const File added = hokuto::edit::AddWiiStreamSound(form, 0, "STRM_NEW", made);
            XSFLIB_CHECK(added.form == form.form && added.native->streams.size() == 2);
            XSFLIB_CHECK(*added.native->streams.at(0).bytes == stream && *added.native->streams.at(3).bytes == adpcm &&
                         !added.native->streams.at(3).from_game);
            XSFLIB_CHECK(hokuto::format::Read(hokuto::format::Write(added)) == added);
            XSFLIB_CHECK(hokuto::format::ReadText(hokuto::format::WriteText(added)) == added);
            const std::vector<uint8_t> exported = hokuto::format::ExportArchive(added);
            const auto with = hokuto::song::LoadWiiArchive(exported, "test.brsar", ntdlib::rvl::snd::kRsarOptions);
            SEQLIB_REQUIRE(with->Sounds().size() == 4);
            const rsar::SoundInfo& sound = with->Sounds()[3];
            XSFLIB_CHECK(sound.name == "STRM_NEW" && sound.type == rsar::SoundType::kStream && sound.file_id == 3 &&
                         sound.volume == 100 && sound.allocate_channels == entry.allocate_channels &&
                         sound.allocate_track_flags == 1);
            XSFLIB_CHECK(hokuto::song::StreamFileOf(*with, 3) == 3u &&
                         hokuto::song::StreamPath(*with, 3) == std::filesystem::path("stream/STRM_NEW.brstm"));
            XSFLIB_CHECK(rsar::ReadArchiveModel(exported).files.at(3).path_size == adpcm.size());

            // The Import window's Play, and the browser's after the import.
            const ntdlib::SndBuild kBuild{20080308, false};
            hokuto::song::SoundPreview before(archive, entry, adpcm, kBuild);
            hokuto::song::SoundPreview after(with, 3, adpcm, kBuild);
            XSFLIB_CHECK(RenderPreview(before, 300 * 96) == RenderPreview(after, 300 * 96));
        }

        // A name that the archive holds, a path that it names already, and a stream of too many tracks.
        XSFLIB_CHECK(
            ErrorOf<std::invalid_argument>([&] { hokuto::edit::AddWiiStreamSound(file, 0, "STRM_BGM", made); }) ==
            "The archive already has an item named STRM_BGM.");
        XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::AddWiiStreamSound(file, 0, "", made); }) ==
                     "Enter a name for the new sound.");
        NewStream taken = made;
        taken.path = "STREAM/bgm.brstm";
        XSFLIB_CHECK(
            ErrorOf<std::invalid_argument>([&] { hokuto::edit::AddWiiStreamSound(file, 0, "STRM_X", taken); }) ==
            "The archive's file table already contains STREAM/bgm.brstm.");
        rsar::StreamFile wide = rsar::StreamFile::Read(adpcm);
        wide.tracks.assign(9, {127, 64, {0}});
        const IncomingStream nine = hokuto::edit::ReadIncomingWiiStreamFile(wide.Write());
        XSFLIB_CHECK(
            ErrorOf<std::invalid_argument>([&] { hokuto::edit::MakeNewWiiStream(*archive, 0, "STRM_X", nine); }) ==
            "A Wii stream sound can have at most 8 tracks and 16 channels. The stream has 2 channels in 9 tracks.");
        XSFLIB_CHECK(
            ErrorOf<std::invalid_argument>([&] { hokuto::edit::MakeNewWiiStream(*archive, 3, "STRM_X", incoming); }) ==
            "The sound to copy doesn't exist in the archive.");
    }
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("wii_streams",
                                    []
                                    {
                                        TestWiiStreams();
                                        TestWiiStreamKeptSamples();
                                        TestWiiStreamLoopBetweenBlocks();
                                        TestWiiStreamAdpcmValues();
                                        TestWiiStreamValueErrors();
                                        TestWiiStreamFileReplacement();
                                        TestWiiReplacementTracks();
                                        TestWiiWavReplacement();
                                        TestWiiNewStreams();
                                    });
}
