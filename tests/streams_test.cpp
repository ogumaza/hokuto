// SPDX-License-Identifier: MIT

// Tests the streams beside an archive (src/song/streams.h), the replacement of a stream sound's stream
// (src/edit/streams.h) and new stream sounds (src/edit/new_items.h). A small archive of the Switch, and the same
// archive of the Wii U, name three stream files outside the archive: one in the folder stream, one at a path with a
// backslash, and one that leads out of the archive's folder. Two stream sounds play the first, one with a stereo track
// and one with two mono tracks. A third stream sound plays the second file and a fourth the third. ntdlib's MakeStream
// makes the streams from a ramp and a sine. The import reads them from a temporary folder. The new stream sounds go
// into the same archive laid out as NintendoWare's tools lay it out, with a wave sound after the stream sounds. ntdlib
// lays out 3DS archives of four versions from its model. Each holds a stream sound. Before CSAR 2.3.0.0, its stream
// file gives its tracks. ntdlib also lays out a Wii archive whose stream sounds play RSTM files beside it, built with
// ntdlib's builder of the Wii's streams. That archive's stream takes new values, replacements of the Wii, the Wii U and
// the Switch and of WAV files, and new stream sounds, in versions 1.1 and 1.4 of the archive. A mono WAV file in place
// of an AAC stream takes the channels of the sound's entry.

#include "edit/streams.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include "ntdlib/cafe/snd/engine.h"
#include "ntdlib/nx/atk/engine.h"
#include "ntdlib/rsar/archive_model.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/stream_file.h"
#include "ntdlib/rvl/snd/engine.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/archive_model.h"
#include "ntdlib/sar/decode.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/stream_file.h"
#include "ntdlib/sound_build.h"
#include "ntdlib/testing/rsar_streams.h"
#include "ntdlib/testing/sar_files.h"
#include "ntdlib/testing/sar_tools.h"
#include "ntdlib/wave/decode.h"
#include "ntdlib/wave/encode.h"
#include "seqlib/wav.h"
#include "xsflib/files.h"
#include "xsflib/testing/check.h"

#include "edit/new_items.h"
#include "format/archive.h"
#include "format/file.h"
#include "format/model.h"
#include "format/text.h"
#include "song/aac.h"
#include "song/archive.h"
#include "song/items.h"
#include "song/preview.h"
#include "song/streams.h"

using hokuto::edit::IncomingStream;
using hokuto::edit::NewStream;
using hokuto::edit::StreamChanges;
using hokuto::format::File;

namespace
{

namespace sar = ntdlib::sar;
namespace sar_files = ntdlib::testing::sar_files;
namespace sar_tools = ntdlib::testing::sar_tools;

// The archive's files and sounds.
constexpr uint32_t kSongFile = 0;
constexpr uint32_t kBackslashFile = 1;
constexpr uint32_t kOutsideFile = 2;
constexpr uint32_t kStereoSound = 0;
constexpr uint32_t kMonoSound = 1;
constexpr uint32_t kBackslashSound = 2;
constexpr uint32_t kOutsideSound = 3;
constexpr uint32_t kWaveSound = 4; // in the archive of the tools' layout

// The streams' rate, and the samples in each channel.
constexpr uint32_t kRate = 32000;
constexpr std::size_t kLength = 40000;

// The versions of the streams: 4.0.0, and 3.0.0 for most of the Wii U's games. On the 3DS, 2.0.0.0 and 2.3.1.0.
constexpr uint32_t kVersion = 0x00040000;
constexpr uint32_t kOldVersion = 0x00030000;
constexpr uint32_t kCtrOldestVersion = 0x02000000;
constexpr uint32_t kCtrNewestVersion = 0x02030100;

// The versions of 3DS archives from which the table of sounds gives a stream sound's tracks, and from which it gives
// the sound's pitch and sends too.
constexpr uint32_t kCtrTracksVersion = 0x02030000;
constexpr uint32_t kCtrFieldsVersion = 0x02030200;

// A folder in the system's temporary folder, deleted again with the object and the files in it.
class TemporaryFolder
{
public:
    TemporaryFolder()
        : path_(std::filesystem::temp_directory_path() /
                ("hokuto_streams_test_" + std::to_string(std::random_device{}())))
    {
        std::filesystem::create_directory(path_);
    }

