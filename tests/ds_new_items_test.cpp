// SPDX-License-Identifier: MIT

// Tests the new items of a DS sound archive (edit/ds_new_items.h). ntdlib's builders make a small archive that the
// SDK's tools could have laid out: two sequences, a sequence archive, a bank and a wave archive. The record of a new
// item is added at the end of its table, and its file at the end of the FAT. Every other file keeps its bytes, and
// every other item its number. The names follow the tools' layout, where the archive has them.

#include "edit/ds_new_items.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ntdlib/sdat/decode.h"
#include "ntdlib/sdat/files.h"
#include "ntdlib/sdat/formats.h"
#include "ntdlib/sdat/layout.h"
#include "ntdlib/testing/sdat_files.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "edit/ds_banks.h"
#include "edit/ds_files.h"
#include "edit/ds_streams.h"
#include "edit/ds_waves.h"
#include "edit/notes.h"
#include "format/archive.h"
#include "format/file.h"
#include "format/model.h"
#include "format/text.h"
#include "song/ds.h"
#include "song/sequence.h"

using hokuto::format::File;
using xsflib::testing::ErrorOf;

namespace
{

namespace edit = hokuto::edit;
namespace files = ntdlib::testing::sdat_files;
namespace sdat = ntdlib::sdat;
namespace song = hokuto::song;

constexpr uint16_t kNone = 0xFFFF;

// A note of `key` that lasts 24 ticks, on the one track.
files::Bytes NoteOf(uint8_t key)
{
    return files::Sequence({key, 0x7F, 0x18, 0xFF});
}

files::Bytes BankOfWave(uint16_t wave)
{
    return files::Bank({files::Instrument{.type = 1, .region = {.wave = wave}}});
}

files::Bytes Waves()
{
    const files::Wave square{.loop = 1, .loop_length = 2, .data = {0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0}};
    return files::WaveArchive({square});
}

// An archive with names or without them. Sounds 0 and 1 play sequence files, and a sequence archive contains two more.
std::vector<uint8_t> MakeArchive(bool names, const files::Bytes& bank = BankOfWave(0))
{
    sdat::ArchiveModel model;
    model.sequences = {sdat::SequenceInfo{0, 0, 100, 60, 70, 0, 0, ""}, sdat::SequenceInfo{1, 0, 90, 50, 40, 0, 0, ""}};
    model.sequence_archives = {sdat::SequenceArchiveInfo{2, "", {}}};
    model.banks = {sdat::BankInfo{3, {0, kNone, kNone, kNone}, ""}};
    model.wave_archives = {sdat::WaveArchiveInfo{4, ""}};
    model.players = {sdat::PlayerInfo{1, 0, 0, 0, ""}};
    const std::vector<sdat::ArchivedSequence> archived = {{0, 0, 80, 30, 20, 0, 0}, {4, 0, 70, 31, 21, 0, 0}};
    const std::vector<uint8_t> bytecode = {60, 0x7F, 0x18, 0xFF, 62, 0x7F, 0x18, 0xFF};
    model.files = {NoteOf(60), NoteOf(62), sdat::SequenceArchiveFile(archived, bytecode), bank, Waves()};
    if (names)
    {
        sdat::Symbols symbols;
        symbols.names[0] = {"SEQ_ONE", "SEQ_TWO"};
        symbols.names[1] = {"SSAR_ONE"};
        symbols.names[2] = {"BANK_ONE"};
        symbols.names[3] = {"WAVES_ONE"};
        symbols.names[4] = {"PLAYER_ONE"};
        symbols.sequence_names = {std::vector<std::optional<std::string>>{"SE_ONE", "SE_TWO"}};
        model.symbols = symbols;
    }

    return sdat::BuildArchive(model);
}

std::shared_ptr<const song::DsArchive> Load(const File& file)
{
    return song::LoadDsArchive(hokuto::format::ExportArchive(file), {});
}

// The files of the archive in `before` keep their bytes in `after`, each on a multiple of 32 bytes.
void CheckFilesKept(std::span<const uint8_t> before, const sdat::Archive& after)
{
    const sdat::Archive old_archive = sdat::Archive::Parse(before);
    XSFLIB_CHECK(after.files.size() >= old_archive.files.size());
    for (uint32_t id = 0; id < old_archive.files.size(); id++)
    {
        XSFLIB_CHECK(std::ranges::equal(after.File(id), old_archive.File(id)));
        XSFLIB_CHECK(after.files[id].offset % 32 == 0);
    }
}

void CheckForms(const File& file)
{
    const std::vector<uint8_t> exported = hokuto::format::ExportArchive(file);
    XSFLIB_CHECK(hokuto::format::ExportArchive(hokuto::format::Read(hokuto::format::Write(file))) == exported);
    XSFLIB_CHECK(hokuto::format::ExportArchive(hokuto::format::ReadText(hokuto::format::WriteText(file))) == exported);
}

// The sounds of the table of sequences come first, and those of the sequence archive start at a fixed number.
void TestSoundNumbers()
{
    // The sounds of the table of sequences come first. Those of the sequence archive start at a fixed number.
    const std::shared_ptr<const song::DsArchive> archive = song::LoadDsArchive(MakeArchive(false), {});
    const std::vector<uint32_t> sounds = song::DsSounds(*archive);
    XSFLIB_CHECK(sounds.size() == 4);
    XSFLIB_CHECK(sounds[0] == 0 && sounds[1] == 1);
    XSFLIB_CHECK(sounds[2] == song::kFirstDsArchivedSound && sounds[3] == song::kFirstDsArchivedSound + 1);
    XSFLIB_CHECK(archive->sounds[sounds[3]]->archived == 1u);

    const edit::DsSoundValues first = edit::DsSoundValuesOf(*archive, 1);
    XSFLIB_CHECK((first == edit::DsSoundValues{0, 90, 50, 40, 0}));
    const edit::DsSoundValues second = edit::DsSoundValuesOf(*archive, sounds[3]);
    XSFLIB_CHECK((second == edit::DsSoundValues{0, 70, 31, 21, 0}));
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::DsSoundValuesOf(*archive, 2); }) ==
                 "The archive has no sound 2.");
}

