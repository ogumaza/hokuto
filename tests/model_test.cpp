// SPDX-License-Identifier: MIT

// Tests the model form of Hokuto's files (src/format/model.h). A small archive of the Switch, laid out as
// NintendoWare's tools lay one out, holds two sequence sounds of one file, a wave sound, two banks and a player. The
// second sound starts at a note. The model gives the archive back byte for byte, and the file comes back from both
// forms. A wait whose length takes a byte more than it requires gives another archive: the model leaves the encoder's
// choice out, and the second sound's start follows its note. A Wii U archive that ntdlib's builder lays out holds a
// stream sound whose stream lies beside it. A 3DS archive that ntdlib's builder lays out as CSAR 2.3.0.0 holds a stream
// sound whose entry holds two values of the extension as options. An archive of the Wii that ntdlib's builder lays out
// holds two sequence sounds of one file and a bank. Its model gives it back, keeps the game's build of NW4R SND, and
// leaves out the encoder's choices. A new command and a new sequence sound go into a file in the model form, and the
// file comes back from both forms. A new file of each console takes a bank and a sound with an empty song. A new file
// of the Wii holds a player alone. A new sound whose commands run on past a return comes back from the cut. Damaged and
// unknown chunks throw with a message. Each volume comes back from its level.

#include "format/model.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/nwsnd/commands.h"
#include "ntdlib/rsar/archive_model.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/sequence_file.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/archive_model.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/sequence_file.h"
#include "ntdlib/sound_build.h"
#include "ntdlib/testing/rsar_tools.h"
#include "ntdlib/testing/sar_files.h"
#include "ntdlib/testing/sar_tools.h"
#include "xsflib/binary.h"
#include "xsflib/testing/check.h"

#include "edit/bytecode.h"
#include "edit/commands.h"
#include "edit/converter.h"
#include "edit/new_items.h"
#include "edit/notes.h"
#include "edit/timelines.h"
#include "format/archive.h"
#include "format/file.h"
#include "format/text.h"
#include "song/archive.h"
#include "song/sequence.h"

using hokuto::format::ArchiveForm;
using hokuto::format::File;
using hokuto::format::Model;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::sar::Flavor;

