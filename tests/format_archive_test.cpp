// SPDX-License-Identifier: MIT

// Tests the sound archives of the Switch, the Wii U, the 3DS and the Wii into Hokuto's files and back
// (src/format/archive.cpp). An archive made for the tests holds two sequence files and a file of other bytes. An
// archive with a group whose file holds a copy of a sequence shows how an export measures growth. A 3DS archive comes
// back the same way, with the 3DS's engine. A sequence file that only the group files beside a 3DS archive hold becomes
// one of the file's sequences. An archive of the Wii comes back too, with the values of its tables and the game's build
// of NW4R SND. A sound's bank slots export in a list of another length. An export measures its growth from the game's
// sizes. The group files beside an archive come back cut, on the Switch and on the 3DS.

#include <variant>

#include "format_testing.h"

namespace
{

// The archive from Archive and the file that imports it.
struct ArchiveFixture
{
    std::vector<uint8_t> archive;
    File file;
};

// The fixture of the tests of a Switch archive.
ArchiveFixture MakeArchiveFixture()
{
    const std::vector<uint8_t> archive = Archive();

    const File file = format::ImportSwitchArchive(archive);

    return {archive, file};
}

// A Switch archive imports into a file and exports back byte for byte, with the group files that the import is given.
void TestArchiveImport()
{
    const ArchiveFixture fixture = MakeArchiveFixture();
    const std::vector<uint8_t>& archive = fixture.archive;
    const File& file = fixture.file;

    // The sequences in units, no effects, the pieces, the tables, the game's sizes, and the archive back.
    SEQLIB_REQUIRE(file.engine == format::Engine::kSwitch && file.sequences.size() == 2 && file.native);
    XSFLIB_CHECK(file.aux_effects == decltype(file.aux_effects){});
    XSFLIB_CHECK(file.sequences[0]->items.size() == 5 && file.sequences[0]->items[0].value.value == 100 * kOne);
    NativeTables tables;
    for (const char* name : {"Song", "Entry", "Other"})
    {
        tables.sounds.push_back({.name = name, .kind = SoundType::kSequence, .player = 0x04000000, .volume = 100});
    }
    tables.players = {{"PLAYER", 1}};
    XSFLIB_CHECK(file.native->tables == tables && file.native->group_files.empty());
    XSFLIB_CHECK((file.native->game_sizes == GameSizes{archive.size(), {}}));
    XSFLIB_CHECK(format::ExportArchive(file) == archive);

    // The file keeps the group files that the import is given. A group file that isn't one of the console's stays one
    // run, and an empty one has no pieces. The export gives them back.
    const std::vector<std::vector<uint8_t>> kGroupFiles = {{1, 2, 3}, {}};
    const File grouped = format::ImportSwitchArchive(archive, kGroupFiles);
    XSFLIB_CHECK(format::ExportGroupFiles(grouped) == kGroupFiles && grouped.native->group_files.size() == 2);
    if (grouped.native->group_files.size() == 2)
    {
        const NativeGroupFile& first = grouped.native->group_files[0];
        XSFLIB_CHECK(!first.group && first.game_size == 3 && first.fields.empty());
        XSFLIB_CHECK((*first.pieces == std::vector<ntdlib::sar::Piece>{std::vector<uint8_t>{1, 2, 3}}));
        XSFLIB_CHECK(grouped.native->group_files[1].pieces->empty() &&
                     grouped.native->group_files[1].game_hash == kEmptyHash);
    }
}

// Both forms give the file back, and the text form may start with a byte-order mark.
void TestArchiveForms()
{
    const ArchiveFixture fixture = MakeArchiveFixture();
    const File& file = fixture.file;

    // Both forms give the file back.
    const std::vector<uint8_t> binary = format::Write(file);
    const std::string text = format::WriteText(file);
    XSFLIB_CHECK(format::IsBinary(binary) && !format::IsText(binary));
    const std::vector<std::string> kIds = {"ENGN", "SEQS", "NATV", "TABL", "SIZE"};
    XSFLIB_CHECK(ChunkIds(binary) == kIds);
    XSFLIB_CHECK(format::Read(binary) == file && format::ReadText(text) == file);
    XSFLIB_CHECK(format::ReadEitherForm(binary) == file);
    XSFLIB_CHECK(format::ReadEitherForm(std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size())) == file);

    // A UTF-8 byte-order mark may start the text form. Some editors on Windows add one.
    const std::string marked = "\xEF\xBB\xBF" + text;
    const std::span<const uint8_t> marked_bytes(reinterpret_cast<const uint8_t*>(marked.data()), marked.size());
    XSFLIB_CHECK(format::IsText(marked_bytes) && format::ReadEitherForm(marked_bytes) == file);
}

// Edits of the sequences and the tables export.
void TestArchiveEdits()
{
    const ArchiveFixture fixture = MakeArchiveFixture();
    const std::vector<uint8_t>& archive = fixture.archive;
    const File& file = fixture.file;

    // Edits export. A longer wait moves the second sound's start, and twenty more volumes make the second sequence file
    // 32 bytes larger.
    File longer = file;
    longer.sequences[0].Edit([](Sequence& sequence) { sequence.items[1].value.value = 200 * kOne; });
    const std::vector<uint8_t> moved = format::ExportArchive(longer);
    ntdlib::sar::Options options;
    options.flavor = Flavor::kNx;
    XSFLIB_CHECK(moved.size() == archive.size());
    XSFLIB_CHECK(ntdlib::sar::SoundArchive::Load(moved, options).Sounds()[1].start_offset == 6);
    XSFLIB_CHECK(format::ImportSwitchArchive(moved).sequences == longer.sequences);
    File more = file;
    const auto add_volumes = [](Sequence& sequence)
    {
        sequence.items.insert(sequence.items.begin() + 1, 20, sequence.items[0]);
    };
    more.sequences[1].Edit(add_volumes);
    const std::vector<uint8_t> grown = format::ExportArchive(more);
    XSFLIB_CHECK(grown.size() == archive.size() + 32);
    XSFLIB_CHECK(format::ImportSwitchArchive(grown).sequences == more.sequences);
    XSFLIB_CHECK(Throws<std::invalid_argument>([] { format::ExportArchive(File{}); }));

    // A sound's volume and a player's sound limit export.
    File louder = file;
    const auto raise = [](NativeArchive& native)
    {
        native.tables->sounds[2].volume = 64;
        native.tables->players[0].sound_limit = 3;
    };
    louder.native.Edit(raise);
    const std::vector<uint8_t> loud = format::ExportArchive(louder);
    XSFLIB_CHECK(loud.size() == archive.size());
    XSFLIB_CHECK(format::ImportSwitchArchive(loud).native->tables == louder.native->tables);
    XSFLIB_CHECK(format::ExportArchive(format::ImportSwitchArchive(loud)) == loud);
}

// An export refuses a pan and bank slots that the archive has no place for, a new name and a missing player.
void TestArchiveRefusals()
{
    const ArchiveFixture fixture = MakeArchiveFixture();
    const File& file = fixture.file;

    // A place that the archive can't take, a new name and a missing player don't. NintendoWare's tools didn't lay the
    // archive out.
    const auto refused = [](const File& edited)
    {
        return ErrorOf<std::invalid_argument>([&] { format::ExportArchive(edited); });
    };

    File panned = file;
    panned.native.Edit([](NativeArchive& native) { native.tables->sounds[0].pan_mode = 1; });
    XSFLIB_CHECK(refused(panned) == "Can't add a field for the pan of sound Song to the archive tables.");
    File unheld = file;
    unheld.native.Edit([](NativeArchive& native) { native.tables->sounds[0].banks = {0x03000000}; });
    XSFLIB_CHECK(refused(unheld) == "Can't write the bank slots of sound Song to the archive tables.");
    File renamed = file;
    renamed.native.Edit([](NativeArchive& native) { native.tables->sounds[1].name = "Start"; });
    XSFLIB_CHECK(refused(renamed).starts_with(
        "The archive tables have sound Entry (sequence) where the file tables have sound Start (sequence)."));
    File fewer = file;
    fewer.native.Edit([](NativeArchive& native) { native.tables->players.clear(); });
    XSFLIB_CHECK(
        refused(fewer).starts_with("The archive tables have player PLAYER where the file tables have nothing more."));
}