// A new DS sound gets the next number and plays its new commands. The other sounds keep their numbers, and a
// replacement of the new sound's sequence reaches its file. With `names`, the archive names its sounds.
void TestNewSound(bool names)
{
    const std::vector<uint8_t> bytes = MakeArchive(names);
    const File file = hokuto::format::ImportDsArchive(bytes);
    XSFLIB_CHECK(!edit::DsAdditionsRefused(file));
    const files::Bytes sequence = NoteOf(64);
    const edit::DsSoundValues values{0, 111, 22, 33, 0};
    const File added = edit::AddDsSequenceSound(file, "SEQ_NEW", sequence, values);

    // The new sound gets the next number, and the file's sequences keep theirs. The other sounds keep theirs too.
    const std::shared_ptr<const song::DsArchive> archive = Load(added);
    CheckFilesKept(bytes, archive->archive);
    XSFLIB_CHECK(archive->archive.sequences.size() == 3);
    const sdat::SequenceInfo& info = *archive->archive.sequences[2];
    XSFLIB_CHECK(info.file_id == 5 && info.volume == 111 && info.channel_priority == 22 && info.player_priority == 33);
    XSFLIB_CHECK(std::ranges::equal(archive->archive.File(5), sequence));
    XSFLIB_CHECK(added.sequences.size() == file.sequences.size() + 1);
    for (std::size_t i = 0; i < file.sequences.size(); i++)
    {
        XSFLIB_CHECK(added.sequences[i].SharesWith(file.sequences[i]));
    }
    const std::vector<uint32_t> sounds = song::DsSounds(*archive);
    XSFLIB_CHECK(sounds.size() == 5 && sounds[2] == 2 && sounds[3] == song::kFirstDsArchivedSound);
    XSFLIB_CHECK(song::DsSoundName(*archive, sounds[4]) ==
                 song::DsSoundName(*song::LoadDsArchive(bytes, {}), song::kFirstDsArchivedSound + 1));
    XSFLIB_CHECK(song::DsSoundName(*archive, 2) == (names ? "SEQ_NEW" : "SEQ_2"));

    // The sound plays the new commands.
    const song::Sequence read = song::ReadSequence(*archive, 2);
    XSFLIB_CHECK(read.tracks.size() == 1 && read.tracks[0].notes.size() == 1 && read.tracks[0].notes[0].key == 64);
    CheckForms(added);

    // A replacement of the new sound's sequence reaches its file.
    const files::Bytes other = NoteOf(66);
    const File replaced = edit::ReplaceDsSequence(added, *archive, 2, other);
    const std::shared_ptr<const song::DsArchive> after = Load(replaced);
    XSFLIB_CHECK(std::ranges::equal(after->archive.File(5), other));
}

