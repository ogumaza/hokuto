// SPDX-License-Identifier: MIT

// Tests the edits of a DS stream (edit/ds_streams.h). ntdlib's builders make a small archive with a stream. The SDK's
// tools could have laid the archive out the same way. A stream file's layout comes from the format: a part of IMA-ADPCM
// starts with 4 bytes of the decoder's state, a block of 512 bytes contains 1,016 samples, and the DATA block ends on a
// multiple of 4 bytes. A stream of 32,728 Hz uses a timer of 16.

#include "edit/ds_streams.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

#include "ntdlib/sdat/decode.h"
#include "ntdlib/sdat/files.h"
#include "ntdlib/sdat/formats.h"
#include "ntdlib/sdat/layout.h"
#include "seqlib/pcm.h"
#include "seqlib/wav.h"
#include "xsflib/testing/check.h"

#include "edit/ds_new_items.h"
#include "format/archive.h"
#include "format/file.h"
#include "format/text.h"
#include "song/ds.h"

using hokuto::format::File;
using xsflib::testing::ErrorOf;

namespace
{

namespace edit = hokuto::edit;
namespace sdat = ntdlib::sdat;
namespace song = hokuto::song;

constexpr uint32_t kRate = 32728;

// A sine and a slower one, `samples` of each.
std::vector<std::vector<int16_t>> Stereo(std::size_t samples)
{
    std::vector<std::vector<int16_t>> channels(2, std::vector<int16_t>(samples));
    for (std::size_t i = 0; i < samples; i++)
    {
        channels[0][i] = static_cast<int16_t>(9000.0 * std::sin(static_cast<double>(i) * 0.05));
        channels[1][i] = static_cast<int16_t>(9000.0 * std::sin(static_cast<double>(i) * 0.013));
    }

    return channels;
}

// An archive with a stream player and a stream, laid out by ntdlib from a model.
std::vector<uint8_t> MakeArchive(std::span<const uint8_t> stream, bool names)
{
    sdat::ArchiveModel model;
    if (names)
    {
        sdat::Symbols symbols;
        symbols.names[6] = {"PLAYER_STRM"};
        symbols.names[7] = {"STRM_ONE"};
        model.symbols = symbols;
    }
    sdat::StreamPlayerInfo player;
    player.channel_count = 2;
    model.stream_players.emplace_back(player);
    sdat::StreamInfo info;
    info.file_id = 0;
    info.volume = 120;
    info.player_priority = 60;
    info.player = 0;
    info.flags = 0;
    model.streams.emplace_back(info);
    model.files.emplace_back(stream.begin(), stream.end());

    return sdat::BuildArchive(model);
}

std::shared_ptr<const song::DsArchive> Load(const File& file)
{
    return song::LoadDsArchive(hokuto::format::ExportArchive(file), {});
}

void CheckForms(const File& file)
{
    const std::vector<uint8_t> exported = hokuto::format::ExportArchive(file);
    XSFLIB_CHECK(hokuto::format::ExportArchive(hokuto::format::Read(hokuto::format::Write(file))) == exported);
    XSFLIB_CHECK(hokuto::format::ExportArchive(hokuto::format::ReadText(hokuto::format::WriteText(file))) == exported);
}

// A stream of real timing: the rate field sets the rate, and a timer of 16 means 32,728 Hz.
void TestRate()
{
    const std::vector<std::vector<int16_t>> channels = Stereo(100);
    std::vector<uint8_t> bytes = edit::EncodeDsStream(channels, kRate, sdat::WaveEncoding::kPcm16, std::nullopt);
    const sdat::Stream parsed = sdat::Stream::Parse(bytes);
    XSFLIB_CHECK(parsed.sample_rate == kRate && parsed.timer == 16);
    XSFLIB_CHECK(song::DsStreamPcmOf(bytes).sample_rate == kRate);

    // The rate comes from the field, whatever value the timer has.
    bytes[0x1E] = 200;
    XSFLIB_CHECK(song::DsStreamPcmOf(bytes).sample_rate == kRate);
}

// The tools' layout of each encoding.
void TestLayout()
{
    const std::vector<std::vector<int16_t>> channels = Stereo(1016 * 2 + 173);
    const std::vector<uint8_t> ima = edit::EncodeDsStream(channels, kRate, sdat::WaveEncoding::kImaAdpcm, std::nullopt);
    const sdat::Stream stream = sdat::Stream::Parse(ima);
    XSFLIB_CHECK(stream.encoding == sdat::WaveEncoding::kImaAdpcm && stream.loop == 0 && stream.channels == 2);
    XSFLIB_CHECK(stream.samples == 1016 * 2 + 173 && stream.loop_start == 0);
    XSFLIB_CHECK(stream.blocks == 3 && stream.block_bytes == 0x200 && stream.block_samples == 1016);
    XSFLIB_CHECK(stream.last_block_bytes == 91 && stream.last_block_samples == 173);
    XSFLIB_CHECK(stream.data_offset == 0x68);

    // 2 blocks of 512 bytes and one of 91, for each channel, end on a multiple of 4 bytes.
    XSFLIB_CHECK(ima.size() == 0x68 + 2 * (2 * 512 + 91) + 2);
    XSFLIB_CHECK(sdat::StreamFile(stream) == ima);

    // The state of a block matches the decoding of the channel there.
    for (uint32_t channel = 0; channel < 2; channel++)
    {
        const std::vector<int16_t> whole = sdat::DecodeStream(stream, channel);
        XSFLIB_CHECK(whole.size() == channels[channel].size());
        double worst = 0.0;
        for (std::size_t i = 0; i < whole.size(); i++)
        {
            worst = std::max(worst, std::abs(double(whole[i]) - double(channels[channel][i])));
        }
        XSFLIB_CHECK(worst < 800.0);
    }

    // 16-bit PCM keeps the samples, in blocks of 256, and PCM8 their high bytes, in blocks of 512.
    const sdat::Stream pcm16_stream = [&]
    {
        static std::vector<uint8_t> bytes;
        bytes = edit::EncodeDsStream(channels, kRate, sdat::WaveEncoding::kPcm16, std::nullopt);
        return sdat::Stream::Parse(bytes);
    }();
    XSFLIB_CHECK(pcm16_stream.block_samples == 256 && pcm16_stream.block_bytes == 512);
    XSFLIB_CHECK(sdat::DecodeStream(pcm16_stream, 1) == channels[1]);
    const sdat::Stream pcm8_stream = [&]
    {
        static std::vector<uint8_t> bytes;
        bytes = edit::EncodeDsStream(channels, kRate, sdat::WaveEncoding::kPcm8, std::nullopt);
        return sdat::Stream::Parse(bytes);
    }();
    XSFLIB_CHECK(pcm8_stream.block_samples == 512 && pcm8_stream.block_bytes == 512);
    XSFLIB_CHECK(sdat::DecodeStream(pcm8_stream, 0).size() == channels[0].size());

    // A stream has one or two channels of the same length, at a rate from 256 to 65,535 Hz.
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>(
                     [&]
                     {
                         const std::vector<std::vector<int16_t>> three(3, channels[0]);
                         edit::EncodeDsStream(three, kRate, sdat::WaveEncoding::kPcm16, std::nullopt);
                     }) == "A DS stream must have one or two channels.");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>(
                     [&] { edit::EncodeDsStream(channels, 70000, sdat::WaveEncoding::kPcm16, std::nullopt); }) ==
                 "The sample rate of a DS stream must be from 256 to 65,535 Hz.");
}