// An archive whose string table is damaged imports without tables and exports unchanged.
void TestArchiveDamagedStrings()
{
    const ArchiveFixture fixture = MakeArchiveFixture();
    const std::vector<uint8_t>& archive = fixture.archive;

    // An archive whose string table counts more names than it holds imports without tables, and exports unchanged. The
    // game's sizes hold its size alone.
    std::vector<uint8_t> damaged = archive;
    const std::string_view kStrg = "STRG";
    const auto block = static_cast<std::size_t>(
        std::search(damaged.begin(), damaged.end(), kStrg.begin(), kStrg.end()) - damaged.begin());
    xsflib::StoreLe32(&damaged[block + 8 + xsflib::LoadLe32(&damaged[block + 12])], 0x7FFFFFFF);
    const File without = format::ImportSwitchArchive(damaged);
    XSFLIB_CHECK(!without.native->tables && format::ExportArchive(without) == damaged);
    XSFLIB_CHECK((without.native->game_sizes == GameSizes{damaged.size(), {}}));
}

// The song of the Wii's test archive: a volume of 100, key 60 for 48 ticks, a wait of 48 ticks, and the end.
const std::vector<uint8_t> kWiiSong = {0xC1, 0x64, 0x3C, 0x64, 0x30, 0x80, 0x30, 0xFF};

// An RSEQ file with `bytecode` and a label at its start.
std::vector<uint8_t> WiiSequence(const std::vector<uint8_t>& bytecode)
{
    ntdlib::rsar::SequenceFile file;
    file.data = bytecode;
    file.labels = {{"SEQ_SONG", 0}};

    return file.Write();
}

// An archive of the Wii with `song`, a bank and its wave data in one group. The layout follows NW4R's tools.
std::vector<uint8_t> WiiArchive(const std::vector<uint8_t>& song, uint16_t version = 0x0104)
{
    const std::vector<uint8_t> kBankBytes(0x40, 0x5A);
    const std::vector<uint8_t> kWaveBytes(0x20, 0xA5);
    return ntdlib::testing::rsar_tools::MakeArchive(version, {{song}, {kBankBytes, kWaveBytes}}, {{"SEQ_SONG", 0, 0}},
                                                    {{0, 1}});
}

// An archive of the Wii, in both forms. The file takes the Wii's engine, the archive's size as the game's size and the
// values of the archive's tables. Edits export, and the files after a longer song move. The functions for the later
// consoles' archives refuse it. An import refuses three bytes that aren't an archive.
void TestWiiArchive()
{
    const std::vector<uint8_t> archive = WiiArchive(WiiSequence(kWiiSong));

    const File file = format::ImportWiiArchive(archive);

    SEQLIB_REQUIRE(file.engine == format::Engine::kWii && file.native && file.native->engine == format::Engine::kWii);
    XSFLIB_CHECK(file.sequences.size() == 1 && file.sequences[0]->items.size() == 4);
    XSFLIB_CHECK((file.native->game_sizes == GameSizes{archive.size(), {}}));

    // The sound's player and bank take the item IDs of the later consoles. The archive's tables hold no banks and no
    // players, and the Wii's tables hold no wave archives.
    const NativeSound kSound{
        .name = "SEQ_SONG", .kind = SoundType::kSequence, .player = 0x04000000, .banks = {0x03000000}};
    SEQLIB_REQUIRE(file.native->tables && (file.native->tables->sounds == std::vector<NativeSound>{kSound}));
    XSFLIB_CHECK(file.native->tables->banks.empty() && file.native->tables->wave_archives.empty() &&
                 file.native->tables->players.empty() && file.native->tables->groups.empty());
    XSFLIB_CHECK(format::ExportArchive(file) == archive && format::ExportGroupFiles(file).empty());
    XSFLIB_CHECK(format::BuildOfWiiArchive(*file.native).date == 20080620);
    XSFLIB_CHECK(
        format::BuildOfWiiArchive(*format::ImportWiiArchive(WiiArchive(WiiSequence(kWiiSong), 0x0101)).native).date ==
        20070101);

    // Both forms give the file back. The engine and the archive take number 4 and the name wii.
    const std::vector<uint8_t> binary = format::Write(file);
    const std::string text = format::WriteText(file);
    XSFLIB_CHECK(format::Read(binary) == file && format::ReadText(text) == file);
    XSFLIB_CHECK(format::CodeOf(format::Engine::kWii) == 4 && format::EngineNamed("wii") == format::Engine::kWii);
    XSFLIB_CHECK(text.find("\nengine wii\n") != std::string::npos && text.find("\nnative wii\n") != std::string::npos);

    // Twenty more volumes make the song's file 32 bytes larger. The tables follow, and the bank and its wave data keep
    // their bytes.
    File more = file;
    const auto lengthen = [](Sequence& sequence)
    {
        sequence.items.insert(sequence.items.begin() + 1, 20, sequence.items[0]);
    };
    more.sequences[0].Edit(lengthen);
    std::vector<uint8_t> longer = kWiiSong;
    for (int i = 0; i < 20; ++i)
    {
        longer.insert(longer.begin() + 2, {0xC1, 0x64});
    }
    const std::vector<uint8_t> grown = format::ExportArchive(more);
    XSFLIB_CHECK(grown.size() == archive.size() + 32 && grown == WiiArchive(WiiSequence(longer)));
    XSFLIB_CHECK(format::ImportWiiArchive(grown).sequences == more.sequences);

    // The sound's start lies in the one field that names it.
    const std::vector<std::optional<uint32_t>> starts = format::StartFieldsOfSounds(file, archive);
    XSFLIB_CHECK(starts.size() == 1 && starts[0].has_value());

    // The functions for the later consoles' archives refuse a Wii archive.
    ntdlib::sar::Options options;
    options.flavor = Flavor::kNx;
    const ntdlib::sar::SoundArchive other = ntdlib::sar::SoundArchive::Load(Archive(), options);
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { format::SequencesOfFiles(file, archive, other); }));
    XSFLIB_CHECK(!format::TableValuePlacesOf(*file.native));
    XSFLIB_CHECK(Throws<xsflib::FormatError>([] { format::ImportWiiArchive(std::vector<uint8_t>{1, 2, 3}); }));
}