// A new DS bank plays in a new sound. Its warnings name the slots that its regions name and the waves that they
// require.
void TestNewBank()
{
    const std::vector<uint8_t> bytes = MakeArchive(true);
    const File file = hokuto::format::ImportDsArchive(bytes);
    const std::array<uint16_t, 4> slots = {0, kNone, kNone, kNone};
    const files::Bytes bank = BankOfWave(0);
    const File added = edit::AddDsBank(file, "BANK_NEW", bank, slots);
    const std::shared_ptr<const song::DsArchive> archive = Load(added);
    CheckFilesKept(bytes, archive->archive);
    XSFLIB_CHECK(archive->archive.banks.size() == 2);
    XSFLIB_CHECK(archive->archive.banks[1]->file_id == 5 && archive->archive.banks[1]->wave_archives == slots);
    XSFLIB_CHECK(song::DsBankName(*archive, 1) == "BANK_NEW");
    XSFLIB_CHECK(std::ranges::equal(archive->archive.File(5), bank));
    CheckForms(added);

    // A new sound plays the new bank.
    const File with_sound = edit::AddDsSequenceSound(added, "SEQ_NEW", NoteOf(60), {1, 127, 64, 64, 0});
    const std::shared_ptr<const song::DsArchive> played = Load(with_sound);
    XSFLIB_CHECK(song::DsBankOf(*played, 2) == 1u);
    XSFLIB_CHECK(song::ReadSequence(*played, 2).tracks[0].notes.size() == 1);

    // The slots that a bank's regions name, and the waves that they require.
    const std::vector<edit::DsBankSlot> named = edit::DsBankSlots(BankOfWave(3));
    XSFLIB_CHECK(named.size() == 1 && named[0].slot == 0 && named[0].waves == 4);
    XSFLIB_CHECK(edit::DsNewBankWarnings(*archive, bank, slots).empty());
    XSFLIB_CHECK(edit::DsNewBankWarnings(*archive, BankOfWave(3), slots).size() == 1);
    XSFLIB_CHECK(edit::DsNewBankWarnings(*archive, bank, {kNone, kNone, kNone, kNone}).size() == 1);
}

// A new DS wave archive is added after the others with its name, its one wave and its bytes.
void TestNewWaveArchive()
{
    const std::vector<uint8_t> bytes = MakeArchive(true);
    const File file = hokuto::format::ImportDsArchive(bytes);
    const files::Bytes waves = Waves();
    const File added = edit::AddDsWaveArchive(file, "WAVES_NEW", waves);
    const std::shared_ptr<const song::DsArchive> archive = Load(added);
    CheckFilesKept(bytes, archive->archive);
    XSFLIB_CHECK(archive->archive.wave_archives.size() == 2);
    XSFLIB_CHECK(song::DsWaveArchiveName(*archive, 1) == "WAVES_NEW");
    XSFLIB_CHECK(song::DsWaveCount(*archive, 1) == 1);
    XSFLIB_CHECK(std::ranges::equal(archive->archive.File(archive->archive.wave_archives[1]->file_id), waves));
    CheckForms(added);
}

// A new DS sound with an empty song shows a track without notes. A note that the editors write there plays from the
// saved file.
void TestEmptySong()
{
    // A new sound with an empty song shows a track without notes.
    const File file = hokuto::format::ImportDsArchive(MakeArchive(true));
    const File added = edit::AddDsSequenceSound(file, "SEQ_NEW", edit::EmptyDsSequenceFile(), {0, 127, 64, 64, 0});
    const song::Sequence read = song::ReadSequence(*Load(added), 2);
    SEQLIB_REQUIRE(read.tracks.size() == 1 && read.tracks[0].notes.empty());
    CheckForms(added);

    // The editors write a note there, and it plays from the saved file.
    const edit::NewNote note{.stretch = read.tracks.at(0).stretches.at(0), .tick = 96, .length = 48};
    const File written = edit::EditNotes(added, 2, {}, std::vector{note}).file;
    const File saved = hokuto::format::Read(hokuto::format::Write(written));
    const song::Sequence played = song::ReadSequence(*Load(saved), 2);
    SEQLIB_REQUIRE(played.tracks.size() == 1 && played.tracks[0].notes.size() == 1);
    XSFLIB_CHECK(played.tracks[0].notes[0].start == 96 && played.tracks[0].notes[0].key == 60);
    CheckForms(written);
}

