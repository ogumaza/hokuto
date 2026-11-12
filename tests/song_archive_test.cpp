// SPDX-License-Identifier: MIT

// Tests the archives of the songs (src/song/archive.cpp): group files, builds and flavors.

#include "song_runs.h"

namespace
{

// The game keeps the test archive's wave archive in group GROUP_WAVES, in a file beside the archive. Opening the
// archive finds the group file and keeps it. Loading the archive with the kept group file reads the wave archive again.
// A .hok file's archive does the same. Without the group file, the wave archive is missing. A file that isn't a group
// file gives an error.
void TestGroupFiles()
{
    sar_files::ArchiveSpec spec = TestArchiveSpec();
    const std::vector<uint8_t> waves = spec.files[3].bytes;
    spec.files[3].group_only = true;
    spec.groups = {{"GROUP_WAVES", sar::kExternalGroupFile}};
    const std::vector<uint8_t> bytes = sar_files::Archive(false, spec);
    const std::vector<uint8_t> group = sar_files::Group(false, {{3, waves}});
    const TemporaryFolder folder;
    xsflib::WriteFile(folder.Path() / "groups.bfsar", bytes);
    xsflib::WriteFile(folder.Path() / "GROUP_WAVES.bfgrp", group);
    const std::vector<std::vector<uint8_t>> kNotGroup = {{1, 2, 3}};

    const std::shared_ptr<const sar::SoundArchive> opened = OpenArchive(folder.Path() / "groups.bfsar");
    const std::shared_ptr<const sar::SoundArchive> kept = LoadArchive(bytes, "groups.bfsar", opened->GroupFiles());
    const std::shared_ptr<const sar::SoundArchive> bare = LoadArchive(bytes, "groups.bfsar");

    SEQLIB_CHECK(opened->GroupFiles() == std::vector<std::vector<uint8_t>>{group});
    SEQLIB_CHECK(std::ranges::equal(opened->FileData(3), waves) && std::ranges::equal(kept->FileData(3), waves));
    SEQLIB_CHECK(bare->FileData(3).empty());
    SEQLIB_CHECK(ThrowsRuntimeError([&] { LoadArchive(bytes, "groups.bfsar", kNotGroup); }));
}

// A game can keep a group file in the userManagementFiles folder beside its archive. Opening the archive finds
// GROUP_WAVES's file there, and an export writes the group's new file there too (PlaceGroupFiles). A group file goes
// beside the archive when the game keeps it there, when the game keeps a file of its name in both places, and when no
// folder holds one. Of two folders, the first that holds a file of the name decides. A SARC archive keeps its name.
void TestGroupFilePlaces()
{
    sar_files::ArchiveSpec spec = TestArchiveSpec();
    const std::vector<uint8_t> waves = spec.files[3].bytes;
    spec.files[3].group_only = true;
    spec.groups = {{"GROUP_WAVES", sar::kExternalGroupFile}};
    const std::vector<uint8_t> group = sar_files::Group(false, {{3, waves}});

    const TemporaryFolder game;
    const TemporaryFolder other;
    const std::filesystem::path managed = game.Path() / hokuto::song::kUserManagementFolder;
    const std::filesystem::path other_managed = other.Path() / hokuto::song::kUserManagementFolder;
    std::filesystem::create_directory(managed);
    std::filesystem::create_directory(other_managed);
    xsflib::WriteFile(game.Path() / "groups.bfsar", sar_files::Archive(false, spec));
    xsflib::WriteFile(managed / "GROUP_WAVES.bfgrp", group);
    const std::vector<uint8_t> kBytes = {1, 2, 3};
    for (const std::filesystem::path& file :
         {game.Path() / "GROUP_BGM.bfgrp", game.Path() / "GROUP_BOTH.bfgrp", managed / "GROUP_BOTH.bfgrp",
          managed / "Sound.sarc", other_managed / "GROUP_BGM.bfgrp", other_managed / "GROUP_OTHER.bcgrp"})
    {
        xsflib::WriteFile(file, kBytes);
    }

    const std::array<std::filesystem::path, 2> folders = {game.Path(), other.Path()};
    const std::vector<Output> kOutputs = {{"GROUP_WAVES.bfgrp", {1}}, {"GROUP_BGM.bfgrp", {2}},
                                          {"GROUP_BOTH.bfgrp", {3}},  {"GROUP_OTHER.bcgrp", {4}},
                                          {"GROUP_NEW.bfgrp", {5}},   {"Sound.sarc", {6}}};
    const std::vector<Output> kExpected = {
        {"userManagementFiles/GROUP_WAVES.bfgrp", {1}}, {"GROUP_BGM.bfgrp", {2}}, {"GROUP_BOTH.bfgrp", {3}},
        {"userManagementFiles/GROUP_OTHER.bcgrp", {4}}, {"GROUP_NEW.bfgrp", {5}}, {"Sound.sarc", {6}}};

    const std::shared_ptr<const sar::SoundArchive> opened = OpenArchive(game.Path() / "groups.bfsar");
    const std::vector<Output> placed = PlaceGroupFiles(kOutputs, folders);

    SEQLIB_CHECK(opened->GroupFiles() == std::vector<std::vector<uint8_t>>{group});
    SEQLIB_CHECK(std::ranges::equal(opened->FileData(3), waves));
    SEQLIB_CHECK(placed == kExpected);
}

// A tempo of 2,000 BPM (0xE1 with 0x07D0), then a note of 48 ticks, a wait of 48 ticks (0x80 with 48) and a note of key
// 72.
const std::vector<uint8_t> kWiiFastBytecode = {0xE1, 0x07, 0xD0, 0x3C, 0x64, 0x30, 0x80, 0x30, 0x48, 0x64, 0x30, 0xFF};

// The archive's version, 1.4, gives a build of NW4R SND of June 2008. That build holds a tempo to 1,023 BPM. The game's
// build of 2007 keeps 2,000 BPM, and the second note starts sooner. The sequence keeps the build that read it, and the
// model plays by it as ntdlib's model of that build plays run directly.
void TestWiiGameBuild()
{
    constexpr std::size_t kFrames = 400;
    const std::shared_ptr<const ntdlib::rsar::SoundArchive> archive = OpenWiiArchive(kWiiFastBytecode);
    const ntdlib::SndBuild kWiiSports{.date = 20070608};
    const auto by_version = std::make_shared<const Sequence>(ReadSequence(*archive, 0));
    const auto by_game = std::make_shared<const Sequence>(ReadSequence(*archive, 0, {}, {}, kWiiSports));
    const std::vector<int16_t> june = RunWiiModel(*archive, kFrames);
    const std::vector<int16_t> sports = RunWiiModel(*archive, kFrames, {}, ntdlib::rvl::ax::kMaxVolume, kWiiSports);
    const auto version_model =
        std::make_shared<SequenceModel>(archive, by_version, hokuto::song::ModelOptions{.snapshot_interval = 50});
    const std::vector<int16_t> played_version = RunToEnd(*version_model, june.size());
    const auto game_model =
        std::make_shared<SequenceModel>(archive, by_game, hokuto::song::ModelOptions{.snapshot_interval = 50});
    const std::vector<int16_t> played_game = RunToEnd(*game_model, sports.size());

    SEQLIB_REQUIRE(!by_version->tempo.empty() && !by_game->tempo.empty());
    SEQLIB_CHECK(by_version->tempo.front().bpm == 1023.0 && by_game->tempo.front().bpm == 2000.0);
    SEQLIB_CHECK(by_version->wii_build && by_version->wii_build->date == 20080620);
    SEQLIB_CHECK(by_game->wii_build == kWiiSports);
    SEQLIB_CHECK(june != sports);
    SEQLIB_CHECK(!played_version.empty() && std::equal(played_version.begin(), played_version.end(), june.begin()));
    SEQLIB_CHECK(!played_game.empty() && std::equal(played_game.begin(), played_game.end(), sports.begin()));
}

// A main.dol's banner dates its build of NW4R SND. The microcode of AX of October 2006 marks the build that comes with
// it. That build has no banner. Bytes without AX's microcode give no build. The main.dol of an extracted disc lies in
// sys, beside the files folder that holds the archive at any depth. A folder without one gives none.
void TestWiiBuildOfDol()
{
    const std::vector<uint8_t> sports = ntdlib::testing::MicrocodeDol(
        0x140, "<< NW4R    - SND \tfinal   build: Jun  8 2007 11:17:15 (0x4199_60831) >>");
    const std::vector<uint8_t> smooth_moves = ntdlib::testing::MicrocodeDol(0x120, "");
    const std::vector<uint8_t> no_banner = ntdlib::testing::MicrocodeDol(0x140, "");
    const std::optional<ntdlib::SndBuild> sports_build = hokuto::song::ReadWiiBuild(sports);
    const std::optional<ntdlib::SndBuild> smooth_moves_build = hokuto::song::ReadWiiBuild(smooth_moves);
    const std::vector<uint8_t> kZeros(0x3000, 0);

    const TemporaryFolder folder;
    const std::filesystem::path disc = folder.Path() / "disc";
    std::filesystem::create_directories(disc / "files" / "Sound" / "Scene");
    std::filesystem::create_directories(disc / "sys");
    const std::filesystem::path archive = disc / "files" / "Sound" / "Scene" / "sound.brsar";
    xsflib::WriteFile(archive, std::vector<uint8_t>{1});
    xsflib::WriteFile(disc / "sys" / "main.dol", sports);
    const std::filesystem::path flat = folder.Path() / "sound.brsar";
    xsflib::WriteFile(flat, std::vector<uint8_t>{1});
    const std::optional<std::filesystem::path> found = hokuto::song::FindMainDol(archive);

    SEQLIB_CHECK(sports_build && sports_build->date == 20070608 && !sports_build->october_2006);
    SEQLIB_CHECK(smooth_moves_build && smooth_moves_build->date == 0 && smooth_moves_build->october_2006);
    // A build without a banner plays as the build of October 2006 on a later AX too. ntdlib's model plays it that way.
    const std::optional<ntdlib::SndBuild> no_banner_build = hokuto::song::ReadWiiBuild(no_banner);
    SEQLIB_CHECK(no_banner_build && no_banner_build->date == 0 && no_banner_build->october_2006);
    SEQLIB_CHECK(!hokuto::song::ReadWiiBuild(kZeros));
    SEQLIB_CHECK(found && std::filesystem::equivalent(*found, disc / "sys" / "main.dol"));
    SEQLIB_CHECK(!hokuto::song::FindMainDol(flat));
}

// An archive's bytes name its console: CSAR the 3DS, a big-endian FSAR the Wii U, and a little-endian one the Switch.
// Each opens with its console's settings of ntdlib's reader and converter, and its sequences take its console's rules.
void TestArchiveFlavors()
{
    for (const auto& [flavor, engine] :
         {std::pair{sar::Flavor::kCtr, export_::Engine::kCtr}, std::pair{sar::Flavor::kCafe, export_::Engine::kCafe},
          std::pair{sar::Flavor::kNx, export_::Engine::kSwitch}})
    {
        sar_files::ArchiveSpec spec;
        spec.sounds = {{.name = "SEQ", .file = 0}};
        spec.files = {{.bytes = sar_files::Sequence(flavor, {0x3C, 0x64, 0x30, 0xFF}, {{"SEQ", 0}})}};
        const std::vector<uint8_t> bytes = sar_files::Archive(flavor, spec);
        SEQLIB_CHECK(hokuto::song::FlavorOfArchive(bytes) == flavor);
        const std::shared_ptr<const sar::SoundArchive> archive = hokuto::song::LoadArchive(bytes, "test");
        SEQLIB_CHECK(archive->ReadOptions().flavor == flavor && hokuto::song::SettingsOf(*archive).engine == engine);
        SEQLIB_CHECK(ReadSequence(*archive, 0).tracks.size() == 1);
    }
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("song_archive",
                                    []
                                    {
                                        TestWiiGameBuild();
                                        TestWiiBuildOfDol();
                                        TestGroupFiles();
                                        TestGroupFilePlaces();
                                        TestArchiveFlavors();
                                    });
}