// The values of a Wii archive's tables, edited in a file. An export writes each one in place: nothing moves, and the
// archive differs from the game's in the bytes of the edited values alone. The import reads them back, and both forms
// keep them. An archive before version 1.2 has no place for a pan mode, and one before version 1.3 none for the release
// priority fix. The export refuses them there, and writes the other values.
void TestWiiTables()
{
    const std::vector<uint8_t> archive = WiiArchive(WiiSequence(kWiiSong));
    File file = format::ImportWiiArchive(archive);
    file.native.Edit(
        [](NativeArchive& native)
        {
            NativeSound& sound = native.tables->sounds[0];
            sound.volume = 100;
            sound.player_priority = 70;
            sound.pan_mode = 1;
            sound.pan_curve = 2;
            sound.banks = {0x03000003};
            sound.tracks = 0x5;
            sound.channel_priority = 90;
            sound.release_priority_fix = true;
        });

    const std::vector<uint8_t> edited = format::ExportArchive(file);

    XSFLIB_CHECK(edited.size() == archive.size());
    XSFLIB_CHECK(std::ranges::count_if(std::views::iota(std::size_t{0}, std::min(edited.size(), archive.size())),
                                       [&](std::size_t i) { return edited[i] != archive[i]; }) == 8);
    XSFLIB_CHECK(format::ImportWiiArchive(edited).native->tables == file.native->tables);
    XSFLIB_CHECK(format::Read(format::Write(file)) == file);
    XSFLIB_CHECK(format::ReadText(format::WriteText(file)) == file);

    // Version 1.1 reads the balance as the pan mode of each sound, without the release priority fix.
    const std::vector<uint8_t> old_archive = WiiArchive(WiiSequence(kWiiSong), 0x0101);
    const File old_file = format::ImportWiiArchive(old_archive);
    SEQLIB_REQUIRE(old_file.native->tables && old_file.native->tables->sounds.size() == 1);
    XSFLIB_CHECK(old_file.native->tables->sounds[0].pan_mode == 1);

    const auto refused = [](const File& wii)
    {
        return ErrorOf<std::invalid_argument>([&] { format::ExportArchive(wii); });
    };

    File panned = old_file;
    panned.native.Edit([](NativeArchive& native) { native.tables->sounds[0].pan_mode = 0; });
    XSFLIB_CHECK(refused(panned) == "The archive tables have no field for the pan mode of sound SEQ_SONG.");
    File fixed = old_file;
    fixed.native.Edit([](NativeArchive& native) { native.tables->sounds[0].release_priority_fix = true; });
    XSFLIB_CHECK(refused(fixed) == "The archive tables have no field for the release priority fix of sound SEQ_SONG.");
    File louder = old_file;
    louder.native.Edit([](NativeArchive& native) { native.tables->sounds[0].volume = 120; });
    const std::vector<uint8_t> loud = format::ExportArchive(louder);
    XSFLIB_CHECK(loud.size() == old_archive.size() &&
                 format::ImportWiiArchive(loud).native->tables == louder.native->tables);
}

// Without the game's main.dol, the version of an archive sets the build: June 2008 from version 1.4 on, and otherwise a
// build of 2007. The build of 2007 keeps a date, so that ntdlib's model doesn't take it for a build without a banner.
// Such a build plays as the build of October 2006 (ntdlib::SndBuildWithAx). Anchor: ntdlib's rule that a date of 0
// marks a build without a banner.
void TestWiiBuildOfVersion()
{
    const ntdlib::SndBuild older = format::BuildOfWiiVersion(0x0102);
    const ntdlib::SndBuild later = format::BuildOfWiiVersion(0x0104);

    XSFLIB_CHECK(older.date != 0 && !older.SinceMarch2008());
    XSFLIB_CHECK(!ntdlib::SndBuildWithAx(older, ntdlib::AxBuild::kDecember2006).october_2006);
    XSFLIB_CHECK(later.date == 20080620 && later.SinceMarch2008());
    XSFLIB_CHECK(!ntdlib::SndBuildWithAx(later, ntdlib::AxBuild::kDecember2006).october_2006);
}

// A Wii file keeps the game's build of NW4R SND in both forms, where its main.dol gave one: a date, and the flag of the
// build that comes with AX of October 2006. The build then takes the place of the archive's version's, and the archive
// comes back byte for byte. A build belongs to an archive of the Wii alone, and a text form refuses one elsewhere.
void TestWiiBuild()
{
    const std::vector<uint8_t> archive = WiiArchive(WiiSequence(kWiiSong));
    const ntdlib::SndBuild kWiiSports{.date = 20070608};
    const ntdlib::SndBuild kSmoothMoves{.date = 0, .october_2006 = true};
    const File sports = format::ImportWiiArchive(archive, kWiiSports);
    const File moves = format::ImportWiiArchive(archive, kSmoothMoves);
    const std::string sports_text = format::WriteText(sports);
    const std::string moves_text = format::WriteText(moves);

    XSFLIB_CHECK(sports.native && sports.native->wii_build == kWiiSports);
    XSFLIB_CHECK(format::BuildOfWiiArchive(*sports.native) == kWiiSports);
    XSFLIB_CHECK(format::BuildOfWiiArchive(*moves.native) == kSmoothMoves);
    XSFLIB_CHECK(format::ExportArchive(sports) == archive && format::ExportArchive(moves) == archive);
    XSFLIB_CHECK(format::Read(format::Write(sports)) == sports);
    XSFLIB_CHECK(format::Read(format::Write(moves)) == moves);
    XSFLIB_CHECK(format::ReadText(sports_text) == sports && format::ReadText(moves_text) == moves);
    XSFLIB_CHECK(sports_text.find("\n    build 20070608\n") != std::string::npos);
    XSFLIB_CHECK(moves_text.find("\n    build 0 october_2006\n") != std::string::npos);
    XSFLIB_CHECK(format::WriteText(format::ImportWiiArchive(archive)).find(" build ") == std::string::npos);

    File other = format::ImportArchive(Archive(), Flavor::kNx);
    other.native.Edit([&](NativeArchive& native) { native.wii_build = kWiiSports; });
    std::string switch_text = format::WriteText(format::ImportArchive(Archive(), Flavor::kNx));
    const std::size_t native = switch_text.find("\nnative switch\n");
    XSFLIB_CHECK(native != std::string::npos);
    if (native != std::string::npos)
    {
        switch_text.insert(native + std::string("\nnative switch\n").size(), "    build 20070608\n");
    }
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { format::Write(other); }));
    XSFLIB_CHECK(Throws<xsflib::FormatError>([&] { format::ReadText(switch_text); }));
}

// A 3DS archive imports with the 3DS's engine, and comes back byte for byte through both forms. sar_files.h doesn't lay
// it out as NintendoWare's tools do. The model form therefore refuses it.
void Test3dsArchive()
{
    sar_files::ArchiveSpec spec;
    spec.sounds = {{.name = "Song", .file = 0}, {.name = "Entry", .file = 0, .start_offset = 5}};
    spec.files = {{.bytes = sar_files::Sequence(Flavor::kCtr, kSong, {{"Song", 0}})}, {.bytes = {1, 2, 3, 4, 5}}};
    const std::vector<uint8_t> archive = sar_files::Archive(Flavor::kCtr, spec);

    const File file = format::ImportArchive(archive, Flavor::kCtr);
    SEQLIB_REQUIRE(file.engine == format::Engine::k3ds && file.native && FlavorOf(*file.native) == Flavor::kCtr);
    SEQLIB_REQUIRE(file.sequences.size() == 1 && file.sequences[0]->items.size() == 5);
    XSFLIB_CHECK(file.sequences[0]->items[0].kind == Kind::kVolume && file.sequences[0]->items[3].kind == Kind::kNote);
    XSFLIB_CHECK(file.native->tables && file.native->tables->sounds.size() == 2 &&
                 file.native->tables->players.empty());
    XSFLIB_CHECK(format::ExportArchive(file) == archive);

    const std::vector<uint8_t> binary = format::Write(file);
    const std::string text = format::WriteText(file);
    XSFLIB_CHECK(text.find("\nengine 3ds\n") != std::string::npos);
    XSFLIB_CHECK(format::Read(binary) == file && format::ReadText(text) == file);
    XSFLIB_CHECK(format::ExportArchive(format::ReadText(text)) == archive);

    XSFLIB_CHECK(Throws<xsflib::FormatError>([&] { format::InModelForm(file); }));
}

