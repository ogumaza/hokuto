// SPDX-License-Identifier: MIT

// Tests the DSE files inside a game's packs (format/dse_files.h). A small cartridge image from ntdlib's builders holds
// a song with its bank in its file system, the same in a pack of files end to end, and the same in a pack that the
// SDK's LZ77 compresses. The search finds all three, the import makes a sequence of each song, and the song layer plays
// them. The import records each file's place, and the export reads the record. The export gives the image back while no
// file changes, writes a changed song of the file system, and refuses a changed song inside a pack, or one that the
// image keeps compressed, with a message that names it. The message names ten files at most. It sends a song to the
// browser, and says that a bank of sound effects, which the browser doesn't export as a file, stays in the project. The
// notes when the image opens name each guess of the playback: the build of the driver, a game that ntdlib's table of
// ticks doesn't list, and the folders that the game's rows don't cover.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/dse/driver.h"
#include "ntdlib/dse/layout.h"
#include "ntdlib/ntr/rom.h"
#include "ntdlib/testing/dse_song_files.h"
#include "ntdlib/testing/nds_files.h"
#include "xsflib/testing/check.h"

#include "binary_form_testing.h"
#include "format/archive.h"
#include "format/dse_files.h"
#include "format/file.h"
#include "song/dse.h"
#include "song/sequence.h"