// A loop moves forward to a block's start, and the samples between repeat after the end.
void TestLoop()
{
    const std::vector<std::vector<int16_t>> channels = Stereo(2000);
    const std::vector<uint8_t> bytes = edit::EncodeDsStream(channels, kRate, sdat::WaveEncoding::kPcm16, 300);
    const sdat::Stream stream = sdat::Stream::Parse(bytes);
    XSFLIB_CHECK(stream.loop == 1 && stream.loop_start == 512);

    // The loop keeps its length of 1,700 samples.
    XSFLIB_CHECK(stream.samples == 512 + 1700);
    const std::vector<int16_t> decoded = sdat::DecodeStream(stream, 0);
    for (std::size_t i = 0; i < 2000; i++)
    {
        XSFLIB_CHECK(decoded[i] == channels[0][i]);
    }
    for (std::size_t i = 2000; i < decoded.size(); i++)
    {
        XSFLIB_CHECK(decoded[i] == channels[0][300 + (i - 2000)]);
    }

    // A loop at a block stays, and adds no samples.
    const std::vector<uint8_t> at_block = edit::EncodeDsStream(channels, kRate, sdat::WaveEncoding::kImaAdpcm, 1016);
    const sdat::Stream on_block = sdat::Stream::Parse(at_block);
    XSFLIB_CHECK(on_block.loop_start == 1016 && on_block.samples == 2000);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>(
                     [&] { edit::EncodeDsStream(channels, kRate, sdat::WaveEncoding::kPcm16, 2000); }) ==
                 "The loop must start before the end of the stream.");
}