// A sound's bank slots export in a list of another length. The list ends the sound's entry, and the entry grows or
// shrinks with it. That requires an archive laid out as NintendoWare's tools lay one out.
void TestBankSlots()
{
    constexpr uint32_t kBank = 0x03000000;
    constexpr uint32_t kEmpty = 0xFFFFFFFF;
    sar_files::ArchiveSpec spec;
    spec.sounds = {{.name = "Song", .file = 0, .banks = {0}}, {.name = "Entry", .file = 0, .start_offset = 5}};
    spec.banks = {{"BANK", 1}};
    spec.players = {{"PLAYER", 1}};
    spec.files = {{.bytes = sar_files::Sequence(Flavor::kNx, kSong, {{"Song", 0}})}, {.bytes = {1, 2, 3, 4, 5}}};
    const std::vector<uint8_t> archive = sar_tools::Archive(Flavor::kNx, spec);
    const File file = format::ImportSwitchArchive(archive);
    const auto longer = [](NativeArchive& native)
    {
        native.tables->sounds[0].banks = {kEmpty, kBank};
        native.tables->sounds[1].banks = {kBank};
    };
    const auto shorter = [](NativeArchive& native)
    {
        native.tables->sounds[0].banks = {kBank};
        native.tables->sounds[1].banks.clear();
    };

    File grown = file;
    grown.native.Edit(longer);
    const std::vector<uint8_t> grown_archive = format::ExportArchive(grown);
    File back = format::ImportSwitchArchive(grown_archive);
    back.native.Edit(shorter);

    const File read = format::ImportSwitchArchive(grown_archive);
    XSFLIB_CHECK(read.native->tables == grown.native->tables && read.sequences == file.sequences);
    XSFLIB_CHECK(format::ExportArchive(back) == archive);
}

// An export measures how much the archive grew from the sizes of another, and the files of each group that grew. An
// archive whose groups ntdlib can't read gives only its size.
void TestGrowth()
{
    const std::vector<uint8_t> song = LongerSong(0);
    const std::vector<uint8_t> longer = LongerSong(20);
    const std::vector<uint8_t> before = GroupArchive(song);
    const std::vector<uint8_t> after = GroupArchive(longer);
    const std::vector<uint8_t> kBroken = {1, 2, 3};
    const GameSizes sizes = format::SizesOf(before, Flavor::kNx);
    const GameSizes broken_sizes = format::SizesOf(kBroken, Flavor::kNx);

    const format::ArchiveGrowth grown = format::MeasureGrowth(sizes, after, Flavor::kNx);
    const format::ArchiveGrowth same = format::MeasureGrowth(sizes, before, Flavor::kNx);
    const format::ArchiveGrowth unread = format::MeasureGrowth(sizes, kBroken, Flavor::kNx);
    const format::ArchiveGrowth from_unread = format::MeasureGrowth(broken_sizes, after, Flavor::kNx);

    XSFLIB_CHECK((sizes == GameSizes{before.size(), {song.size()}}));
    XSFLIB_CHECK((broken_sizes == GameSizes{kBroken.size(), {}}));
    XSFLIB_CHECK(longer.size() > song.size() && after.size() > before.size());
    XSFLIB_CHECK(grown.before == before.size() && grown.after == after.size());
    XSFLIB_CHECK(grown.groups == std::vector<format::GroupGrowth>({{"GROUP_SONG", song.size(), longer.size()}}));
    XSFLIB_CHECK(same.before == same.after && same.groups.empty());
    XSFLIB_CHECK(unread.before == before.size() && unread.after == kBroken.size() && unread.groups.empty());
    XSFLIB_CHECK(from_unread.before == kBroken.size() && from_unread.after == after.size() &&
                 from_unread.groups.empty());
}

// An archive whose group loads a copy of a song, the file that imports it, and that file in both forms.
struct GameSizesFixture
{
    std::vector<uint8_t> song;
    std::vector<uint8_t> longer;
    std::vector<uint8_t> archive;
    std::vector<uint8_t> grown;
    File file;
    File largest;
    File without;
    std::vector<uint8_t> binary;
    std::string text;
    File saved;
    format::ArchiveGrowth growth;
};

// The fixture of the tests of the game's sizes.
GameSizesFixture MakeGameSizesFixture()
{
    const std::vector<uint8_t> song = LongerSong(0);
    const std::vector<uint8_t> longer = LongerSong(20);
    const std::vector<uint8_t> archive = GroupArchive(song);
    const std::vector<uint8_t> grown = GroupArchive(longer);
    const File file = format::ImportSwitchArchive(archive);
    File largest = file;
    largest.native.Edit([](NativeArchive& native) { native.game_sizes = GameSizes{UINT64_MAX, {0}}; });
    File without = file;
    without.native.Edit([](NativeArchive& native) { native.game_sizes.reset(); });

    const std::vector<uint8_t> binary = format::Write(file);
    const std::string text = format::WriteText(file);
    const File saved = format::Read(binary);
    const format::ArchiveGrowth growth = format::MeasureGrowth(*saved.native->game_sizes, grown, Flavor::kNx);

    return {song, longer, archive, grown, file, largest, without, binary, text, saved, growth};
}

// The game's sizes, in both forms. The import measures the archive and the files that its group loads: a copy of the
// song. SIZE follows TABL. The sizes stay after a save, and an export measures its growth from them.
void TestGameSizesForms()
{
    const GameSizesFixture fixture = MakeGameSizesFixture();
    const std::vector<uint8_t>& song = fixture.song;
    const std::vector<uint8_t>& longer = fixture.longer;
    const std::vector<uint8_t>& archive = fixture.archive;
    const std::vector<uint8_t>& grown = fixture.grown;
    const File& file = fixture.file;
    const File& largest = fixture.largest;
    const File& without = fixture.without;
    const std::vector<uint8_t>& binary = fixture.binary;
    const std::string& text = fixture.text;
    const File& saved = fixture.saved;
    const format::ArchiveGrowth& growth = fixture.growth;

    XSFLIB_CHECK((file.native->game_sizes == GameSizes{archive.size(), {song.size()}}));
    XSFLIB_CHECK(saved == file && format::ReadText(text) == file);
    XSFLIB_CHECK(format::WriteText(format::ReadText(text)) == text);
    for (const File& other : {largest, without})
    {
        XSFLIB_CHECK(format::Read(format::Write(other)) == other);
        XSFLIB_CHECK(format::ReadText(format::WriteText(other)) == other);
    }
    XSFLIB_CHECK(growth.before == archive.size() && growth.after == grown.size());
    XSFLIB_CHECK(growth.groups == std::vector<format::GroupGrowth>({{"GROUP_SONG", song.size(), longer.size()}}));

    // The archive's size in 8 bytes, the number of groups in 4, and each group's size in 8. A file without the game's
    // sizes has no SIZE chunk.
    std::vector<uint8_t> expected(20);
    xsflib::StoreLe64(&expected[0], archive.size());
    xsflib::StoreLe32(&expected[8], 1);
    xsflib::StoreLe64(&expected[12], song.size());
    const std::vector<std::string> kIds = {"ENGN", "SEQS", "NATV", "TABL", "SIZE"};
    XSFLIB_CHECK(ChunkIds(binary) == kIds);
    XSFLIB_CHECK(std::ranges::equal(ChunkBytes(binary, 4), expected));
    XSFLIB_CHECK(ChunkIds(format::Write(without)).size() == kIds.size() - 1);
}