// A new file has a bare SDAT with names: a player that plays one sound at a time on any channel without a heap of its
// own, a stream player on channels 4 and 5, and a bank without instruments. Sounds, banks, wave archives and streams go
// in through the edits of Project > Add Sequence Sound… and File > Import, and the file comes back from both forms.
void TestNewFile()
{
    namespace format = hokuto::format;

    const File file = format::NewFile(format::Engine::kDs);
    const std::vector<uint8_t> bytes = format::ExportArchive(file);
    const sdat::Archive archive = sdat::Archive::Parse(bytes);
    XSFLIB_CHECK(file.engine == format::Engine::kDs && file.native->engine == format::Engine::kDs &&
                 !file.native->cartridge);
    XSFLIB_CHECK(file.form == format::ArchiveForm::kNative && file.sequences.empty());
    XSFLIB_CHECK(std::ranges::equal(std::span(bytes).first(4), std::string_view("SDAT")));
    XSFLIB_CHECK(archive.has_symbols && archive.symbols.names[4].at(0) == "PLAYER_DEFAULT");
    XSFLIB_CHECK(archive.symbols.names[2].at(0) == "BANK_DEFAULT");
    XSFLIB_CHECK(archive.symbols.names[6].at(0) == "STRM_PLAYER_DEFAULT");
    XSFLIB_CHECK(archive.sequences.empty() && archive.sequence_archives.empty() && archive.wave_archives.empty());
    XSFLIB_CHECK(archive.players.size() == 1 && archive.players[0]->max_sequences == 1);
    XSFLIB_CHECK(archive.players[0]->channels == 0 && archive.players[0]->heap_size == 0);
    XSFLIB_CHECK(archive.stream_players.size() == 1 && archive.stream_players[0]->channel_count == 2);
    XSFLIB_CHECK(archive.stream_players[0]->channels[0] == 4 && archive.stream_players[0]->channels[1] == 5);
    XSFLIB_CHECK(archive.banks.size() == 1 && archive.banks[0]->wave_archives[0] == kNone);
    XSFLIB_CHECK(sdat::Bank::Parse(archive.File(archive.banks[0]->file_id)).instruments.empty());
    XSFLIB_CHECK(!edit::DsAdditionsRefused(file));
    XSFLIB_CHECK(song::DsSounds(*Load(file)).empty() && format::DsSoundSequences(file, bytes).empty());
    CheckForms(file);

    // Project > Add Sequence Sound…, on the empty bank.
    const File song = edit::AddDsSequenceSound(file, "SEQ_NEW", edit::EmptyDsSequenceFile(), {});
    const std::shared_ptr<const song::DsArchive> with_song = Load(song);
    XSFLIB_CHECK(song::DsSounds(*with_song) == std::vector<uint32_t>{0});
    XSFLIB_CHECK(song::DsSoundName(*with_song, 0) == "SEQ_NEW" && song::DsBankOf(*with_song, 0) == 0u);
    XSFLIB_CHECK(song.sequences.size() == 1);
    CheckForms(song);

    // A wave archive and a bank through File > Import, and a sound that plays the bank's instrument.
    const File with_waves = edit::AddDsWaveArchive(song, "WAVES_NEW", Waves());
    const File with_bank = edit::AddDsBank(with_waves, "BANK_NEW", BankOfWave(0), {0, kNone, kNone, kNone});
    const File played = edit::AddDsSequenceSound(with_bank, "SEQ_NOTE", NoteOf(64), {.bank = 1});
    const song::Sequence read = song::ReadSequence(*Load(played), 1);
    SEQLIB_REQUIRE(read.tracks.size() == 1 && read.tracks[0].notes.size() == 1);
    XSFLIB_CHECK(read.tracks[0].notes[0].key == 64);
    CheckForms(played);

    // A stream through File > Import > Stream…, on the stream player.
    const std::vector<std::vector<int16_t>> samples(2, std::vector<int16_t>(2000, 0));
    const std::vector<uint8_t> stream = edit::EncodeDsStream(samples, 32000, sdat::WaveEncoding::kPcm16, std::nullopt);
    const File with_stream = edit::AddDsStream(played, "STRM_NEW", stream, {});
    const std::shared_ptr<const song::DsArchive> streams = Load(with_stream);
    XSFLIB_CHECK(song::DsStreams(*streams).size() == 1 && song::DsStreamName(*streams, 0) == "STRM_NEW");
    XSFLIB_CHECK(streams->archive.streams[0]->player == 0);
    CheckForms(with_stream);
}

// The Bank Editor's edits of a bank's instruments. The bank file of MakeArchive has a wave at program 0.
struct BankModelFixture
{
    File file;
    std::shared_ptr<const song::DsArchive> archive;
    sdat::Bank bank;
    sdat::Region square;
};

// The fixture of the tests of the Bank Editor: the archive, its bank 0 and a new region of a square wave. A test that
// reads the region at program 0 first checks that bank 0 has one instrument.
BankModelFixture MakeBankModelFixture()
{
    const File file = hokuto::format::ImportDsArchive(MakeArchive(true));
    const std::shared_ptr<const song::DsArchive> archive = Load(file);
    const sdat::Bank bank = edit::DsBankModelOf(*archive, 0);
    const sdat::Region square = edit::NewDsRegion(std::nullopt);

    return {file, archive, bank, square};
}

// `model` read back after it replaces bank 0 of `file`. The other files of the archive stay as they were.
sdat::Bank Through(const File& file, const std::shared_ptr<const song::DsArchive>& archive, const sdat::Bank& model)
{
    // The bank of each edit is written to the bank file and read back.
    const File edited = edit::WithDsBankModel(file, *archive, 0, model);
    const std::shared_ptr<const song::DsArchive> after = Load(edited);
    for (uint32_t id = 0; id < archive->archive.files.size(); id++)
    {
        XSFLIB_CHECK(id == archive->archive.banks[0]->file_id ||
                     std::ranges::equal(after->archive.File(id), archive->archive.File(id)));
    }
    CheckForms(edited);

    return edit::DsBankModelOf(*after, 0);
}