// The properties' edits, and a new stream file in the place of the current one.
void TestEdits()
{
    const std::vector<std::vector<int16_t>> channels = Stereo(3000);
    const std::vector<uint8_t> bytes =
        edit::EncodeDsStream(channels, kRate, sdat::WaveEncoding::kImaAdpcm, std::nullopt);
    const File file = hokuto::format::ImportDsArchive(MakeArchive(bytes, true));
    const std::shared_ptr<const song::DsArchive> archive = Load(file);
    const edit::DsStreamValues values = edit::DsStreamValuesOf(*archive, 0);
    XSFLIB_CHECK(values.rate == kRate && !values.loop && values.loop_start == 0 && !values.encoding);
    XSFLIB_CHECK(edit::DsStreamFileId(*archive, 0) == 0);

    const edit::WaveChanges changes = edit::ChangesOfDsStream(*archive, 0);
    XSFLIB_CHECK(changes.channels == 2 && changes.rate == kRate && !changes.loop && changes.loop_end == 3000 &&
                 changes.size == bytes.size() && changes.encoding == ntdlib::sar::WaveEncoding::kImaAdpcm);

    // The same values give the same archive.
    XSFLIB_CHECK(hokuto::format::ExportArchive(edit::WithDsStream(file, *archive, 0, values)) ==
                 hokuto::format::ExportArchive(file));

    // A new rate and loop change those fields, and the samples stay.
    edit::DsStreamValues changed = values;
    changed.rate = 22050;
    changed.loop = true;
    changed.loop_start = 1016;
    const File edited = edit::WithDsStream(file, *archive, 0, changed);
    CheckForms(edited);
    const std::shared_ptr<const song::DsArchive> after = Load(edited);
    const std::span<const uint8_t> edited_bytes = after->archive.File(0);
    const sdat::Stream parsed = sdat::Stream::Parse(edited_bytes);
    XSFLIB_CHECK(parsed.sample_rate == 22050 && parsed.timer == 16756991 / 22050 / 32);
    XSFLIB_CHECK(parsed.loop == 1 && parsed.loop_start == 1016 && parsed.samples == 3000);
    XSFLIB_CHECK(edited_bytes.size() == bytes.size());
    for (std::size_t i = 0x28; i < bytes.size(); i++)
    {
        XSFLIB_CHECK(edited_bytes[i] == bytes[i]);
    }

    // Another encoding makes the stream again, and its loop keeps its place at a block of the new encoding.
    changed.encoding = sdat::WaveEncoding::kPcm16;
    changed.loop_start = 1016;
    const std::shared_ptr<const song::DsArchive> pcm = Load(edit::WithDsStream(file, *archive, 0, changed));
    const sdat::Stream pcm_stream = sdat::Stream::Parse(pcm->archive.File(0));
    XSFLIB_CHECK(pcm_stream.encoding == sdat::WaveEncoding::kPcm16 && pcm_stream.block_samples == 256);
    XSFLIB_CHECK(pcm_stream.loop == 1 && pcm_stream.loop_start == 1024);
    XSFLIB_CHECK(pcm_stream.sample_rate == 22050);

    // A loop starts at a block, before the end, and a rate falls within its limits.
    edit::DsStreamValues off = values;
    off.loop = true;
    off.loop_start = 100;
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::WithDsStream(file, *archive, 0, off); }) ==
                 "The loop start of this stream must be a multiple of 1016 samples.");
    off.loop_start = 1016 * 3;
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::WithDsStream(file, *archive, 0, off); }) ==
                 "The loop must start before the end of the stream.");
    edit::DsStreamValues fast = values;
    fast.rate = 65536;
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::WithDsStream(file, *archive, 0, fast); }) ==
                 "The sample rate of a DS stream must be from 256 to 65,535 Hz.");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::WithDsStream(file, *archive, 1, values); }) ==
                 "The archive has no stream 1.");

    // A stream file in the place of the stream, of another size.
    const std::vector<uint8_t> other = edit::EncodeDsStream(Stereo(500), 16000, sdat::WaveEncoding::kPcm8, 256);
    const File replaced = edit::WithDsStreamBytes(file, *archive, 0, other);
    CheckForms(replaced);
    const std::shared_ptr<const song::DsArchive> stored = Load(replaced);
    const std::span<const uint8_t> held = stored->archive.File(0);
    XSFLIB_CHECK(std::vector<uint8_t>(held.begin(), held.end()) == other);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>(
                     [&]
                     {
                         const std::vector<uint8_t> junk(64, 0);
                         edit::WithDsStreamBytes(file, *archive, 0, junk);
                     })
                     .starts_with("Can't read the file as an STRM stream"));
    XSFLIB_CHECK(edit::IsDsStreamFile(other) && !edit::IsDsStreamFile(std::vector<uint8_t>{1, 2, 3, 4}));
}