// The game_size line follows the native line, and the group's line gives the group's size.
void TestGameSizesText()
{
    const GameSizesFixture fixture = MakeGameSizesFixture();
    const std::vector<uint8_t>& song = fixture.song;
    const std::vector<uint8_t>& archive = fixture.archive;
    const File& without = fixture.without;
    const std::string& text = fixture.text;

    // The lines, then lines that a reader refuses: a second game_size line, one after the tables, a group's size
    // without the game_size line, a group's line without its size, and a size below 0. Each message names the changed
    // line.
    const std::string archive_line = "game_size " + std::to_string(archive.size());
    const std::string group_line = "group GROUP_SONG game_size " + std::to_string(song.size()) + " # ID 0x6000000";
    const std::string bare = format::WriteText(without);
    XSFLIB_CHECK(text.find("\nnative switch\n    " + archive_line + "\n") != std::string::npos);
    XSFLIB_CHECK(text.find("\n        " + group_line + "\n") != std::string::npos);
    const std::size_t archive_at = LineOf(text, archive_line);
    const std::size_t tables_at = LineOf(bare, "    tables\n");
    const std::size_t group_at = LineOf(text, group_line);
    XSFLIB_CHECK(TextError(Replaced(text, archive_line, archive_line + "\n    game_size 1"))
                     .starts_with(LineStart(archive_at + 1) + "the native archive has one game_size line"));
    XSFLIB_CHECK(TextError(Replaced(bare, "    tables\n", "    tables\n    game_size 1\n"))
                     .starts_with(LineStart(tables_at + 1) + "the native archive's game_size comes before the tables"));
    XSFLIB_CHECK(TextError(Replaced(bare, "group GROUP_SONG #", "group GROUP_SONG game_size 1 #"))
                     .starts_with(LineStart(group_at - 1) +
                                  "a group's game_size requires the native archive's game_size line, before the "
                                  "tables"));
    XSFLIB_CHECK(
        TextError(Replaced(text, group_line, "group GROUP_SONG"))
            .starts_with(LineStart(group_at) + "group GROUP_SONG has no game_size, but the native archive has one"));
    XSFLIB_CHECK(TextError(Replaced(text, archive_line, "game_size -1"))
                     .starts_with(LineStart(archive_at) +
                                  "a size has to be a whole number from 0 to 18446744073709551615, not -1"));
}

// The binary form refuses damaged sizes. Neither form writes sizes that don't cover the groups.
void TestGameSizesRefused()
{
    const GameSizesFixture fixture = MakeGameSizesFixture();
    const File& file = fixture.file;
    const std::vector<uint8_t>& binary = fixture.binary;

    // The binary form refuses a SIZE chunk that covers no group, one that ends early, two SIZE chunks, and the game's
    // sizes without a native archive. Byte 8 of SIZE holds the number of groups. The table of chunks holds ENGN, SEQS
    // and NATV first, and each chunk's ID starts its entry.
    XSFLIB_CHECK(Patched(binary, 4, 8, 0) ==
                 "The file is damaged: the game's sizes cover 0 groups, and the tables list 1 group.");
    XSFLIB_CHECK(Patched(binary, 4, 8, 2) == "The file is damaged: the SIZE chunk ends early.");

    const auto renamed = [](std::vector<uint8_t> bytes, std::size_t chunk, std::string_view id)
    {
        std::copy(id.begin(), id.end(), bytes.begin() + 24 + 24 * static_cast<std::ptrdiff_t>(chunk));

        return ErrorOf<xsflib::FormatError>([&] { format::Read(bytes); });
    };

    File loose;
    loose.native = NativeArchive{.pieces = std::vector<ntdlib::sar::Piece>{std::vector<uint8_t>{1, 2, 3}},
                                 .game_sizes = GameSizes{3, {}}};
    XSFLIB_CHECK(renamed(binary, 3, "SIZE") == "The file is damaged: it contains two SIZE chunks.");
    XSFLIB_CHECK(renamed(format::Write(loose), 2, "XTRA") ==
                 "The file is damaged: it contains the game sizes, but no native archive.");
    XSFLIB_CHECK(format::Read(format::Write(loose)) == loose);
    XSFLIB_CHECK(format::ReadText(format::WriteText(loose)) == loose);

    // Neither form writes sizes that don't cover the tables' groups.
    File more = file;
    more.native.Edit([](NativeArchive& native) { native.game_sizes->groups.push_back(1); });
    File untabled = loose;
    untabled.native.Edit([](NativeArchive& native) { native.game_sizes->groups.push_back(1); });
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { format::Write(more); }) ==
                 "the game's sizes cover 2 groups, and the tables list 1 group");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { format::WriteText(more); }) ==
                 "the game's sizes cover 2 groups, and the tables list 1 group");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { format::Write(untabled); }) ==
                 "the game's sizes cover 1 group, and the native archive has no tables");
}
// The group files that a game keeps beside an archive.
struct GroupFilesFixture
{
    std::vector<uint8_t> song;
    std::vector<uint8_t> archive;
    std::vector<uint8_t> group;
    File file;
};

// The fixture of the tests of the group files of a Switch archive.
GroupFilesFixture MakeGroupFilesFixture()
{
    const std::vector<uint8_t> song = LongerSong(0);
    sar_files::ArchiveSpec spec;
    spec.sounds = {{.name = "Song", .file = 0}};
    spec.players = {{"PLAYER", 1}};
    spec.groups = {{"GROUP_SONG", ntdlib::sar::kExternalGroupFile}};
    spec.files = {{.bytes = song, .groups = {0}}, {.group_only = true, .groups = {0}}};
    const std::vector<uint8_t> archive = sar_tools::Archive(Flavor::kNx, spec);
    const std::vector<uint8_t> group = sar_files::Group(Flavor::kNx, {{0, song}, {1, {1, 2, 3, 4, 5}}});

    const File file = format::ImportSwitchArchive(archive, {group});

    return {song, archive, group, file};
}

// An import cuts them: the copy of the archive's sequence file becomes a piece of its sequence, and the other file a
// run. Both forms keep the pieces, the group, and the game's size and SHA-1.
void TestGroupFilesImport()
{
    const GroupFilesFixture fixture = MakeGroupFilesFixture();
    const std::vector<uint8_t>& group = fixture.group;
    const File& file = fixture.file;

    XSFLIB_CHECK(file.sequences.size() == 1 && file.native->group_files.size() == 1);
    if (file.sequences.size() != 1 || file.native->group_files.size() != 1)
    {
        return;
    }
    const NativeGroupFile& imported = file.native->group_files[0];
    const auto is_sequence = [](const ntdlib::sar::Piece& piece)
    {
        return std::holds_alternative<ntdlib::sar::SequencePiece>(piece);
    };
    const auto sequence_pieces = std::ranges::count_if(*imported.pieces, is_sequence);
    XSFLIB_CHECK(imported.group == 0u && imported.game_size == group.size() && sequence_pieces == 1);
    XSFLIB_CHECK(imported.game_hash == xsflib::Sha1::Of(group) && !imported.fields.empty());
    XSFLIB_CHECK(format::ExportGroupFiles(file) == std::vector<std::vector<uint8_t>>{group});
    XSFLIB_CHECK(format::ChangedGroupFiles(file).empty());
    XSFLIB_CHECK(format::Read(format::Write(file)) == file);
    XSFLIB_CHECK(format::ReadText(format::WriteText(file)) == file);
}