namespace
{

namespace format = hokuto::format;
namespace rsar = ntdlib::rsar;
namespace sar = ntdlib::sar;
namespace sar_files = ntdlib::testing::sar_files;
namespace sar_tools = ntdlib::testing::sar_tools;

// The archive's sequence file. The second sound starts at the note, at offset 4.
const std::vector<uint8_t> kBytecode = {
    0xB0, 48,      // 0: timebase 48
    0xC1, 100,     // 2: volume 100
    60,   100, 24, // 4: key 60 for 24 ticks
    0xFF,          // 7: the end
};

// The same with a wait of 5 ticks whose length takes two bytes. The note lies at offset 5.
const std::vector<uint8_t> kLongWait = {
    0xB0, 48,         // 0: timebase 48
    0x80, 0x80, 0x05, // 2: a wait of 5 ticks
    60,   100,  24,   // 5: key 60 for 24 ticks
    0xFF,             // 8: the end
};

// A Switch archive laid out as the tools lay it out, with `bytecode` in its sequence file, and the second sound's start
// at `start`. The first sound has pan mode 1, pan curve 2, player priority 48 and actor player 2.
std::vector<uint8_t> SwitchArchive(const std::vector<uint8_t>& bytecode, uint32_t start)
{
    sar_files::SoundSpec first;
    first.name = "SEQ_FIRST";
    first.volume = 90;
    first.pan = 0x0201;
    first.priority = 0x0230;
    first.banks = {0, 1};
    first.allocate_track_flags = 3;
    first.start_offset = 0;
    sar_files::SoundSpec second;
    second.name = "SEQ_SECOND";
    second.banks = {1};
    second.start_offset = start;
    sar_files::SoundSpec wave_sound;
    wave_sound.name = "WSD_SOUND";
    wave_sound.kind = sar_files::SoundSpec::kWaveSound;
    wave_sound.file = 1;

    sar_files::ArchiveSpec spec;
    spec.sounds = {first, second, wave_sound};
    spec.banks = {{"BANK_A", 2}, {"BANK_B", 3}};
    spec.players = {{"PLAYER_TEST", 1}};
    spec.files = {{sar_files::Sequence(Flavor::kNx, bytecode, {{"First", 0}, {"Second", start}}), "", false},
                  {sar_files::WaveSoundFile(Flavor::kNx, {}), "", false},
                  {sar_files::Bank(Flavor::kNx, {}), "", false},
                  {sar_files::Bank(Flavor::kNx, {}), "", false}};

    return sar_tools::Archive(Flavor::kNx, spec);
}

// The sequence bytecode of the Wii U's and the 3DS's archives: a timebase, a note and the end.
const std::vector<uint8_t> kNoteBytecode = {0xB0, 48, 60, 100, 24, 0xFF};

// The stream beside the Wii U archive: any bytes. The model keeps them whole.
const std::vector<uint8_t> kStream = {'F', 'S', 'T', 'M', 1, 2, 3, 4};

// A Wii U archive that ntdlib lays out from its model: a sequence sound that plays a bank, and a stream sound whose
// stream lies beside the archive.
std::vector<uint8_t> WiiUArchive()
{
    sar::ArchiveModel model;
    model.flavor = Flavor::kCafe;
    model.version = 0x00020200;

    sar::SoundModel sequence;
    sequence.name = "SEQ_TEST";
    sequence.file_id = 0;
    sequence.volume = 64;
    sequence.banks = {sar::ItemId(sar::kBankItem, 0)};
    sar::SoundModel stream;
    stream.name = "STRM_TEST";
    stream.type = sar::SoundType::kStream;
    stream.file_id = 2;
    stream.stream.valid_tracks = 1;
    stream.stream.channel_count = 2;
    stream.stream.tracks = {{.channels = {0, 1}, .volume = 100, .pan = 32, .low_pass = 48}};
    stream.stream.aux_sends = {10, 0, 0};
    model.sounds = {sequence, stream};
    model.banks = {{.name = "BANK_TEST", .file_id = 1}};
    model.players = {{.name = "PLAYER_TEST", .playable_sound_max = 1}};

    sar::FileModel stream_file;
    stream_file.place = sar::FilePlace::kPath;
    stream_file.path = "stream/STRM_TEST.bfstm";
    model.files = {{.bytes = sar_files::Sequence(Flavor::kCafe, kNoteBytecode, {{"Test", 0}})},
                   {.bytes = sar_files::Bank(Flavor::kCafe, {})},
                   stream_file};

    return sar::BuildArchive(model);
}

// A 3DS archive of CSAR 2.3.0.0 that ntdlib lays out from its model: a sequence sound, and a stream sound whose entry
// holds the stream's type and the loop's end as options. The stream lies beside the archive.
std::vector<uint8_t> CtrArchive()
{
    sar::ArchiveModel model;
    model.flavor = Flavor::kCtr;
    model.version = 0x02030000;

    sar::SoundModel sequence;
    sequence.name = "SEQ_TEST";
    sequence.file_id = 0;
    sequence.volume = 100;
    sequence.banks = {sar::ItemId(sar::kBankItem, 0)};
    sar::SoundModel stream;
    stream.name = "STRM_TEST";
    stream.type = sar::SoundType::kStream;
    stream.file_id = 2;
    stream.stream.valid_tracks = 1;
    stream.stream.channel_count = 2;
    stream.stream.tracks = {{.channels = {0, 1}, .volume = 90, .pan = 40}};
    stream.stream_fields = sar::StreamFields::kNone;
    stream.stream_options = 0b101;
    stream.stream_extension = sar::StreamExtension{.stream_type = 1, .loop_end = 4000};
    model.sounds = {sequence, stream};
    model.banks = {{.name = "BANK_TEST", .file_id = 1}};
    model.players = {{.name = "PLAYER_TEST", .playable_sound_max = 1}};

    sar::FileModel stream_file;
    stream_file.place = sar::FilePlace::kPath;
    stream_file.path = "stream/STRM_TEST.bcstm";
    model.files = {{.bytes = sar_files::Sequence(Flavor::kCtr, kNoteBytecode, {{"Test", 0}})},
                   {.bytes = sar_files::Bank(Flavor::kCtr, {})},
                   stream_file};

    return sar::BuildArchive(model);
}

// The Wii's sequence bytecode: a volume, a note and the end. The second sound starts at the note, at offset 2.
const std::vector<uint8_t> kWiiBytecode = {0xC1, 100, 60, 100, 48, 0xFF};

// The same with a wait of 5 ticks whose length takes two bytes. The note lies at offset 5.
const std::vector<uint8_t> kWiiLongWait = {0xC1, 100, 0x80, 0x80, 0x05, 60, 100, 48, 0xFF};

// A Wii archive that ntdlib lays out from its model, with `bytecode` in its sequence file: two sequence sounds of the
// file, the second at `start`, and a bank with wave data. A group holds the sequence file, and the last group the bank.
// The first sound has pan mode 1, pan curve 2 and the release priority fix.
std::vector<uint8_t> WiiArchive(const std::vector<uint8_t>& bytecode, uint32_t start)
{
    rsar::SequenceFile sequence;
    sequence.data = bytecode;
    sequence.labels = {{"SEQ_FIRST", 0}, {"SEQ_SECOND", start}};

    rsar::ArchiveModel model;
    model.version = 0x0104;
    model.players = {{.name = "PLAYER_TEST", .playable_sound_count = 1}};
    model.banks = {{.name = "BANK_TEST", .file_id = 1}};
    model.groups = {{.name = "GROUP_TEST", .file_ids = {0}}};
    model.files = {{.bytes = sequence.Write()},
                   {.bytes = std::vector<uint8_t>(0x40, 0x5A), .wave_data = std::vector<uint8_t>(0x20, 0xA5)}};

    rsar::SoundModel first;
    first.name = "SEQ_FIRST";
    first.volume = 90;
    first.pan_mode = 1;
    first.pan_curve = 2;
    first.release_priority_fix = true;
    first.allocate_track_flags = 3;
    rsar::SoundModel second;
    second.name = "SEQ_SECOND";
    second.start_offset = start;
    model.sounds = {first, second};

    return rsar::BuildArchive(model);
}

// The IDs in the table of chunks of `binary`, a file in the binary form.
std::vector<std::string> ChunkIds(std::span<const uint8_t> binary)
{
    std::vector<std::string> ids;
    const uint32_t count = xsflib::LoadLe32(&binary[16]);
    for (std::size_t c = 0; c < count; ++c)
    {
        const uint8_t* entry = &binary[24 + 24 * c];
        ids.emplace_back(entry, entry + 4);
    }

    return ids;
}

// The message that Read throws for `binary` with byte `at` of chunk `id` set to `value`, or nothing. The chunk's CRC-32
// matches its new bytes.
std::string Patched(std::vector<uint8_t> binary, std::string_view id, std::size_t at, uint8_t value)
{
    const std::vector<std::string> ids = ChunkIds(binary);
    const auto chunk = static_cast<std::size_t>(std::ranges::find(ids, id) - ids.begin());
    uint8_t* entry = &binary[24 + 24 * chunk];
    const auto start = static_cast<std::size_t>(xsflib::LoadLe64(entry + 8));
    const auto size = static_cast<std::size_t>(xsflib::LoadLe64(entry + 16));
    binary[start + at] = value;
    xsflib::StoreLe32(entry + 4, format::Crc32(std::span(binary).subspan(start, size)));
    try
    {
        format::Read(binary);
    }
    catch (const xsflib::FormatError& error)
    {
        return error.what();
    }

    return "";
}

// The message that `action` throws as an Error, or nothing.
template <typename Error, typename Action>
std::string Message(const Action& action)
{
    try
    {
        action();
    }
    catch (const Error& error)
    {
        return error.what();
    }

    return "";
}

// Whether `file` comes back from both forms unchanged.
bool ComesBack(const File& file)
{
    return format::Read(format::Write(file)) == file && format::ReadText(format::WriteText(file)) == file;
}

// `data` without the zeros at its end: a sequence file's padding.
std::vector<uint8_t> WithoutTrailingZeros(std::vector<uint8_t> data)
{
    while (!data.empty() && data.back() == 0)
    {
        data.pop_back();
    }

    return data;
}

// The bytecode of the sequence file of sound `sound` of `archive`, an archive of `flavor`.
std::vector<uint8_t> BytecodeOf(const std::vector<uint8_t>& archive, Flavor flavor, uint32_t sound)
{
    sar::Options options;
    options.flavor = flavor;
    const sar::SoundArchive read = sar::SoundArchive::Load(archive, options);
    return WithoutTrailingZeros(sar::SequenceFile::Read(read.FileData(read.Sounds().at(sound).file_id)).data);
}

// Each volume comes back from its level. Full volume is level 127, and the levels past the volumes take the ends.
void TestLevels()
{
    bool back = true;
    for (int volume = 0; volume <= 255; ++volume)
    {
        back = back && format::VolumeOfLevel(format::LevelOfVolume(static_cast<uint8_t>(volume))) == volume;
    }
    XSFLIB_CHECK(back);
    XSFLIB_CHECK(format::LevelOfVolume(127) == 127 * kOne && format::LevelOfVolume(0) == 0);
    XSFLIB_CHECK(format::VolumeOfLevel(-5 * kOne) == 0 && format::VolumeOfLevel(1000 * kOne) == 255);
}

// The model of a Switch archive that the tools laid out gives the archive back. The file keeps the model's chunks and
// no native layout, and comes back from both forms. The second sound starts at the note: item 2.
void TestSwitch()
{
    const std::vector<uint8_t> archive = SwitchArchive(kBytecode, 4);
    const File file = format::ImportSwitchArchive(archive);
    const File model = format::InModelForm(file);
    XSFLIB_CHECK(model.form == ArchiveForm::kModel && file.form == ArchiveForm::kNative);
    XSFLIB_CHECK(format::ExportArchive(model) == archive);
    XSFLIB_CHECK(model.native->tables == file.native->tables && !model.native->game_sizes);
    XSFLIB_CHECK(model.sequences.size() == 1 && *model.sequences[0] == *file.sequences[0]);

    const std::vector<uint8_t> binary = format::Write(model);
    const std::vector<std::string> ids = ChunkIds(binary);
    XSFLIB_CHECK(std::ranges::count(ids, "SNDS") == 1 && std::ranges::count(ids, "ITMS") == 1);
    XSFLIB_CHECK(std::ranges::count(ids, "NATV") == 0 && std::ranges::count(ids, "TABL") == 0);
    XSFLIB_CHECK(ComesBack(model));
    XSFLIB_CHECK(format::InModelForm(model) == model);

    const Model held = format::ModelOf(model);
    XSFLIB_CHECK((held.starts == std::map<uint32_t, uint32_t>{{0, 0}, {1, 2}}));
    XSFLIB_CHECK(held.sequences.size() == 1 && held.sequences.at(0).sequence == 0);
    XSFLIB_CHECK(held.archive.files.at(0).bytes.empty() && !held.archive.files.at(1).bytes.empty());
    XSFLIB_CHECK(held.archive.sounds.at(0).volume == 90 && held.archive.sounds.at(1).start_offset == 0);
    XSFLIB_CHECK(held.archive.sounds.at(0).pan_mode == 1 && held.archive.sounds.at(0).pan_curve == 2);
    XSFLIB_CHECK(format::ReadModel(format::SoundsChunk(held), format::ItemsChunk(held)) == held);
}

// The model leaves out the encoder's choice of a wait's two bytes. The second sound's start follows the note to its new
// place at offset 4.
void TestChoices()
{
    const std::vector<uint8_t> archive = SwitchArchive(kLongWait, 5);
    const File file = format::ImportSwitchArchive(archive);
    XSFLIB_CHECK(std::ranges::any_of(file.sequences[0]->items, format::HasChoices));

    const File model = format::InModelForm(file);
    XSFLIB_CHECK(std::ranges::none_of(model.sequences[0]->items, format::HasChoices));
    const std::vector<uint8_t> exported = format::ExportArchive(model);
    XSFLIB_CHECK(exported != archive);
    const std::vector<uint8_t> kShortWait = {0xB0, 48, 0x80, 0x05, 60, 100, 24, 0xFF};
    XSFLIB_CHECK(BytecodeOf(exported, Flavor::kNx, 0) == kShortWait);
    sar::Options options;
    options.flavor = Flavor::kNx;
    XSFLIB_CHECK(sar::SoundArchive::Load(exported, options).Sounds().at(1).start_offset == 4);
    XSFLIB_CHECK(ComesBack(model));
}

// A Wii U archive with a stream beside it. The stream counts as Hokuto's, and an export writes it. The stream sound's
// values come back through Hokuto's units.
void TestWiiU()
{
    const std::vector<uint8_t> archive = WiiUArchive();
    const File file = format::ImportArchive(archive, Flavor::kCafe, {}, {{2, kStream}});
    const File model = format::InModelForm(file);
    XSFLIB_CHECK(format::ExportArchive(model) == archive);
    XSFLIB_CHECK(model.native->streams.size() == 1 && *model.native->streams.at(2).bytes == kStream);
    XSFLIB_CHECK(!model.native->streams.at(2).from_game && file.native->streams.at(2).from_game);
    XSFLIB_CHECK(ComesBack(model));

    const Model held = format::ModelOf(model);
    const sar::StreamTrackInfo& track = held.archive.sounds.at(1).stream.tracks.at(0);
    XSFLIB_CHECK(track.volume == 100 && track.pan == 32 && track.low_pass == 48 && track.channels.size() == 2);
    XSFLIB_CHECK(held.archive.sounds.at(1).stream.aux_sends[0] == 10 && held.archive.sounds.at(0).volume == 64);
    XSFLIB_CHECK(held.streams.size() == 1 && held.streams.at(2) == kStream);
}

// A 3DS archive in the model form. The stream sound's options and the values of its extension come back from SNDS.
void TestCtr()
{
    const std::vector<uint8_t> archive = CtrArchive();
    const File file = format::ImportArchive(archive, Flavor::kCtr, {}, {});
    const File model = format::InModelForm(file);
    XSFLIB_CHECK(model.form == ArchiveForm::kModel && format::ExportArchive(model) == archive);
    XSFLIB_CHECK(model.native->tables == file.native->tables && ComesBack(model));

    const Model held = format::ModelOf(model);
    const sar::SoundModel& stream = held.archive.sounds.at(1);
    XSFLIB_CHECK(held.archive.flavor == Flavor::kCtr && held.archive.version == 0x02030000);
    XSFLIB_CHECK(stream.stream_options == 0b101 && stream.stream_extension &&
                 stream.stream_extension->stream_type == 1 && stream.stream_extension->loop_end == 4000);
    XSFLIB_CHECK(stream.stream.tracks.at(0).volume == 90 && stream.stream.tracks.at(0).pan == 40);
    XSFLIB_CHECK(held.archive.files.at(0).groups.empty() && held.sequences.at(0).version == 0x01000000);
    XSFLIB_CHECK(format::ReadModel(format::SoundsChunk(held), format::ItemsChunk(held)) == held);
}

// The model of a Wii archive that the tools laid out gives the archive back. The file keeps the game's build of NW4R
// SND, and comes back from both forms. The second sound starts at the note: item 1. A wait whose length takes a byte
// more than it requires gives another archive: the model leaves the encoder's choice out, and the second sound's start
// follows its note. A new command goes into the file in the model form. An archive that the tools didn't lay out stays
// in the native form.
void TestWii()
{
    const std::vector<uint8_t> archive = WiiArchive(kWiiBytecode, 2);
    const ntdlib::SndBuild kBuild{.date = 20070608};
    const File file = format::ImportWiiArchive(archive, kBuild);
    const File model = format::InModelForm(file);
    XSFLIB_CHECK(model.form == ArchiveForm::kModel && model.engine == format::Engine::kWii);
    XSFLIB_CHECK(format::ExportArchive(model) == archive);
    XSFLIB_CHECK(model.native->tables == file.native->tables && !model.native->game_sizes);
    XSFLIB_CHECK(model.native->wii && model.native->wii_build == kBuild);
    XSFLIB_CHECK(model.sequences.size() == 1 && *model.sequences[0] == *file.sequences[0]);

    const std::vector<uint8_t> binary = format::Write(model);
    const std::vector<std::string> ids = ChunkIds(binary);
    XSFLIB_CHECK(std::ranges::count(ids, "SNDS") == 1 && std::ranges::count(ids, "ITMS") == 1);
    XSFLIB_CHECK(std::ranges::count(ids, "NATV") == 0 && std::ranges::count(ids, "TABL") == 0);
    XSFLIB_CHECK(ComesBack(model));
    XSFLIB_CHECK(format::InModelForm(model) == model);

    const Model held = format::ModelOf(model);
    XSFLIB_CHECK(held.wii && held.wii_build == kBuild && held.wii->version == 0x0104);
    XSFLIB_CHECK((held.starts == std::map<uint32_t, uint32_t>{{0, 0}, {1, 1}}));
    XSFLIB_CHECK(held.sequences.size() == 1 && held.sequences.at(0).sequence == 0);
    XSFLIB_CHECK(held.wii->files.at(0).bytes.empty() && held.wii->files.at(1).bytes.size() == 0x40);
    XSFLIB_CHECK(held.wii->sounds.at(0).volume == 90 && held.wii->sounds.at(0).release_priority_fix);
    XSFLIB_CHECK(held.wii->sounds.at(0).pan_mode == 1 && held.wii->sounds.at(0).pan_curve == 2);
    XSFLIB_CHECK(held.wii->sounds.at(1).start_offset == 0 && held.wii->groups.size() == 1);
    XSFLIB_CHECK(format::ReadModel(format::SoundsChunk(held), format::ItemsChunk(held)) == held);
    XSFLIB_CHECK(Patched(binary, "ITMS", 1, 0x80) ==
                 "The file holds a flag of the Wii's archive that this version of Hokuto doesn't know. A later version "
                 "made it.");

    // Without the game's build, the archive's version gives it, and the file keeps none.
    const File plain = format::InModelForm(format::ImportWiiArchive(archive));
    XSFLIB_CHECK(!plain.native->wii_build && !format::ModelOf(plain).wii_build && ComesBack(plain));
    XSFLIB_CHECK(format::BuildOfWiiArchive(*plain.native).date == 20080620);

    // The shorter wait moves the note to offset 4.
    const File waits = format::InModelForm(format::ImportWiiArchive(WiiArchive(kWiiLongWait, 5)));
    const std::vector<uint8_t> exported = format::ExportArchive(waits);
    const rsar::SoundArchive read = rsar::SoundArchive::Load(exported, ntdlib::rvl::snd::kRsarOptions);
    const std::vector<uint8_t> bytecode = WithoutTrailingZeros(rsar::SequenceFile::Read(read.FileData(0)).data);
    XSFLIB_CHECK((bytecode == std::vector<uint8_t>{0xC1, 100, 0x80, 0x05, 60, 100, 48, 0xFF}));
    XSFLIB_CHECK(read.Sounds().at(1).start_offset == 4 && ComesBack(waits));

    // A volume goes in before the note.
    const auto loaded =
        hokuto::song::LoadWiiArchive(format::ExportArchive(model), "test.brsar", ntdlib::rvl::snd::kRsarOptions);
    const hokuto::song::Sequence song = hokuto::song::ReadSequence(*loaded, 0, {}, {}, kBuild);
    ntdlib::commands::Command volume = hokuto::edit::NewCommand(Kind::kVolume, 48, 0);
    volume.value.value = 50 * kOne;
    const hokuto::edit::Edited louder =
        hokuto::edit::InsertCommand(model, 0, {2, song.tracks.at(0).stretches.at(0)}, false, volume);
    XSFLIB_CHECK(louder.file.form == ArchiveForm::kModel && louder.file.sequences[0]->items.size() == 4);
    const File back = format::Read(format::Write(louder.file));
    XSFLIB_CHECK(format::ExportArchive(back) == format::ExportArchive(louder.file));
    XSFLIB_CHECK(back.native->wii_build == kBuild && ComesBack(back));

    // The test builder leaves SYMB's trees out.
    const std::vector<uint8_t> untreed = ntdlib::testing::rsar_tools::MakeArchive(
        0x0104, {{rsar::SequenceFile{.data = kWiiBytecode}.Write()}}, {{"SEQ_FIRST", 0, 0}}, {{0}});
    XSFLIB_CHECK(
        !Message<xsflib::FormatError>([&] { format::InModelForm(format::ImportWiiArchive(untreed)); }).empty());
}

// A new command and a new sequence sound go into a file in the model form. The file stays in the model form, and comes
// back from both forms.
void TestEdits()
{
    const File model = format::InModelForm(format::ImportSwitchArchive(SwitchArchive(kBytecode, 4)));
    const auto archive = hokuto::song::LoadArchive(format::ExportArchive(model), "test.bfsar");
    const hokuto::song::Sequence song = hokuto::song::ReadSequence(*archive, 0);
    ntdlib::commands::Command volume = hokuto::edit::NewCommand(Kind::kVolume, 48, 0);
    volume.value.value = 50 * kOne;
    const hokuto::edit::Edited louder =
        hokuto::edit::InsertCommand(model, 0, {4, song.tracks.at(0).stretches.at(0)}, false, volume);
    XSFLIB_CHECK(louder.file.form == ArchiveForm::kModel && louder.file.sequences[0]->items.size() == 5);
    const File back = format::Read(format::Write(louder.file));
    XSFLIB_CHECK(format::ExportArchive(back) == format::ExportArchive(louder.file));
    XSFLIB_CHECK(ComesBack(back));

    const File added = hokuto::edit::AddSequenceSound(back, 0, "SEQ_NEW", *back.sequences[0], 0, {});
    XSFLIB_CHECK(added.form == ArchiveForm::kModel && added.native->tables->sounds.size() == 4);
    const File again = format::Read(format::Write(added));
    XSFLIB_CHECK(format::ExportArchive(again) == format::ExportArchive(added));
    XSFLIB_CHECK(again.native->tables->sounds.at(3).name == "SEQ_NEW" && ComesBack(again));
}

// A new file of each console holds an archive with its player alone, in the model form. A wave archive and a bank come
// in, then a sound with an empty song and Hokuto's settings. Its bank slot takes the bank. The song opens as a
// timeline, and a note goes in at tick 96, past the end of its track. The saved file plays the note, and comes back
// from both forms. A new file of the Wii holds RSAR 1.4 and its player alone. A file in the native form refuses a sound
// without a model.
void TestNewFile()
{
    for (const format::Engine engine : {format::Engine::kSwitch, format::Engine::kWiiU, format::Engine::k3ds})
    {
        const bool wii_u = engine == format::Engine::kWiiU;
        const Flavor flavor = wii_u ? Flavor::kCafe : engine == format::Engine::k3ds ? Flavor::kCtr : Flavor::kNx;
        const uint32_t version = wii_u ? 0x00020000 : engine == format::Engine::k3ds ? 0x02000000 : 0x00020200;
        const File empty = format::NewFile(engine);
        const format::NativeTables& tables = *empty.native->tables;
        XSFLIB_CHECK(empty.form == ArchiveForm::kModel && empty.engine == engine && empty.sequences.empty());
        XSFLIB_CHECK(tables.sounds.empty() && tables.banks.empty() && tables.wave_archives.empty());
        XSFLIB_CHECK(tables.players.size() == 1 && tables.players[0].name == format::kDefaultPlayer &&
                     tables.players[0].sound_limit == 1);
        XSFLIB_CHECK(format::ModelOf(empty).archive.version == version);
        XSFLIB_CHECK(ComesBack(empty));

        // A wave archive with a wave, and a bank whose instrument plays it.
        sar_files::Region region;
        region.original_key = 60;
        sar_files::VelocityTable velocities;
        velocities.entries = {{0, region}};
        sar_files::KeyTable keys;
        keys.entries = {{0, velocities}};
        sar_files::BankSpec bank;
        bank.waves = {{sar::ItemId(sar::kWaveArchiveItem, 0), 0}};
        bank.instruments = {keys};
        const File waves = hokuto::edit::AddWaveArchive(
            empty, "", sar_files::WaveArchive(flavor, {sar_files::Wave(flavor, sar_files::ConstantWave(wii_u))}));
        const File banked = hokuto::edit::AddBank(waves, "BANK_NEW", sar_files::Bank(flavor, bank),
                                                  {{sar::ItemId(sar::kWaveArchiveItem, 0), 0}});

        const File added = hokuto::edit::AddSequenceSound(
            banked, std::nullopt, "SEQ_NEW", hokuto::edit::EmptySong("SEQ_NEW"), hokuto::edit::kEmptySongStart, {});
        XSFLIB_CHECK(added.form == ArchiveForm::kModel && added.sequences.size() == 1);
        const format::NativeTables& now = *added.native->tables;
        XSFLIB_CHECK(now.sounds.size() == 1 && now.sounds[0].name == "SEQ_NEW" &&
                     now.sounds[0].kind == sar::SoundType::kSequence &&
                     now.sounds[0].player == sar::ItemId(sar::kPlayerItem, 0) && now.sounds[0].volume == 127 &&
                     now.sounds[0].banks == std::vector<uint32_t>{sar::ItemId(sar::kBankItem, 0)} &&
                     now.sounds[0].tracks == 1);
        XSFLIB_CHECK(ComesBack(added));

        // The new song is a timeline. A note goes in past the end of its track, and plays from the saved file.
        const File opened = hokuto::edit::WithNewModes(added, std::vector<uint32_t>{0});
        XSFLIB_CHECK(hokuto::edit::IsTimeline(opened, 0));

        const auto read = [](const File& file)
        {
            return hokuto::song::ReadSequence(
                *hokuto::song::LoadArchive(format::ExportArchive(file), "test.bfsar", format::ExportGroupFiles(file)),
                0);
        };
        const hokuto::song::Sequence song = read(opened);
        XSFLIB_CHECK(song.tracks.size() == 1 && song.tracks[0].notes.empty());
        const hokuto::edit::NewNote note{.stretch = song.tracks.at(0).stretches.at(0), .tick = 96, .length = 48};
        const File written = hokuto::edit::FoldTimelines(
            hokuto::edit::SettleTimelines(opened, hokuto::edit::EditNotes(opened, 0, {}, std::vector{note}).file).file);
        const File saved = format::Read(format::Write(written));
        const hokuto::song::Sequence played = read(saved);
        XSFLIB_CHECK(played.tracks.size() == 1 && played.tracks[0].notes.size() == 1 &&
                     played.tracks[0].notes[0].start == 96 && played.tracks[0].notes[0].key == 60);
        XSFLIB_CHECK(ComesBack(written));
    }

    const File wii = format::NewFile(format::Engine::kWii);
    XSFLIB_CHECK(wii.form == ArchiveForm::kModel && wii.engine == format::Engine::kWii && wii.sequences.empty());
    XSFLIB_CHECK(wii.native->wii && !wii.native->wii_build && wii.native->tables->sounds.empty());
    const Model wii_model = format::ModelOf(wii);
    XSFLIB_CHECK(wii_model.wii && wii_model.wii->version == 0x0104 && wii_model.wii->sounds.empty() &&
                 wii_model.wii->files.empty() && wii_model.wii->players.size() == 1 &&
                 wii_model.wii->players[0].name == format::kDefaultPlayer &&
                 wii_model.wii->players[0].playable_sound_count == 1);
    XSFLIB_CHECK(format::BuildOfWiiArchive(*wii.native).date == 20080620 && ComesBack(wii));
    const rsar::SoundArchive empty_wii =
        rsar::SoundArchive::Load(format::ExportArchive(wii), ntdlib::rvl::snd::kRsarOptions);
    XSFLIB_CHECK(empty_wii.Sounds().empty() && empty_wii.Players().size() == 1);

    const File native = format::ImportSwitchArchive(SwitchArchive(kBytecode, 4));
    const auto add = [&native]
    {
        hokuto::edit::AddSequenceSound(native, std::nullopt, "SEQ_NEW", hokuto::edit::EmptySong("SEQ_NEW"),
                                       hokuto::edit::kEmptySongStart, {});
    };
    XSFLIB_CHECK(Message<std::invalid_argument>(add).starts_with("The archive holds no sequence sound"));
}

// A sequence whose commands run on past a return to commands that no label or start leads to: a volume, a return, a
// jump to offset 8, an end that nothing reaches, then a volume and the end at offset 8. The cut leaves such a file
// among the archive's bytes. `big` gives the jump's target in big-endian order, as on the Wii.
std::vector<uint8_t> RunOn(bool big)
{
    std::vector<uint8_t> bytes = {0xC1, 100, 0xFD, 0x89, 0x08, 0x00, 0x00, 0xFF, 0xC1, 32, 0xFF};
    if (big)
    {
        std::swap(bytes[4], bytes[6]);
    }

    return bytes;
}

// The commands of RunOn(`big`) for the console of `file`, with the label of their start alone. A label at the jump
// leads the decoder to the commands after the return.
ntdlib::commands::Sequence RunOnCommands(const File& file, bool big)
{
    const std::vector<ntdlib::nwsnd::LabelOffset> kLabels = {{"SEQ_RUN_ON", 0}, {"After", 3}};
    ntdlib::commands::Sequence sequence =
        ntdlib::nwsnd::DecodeSequence(RunOn(big), kLabels, hokuto::song::GrammarOf(hokuto::edit::ConsoleOf(file)))
            .sequence;
    sequence.labels.pop_back();

    return sequence;
}

// Whether `added` holds `commands` in its last sequence, `count` sequences in all, and comes back from a save with them
// and the same archive.
bool SavesRunOn(const File& added, const ntdlib::commands::Sequence& commands, std::size_t count)
{
    const File again = format::Read(format::Write(added));
    return added.sequences.size() == count && *added.sequences.back() == commands && again.sequences.size() == count &&
           *again.sequences.back() == commands && format::ExportArchive(again) == format::ExportArchive(added) &&
           ComesBack(again);
}

// A new sound plays the commands of RunOn. Its file's sequence comes back from the cut all the same, in a file of the
// game's layout and in the model form, on the Switch and on the Wii, and in a new file of the Switch. A save keeps the
// sequence.
void TestRunOn()
{
    const File native = format::ImportSwitchArchive(SwitchArchive(kBytecode, 4));
    const ntdlib::commands::Sequence commands = RunOnCommands(native, false);
    XSFLIB_CHECK(commands.items.size() == 6 && commands.labels.size() == 1);
    for (const File& file : {native, format::InModelForm(native)})
    {
        XSFLIB_CHECK(SavesRunOn(hokuto::edit::AddSequenceSound(file, 0, "SEQ_RUN_ON", commands, 0, {}), commands, 2));
    }
    const File fresh = hokuto::edit::AddSequenceSound(format::NewFile(format::Engine::kSwitch), std::nullopt,
                                                      "SEQ_RUN_ON", commands, 0, {});
    XSFLIB_CHECK(SavesRunOn(fresh, commands, 1));

    const File wii = format::ImportWiiArchive(WiiArchive(kWiiBytecode, 2));
    const ntdlib::commands::Sequence wii_commands = RunOnCommands(wii, true);
    XSFLIB_CHECK(wii_commands.items.size() == 6 && wii_commands.labels.size() == 1);
    for (const File& file : {wii, format::InModelForm(wii)})
    {
        const File added = hokuto::edit::AddWiiSequenceSound(file, 0, "SEQ_RUN_ON", wii_commands, 0, {});
        XSFLIB_CHECK(SavesRunOn(added, wii_commands, 2));
    }
}

// Damaged and unknown parts of the model throw with a message.
void TestDamaged()
{
    const File model = format::InModelForm(format::ImportSwitchArchive(SwitchArchive(kBytecode, 4)));
    const Model held = format::ModelOf(model);

    // SNDS: the count, then the first sound's name of 9 bytes, its kind and its flags.
    const std::vector<uint8_t> binary = format::Write(model);
    XSFLIB_CHECK(
        Patched(binary, "SNDS", 17, 9) ==
        "The file holds a kind of sound (9) that this version of Hokuto doesn't know. A later version made it.");
    XSFLIB_CHECK(Patched(binary, "SNDS", 18, 0x80) ==
                 "The file holds a flag of a sound that this version of Hokuto doesn't know. A later version made it.");
    XSFLIB_CHECK(
        Patched(binary, "ITMS", 0, 7) ==
        "The file holds a kind of archive (7) that this version of Hokuto doesn't know. A later version made it.");
    XSFLIB_CHECK(Patched(binary, "SNDS", 0, 9) == "The file is damaged: the SNDS chunk ends early.");

    Model past = held;
    past.starts[1] = 4;
    XSFLIB_CHECK(Message<xsflib::FormatError>([&] { format::WithModel(model, past); }) ==
                 "The file is damaged: sound 1 of the model starts past its sequence's items.");
    Model unnamed = held;
    unnamed.sequences.at(0).sequence = 5;
    XSFLIB_CHECK(Message<xsflib::FormatError>([&] { format::WithModel(model, unnamed); }) ==
                 "The file is damaged: file 0 of the model names no file or no sequence.");

    // The text form holds both chunks, in a file without a native archive.
    const std::string text = format::WriteText(model);
    const std::size_t items = text.find("\nchunk ITMS");
    const std::string without_items = text.substr(0, items + 1);
    const std::string message = Message<xsflib::FormatError>([&] { format::ReadText(without_items); });
    XSFLIB_CHECK(message.ends_with("a file in the model form holds both the SNDS and the ITMS chunk."));
    const std::string native = format::WriteText(format::ImportSwitchArchive(SwitchArchive(kBytecode, 4)));
    const std::string both = native + text.substr(text.find("\nchunk SNDS"));
    XSFLIB_CHECK(Message<xsflib::FormatError>([&] { format::ReadText(both); })
                     .ends_with("the SNDS chunk comes once, in a file without a native archive."));
}

} // namespace

int main()
{
    TestLevels();
    TestSwitch();
    TestChoices();
    TestWiiU();
    TestCtr();
    TestWii();
    TestEdits();
    TestNewFile();
    TestRunOn();
    TestDamaged();

    return xsflib::testing::Summary("model");
}