// A new region, the next free program, and a new instrument, a copy, a move and a deletion.
void TestBankInstruments()
{
    using sdat::InstrumentType;
    const BankModelFixture fixture = MakeBankModelFixture();
    const File& file = fixture.file;
    const std::shared_ptr<const song::DsArchive>& archive = fixture.archive;
    const sdat::Bank& bank = fixture.bank;
    const sdat::Region& square = fixture.square;
    SEQLIB_REQUIRE(bank.instruments.size() == 1 && bank.instruments[0].type == InstrumentType::kPcm);
    const sdat::Region region = bank.instruments[0].region;

    // A new region plays the wave that it's given at full level, or else a square wave of 50 % duty.
    XSFLIB_CHECK(square.type == InstrumentType::kPsg && square.wave == 3 && square.root_key == 60 &&
                 square.attack == 127 && square.sustain == 127 && square.release == 125 && square.pan == 64);
    const sdat::Region wave = edit::NewDsRegion(edit::DsWavePlace{2, 7});
    XSFLIB_CHECK(wave.type == InstrumentType::kPcm && wave.wave_archive == 2 && wave.wave == 7);

    // The next free program can be at the table's end.
    XSFLIB_CHECK(edit::FreeDsProgram(bank, 0) == 1u);
    XSFLIB_CHECK(!edit::FreeDsProgram(bank, edit::kHighestDsProgram + 1));

    // A new instrument, a copy, a move and a deletion.
    const sdat::Bank added = Through(file, archive, edit::WithNewDsInstrument(bank, 3, square));
    SEQLIB_REQUIRE(added.instruments.size() == 4);
    XSFLIB_CHECK(added.instruments[0].region == region && added.instruments[1].type == InstrumentType::kNone &&
                 added.instruments[3].type == InstrumentType::kPsg && added.instruments[3].region == square);
    const sdat::Bank copied = Through(file, archive, edit::WithDsInstrumentCopy(bank, 0, 2));
    SEQLIB_REQUIRE(copied.instruments.size() == 3);
    XSFLIB_CHECK(copied.instruments[2].region == region &&
                 copied.instruments[2].offset != copied.instruments[0].offset);
    const sdat::Bank moved = Through(file, archive, edit::WithDsInstrumentMoved(added, 3, 1));
    XSFLIB_CHECK(moved.instruments[1].region == square && moved.instruments[3].type == InstrumentType::kNone);
    const sdat::Bank without = Through(file, archive, edit::WithoutDsInstrument(added, 3));
    XSFLIB_CHECK(without.instruments.size() == 4 && without.instruments[3].type == InstrumentType::kNone);
}

// The edits of key splits and drum sets, and the edits that the bank refuses.
void TestBankKeySplits()
{
    using sdat::InstrumentType;
    const BankModelFixture fixture = MakeBankModelFixture();
    const File& file = fixture.file;
    const std::shared_ptr<const song::DsArchive>& archive = fixture.archive;
    const sdat::Bank& bank = fixture.bank;
    const sdat::Region& square = fixture.square;
    SEQLIB_REQUIRE(bank.instruments.size() == 1 && bank.instruments[0].type == InstrumentType::kPcm);
    const sdat::Region region = bank.instruments[0].region;

    // A key split of one run splits, and a run's keys go to the next run, or for the last to the run before it.
    const sdat::Bank one_run = Through(file, archive, edit::WithDsKeySplit(bank, 0));
    SEQLIB_REQUIRE(one_run.instruments[0].type == InstrumentType::kKeySplit);
    XSFLIB_CHECK(one_run.instruments[0].regions == std::vector{region} && one_run.instruments[0].split_keys[0] == 127);
    const sdat::Bank two_runs = Through(file, archive, edit::WithDsSplit(one_run, 0, 0, 60));
    XSFLIB_CHECK(two_runs.instruments[0].regions == std::vector(2, region));
    XSFLIB_CHECK(two_runs.instruments[0].split_keys[0] == 59 && two_runs.instruments[0].split_keys[1] == 127 &&
                 two_runs.instruments[0].split_keys[2] == 0);
    const sdat::Bank three_runs = Through(file, archive, edit::WithDsSplit(two_runs, 0, 1, 72));
    XSFLIB_CHECK(three_runs.instruments[0].split_keys[0] == 59 && three_runs.instruments[0].split_keys[1] == 71 &&
                 three_runs.instruments[0].split_keys[2] == 127);
    const sdat::Bank next_takes = Through(file, archive, edit::WithoutDsRun(three_runs, 0, 1));
    XSFLIB_CHECK(next_takes.instruments[0].regions.size() == 2 && next_takes.instruments[0].split_keys[0] == 59 &&
                 next_takes.instruments[0].split_keys[1] == 127 && next_takes.instruments[0].split_keys[2] == 0);
    const sdat::Bank before_takes = Through(file, archive, edit::WithoutDsRun(three_runs, 0, 2));
    XSFLIB_CHECK(before_takes.instruments[0].split_keys[0] == 59 && before_takes.instruments[0].split_keys[1] == 127);
    XSFLIB_CHECK(Through(file, archive, edit::WithDsOneRegion(two_runs, 0, 1)).instruments[0].region == region);

    // A drum set's new keys take the nearest key's values.
    const sdat::Bank drums = Through(file, archive, edit::WithDsDrumSet(bank, 0, 60));
    SEQLIB_REQUIRE(drums.instruments[0].type == InstrumentType::kDrumSet);
    XSFLIB_CHECK(drums.instruments[0].min_key == 60 && drums.instruments[0].max_key == 60 &&
                 drums.instruments[0].regions == std::vector{region});
    const sdat::Bank wider = Through(file, archive, edit::WithDsDrumKeys(drums, 0, 58, 62));
    XSFLIB_CHECK(wider.instruments[0].min_key == 58 && wider.instruments[0].max_key == 62 &&
                 wider.instruments[0].regions == std::vector(5, region));
    XSFLIB_CHECK(Through(file, archive, edit::WithDsDrumKeys(wider, 0, 61, 61)).instruments[0].regions ==
                 std::vector{region});

    // The refusals.
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::WithNewDsInstrument(bank, 0, square); }) ==
                 "Program 0 already has an instrument.");
    XSFLIB_CHECK(
        ErrorOf<std::invalid_argument>([&] { edit::WithNewDsInstrument(bank, edit::kHighestDsProgram + 1, square); }) ==
        "Program numbers must be from 0 to 32767.");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::WithDsInstrumentCopy(bank, 1, 2); }) ==
                 "The bank has no instrument at program 1.");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::WithDsKeySplit(one_run, 0); }) ==
                 "Program 0 has more than one region.");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::WithDsSplit(two_runs, 0, 0, 0); }) ==
                 "The region covers keys 0–59. Choose a split key above 0, up to 59 and no lower than 2.");
    XSFLIB_CHECK(
        ErrorOf<std::invalid_argument>([&] { edit::WithDsSplit(two_runs, 0, 0, 1); }).find("no lower than 2") !=
        std::string::npos);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::WithoutDsRun(one_run, 0, 0); }) ==
                 "A key split must have at least one region.");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::WithDsDrumKeys(drums, 0, 62, 58); }) ==
                 "Drum set keys must be from 0 to 127, with the low key first.");
    sdat::Bank full = one_run;
    for (int key = 10; key < 80; key += 10)
    {
        full = edit::WithDsSplit(full, 0, full.instruments[0].regions.size() - 1, key);
    }
    XSFLIB_CHECK(full.instruments[0].regions.size() == 8);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::WithDsSplit(full, 0, 7, 100); }) ==
                 "A key split can have up to 8 regions.");
}