// An edit of the sequence reaches the copy, and the group file's tables follow. An export writes a group file that
// changed, into a SARC archive that holds a file of its group if there is one.
void TestGroupFilesEdit()
{
    const GroupFilesFixture fixture = MakeGroupFilesFixture();
    const std::vector<uint8_t>& song = fixture.song;
    const std::vector<uint8_t>& group = fixture.group;
    const File& file = fixture.file;

    if (file.sequences.size() != 1 || file.native->group_files.size() != 1)
    {
        return;
    }

    // 20 more volumes at the sequence's start. The group file's copy takes the archive copy's new bytes.
    File longer = file;
    const auto lengthen = [](Sequence& sequence)
    {
        sequence.items.insert(sequence.items.begin(), 20, sequence.items[0]);
    };
    longer.sequences[0].Edit(lengthen);
    const std::vector<std::vector<uint8_t>> groups = format::ExportGroupFiles(longer);
    const std::vector<format::ChangedGroupFile> changed = format::ChangedGroupFiles(longer);
    ntdlib::sar::Options options;
    options.flavor = Flavor::kNx;
    const ntdlib::sar::SoundArchive read = ntdlib::sar::SoundArchive::Load(format::ExportArchive(longer), options);
    XSFLIB_CHECK(groups.size() == 1 && changed.size() == 1);
    if (groups.size() == 1 && changed.size() == 1)
    {
        const ntdlib::sar::GroupContents contents = ntdlib::sar::ReadGroupFile(groups[0], Flavor::kNx);
        const std::span<const uint8_t> copy = read.FileData(0);
        XSFLIB_CHECK(contents.files.size() == 2 && copy.size() > song.size());
        if (contents.files.size() == 2)
        {
            XSFLIB_CHECK(contents.files[0].copy && std::ranges::equal(*contents.files[0].copy, copy));
            XSFLIB_CHECK(contents.files[1].copy == std::vector<uint8_t>({1, 2, 3, 4, 5}));
        }
        XSFLIB_CHECK(changed[0].index == 0 && changed[0].name == "GROUP_SONG" && changed[0].bytes == groups[0]);

        // An export writes the group file under its group's name, or into the first SARC archive that holds a file of
        // its group, in that file's place. The new group file outgrows its place there, and goes after the next file.
        // The SARC archive goes out under its name. A SARC archive that holds an earlier export's file of the group
        // takes the group file too. A group file whose group Hokuto couldn't find stays out.
        const std::vector<uint8_t> exported = format::ExportArchive(longer);
        const std::vector<FileBeside> alone = format::GroupFileOutputs(changed, exported, Flavor::kNx, {});
        XSFLIB_CHECK(alone.size() == 1 && alone[0].first == "GROUP_SONG.bfgrp" && alone[0].second == groups[0]);
        const std::vector<FileBeside> sarcs = {
            {"Other.sarc", sar_files::Sarc(false, {{"Other.bfgrp", {1, 2, 3}}})},
            {"Song.sarc", sar_files::Sarc(false, {{"Song.bfgrp", group}, {"After.bin", {4, 5, 6}}})},
            {"Again.sarc", sar_files::Sarc(false, {{"Song.bfgrp", group}})}};
        const std::vector<FileBeside> into = format::GroupFileOutputs(changed, exported, Flavor::kNx, sarcs);
        XSFLIB_CHECK(into.size() == 1 && into[0].first == "Song.sarc");
        if (into.size() == 1)
        {
            const std::vector<ntdlib::sar::SarcFile> files = ntdlib::sar::SarcFiles(into[0].second);
            XSFLIB_CHECK(files.size() == 2 && files[0].name == "Song.bfgrp" &&
                         std::ranges::equal(files[0].data, groups[0]) && files[1].name == "After.bin" &&
                         std::ranges::equal(files[1].data, std::vector<uint8_t>{4, 5, 6}));
        }
        const std::vector<FileBeside> earlier = {{"Song.sarc", sar_files::Sarc(false, {{"Song.bfgrp", groups[0]}})}};
        const std::vector<FileBeside> again = format::GroupFileOutputs(changed, exported, Flavor::kNx, earlier);
        XSFLIB_CHECK(again.size() == 1 && again[0].first == "Song.sarc" &&
                     ntdlib::sar::SarcFiles(again[0].second).size() == 1 &&
                     std::ranges::equal(ntdlib::sar::SarcFiles(again[0].second)[0].data, groups[0]));
        std::vector<format::ChangedGroupFile> unknown = changed;
        unknown[0].group.reset();
        XSFLIB_CHECK(format::GroupFileOutputs(unknown, exported, Flavor::kNx, sarcs).empty());
    }
}

// A GRPF chunk in the form of an earlier Hokuto gives each group file as one run of bytes.
void TestGroupFilesEarlierForm()
{
    const GroupFilesFixture fixture = MakeGroupFilesFixture();
    const std::vector<uint8_t>& group = fixture.group;

    // The earlier form: the count, then each group file's size in 8 bytes and its bytes.
    std::vector<uint8_t> earlier = {2, 0, 0, 0};
    for (const std::vector<uint8_t>& bytes : {group, std::vector<uint8_t>()})
    {
        for (int i = 0; i < 8; ++i)
        {
            earlier.push_back(static_cast<uint8_t>(static_cast<uint64_t>(bytes.size()) >> (8 * i)));
        }
        earlier.insert(earlier.end(), bytes.begin(), bytes.end());
    }

    std::vector<Sequence> sequences;
    const std::vector<NativeGroupFile> whole = format::ReadGroupFiles(earlier, sequences);
    XSFLIB_CHECK(whole.size() == 2);
    if (whole.size() == 2)
    {
        XSFLIB_CHECK(!whole[0].group && whole[0].game_size == group.size() && whole[0].fields.empty());
        XSFLIB_CHECK((*whole[0].pieces == std::vector<ntdlib::sar::Piece>{group}));
        XSFLIB_CHECK(whole[0].game_hash == xsflib::Sha1::Of(group) && whole[1].pieces->empty());
    }

    // A field of a group file that lies in a sequence piece.
    std::vector<uint8_t> damaged = kGroupFilesChunk;
    damaged[84] = 1;
    std::vector<Sequence> two(2);
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { format::ReadGroupFiles(damaged, two); }) ==
                 "The file is damaged: field 0 of group file 0 is outside a run of bytes.");
}

// A group that a 3DS game keeps outside its archive. The 3DS's file table holds no lists of groups. Only the label of
// the group's file gives the group file its group. The program finds the file under the group's name. An edit of the
// sequence reaches the group file's copy, and an export writes the new group file under the group's name and .bcgrp.
// Without the label, the group file has no group, and the export leaves it out. A file in the model form keeps the
// group through both forms.
void TestGroupFilesOfThe3ds()
{
    const std::vector<uint8_t> song = sar_files::Sequence(Flavor::kCtr, kSong, {{"Song", 0}});
    sar_files::ArchiveSpec spec;
    spec.sounds = {{.name = "Song", .file = 0}};
    spec.players = {{"PLAYER", 1}};
    spec.groups = {{"GROUP_SONG", ntdlib::sar::kExternalGroupFile}};
    spec.files = {{.bytes = song}};
    const std::vector<uint8_t> archive = sar_tools::Archive(Flavor::kCtr, spec);
    const std::vector<uint8_t> group = sar_files::Group(Flavor::kCtr, {{0, song}});
    const std::vector<std::string> kLabels = {"GROUP_SONG"};

    const auto longer = [](File file)
    {
        const auto lengthen = [](Sequence& sequence)
        {
            sequence.items.insert(sequence.items.begin(), 20, sequence.items[0]);
        };
        file.sequences[0].Edit(lengthen);

        return file;
    };

    const auto outputs = [](const File& file)
    {
        return format::GroupFileOutputs(format::ChangedGroupFiles(file), format::ExportArchive(file), Flavor::kCtr, {});
    };

    const File labelled = format::ImportArchive(archive, Flavor::kCtr, {group}, {}, kLabels);
    const File unlabelled = format::ImportArchive(archive, Flavor::kCtr, {group});
    const File model = format::InModelForm(labelled);

    XSFLIB_CHECK(labelled.native->group_files.size() == 1 && unlabelled.native->group_files.size() == 1);
    if (labelled.native->group_files.size() != 1 || unlabelled.native->group_files.size() != 1)
    {
        return;
    }
    XSFLIB_CHECK(labelled.native->group_files[0].group == 0u && !unlabelled.native->group_files[0].group);
    XSFLIB_CHECK(format::ExportGroupFiles(labelled) == std::vector<std::vector<uint8_t>>{group});
    const std::vector<format::FileBeside> written = outputs(longer(labelled));
    XSFLIB_CHECK(written.size() == 1 && written[0].first == "GROUP_SONG.bcgrp" &&
                 written[0].second == format::ExportGroupFiles(longer(labelled)).at(0) && written[0].second != group);
    XSFLIB_CHECK(outputs(longer(unlabelled)).empty());
    XSFLIB_CHECK(format::ModelOf(model).group_file_groups == std::vector<std::optional<uint32_t>>{0u});
    const File binary = format::Read(format::Write(model));
    const File text = format::ReadText(format::WriteText(model));
    XSFLIB_CHECK(binary.native->group_files.at(0).group == 0u && text.native->group_files.at(0).group == 0u);
    const std::vector<format::FileBeside> from_model = outputs(longer(binary));
    XSFLIB_CHECK(from_model.size() == 1 && from_model[0].first == "GROUP_SONG.bcgrp");
}