namespace
{

namespace files = ntdlib::testing::dse_song_files;

using Bytes = std::vector<uint8_t>;
using hokuto::format::File;
using xsflib::testing::Check;

// `bytes` as the SDK's LZ77 compresses them with no copies: the type 0x10 and the size, then a flag byte of 0 before
// each 8 bytes.
Bytes Lz77Literals(const Bytes& bytes)
{
    Bytes out = {0x10, static_cast<uint8_t>(bytes.size()), static_cast<uint8_t>(bytes.size() >> 8),
                 static_cast<uint8_t>(bytes.size() >> 16)};
    for (std::size_t at = 0; at < bytes.size(); at += 8)
    {
        out.push_back(0);
        out.insert(out.end(), bytes.begin() + static_cast<std::ptrdiff_t>(at),
                   bytes.begin() + static_cast<std::ptrdiff_t>(std::min(at + 8, bytes.size())));
    }

    return out;
}

// An image of the game `game_code` with the main bank and a song with its bank in its file system, the same song and
// bank in SOUND/pack.bin after 16 bytes of the pack's header, and in SOUND/packed.csnd, compressed. Without `loose`,
// the file system holds no DSE file.
Bytes PackImage(const std::string& game_code, bool loose = true)
{
    const Bytes song_and_bank = files::Join({files::SongFile(), files::BankFile()});
    ntdlib::testing::nds_files::Image image;
    image.game_code = game_code;
    if (loose)
    {
        image.files.push_back({"SOUND/BGM/bgm.swd", files::MainBankFile()});
        image.files.push_back({"SOUND/BGM/bgm0001.smd", files::SongFile()});
        image.files.push_back({"SOUND/BGM/bgm0001.swd", files::BankFile()});
    }
    image.files.push_back({"SOUND/pack.bin", files::Join({Bytes(16, 0), song_and_bank})});
    image.files.push_back({"SOUND/packed.csnd", Lz77Literals(song_and_bank)});

    return ntdlib::testing::nds_files::BuildImage(image).first;
}

// The number of the sequence of `file` whose label starts with `label`. Throws std::runtime_error where none does.
std::size_t SequenceLabelled(const File& file, const std::string& label)
{
    const auto labelled = [&label](const auto& sequence)
    {
        return !sequence->labels.empty() && sequence->labels[0].name.starts_with(label);
    };
    const auto found = std::ranges::find_if(file.sequences, labelled);
    if (found == file.sequences.end())
    {
        throw std::runtime_error("no sequence's label starts with " + label);
    }

    return static_cast<std::size_t>(found - file.sequences.begin());
}

// `file` with sequence `sequence` longer by 8 waits.
File Longer(const File& file, std::size_t sequence)
{
    File longer = file;
    longer.sequences[sequence].Edit(
        [](ntdlib::commands::Sequence& edited)
        {
            const auto wait =
                std::ranges::find(edited.items, ntdlib::commands::Kind::kWait, &ntdlib::commands::Command::kind);
            SEQLIB_REQUIRE(wait != edited.items.end());
            edited.items.insert(wait + 1, 8, *wait);
        });

    return longer;
}

// The message of the std::invalid_argument that `function` throws, or an empty string.
template <typename Function>
std::string RefusalOf(Function function)
{
    try
    {
        function();
    }
    catch (const std::invalid_argument& error)
    {
        return error.what();
    }

    return {};
}

// The search finds the loose files and the files of both packs, each with the image's file that holds it.
void TestSearch()
{
    const Bytes image = PackImage("ATST");
    const ntdlib::ntr::Rom rom = ntdlib::ntr::Rom::Parse(image);

    std::vector<std::vector<uint8_t>> storage;
    const std::vector<hokuto::format::DseImageFile> found = hokuto::format::DseFilesOf(rom, storage);

    // The image's file system lists the files of SOUND/ before those of SOUND/BGM/.
    std::map<std::string, const hokuto::format::DseImageFile*> by_path;
    for (const hokuto::format::DseImageFile& file : found)
    {
        by_path.emplace(file.named.path, &file);
    }
    SEQLIB_REQUIRE(found.size() == 7 && by_path.size() == 7);
    const std::vector<std::string> loose = {"SOUND/BGM/bgm.swd", "SOUND/BGM/bgm0001.smd", "SOUND/BGM/bgm0001.swd"};
    const std::vector<std::string> packed = {"SOUND/pack.bin/B_TEST.smd", "SOUND/pack.bin/B_TEST.swd",
                                             "SOUND/packed.csnd/B_TEST.smd", "SOUND/packed.csnd/B_TEST.swd"};
    const auto is_loose = [&by_path](const std::string& path)
    {
        return by_path.contains(path) && by_path.at(path)->loose && by_path.at(path)->image_path == path;
    };
    const auto is_packed = [&by_path](const std::string& path)
    {
        return by_path.contains(path) && !by_path.at(path)->loose &&
               path.starts_with(by_path.at(path)->image_path + "/");
    };
    Check(std::ranges::all_of(loose, is_loose), "the files of the file system are loose, under their paths");
    Check(std::ranges::all_of(packed, is_packed), "the files in the packs, under the packs' paths");
    Check(std::ranges::equal(by_path.at(packed[0])->named.bytes, files::SongFile()) &&
              std::ranges::equal(by_path.at(packed[2])->named.bytes, files::SongFile()),
          "a song in a pack, and one in a compressed pack, keep their bytes");
    Check(hokuto::format::HoldsDseFiles(PackImage("ATST", false)), "an image with DSE files in its packs alone");
}

// The import makes a sequence of each song, and the song layer plays each with its bank.
void TestImport()
{
    const Bytes image = PackImage("ATST");

    const File file = hokuto::format::ImportDseImage(image);
    const std::shared_ptr<const hokuto::song::DseSet> set = hokuto::song::LoadDseSet(image);

    Check(file.sequences.size() == 3, "a sequence of each song");
    SEQLIB_REQUIRE(set->sounds.size() == 3);
    for (uint32_t sound = 0; sound < 3; sound++)
    {
        Check(set->sounds[sound].song && !set->sounds[sound].banks.empty(), "each song with its bank");
        Check(!hokuto::song::ReadSequence(*set, sound).tracks.empty(), "each song reads");
    }
    std::vector<std::string> folders;
    for (uint32_t sound = 0; sound < 3; sound++)
    {
        folders.push_back(hokuto::song::DseSoundFolder(*set, sound));
    }
    std::ranges::sort(folders);
    Check(folders == std::vector<std::string>{"SOUND/BGM/", "SOUND/pack.bin/", "SOUND/packed.csnd/"},
          "the folders of the songs");
}

// The export gives the image back unchanged, writes a changed song of the file system, and refuses a changed song in a
// pack.
void TestExport()
{
    const Bytes image = PackImage("ATST");
    const File file = hokuto::format::ImportDseImage(image);
    const std::size_t loose = SequenceLabelled(file, "SOUND_BGM_bgm0001");
    const std::size_t in_pack = SequenceLabelled(file, "SOUND_pack_bin");
    const std::size_t compressed = SequenceLabelled(file, "SOUND_packed_csnd");

    const hokuto::format::DsImageExport same = hokuto::format::ExportDsImage(file);
    const File loose_longer = Longer(file, loose);
    const hokuto::format::DsImageExport grown = hokuto::format::ExportDsImage(loose_longer);

    Check(same.image == image, "the image comes back unchanged");
    const File again = hokuto::format::ImportDseImage(grown.image);
    Check(grown.image != image && *again.sequences[loose] == *loose_longer.sequences[loose] &&
              *again.sequences[in_pack] == *file.sequences[in_pack],
          "a changed song of the file system goes into the image");
    Check(RefusalOf([&] { hokuto::format::ExportDsImage(Longer(file, in_pack)); }) ==
              "Can't write B_TEST.smd into the image. It's inside the pack SOUND/pack.bin. Export it from the browser "
              "instead.",
          "a changed song in a pack is refused");
    Check(RefusalOf([&] { hokuto::format::ExportDsImage(Longer(Longer(file, in_pack), compressed)); }) ==
              "Can't write 2 files into the image. They're inside packs: B_TEST.smd (SOUND/pack.bin), B_TEST.smd "
              "(SOUND/packed.csnd). Export them from the browser instead.",
          "two changed songs in packs are refused together");
}

// The import records each file's place in the image, in the order of the set's files: a file of the file system at 0 in
// its file, a file of the pack after the pack's 16 bytes, and a file of the compressed pack in its decompressed bytes.
// The record gives the same files as the search, and comes back from both forms. A file without the record, from a
// build before it, exports as one with it. A reader refuses a place with a flag that this version doesn't define.
void TestRecord()
{
    namespace format = hokuto::format;
    const Bytes image = PackImage("ATST");
    const File file = format::ImportDseImage(image);
    const std::vector<format::DsePlace>& places = file.native->cartridge->places;
    const ntdlib::ntr::Rom rom = ntdlib::ntr::Rom::Parse(image);
    std::vector<std::vector<uint8_t>> storage;
    const std::vector<format::DseImageFile> searched = format::DseFilesOf(rom, storage);
    const std::vector<format::DseImageFile> recorded = format::DseFilesAt(rom, places, storage);
    File without = file;
    without.native.Edit([](format::NativeArchive& native) { native.cartridge->places.clear(); });
    const std::size_t loose = SequenceLabelled(file, "SOUND_BGM_bgm0001");
    const Bytes binary = format::Write(file);

    const format::DsImageExport same = format::ExportDsImage(without);
    const format::DsImageExport grown = format::ExportDsImage(Longer(file, loose));
    const format::DsImageExport grown_without = format::ExportDsImage(Longer(without, loose));

    std::vector<std::string> set_paths;
    for (const ntdlib::dse::NamedFile& named : ntdlib::dse::ReadSet(format::ExportArchive(file)))
    {
        set_paths.push_back(named.path);
    }
    std::vector<std::string> place_paths;
    std::map<std::string, format::DsePlace> by_path;
    for (const format::DsePlace& place : places)
    {
        place_paths.push_back(place.path);
        by_path.emplace(place.path, place);
    }
    const auto song_size = static_cast<uint32_t>(files::SongFile().size());
    Check(place_paths == set_paths, "a place for each file of the set, in its order");
    Check(by_path.at("SOUND/BGM/bgm0001.smd") ==
              format::DsePlace{"SOUND/BGM/bgm0001.smd", "SOUND/BGM/bgm0001.smd", 0, song_size, false},
          "a file of the file system");
    Check(by_path.at("SOUND/pack.bin/B_TEST.smd") ==
              format::DsePlace{"SOUND/pack.bin/B_TEST.smd", "SOUND/pack.bin", 16, song_size, false},
          "a file of the pack");
    Check(by_path.at("SOUND/packed.csnd/B_TEST.smd") ==
              format::DsePlace{"SOUND/packed.csnd/B_TEST.smd", "SOUND/packed.csnd", 0, song_size, true},
          "a file of the compressed pack");
    const auto same_file = [](const format::DseImageFile& a, const format::DseImageFile& b)
    {
        return a.named.path == b.named.path && std::ranges::equal(a.named.bytes, b.named.bytes) &&
               a.image_file == b.image_file && a.image_path == b.image_path && a.loose == b.loose &&
               a.offset == b.offset && a.compressed == b.compressed;
    };
    Check(std::ranges::equal(recorded, searched, same_file), "the record gives the files that the search finds");
    Check(hokuto::testing::ComesBack(file), "the record comes back from both forms");
    Check(same.image == image && grown.image == grown_without.image, "a file without the record exports the same");
    const std::size_t flags = 16 + 4 + 4 + places[0].path.size() + 4 + places[0].image_file.size() + 8;
    Check(hokuto::testing::Patched(binary, "CART", flags, 2).ends_with("A later version of Hokuto saved it."),
          "a place's flag that this version doesn't define");
}

// An image with a pack of a bank of sound effects, a song and its bank, and the main bank in its file system. The
// browser exports no bank of sound effects as a file. The refusal of a changed one says that the edit stays in the
// project, and still sends the song to the browser.
void TestExportEffects()
{
    const files::Effect effect{.tracks = {{0, 0, 0, 0, 0x40, 0x65, 0x30, 0x92, 0x30, 0x98}}};
    ntdlib::testing::nds_files::Image spec;
    spec.files = {{"SOUND/BGM/bgm.swd", files::MainBankFile()},
                  {"SOUND/fx.bin", files::Join({Bytes(16, 0), files::EffectsFile({effect}, 0x6430), files::SongFile(),
                                                files::BankFile()})}};
    const File file = hokuto::format::ImportDseImage(ntdlib::testing::nds_files::BuildImage(spec).first);
    const std::size_t sound_effect = SequenceLabelled(file, "SOUND_fx_bin_B_TEST_sed");
    const std::size_t song = SequenceLabelled(file, "SOUND_fx_bin_B_TEST_smd");

    const std::string alone = RefusalOf([&] { hokuto::format::ExportDsImage(Longer(file, sound_effect)); });
    const std::string with_song =
        RefusalOf([&] { hokuto::format::ExportDsImage(Longer(Longer(file, sound_effect), song)); });

    Check(alone ==
              "Can't write B_TEST.sed into the image. It's inside the pack SOUND/fx.bin. Hokuto can't export a "
              "bank of sound effects as a file. Save the project to keep the edits.",
          "a changed bank of sound effects in a pack stays in the project");
    Check(with_song ==
              "Can't write 2 files into the image. They're inside packs: B_TEST.sed (SOUND/fx.bin), "
              "B_TEST.smd (SOUND/fx.bin). Export the songs and wave banks from the browser instead. Hokuto "
              "can't export a bank of sound effects as a file. Save the project to keep the edits.",
          "a song goes to the browser, and the bank of sound effects stays in the project");
}

// An image that keeps a song compressed as one file, with a pack of 12 songs and the banks in its file system. The
// export refuses a changed compressed song, and names it. A refusal of many files names the first ten and counts the
// others.
void TestExportCompressed()
{
    Bytes songs(16, 0);
    for (int i = 0; i < 12; i++)
    {
        const Bytes song = files::SongFile();
        songs.insert(songs.end(), song.begin(), song.end());
    }
    ntdlib::testing::nds_files::Image spec;
    spec.files = {{"SOUND/BGM/bgm.swd", files::MainBankFile()},
                  {"SOUND/BGM/bgm0001.swd", files::BankFile()},
                  {"SOUND/song.csnd", Lz77Literals(files::SongFile())},
                  {"SOUND/songs.bin", songs}};
    const File file = hokuto::format::ImportDseImage(ntdlib::testing::nds_files::BuildImage(spec).first);
    const std::size_t compressed = SequenceLabelled(file, "SOUND_song_csnd");
    std::vector<std::size_t> in_pack;
    for (std::size_t i = 0; i < file.sequences.size(); i++)
    {
        const auto& labels = file.sequences[i]->labels;
        if (!labels.empty() && labels[0].name.starts_with("SOUND_songs_bin"))
        {
            in_pack.push_back(i);
        }
    }
    File all_changed = Longer(file, compressed);
    for (const std::size_t sequence : in_pack)
    {
        all_changed = Longer(all_changed, sequence);
    }

    const std::string alone = RefusalOf([&] { hokuto::format::ExportDsImage(Longer(file, compressed)); });
    const std::string with_pack =
        RefusalOf([&] { hokuto::format::ExportDsImage(Longer(Longer(file, compressed), in_pack[0])); });
    const std::string many = RefusalOf([&] { hokuto::format::ExportDsImage(all_changed); });

    Check(in_pack.size() == 12, "the search finds the 12 songs of the pack");
    Check(alone ==
              "Can't write SOUND/song.csnd into the image. The image keeps it compressed. Export it from the browser "
              "instead.",
          "a changed compressed song is refused");
    Check(with_pack ==
              "Can't write 2 files into the image. They're inside packs or compressed: SOUND/song.csnd (compressed), "
              "B_TEST.smd (SOUND/songs.bin). Export them from the browser instead.",
          "a compressed song and a song in a pack are refused together");
    Check(many ==
              "Can't write 13 files into the image. They're inside packs or compressed: SOUND/song.csnd (compressed), "
              "B_TEST.smd (SOUND/songs.bin), B_TEST_2.smd (SOUND/songs.bin), B_TEST_3.smd (SOUND/songs.bin), "
              "B_TEST_4.smd (SOUND/songs.bin), B_TEST_5.smd (SOUND/songs.bin), B_TEST_6.smd (SOUND/songs.bin), "
              "B_TEST_7.smd (SOUND/songs.bin), B_TEST_8.smd (SOUND/songs.bin), B_TEST_9.smd (SOUND/songs.bin), and 3 "
              "more. Export them from the browser instead.",
          "a refusal of 13 files names the first 10");
}

// The notes name the build that no model covers, a game that the table of ticks doesn't list, and the folders of a
// listed game that its rows don't cover.
void TestNotes()
{
    const Bytes unlisted = PackImage("ATST");
    const Bytes uncovered = PackImage("BLFE");
    const Bytes covered = PackImage("YFTE");
    const std::string build =
        "No model covers this game's build of DSE. Its sounds play on the model of the later build.";
    const std::string game =
        "This game's DSE tick is unknown. Its sounds play with a tick of 10 ms, and their "
        "timing, envelopes and LFOs may differ from the game.";
    const std::string folders =
        "The DSE tick of these folders is unknown: SOUND/pack.bin/, SOUND/packed.csnd/, "
        "SOUND/BGM/. Their sounds play with a tick of 10 ms, and their timing, envelopes "
        "and LFOs may differ from the game.";

    const std::vector<std::string> unlisted_notes =
        hokuto::song::DseOpenNotes(unlisted, *hokuto::song::LoadDseSet(unlisted));
    const std::vector<std::string> uncovered_notes =
        hokuto::song::DseOpenNotes(uncovered, *hokuto::song::LoadDseSet(uncovered));
    const std::vector<std::string> covered_notes =
        hokuto::song::DseOpenNotes(covered, *hokuto::song::LoadDseSet(covered));
    const hokuto::song::DseSet empty;

    Check(unlisted_notes == std::vector<std::string>{build, game}, "a game that the table doesn't list");
    Check(uncovered_notes == std::vector<std::string>{build, folders}, "folders that the game's rows don't cover");
    Check(covered_notes == std::vector<std::string>{build}, "a game whose rows cover every folder");
    Check(hokuto::song::DseOpenNotes(unlisted, empty).empty(), "no note without sounds");
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("dse_packs",
                                    []
                                    {
                                        TestSearch();
                                        TestRecord();
                                        TestImport();
                                        TestExport();
                                        TestExportEffects();
                                        TestExportCompressed();
                                        TestNotes();
                                    });
}