// The wave archives in a bank's slots, and an empty bank file.
void TestBankSlots()
{
    const BankModelFixture fixture = MakeBankModelFixture();
    const File& file = fixture.file;

    // The slots get other wave archives, and an empty bank has no instrument.
    const File slots = edit::WithDsBankSlots(file, 0, {0, 0, kNone, kNone});
    XSFLIB_CHECK(Load(slots)->archive.banks[0]->wave_archives == (std::array<uint16_t, 4>{0, 0, kNone, kNone}));
    CheckFilesKept(hokuto::format::ExportArchive(file), Load(slots)->archive);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::WithDsBankSlots(file, 0, {1, kNone, kNone, kNone}); }) ==
                 "The archive has no wave archive 1.");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::WithDsBankSlots(file, 9, {0, kNone, kNone, kNone}); }) ==
                 "The archive has no bank 9.");
    XSFLIB_CHECK(sdat::Bank::Parse(edit::EmptyDsBankFile()).instruments.empty());
}

// Waves dropped on a DS bank are added after the waves of the wave archive in the first slot, and each makes an
// instrument at a free program or sets the wave of a region or a key split. Several waves on a region, a missing region
// and a slot without a wave archive are refused.
void TestDroppedWaves()
{
    using sdat::InstrumentType;

    const File file = hokuto::format::ImportDsArchive(MakeArchive(true));
    const std::shared_ptr<const song::DsArchive> archive = Load(file);
    const sdat::Bank bank = edit::DsBankModelOf(*archive, 0);
    const std::size_t waves = song::DsWaveCount(*archive, 0);
    const std::vector<std::vector<uint8_t>> incoming = {
        sdat::EncodeWave(std::vector<int16_t>(64, 1000), 22050, sdat::WaveEncoding::kPcm16, std::nullopt),
        sdat::EncodeWave(std::vector<int16_t>(32, -1000), 11025, sdat::WaveEncoding::kPcm8, std::nullopt)};

    // The waves go after the waves of the first slot's wave archive, and each makes an instrument at a free program.
    const edit::DsNewWaves added = edit::DsNewWavePlaces(*archive, 0, incoming.size());
    XSFLIB_CHECK(added.wave_archive == 0);
    SEQLIB_REQUIRE(added.places.size() == 2);
    XSFLIB_CHECK(added.places[0].slot == 0 && added.places[0].wave == waves && added.places[1].wave == waves + 1);
    const edit::DroppedDsWaves listed = edit::WithDroppedDsWaves(bank, std::nullopt, added.places);
    XSFLIB_CHECK(listed.name == "Add Instruments" && listed.programs == std::vector<uint32_t>({1, 2}));

    const File edited =
        edit::WithDsBankModel(edit::WithDsNewWaves(file, *archive, 0, incoming), *archive, 0, listed.bank);
    CheckForms(edited);
    const std::shared_ptr<const song::DsArchive> after = Load(edited);
    XSFLIB_CHECK(song::DsWaveCount(*after, 0) == waves + 2);
    const sdat::Bank read = edit::DsBankModelOf(*after, 0);
    SEQLIB_REQUIRE(read.instruments.size() == 3);
    XSFLIB_CHECK(read.instruments[0].region == bank.instruments[0].region);
    XSFLIB_CHECK(read.instruments[1].type == InstrumentType::kPcm && read.instruments[1].region.wave == waves);
    XSFLIB_CHECK(read.instruments[2].type == InstrumentType::kPcm && read.instruments[2].region.wave == waves + 1);

    // The old waves keep their indexes and their bytes, and the new ones follow them.
    const sdat::WaveArchive before_waves =
        sdat::WaveArchive::Parse(archive->archive.File(archive->archive.wave_archives[0]->file_id));
    const sdat::WaveArchive after_waves =
        sdat::WaveArchive::Parse(after->archive.File(after->archive.wave_archives[0]->file_id));
    for (std::size_t wave = 0; wave < waves; wave++)
    {
        XSFLIB_CHECK(sdat::DecodeWave(before_waves.waves[wave]) == sdat::DecodeWave(after_waves.waves[wave]));
    }
    XSFLIB_CHECK(sdat::DecodeWave(after_waves.waves[waves]) == std::vector<int16_t>(64, 1000));

    // One wave on a region gives the region the wave. A region of any kind becomes a wave. Its other values stay.
    sdat::Bank square = edit::WithNewDsInstrument(bank, 1, edit::NewDsRegion(std::nullopt));
    const edit::DsWavePlace place{0, 1};
    const edit::DroppedDsWaves changed =
        edit::WithDroppedDsWaves(square, std::pair(1u, std::size_t{0}), std::span(&place, 1));
    XSFLIB_CHECK(changed.name == "Change Wave" && changed.programs == std::vector<uint32_t>({1}));
    XSFLIB_CHECK(changed.bank.instruments[1].type == InstrumentType::kPcm &&
                 changed.bank.instruments[1].region.type == InstrumentType::kPcm &&
                 changed.bank.instruments[1].region.wave == 1 &&
                 changed.bank.instruments[1].region.release == square.instruments[1].region.release);

    // A run of a key split gets the wave, and the others stay.
    const sdat::Bank split = edit::WithDsSplit(edit::WithDsKeySplit(bank, 0), 0, 0, 60);
    const sdat::Bank run = edit::WithDsRegionWave(split, 0, 1, place);
    XSFLIB_CHECK(run.instruments[0].type == InstrumentType::kKeySplit && run.instruments[0].regions[1].wave == 1 &&
                 run.instruments[0].regions[0] == split.instruments[0].regions[0]);

    // Several waves on a region, a region that is missing from the instrument, and a slot without a wave archive are
    // refused.
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>(
                     [&] { edit::WithDroppedDsWaves(bank, std::pair(0u, std::size_t{0}), added.places); }) ==
                 "A region plays one wave. Drag one wave onto the region, or several onto the program list.");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::WithDsRegionWave(bank, 0, 1, place); }) ==
                 "Program 0 has no region 1.");
    const File no_slots = edit::WithDsBankSlots(file, 0, {kNone, kNone, kNone, kNone});
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::DsNewWavePlaces(*Load(no_slots), 0, 1); }) ==
                 "The bank has no wave archive. Choose one under Wave Archives… first.");
    XSFLIB_CHECK(edit::DsSlotOfWaveArchive(*archive, 0, 0) == uint16_t{0});
    XSFLIB_CHECK(!edit::DsSlotOfWaveArchive(*Load(no_slots), 0, 0));
}