// A sequence file that only the group files beside a 3DS archive hold, as in a game that keeps its music in its group
// files.
struct GroupSequencesFixture
{
    sar_files::ArchiveSpec spec;
    std::vector<uint8_t> archive;
    std::vector<std::vector<uint8_t>> groups;
    File file;
    ntdlib::sar::Options options;
};

// The fixture of the tests of a sequence that only group files hold.
GroupSequencesFixture MakeGroupSequencesFixture()
{
    const std::vector<uint8_t> song = sar_files::Sequence(Flavor::kCtr, kSong, {{"Song", 0}});
    sar_files::ArchiveSpec spec;
    spec.sounds = {{.name = "Song", .file = 0}, {.name = "Entry", .file = 0, .start_offset = 5}};
    spec.players = {{"PLAYER", 1}};
    spec.groups = {{"GROUP_A", ntdlib::sar::kExternalGroupFile}, {"GROUP_B", ntdlib::sar::kExternalGroupFile}};
    spec.files = {{.group_only = true}};
    const std::vector<uint8_t> archive = sar_tools::Archive(Flavor::kCtr, spec);
    const std::vector<std::vector<uint8_t>> groups(2, sar_tools::Group(Flavor::kCtr, {.files = {{0, song}}}));
    const std::vector<std::string> kLabels = {"GROUP_A", "GROUP_B"};
    const File file = format::ImportArchive(archive, Flavor::kCtr, groups, {}, kLabels);

    ntdlib::sar::Options options;
    options.flavor = Flavor::kCtr;

    return {spec, archive, groups, file, options};
}

// The import makes it one of the file's sequences, from the first group file's copy. The archive's fields of the
// sounds' starts name items of the sequence, and both forms keep them. SequencesOfFiles finds the file's sequence in
// the group files.
void TestGroupSequencesImport()
{
    const GroupSequencesFixture fixture = MakeGroupSequencesFixture();
    const std::vector<uint8_t>& archive = fixture.archive;
    const std::vector<std::vector<uint8_t>>& groups = fixture.groups;
    const File& file = fixture.file;
    const ntdlib::sar::Options& options = fixture.options;

    XSFLIB_CHECK(file.sequences.size() == 1 && file.native->group_files.size() == 2);
    if (file.sequences.size() != 1 || file.native->group_files.size() != 2)
    {
        return;
    }

    const auto is_sequence = [](const ntdlib::sar::Piece& piece)
    {
        return std::holds_alternative<ntdlib::sar::SequencePiece>(piece);
    };
    XSFLIB_CHECK(std::ranges::none_of(*file.native->pieces, is_sequence));
    for (const NativeGroupFile& group_file : file.native->group_files)
    {
        XSFLIB_CHECK(std::ranges::count_if(*group_file.pieces, is_sequence) == 1);
    }

    // The fields of Song's start and Entry's.
    std::vector<ntdlib::sar::Field> starts;
    std::ranges::copy_if(file.native->fields, std::back_inserter(starts),
                         [](const ntdlib::sar::Field& field) { return field.from.sequence || field.to.sequence; });
    const ntdlib::sar::Place kFirst{0, 0, true, true};
    XSFLIB_CHECK(starts.size() == 2);
    XSFLIB_CHECK((starts.size() == 2 && starts[0].from == kFirst && starts[0].to == kFirst &&
                  starts[1].from == kFirst && starts[1].to == ntdlib::sar::Place{0, 3, true, true}));
    XSFLIB_CHECK(format::ExportArchive(file) == archive && format::ExportGroupFiles(file) == groups);
    const std::string text = format::WriteText(file);
    XSFLIB_CHECK(text.find(" from sequence 0 item 0 to sequence 0 item ") != std::string::npos);
    XSFLIB_CHECK(format::Read(format::Write(file)) == file);
    XSFLIB_CHECK(format::ReadText(text) == file);

    const ntdlib::sar::SoundArchive read = ntdlib::sar::SoundArchive::Load(archive, options);
    XSFLIB_CHECK(format::SequencesOfFiles(file, archive, read) == std::vector<std::optional<uint32_t>>{0u});
}

// Ten volumes before a start move it by 20 bytes, and both group files' copies take the new bytes.
void TestGroupSequencesLonger()
{
    const GroupSequencesFixture fixture = MakeGroupSequencesFixture();
    const std::vector<uint8_t>& archive = fixture.archive;
    const File& file = fixture.file;
    const ntdlib::sar::Options& options = fixture.options;

    if (file.sequences.size() != 1 || file.native->group_files.size() != 2)
    {
        return;
    }

    // Ten volumes after the first.
    File longer = file;
    const auto lengthen = [](Sequence& sequence)
    {
        sequence.items.insert(sequence.items.begin() + 1, 10, sequence.items[0]);
    };
    longer.sequences[0].Edit(lengthen);
    longer.native.Edit(
        [](NativeArchive& native)
        {
            for (ntdlib::sar::Field& field : native.fields)
            {
                field.to.offset += field.to.sequence && field.to.offset > 0 ? 10 : 0;
            }
        });

    std::vector<uint8_t> longer_song;
    for (int i = 0; i < 11; ++i)
    {
        longer_song.insert(longer_song.end(), kSong.begin(), kSong.begin() + 2);
    }
    longer_song.insert(longer_song.end(), kSong.begin() + 2, kSong.end());

    const std::vector<uint8_t> moved = format::ExportArchive(longer);
    const ntdlib::sar::SoundArchive moved_read = ntdlib::sar::SoundArchive::Load(moved, options);
    const std::vector<uint8_t> longer_group =
        sar_tools::Group(Flavor::kCtr, {.files = {{0, sar_files::Sequence(Flavor::kCtr, longer_song, {{"Song", 0}})}}});
    XSFLIB_CHECK(moved.size() == archive.size() && moved_read.Sounds().size() == 2);
    XSFLIB_CHECK(moved_read.Sounds().size() == 2 && moved_read.Sounds()[0].start_offset == 0 &&
                 moved_read.Sounds()[1].start_offset == 25);
    XSFLIB_CHECK(format::ExportGroupFiles(longer) == std::vector<std::vector<uint8_t>>(2, longer_group));
    XSFLIB_CHECK(format::ChangedGroupFiles(longer).size() == 2);
}