// A WAV file and a stream file that come in.
void TestIncoming()
{
    const std::vector<std::vector<int16_t>> channels = Stereo(3000);
    seqlib::Pcm source;
    source.channels = channels;
    const std::vector<uint8_t> bytes = seqlib::WavFile(source, kRate);

    const edit::IncomingWave pcm = edit::ReadIncomingDsStream(bytes, ntdlib::sar::WaveEncoding::kPcm16);
    XSFLIB_CHECK(pcm.channels == 2 && pcm.rate == kRate && pcm.frames == 3000 && !pcm.loop && !pcm.signal_to_noise);
    const sdat::Stream made = sdat::Stream::Parse(pcm.bytes);
    XSFLIB_CHECK(sdat::DecodeStream(made, 0) == channels[0] && sdat::DecodeStream(made, 1) == channels[1]);

    // IMA-ADPCM measures its noise.
    const edit::IncomingWave ima = edit::ReadIncomingDsStream(bytes, ntdlib::sar::WaveEncoding::kImaAdpcm);
    XSFLIB_CHECK(ima.signal_to_noise && *ima.signal_to_noise > 20.0);

    // A loop from the window moves forward to a block.
    const edit::IncomingWave looped =
        edit::ReadIncomingDsStream(bytes, ntdlib::sar::WaveEncoding::kPcm16, edit::WaveLoop{300, 3000});
    XSFLIB_CHECK(looped.loop && looped.loop_start == 300 && looped.loop_moved == 212);

    // A stream file is imported as it is.
    const edit::IncomingWave stream = edit::ReadIncomingDsStreamFile(ima.bytes);
    XSFLIB_CHECK(stream.bytes == ima.bytes && stream.channels == 2 && stream.rate == kRate && stream.frames == 3000 &&
                 stream.encoding == ntdlib::sar::WaveEncoding::kImaAdpcm);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::ReadIncomingDsStreamFile(std::vector<uint8_t>(16, 0)); })
                     .starts_with("Can't read it as a DS stream file"));
}

// A new stream is added at the end of the table, with its file at the end of the FAT.
void TestNewStream(bool names)
{
    const std::vector<uint8_t> bytes =
        edit::EncodeDsStream(Stereo(1000), kRate, sdat::WaveEncoding::kImaAdpcm, std::nullopt);
    const File file = hokuto::format::ImportDsArchive(MakeArchive(bytes, names));
    const std::shared_ptr<const song::DsArchive> archive = Load(file);
    const edit::DsStreamSettings settings = edit::DsStreamSettingsOf(*archive, 0);
    XSFLIB_CHECK(settings.volume == 120 && settings.player_priority == 60 && settings.player == 0 &&
                 settings.flags == 0);

    const std::vector<uint8_t> other =
        edit::EncodeDsStream(Stereo(500), kRate, sdat::WaveEncoding::kPcm16, std::nullopt);
    const File edited = edit::AddDsStream(file, "STRM_TWO", other, settings);
    CheckForms(edited);
    const std::shared_ptr<const song::DsArchive> after = Load(edited);
    const sdat::Archive& tables = after->archive;
    SEQLIB_REQUIRE(tables.streams.size() == 2);
    XSFLIB_CHECK(tables.streams[1]->volume == 120 && tables.streams[1]->player_priority == 60);
    XSFLIB_CHECK(std::ranges::equal(tables.File(tables.streams[1]->file_id & sdat::kFileIdBits), other));
    XSFLIB_CHECK(std::ranges::equal(tables.File(tables.streams[0]->file_id & sdat::kFileIdBits), bytes));
    if (names)
    {
        XSFLIB_CHECK(song::DsStreamName(*after, 1) == "STRM_TWO");
        XSFLIB_CHECK(song::DsStreamName(*after, 0) == "STRM_ONE");
        XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::AddDsStream(file, "STRM_ONE", other, settings); }) ==
                     "The archive already has an item named STRM_ONE.");
    }
    edit::DsStreamSettings no_player = settings;
    no_player.player = 3;
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::AddDsStream(file, "STRM_TWO", other, no_player); }) ==
                 "The archive has no stream player 3. A stream requires one.");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>(
                     [&] { edit::AddDsStream(file, "STRM_TWO", std::vector<uint8_t>(32, 0), settings); })
                     .starts_with("Can't read the file as an STRM stream"));
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("ds_streams",
                                    []
                                    {
                                        TestRate();
                                        TestLayout();
                                        TestLoop();
                                        TestEdits();
                                        TestIncoming();
                                        TestNewStream(true);
                                        TestNewStream(false);
                                    });
}