    TemporaryFolder(const TemporaryFolder&) = delete;
    TemporaryFolder& operator=(const TemporaryFolder&) = delete;

    ~TemporaryFolder()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& Path() const
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

// What a function throws, or nothing.
template <typename Exception, typename Function>
std::string Message(Function function)
{
    try
    {
        function();
    }
    catch (const Exception& error)
    {
        return error.what();
    }

    return "";
}

// Two channels: a ramp, and a sine of 440 Hz.
std::vector<std::vector<int16_t>> Channels(std::size_t length = kLength)
{
    std::vector<std::vector<int16_t>> channels(2, std::vector<int16_t>(length));
    for (std::size_t i = 0; i < length; i++)
    {
        channels[0][i] = static_cast<int16_t>(static_cast<int>(i % 4000) * 8 - 16000);
        channels[1][i] =
            static_cast<int16_t>(std::lround(12000.0 * std::sin(2.0 * 3.14159265358979 * 440.0 * i / kRate)));
    }

    return channels;
}

// A stream of `flavor` and `version` that holds Channels(), with a loop from sample 1000.
std::vector<uint8_t> StreamOf(sar::Flavor flavor, sar::WaveEncoding encoding = sar::WaveEncoding::kDspAdpcm,
                              uint32_t version = kVersion)
{
    return sar::MakeStream(flavor, version, encoding, kRate, Channels(), 1000).Write();
}

// A stream sound's details: the channels that each track plays, and each track's pan. Each track's volume is 100. The
// sound's pitch is 1.5 and its main send 110.
sar::StreamSoundInfo Details(std::vector<std::vector<uint8_t>> tracks, std::vector<uint8_t> pans)
{
    sar::StreamSoundInfo info;
    info.channel_count = 2;
    info.pitch = 1.5f;
    info.main_send = 110;
    for (std::size_t t = 0; t < tracks.size(); t++)
    {
        info.valid_tracks = static_cast<uint16_t>(info.valid_tracks | 1u << t);
        sar::StreamTrackInfo track;
        track.channels = std::move(tracks[t]);
        track.pan = pans[t];
        track.volume = 100;
        info.tracks.push_back(std::move(track));
    }

    return info;
}

// The archive of `flavor`, laid out as ntdlib's tests lay it out or, with `tools`, as NintendoWare's tools do. That one
// holds a wave sound after the stream sounds.
std::vector<uint8_t> ArchiveOf(sar::Flavor flavor, bool tools = false)
{
    sar_files::ArchiveSpec spec;
    spec.files = {{{}, "stream/STRM_SONG.bfstm"}, {{}, "stream\\STRM_BACK.bfstm"}, {{}, "../STRM_OUT.bfstm"}};
    spec.players = {{"PLAYER", 1}};
    spec.sounds = {{.name = "STRM_SONG",
                    .kind = sar_files::SoundSpec::kStream,
                    .file = kSongFile,
                    .stream = Details({{0, 1}}, {64})},
                   {.name = "STRM_SONG_MONO",
                    .kind = sar_files::SoundSpec::kStream,
                    .file = kSongFile,
                    .stream = Details({{0}, {1}}, {0, 127})},
                   {.name = "STRM_BACK",
                    .kind = sar_files::SoundSpec::kStream,
                    .file = kBackslashFile,
                    .stream = Details({{0, 1}}, {64})},
                   {.name = "STRM_OUT",
                    .kind = sar_files::SoundSpec::kStream,
                    .file = kOutsideFile,
                    .stream = Details({{0, 1}}, {64})}};
    if (!tools)
    {
        return sar_files::Archive(flavor, spec);
    }

    spec.files.push_back({sar_files::WaveSoundFile(flavor, {}), "", false});
    spec.sounds.push_back({.name = "WSD_A", .kind = sar_files::SoundSpec::kWaveSound, .file = 3, .volume = 70});

    return sar_tools::Archive(flavor, spec);
}

// A WAV file of `count` channels: Channels() over and over.
std::vector<uint8_t> WavOf(std::size_t count)
{
    seqlib::Pcm pcm;
    const std::vector<std::vector<int16_t>> channels = Channels(20000);
    for (std::size_t c = 0; c < count; c++)
    {
        pcm.channels.push_back(channels[c % 2]);
    }

    return seqlib::WavFile(pcm, kRate);
}

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
    XSFLIB_CHECK(Message<std::runtime_error>([&] { hokuto::song::StreamPaths(*archive, kOutside); }) ==
                 "The archive's file table gives stream file 2 no path in the archive's folder.");
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
    XSFLIB_CHECK(Message<std::runtime_error>(
                     [&]
                     {
                         hokuto::song::ConvertItemFile(stream, hokuto::song::ItemKind::kStream, sar::Flavor::kCtr,
                                                       flavor, versions);
                     }) == "It isn't a BCSTM file of the 3DS.");
    XSFLIB_CHECK(Message<std::runtime_error>([&] { hokuto::song::ConvertStreamFile(stream, other, 0x00020000); })
                     .starts_with("Hokuto can't convert the stream. "));
}

// The first `frames` stereo frames of a preview, or fewer at its end, in blocks of 256.
std::vector<float> RenderPreview(hokuto::audio::Renderer& preview, std::size_t frames)
{
    constexpr std::size_t kBlock = 256;
    std::vector<float> samples(frames * 2);
    std::size_t done = 0;
    while (done < frames)
    {
        const std::size_t rendered = preview.Render(samples.data() + done * 2, std::min(kBlock, frames - done));
        done += rendered;
        if (rendered == 0)
        {
            break;
        }
    }
    samples.resize(done * 2);

    return samples;
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
    XSFLIB_CHECK(Message<std::runtime_error>(
                     [&] { const hokuto::song::SoundPreview preview(archive, kStereoSound, StreamOf(other)); }) ==
                 "The stream of STRM_SONG is a stream of another console.");
    XSFLIB_CHECK(Message<std::runtime_error>([&] { const hokuto::song::SoundPreview preview(archive, kStereoSound); })
                     .starts_with("Hokuto can't read the stream of STRM_SONG. "));
    sar::SoundInfo silent = once;
    silent.stream.valid_tracks = 0;
    XSFLIB_CHECK(
        Message<std::runtime_error>([&] { const hokuto::song::SoundPreview preview(archive, silent, stream); }) ==
        "STRM_SONG plays no track when it starts.");
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
        Message<std::invalid_argument>([&] { hokuto::edit::ChangesOfStream(file, *archive, kOutsideSound); }) ==
        "Hokuto replaces only streams beside the archive, in its folder.");

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
        Message<std::invalid_argument>([&] { hokuto::edit::ReadIncomingStreamFile(bytes, flavor, kVersion); }) ==
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
    XSFLIB_CHECK(Message<std::invalid_argument>([&] { hokuto::edit::ReadIncomingStreamWav(stream, flavor, kVersion); })
                     .starts_with("Hokuto can't read it as a WAV file"));
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
void TestNewStreams(sar::Flavor flavor)
{
    const std::vector<uint8_t> bytes = ArchiveOf(flavor, true);
    const std::vector<uint8_t> stream = StreamOf(flavor);
    const File file = hokuto::format::ImportArchive(bytes, flavor, {}, {{kSongFile, stream}});
    const auto archive = hokuto::song::LoadArchive(bytes, "test.bfsar", std::vector<std::vector<uint8_t>>{});

    // Three channels make a stereo track and a mono one. The first takes the settings of the model's track, and the
    // second those of a track that sets none.
    const IncomingStream three = hokuto::edit::ReadIncomingStreamWav(WavOf(3), flavor, kVersion);
    const NewStream made = hokuto::edit::MakeNewStream(*archive, kStereoSound, "STRM_NEW", three);
    const sar::StreamSoundInfo& details = made.details;
    XSFLIB_CHECK(made.path == "stream/STRM_NEW.dspadpcm.bfstm" && made.bytes == three.bytes);
    XSFLIB_CHECK(details.channel_count == 3 && details.valid_tracks == 3 && details.tracks.size() == 2);
    XSFLIB_CHECK(details.tracks.size() == 2 && details.tracks[0].channels == std::vector<uint8_t>({0, 1}) &&
                 details.tracks[1].channels == std::vector<uint8_t>({2}));
    XSFLIB_CHECK(details.tracks.size() == 2 && details.tracks[0].volume == 100 && details.tracks[1].volume == 127 &&
                 details.tracks[1].pan == 64 && details.tracks[1].main_send == 127);
    XSFLIB_CHECK(details.pitch == 1.5f && details.main_send == 110);

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

    // A stream file's tracks give the sound's tracks, with their volumes and pans.
    sar::StreamFile tracked = sar::StreamFile::Read(stream);
    tracked.tracks = std::vector<sar::StreamFile::Track>{{.volume = 90, .pan = 0, .channels = {0}},
                                                         {.volume = 80, .pan = 127, .channels = {1}}};
    const IncomingStream from_file = hokuto::edit::ReadIncomingStreamFile(tracked.Write(), flavor, kVersion);
    const NewStream tracks = hokuto::edit::MakeNewStream(*archive, kStereoSound, "STRM_TRACKS", from_file);
    XSFLIB_CHECK(tracks.details.tracks.size() == 2 && tracks.details.tracks[0].volume == 90 &&
                 tracks.details.tracks[1].pan == 127 && tracks.details.tracks[1].channels == std::vector<uint8_t>({1}));

    // A taken name, an empty one, a path that the file table names, a model past the table, more than 8 tracks, and a
    // file without the archive's tables.
    NewStream taken = made;
    taken.path = "stream/STRM_SONG.bfstm";
    File bare = file;
    bare.native.Edit([](hokuto::format::NativeArchive& native) { native.tables.reset(); });
    XSFLIB_CHECK(
        Message<std::invalid_argument>([&] { hokuto::edit::AddStreamSound(file, kStereoSound, "STRM_SONG", made); }) ==
        "The archive holds the name STRM_SONG already.");
    XSFLIB_CHECK(
        !Message<std::invalid_argument>([&] { hokuto::edit::AddStreamSound(file, kStereoSound, "", made); }).empty());
    XSFLIB_CHECK(
        Message<std::invalid_argument>([&] { hokuto::edit::AddStreamSound(file, kStereoSound, "STRM_X", taken); }) ==
        "The archive's file table names stream/STRM_SONG.bfstm already.");
    XSFLIB_CHECK(
        !Message<std::invalid_argument>([&] { hokuto::edit::AddStreamSound(file, 9, "STRM_X", made); }).empty());
    XSFLIB_CHECK(
        !Message<std::invalid_argument>([&] { hokuto::edit::AddStreamSound(bare, 0, "STRM_X", made); }).empty());
    const IncomingStream seventeen =
        hokuto::edit::ReadIncomingStreamWav(WavOf(17), flavor, kVersion, sar::WaveEncoding::kPcm16);
    XSFLIB_CHECK(
        Message<std::invalid_argument>([&] { hokuto::edit::MakeNewStream(*archive, 0, "STRM_X", seventeen); }) ==
        "A stream sound plays 8 tracks at most. The stream holds 17 channels.");
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
    XSFLIB_CHECK(Message<std::invalid_argument>([&] { hokuto::edit::StreamWithValues(stream, silent); }) ==
                 "A stream plays at a rate above 0.");
    XSFLIB_CHECK(Message<std::invalid_argument>([&] { hokuto::edit::StreamWithValues(stream, longer); }) ==
                 "The stream holds 53336 samples.");
    XSFLIB_CHECK(Message<std::invalid_argument>([&] { hokuto::edit::StreamWithValues(stream, late); }) ==
                 "The loop starts at the stream's end or after it.");
}

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
void TestCtrStreams()
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

// New stream sounds in 3DS archives of four versions. Before 2.3.0.0, the sound plays the tracks of its stream file,
// and a stream without tracks takes those of the pairs of its channels. From 2.3.0.0 on, the archive's table gives the
// tracks, as on the Wii U and the Switch. The 3DS's player plays 4 tracks of 2 channels at most, and 8 channels.
void TestCtrNewStreams()
{
    sar::StreamFile present =
        sar::MakeStream(sar::Flavor::kCtr, kCtrOldestVersion, sar::WaveEncoding::kDspAdpcm, kRate, Channels(), 1000);
    present.tracks = std::vector<sar::StreamFile::Track>{{.volume = 100, .pan = 40, .channels = {0, 1}}};
    const std::vector<uint8_t> stream = present.Write();
    for (const uint32_t version : {kCtrOldestVersion, 0x02020000u, kCtrTracksVersion, kCtrFieldsVersion})
    {
        const std::vector<uint8_t> bytes = CtrArchive(version);
        const auto archive = hokuto::song::LoadArchive(bytes, "test.bcsar", std::vector<std::vector<uint8_t>>{});
        const File file = hokuto::format::ImportArchive(bytes, sar::Flavor::kCtr, {}, {{0, stream}});
        const bool in_file = version < kCtrTracksVersion;

        // Three channels make a stereo track and a mono one. Where the stream file holds the tracks, they take full
        // volume and the centre. Otherwise the first takes the settings of the model's track, and the second those of a
        // track that sets none.
        const IncomingStream three =
            hokuto::edit::ReadIncomingStreamWav(WavOf(3), sar::Flavor::kCtr, kCtrOldestVersion);
        const NewStream made = hokuto::edit::MakeNewStream(*archive, 0, "STRM_NEW", three);
        const sar::StreamSoundInfo& details = made.details;
        XSFLIB_CHECK(made.path == "stream/STRM_NEW.dspadpcm.bcstm" && made.tracks_in_file == in_file &&
                     !made.file_tracks);
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

        // The new sound holds the details, and plays the stream from its new file. The file keeps the stream as
        // Hokuto's, and the game's stream as the game's.
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
        sar::StreamFile tracked =
            sar::StreamFile::Read(hokuto::edit::ReadIncomingStreamWav(WavOf(2), sar::Flavor::kCtr, kCtrOldestVersion,
                                                                      sar::WaveEncoding::kPcm16)
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

    // More than 4 tracks, a track of more than 2 channels, and more than 8 channels.
    const auto archive = hokuto::song::LoadArchive(CtrArchive(), "test.bcsar", std::vector<std::vector<uint8_t>>{});
    const IncomingStream nine =
        hokuto::edit::ReadIncomingStreamWav(WavOf(9), sar::Flavor::kCtr, kCtrOldestVersion, sar::WaveEncoding::kPcm16);
    XSFLIB_CHECK(Message<std::invalid_argument>([&] { hokuto::edit::MakeNewStream(*archive, 0, "STRM_X", nine); }) ==
                 "A stream sound of the 3DS plays 4 tracks at most. The stream holds 9 channels.");
    sar::StreamFile wide = sar::StreamFile::Read(nine.bytes);
    wide.tracks = std::vector<sar::StreamFile::Track>{{.channels = {0, 1, 2}}};
    const IncomingStream three_wide =
        hokuto::edit::ReadIncomingStreamFile(wide.Write(), sar::Flavor::kCtr, kCtrOldestVersion);
    XSFLIB_CHECK(
        Message<std::invalid_argument>([&] { hokuto::edit::MakeNewStream(*archive, 0, "STRM_X", three_wide); }) ==
        "A track of a stream sound of the 3DS plays 2 channels at most. Track 0 of the stream plays 3.");
    wide.tracks = std::vector<sar::StreamFile::Track>{{.channels = {0, 1}}};
    const IncomingStream nine_wide =
        hokuto::edit::ReadIncomingStreamFile(wide.Write(), sar::Flavor::kCtr, kCtrOldestVersion);
    XSFLIB_CHECK(
        Message<std::invalid_argument>([&] { hokuto::edit::MakeNewStream(*archive, 0, "STRM_X", nine_wide); }) ==
        "A stream sound of the 3DS plays 8 channels at most. The stream holds 9 channels in 1 track.");
}

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
        Message<std::runtime_error>(
            [&] { const hokuto::song::SoundPreview preview(archive, 0, std::vector<uint8_t>{1, 2, 3}, std::nullopt); })
            .starts_with("Hokuto can't read the stream of STRM_BGM. "));
    XSFLIB_CHECK(Message<std::runtime_error>([&] { const hokuto::song::SoundPreview preview(archive, 0); }) ==
                 "The file holds no stream of STRM_BGM.");
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

// The properties of a Wii stream. A new rate, a loop from another block's start and no loop keep the samples and the
// tracks. A loop between blocks makes the stream again: the loop moves forward to the next block's start, and the
// samples between follow the loop's end. NW4R's tools lay a stream out that way. In DSP-ADPCM, each channel's loop
// state holds the predictor, scale and history of the loop's start: each later pass decodes to the first pass's
// samples. A DSP-ADPCM stream without a loop holds the start's predictor and scale, and no history. An earlier end
// makes the stream again with fewer samples.
void TestWiiStreamValues()
{
    namespace rsar = ntdlib::rsar;
    using hokuto::edit::StreamValues;
    const std::vector<uint8_t> pcm = WiiStreamOf(rsar::WaveEncoding::kPcm16);
    const rsar::StreamFile read = rsar::StreamFile::Read(pcm);
    const StreamValues values = hokuto::edit::StreamValuesOf(pcm);
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

    // A loop between blocks.
    StreamValues between = values;
    between.loop_start = 5000;
    const std::vector<uint8_t> moved_bytes = hokuto::edit::StreamWithValues(pcm, between);
    const rsar::StreamFile moved = rsar::StreamFile::Read(moved_bytes);
    XSFLIB_CHECK(moved.loop && moved.loop_start == 8192 && moved.sample_count == kLength + 8192 - 5000);
    XSFLIB_CHECK(moved.tracks == read.tracks && moved.extended_tracks);
    const sar::Pcm moved_pcm = hokuto::song::DecodeStreamFile(moved_bytes);
    const std::vector<std::vector<int16_t>> channels = Channels();
    SEQLIB_REQUIRE(moved_pcm.channels.size() == channels.size());
    for (std::size_t c = 0; c < channels.size(); c++)
    {
        SEQLIB_REQUIRE(moved_pcm.channels[c].size() >= kLength + 8192 - 5000 &&
                       moved_pcm.channels[c].size() >= channels[c].size());
        XSFLIB_CHECK(std::equal(channels[c].begin(), channels[c].end(), moved_pcm.channels[c].begin()));
        XSFLIB_CHECK(std::equal(channels[c].begin() + 5000, channels[c].begin() + 8192,
                                moved_pcm.channels[c].begin() + kLength));
    }

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

    // Values that a stream can't hold.
    StreamValues stopped = values;
    stopped.rate = 0;
    XSFLIB_CHECK(Message<std::invalid_argument>([&] { hokuto::edit::StreamWithValues(pcm, stopped); }) ==
                 "A stream plays at a rate above 0.");
    StreamValues longer = values;
    longer.length = kLength + 1;
    XSFLIB_CHECK(Message<std::invalid_argument>([&] { hokuto::edit::StreamWithValues(pcm, longer); }) ==
                 "The stream holds 40000 samples.");
    StreamValues past = values;
    past.loop_start = kLength;
    XSFLIB_CHECK(Message<std::invalid_argument>([&] { hokuto::edit::StreamWithValues(pcm, past); }) ==
                 "The loop starts at the stream's end or after it.");
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

// Replace… of a Wii stream sound's stream. A stream file of the Wii goes in as it is, without the bytes past the size
// in its header. A stream file of the Wii U goes in converted: the same samples in the Wii's blocks. A WAV file goes in
// as a stream that NW4R's tools would lay out, in 16-bit PCM or DSP-ADPCM. A WAV file, or a stream without tracks,
// takes the present stream's tracks where its channels allow, and otherwise a track for each pair of channels. A stream
// that ntdlib can't read, such as one of another version, is refused with ntdlib's reason as a sentence. An export
// writes the new stream's size into the file table, and nothing else changes. The browser plays the new stream as
// ntdlib's model run directly plays it.
void TestWiiStreamReplacement()
{
    namespace rsar = ntdlib::rsar;
    const std::vector<uint8_t> stream = WiiStreamOf(rsar::WaveEncoding::kPcm16);
    const std::vector<uint8_t> bytes = WiiArchiveOf(static_cast<uint32_t>(stream.size()));
    const File file = hokuto::format::ImportWiiArchive(bytes, std::nullopt, {{0, stream}});
    const auto archive = hokuto::song::LoadWiiArchive(bytes, "test.brsar", ntdlib::rvl::snd::kRsarOptions);

    // The present stream, and what plays it. The entry of version 1.4 counts the channels.
    const StreamChanges changes = hokuto::edit::ChangesOfWiiStream(file, *archive, 0);
    XSFLIB_CHECK(changes.file == 0 && changes.sounds.empty() && changes.present && changes.tracks_in_file);
    XSFLIB_CHECK(changes.encoding == sar::WaveEncoding::kPcm16 && changes.channels == 2 && changes.rate == kRate &&
                 changes.loop && changes.loop_start == 4096 && changes.length == kLength && changes.from_game);
    XSFLIB_CHECK(changes.size == stream.size() && changes.table_channels == 2);
    const std::vector<rsar::StreamFile::Track> kPresent = {{100, 40, {0, 1}}};
    XSFLIB_CHECK(changes.wii_tracks == kPresent);
    XSFLIB_CHECK(Message<std::invalid_argument>([&] { hokuto::edit::ChangesOfWiiStream(file, *archive, 1); }) ==
                 "Hokuto replaces only streams beside the archive, in its folder.");

    // A stream file of the Wii, and the export.
    const std::vector<uint8_t> adpcm = WiiStreamOf(rsar::WaveEncoding::kDspAdpcm);
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
    XSFLIB_CHECK(Message<std::invalid_argument>([&] { hokuto::edit::ReadIncomingWiiStreamFile(bytes); }) ==
                 "It's a sound archive of the Wii, not a stream file.");

    // ntdlib's reason for a stream that it can't read follows as a sentence. The version at 0x06 becomes 1.1.
    std::vector<uint8_t> newer = adpcm;
    newer[0x07] = 0x01;
    const std::string unread = Message<std::invalid_argument>([&] { hokuto::edit::ReadIncomingWiiStreamFile(newer); });
    constexpr std::string_view kUnread = "Hokuto can't read the stream. ";
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
            Message<std::invalid_argument>([&] { hokuto::edit::AddWiiStreamSound(file, 0, "STRM_BGM", made); }) ==
            "The archive holds the name STRM_BGM already.");
        XSFLIB_CHECK(Message<std::invalid_argument>([&] { hokuto::edit::AddWiiStreamSound(file, 0, "", made); }) ==
                     "Give the new sound a name.");
        NewStream taken = made;
        taken.path = "STREAM/bgm.brstm";
        XSFLIB_CHECK(
            Message<std::invalid_argument>([&] { hokuto::edit::AddWiiStreamSound(file, 0, "STRM_X", taken); }) ==
            "The archive's file table names STREAM/bgm.brstm already.");
        rsar::StreamFile wide = rsar::StreamFile::Read(adpcm);
        wide.tracks.assign(9, {127, 64, {0}});
        const IncomingStream nine = hokuto::edit::ReadIncomingWiiStreamFile(wide.Write());
        XSFLIB_CHECK(
            Message<std::invalid_argument>([&] { hokuto::edit::MakeNewWiiStream(*archive, 0, "STRM_X", nine); }) ==
            "A stream sound of the Wii plays 8 tracks and 16 channels at most. The stream holds 2 channels "
            "in 9 tracks.");
        XSFLIB_CHECK(
            Message<std::invalid_argument>([&] { hokuto::edit::MakeNewWiiStream(*archive, 3, "STRM_X", incoming); }) ==
            "The sound to copy lies past the archive's sounds.");
    }
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
    TestPaths();
    TestAacChannels();
    TestCtrStreams();
    TestCtrNewStreams();
    TestWiiStreams();
    TestWiiStreamValues();
    TestWiiStreamReplacement();
    TestWiiNewStreams();
    for (const sar::Flavor flavor : {sar::Flavor::kNx, sar::Flavor::kCafe})
    {
        TestImport(flavor);
        TestConversions(flavor);
        TestStreamPreview(flavor);
        TestReplacement(flavor);
        TestVersions(flavor);
        TestNewStreams(flavor);
        TestStreamValues(flavor);
    }

    return xsflib::testing::Summary("streams");
}