// On the Switch, a file in the model form keeps such a sequence. When the sequence changes outside the program, as in
// the text form, the group file's copy takes the new bytes too.
void TestGroupSequencesModelForm()
{
    const GroupSequencesFixture fixture = MakeGroupSequencesFixture();
    const sar_files::ArchiveSpec& spec = fixture.spec;

    // The model form, on the Switch. A volume before the last end.
    sar_files::ArchiveSpec switch_spec = spec;
    switch_spec.groups.pop_back();
    switch_spec.files = {{.group_only = true, .groups = {0}}};
    const std::vector<uint8_t> switch_archive = sar_tools::Archive(Flavor::kNx, switch_spec);
    const std::vector<uint8_t> switch_group =
        sar_tools::Group(Flavor::kNx, {.files = {{0, sar_files::Sequence(Flavor::kNx, kSong, {{"Song", 0}})}}});
    const File model = format::InModelForm(format::ImportArchive(switch_archive, Flavor::kNx, {switch_group}));
    XSFLIB_CHECK(model.sequences.size() == 1 && format::ExportArchive(model) == switch_archive);
    XSFLIB_CHECK(format::ExportGroupFiles(model) == std::vector<std::vector<uint8_t>>{switch_group});
    XSFLIB_CHECK(format::ReadText(format::WriteText(model)) == model);
    File edited = model;
    const auto add_command = [](Sequence& sequence)
    {
        sequence.items.insert(sequence.items.end() - 1, sequence.items[0]);
    };
    edited.sequences[0].Edit(add_command);
    std::vector<uint8_t> edited_song(kSong.begin(), kSong.end() - 1);
    edited_song.insert(edited_song.end(), {0xC1, 0x64, 0xFF});
    const File reread = format::WithModel(edited, format::ModelOf(model));
    XSFLIB_CHECK(format::ExportArchive(reread) == switch_archive);
    XSFLIB_CHECK(format::ExportGroupFiles(reread) ==
                 std::vector<std::vector<uint8_t>>{sar_tools::Group(
                     Flavor::kNx, {.files = {{0, sar_files::Sequence(Flavor::kNx, edited_song, {{"Song", 0}})}}})});
}

// The streams beside the archive: one from the game, and an empty one that the game lacks. Both forms keep them in the
// STRM chunk, after GRPF.
void TestStreams()
{
    File file = Unusual();
    file.native.Edit(
        [](NativeArchive& native)
        {
            native.streams = {{2, NativeStream{std::vector<uint8_t>{0x46, 0x53, 0x54, 0x4D}, true}},
                              {7, NativeStream{std::vector<uint8_t>(), false}}};
        });

    const std::vector<uint8_t> binary = format::Write(file);
    const std::string text = format::WriteText(file);
    XSFLIB_CHECK(format::Read(binary) == file && format::ReadText(text) == file);
    XSFLIB_CHECK(format::WriteText(format::ReadText(text)) == text);

    // The number of streams, then each one's file ID, its flags, its size in 8 bytes and its bytes. Flag 1 marks a
    // stream that the game lacks.
    const std::vector<uint8_t> kExpected = {2,    0,    0,    0,    2, 0, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0,
                                            0x46, 0x53, 0x54, 0x4D, 7, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    const std::vector<std::string> kIds = {"ENGN", "MIXR", "SEQS", "NATV", "TABL", "GRPF", "STRM", "XTRA", "x y!"};
    XSFLIB_CHECK(ChunkIds(binary) == kIds);
    XSFLIB_CHECK(std::ranges::equal(ChunkBytes(binary, 6), kExpected));
    XSFLIB_CHECK(text.find(std::string("\nchunk GRPF 160\n") + kGroupFilesText +
                           "\nchunk STRM 40\n"
                           "    AgAAAAIAAAAAAAAABAAAAAAAAABGU1RNBwAAAAEAAAAAAAAAAAAAAA==\n\nchunk XTRA") !=
                 std::string::npos);

    // The binary form refuses file IDs that don't rise, a flag of a later version, a stream that runs past the chunk's
    // end, and streams without a native archive. The second stream's ID lies at byte 24 of STRM, and its flags at 28.
    XSFLIB_CHECK(Patched(binary, 6, 24, 2) == "The file is damaged: the file IDs of its streams don't rise.");
    XSFLIB_CHECK(Patched(binary, 6, 28, 3) ==
                 "The file contains a stream with a flag that this version of Hokuto doesn't "
                 "support. A later version of Hokuto saved it.");
    XSFLIB_CHECK(Patched(binary, 6, 12, 5) == "The file is damaged: the STRM chunk ends early.");
    File loose;
    loose.native = NativeArchive{.pieces = std::vector<ntdlib::sar::Piece>{std::vector<uint8_t>{1, 2, 3}},
                                 .streams = {{0, NativeStream{std::vector<uint8_t>{4}, true}}}};
    std::vector<uint8_t> unnative = format::Write(loose);
    XSFLIB_CHECK(ChunkIds(unnative) == std::vector<std::string>({"ENGN", "SEQS", "NATV", "STRM"}));
    std::ranges::copy(std::string_view("XTRA"), unnative.begin() + 24 + 24 * 2);
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { format::Read(unnative); }) ==
                 "The file is damaged: it contains streams, but no native archive.");
    XSFLIB_CHECK(format::ReadText(format::WriteText(loose)) == loose);

    // The text form refuses streams before the native archive, a second STRM chunk, group files after the streams, and
    // a chunk that ends early. Each message names the chunk's line.
    const std::string kEmptyChunk = "chunk STRM 4\n    AAAAAA==\n";
    XSFLIB_CHECK(TextError("hok 1.1\nengine switch\n" + kEmptyChunk)
                     .starts_with("Line 3: the STRM chunk comes once, after the native archive"));
    XSFLIB_CHECK(TextError(kNativeOnly + kEmptyChunk + kEmptyChunk).starts_with("Line 8: the STRM chunk comes once"));
    XSFLIB_CHECK(
        TextError(kNativeOnly + kEmptyChunk + "chunk GRPF 4\n    AAAAAA==\n")
            .starts_with("Line 8: the GRPF chunk comes once, after the native archive and before its streams"));
    XSFLIB_CHECK(TextError(kNativeOnly + "chunk STRM 4\n    AQAAAA==\n") ==
                 "Line 6: The file is damaged: the STRM chunk ends early.");
    XSFLIB_CHECK(TextError(kNativeOnly + kEmptyChunk).empty());

    // A chunk line refuses the ID of a chunk that Hokuto reads.
    XSFLIB_CHECK(TextError("hok 1.1\nengine switch\nchunk TAGS 4\n    AAAAAA==\n")
                     .starts_with("Line 3: a chunk line holds only a chunk that Hokuto doesn't read, not TAGS"));
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("format_archive",
                                    []
                                    {
                                        TestArchiveImport();
                                        TestArchiveForms();
                                        TestArchiveEdits();
                                        TestArchiveRefusals();
                                        TestArchiveDamagedStrings();
                                        Test3dsArchive();
                                        TestWiiArchive();
                                        TestWiiBuildOfVersion();
                                        TestWiiTables();
                                        TestWiiBuild();
                                        TestBankSlots();
                                        TestGrowth();
                                        TestGameSizesForms();
                                        TestGameSizesText();
                                        TestGameSizesRefused();
                                        TestGroupFilesImport();
                                        TestGroupFilesEdit();
                                        TestGroupFilesEarlierForm();
                                        TestGroupFilesOfThe3ds();
                                        TestGroupSequencesImport();
                                        TestGroupSequencesLonger();
                                        TestGroupSequencesModelForm();
                                        TestStreams();
                                    });
}