// A wave archive of more than 2 MiB loads its waves one at a time.
void TestLargeWaveArchive()
{
    const File file = hokuto::format::ImportDsArchive(MakeArchive(true));
    const files::Wave large{.data = files::Bytes(0x200000, 0x40)};
    const File added = edit::AddDsWaveArchive(file, "WAVES_LARGE", files::WaveArchive({large}));
    const std::shared_ptr<const song::DsArchive> archive = Load(added);
    XSFLIB_CHECK(archive->archive.wave_archives[1]->file_id >> 24 == 1);
    XSFLIB_CHECK(archive->archive.wave_archives[0]->file_id >> 24 == 0);
    XSFLIB_CHECK(song::DsWaveCount(*archive, 1) == 1);
}

// A DS sequence that selects a program missing from its bank gets a warning. A bank that has the program accepts the
// change without one.
void TestWarnings()
{
    const std::shared_ptr<const song::DsArchive> archive = song::LoadDsArchive(MakeArchive(true), {});

    // The bank has program 0. A program change to 5 selects a program that is missing from the bank.
    const files::Bytes held = files::Sequence({0x81, 0x00, 60, 0x7F, 0x18, 0xFF});
    XSFLIB_CHECK(edit::DsSequenceWarnings(*archive, 0, held).empty());
    const files::Bytes lacked = files::Sequence({0x81, 0x05, 60, 0x7F, 0x18, 0xFF});
    const std::vector<std::string> warnings = edit::DsSequenceWarnings(*archive, 0, lacked);
    XSFLIB_CHECK(warnings.size() == 1);
    XSFLIB_CHECK(warnings[0] == "Programs missing from BANK_ONE: 5. Their notes will be silent.");

    // A bank that has program 5 accepts that program change without a warning.
    const files::Instrument played{.type = 1, .region = {.wave = 0}};
    const files::Bytes bank = files::Bank({played, {}, {}, {}, {}, played});
    const std::shared_ptr<const song::DsArchive> holding = song::LoadDsArchive(MakeArchive(true, bank), {});
    XSFLIB_CHECK(edit::DsSequenceWarnings(*holding, 0, lacked).empty());
}

// Additions to a DS archive that isn't laid out as the tools lay one out are refused with a message.
void TestRefusals()
{
    const File file = hokuto::format::ImportDsArchive(MakeArchive(true));
    const files::Bytes sequence = NoteOf(64);
    const auto refused = [&](const std::function<void()>& call)
    {
        return ErrorOf<std::invalid_argument>(call);
    };
    XSFLIB_CHECK(refused([&] { edit::AddDsSequenceSound(file, "", sequence, {}); }) ==
                 "Enter a name for the new item.");
    XSFLIB_CHECK(refused([&] { edit::AddDsSequenceSound(file, "SEQ_TWO", sequence, {}); }) ==
                 "The archive already has an item named SEQ_TWO.");
    XSFLIB_CHECK(refused([&] { edit::AddDsSequenceSound(file, "SE_TWO", sequence, {}); }) ==
                 "The archive already has an item named SE_TWO.");
    XSFLIB_CHECK(refused([&] { edit::AddDsSequenceSound(file, "SEQ_NEW", sequence, {.bank = 7}); }) ==
                 "The archive has no bank 7.");
    XSFLIB_CHECK(refused([&] { edit::AddDsSequenceSound(file, "SEQ_NEW", sequence, {.player = 7}); }) ==
                 "The archive has no player 7.");
    XSFLIB_CHECK(refused([&] { edit::AddDsSequenceSound(file, "SEQ_NEW", files::Bytes(40, 0xAB), {}); }) ==
                 "Can't read the file as an SSEQ sequence file.");
    XSFLIB_CHECK(!refused([&] { edit::AddDsBank(file, "BANK_NEW", files::Bytes(40, 0xAB), {}); }).empty());
    XSFLIB_CHECK(refused([&] { edit::AddDsBank(file, "BANK_NEW", BankOfWave(0), {7, kNone, kNone, kNone}); }) ==
                 "The archive has no wave archive 7.");
    XSFLIB_CHECK(!refused([&] { edit::AddDsWaveArchive(file, "WAVES_NEW", files::Bytes(40, 0xAB)); }).empty());

    // A file that shares its bytes with another isn't laid out as the tools lay one out.
    std::vector<uint8_t> shared = MakeArchive(true);
    const sdat::Archive parsed = sdat::Archive::Parse(shared);
    const uint32_t fat = xsflib::LoadLe32(&shared[0x20]);
    xsflib::StoreLe32(&shared[fat + 12 + 1 * 16], parsed.files[0].offset);
    xsflib::StoreLe32(&shared[fat + 12 + 1 * 16 + 4], parsed.files[0].size);
    const File shared_file = hokuto::format::ImportDsArchive(shared);
    XSFLIB_CHECK(edit::DsAdditionsRefused(shared_file).has_value());
    XSFLIB_CHECK(refused([&] { edit::AddDsWaveArchive(shared_file, "WAVES_NEW", Waves()); }) ==
                 "The archive doesn't use the standard SDK layout. Hokuto can't add items to it.");
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("ds_new_items",
                                    []
                                    {
                                        TestSoundNumbers();
                                        TestNewSound(true);
                                        TestNewSound(false);
                                        TestNewBank();
                                        TestNewWaveArchive();
                                        TestEmptySong();
                                        TestNewFile();
                                        TestBankInstruments();
                                        TestBankKeySplits();
                                        TestBankSlots();
                                        TestDroppedWaves();
                                        TestLargeWaveArchive();
                                        TestWarnings();
                                        TestRefusals();
                                    });
}
