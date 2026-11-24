// SPDX-License-Identifier: MIT

// Tests the length of DSE's tick (song/dse.h). The game's settings give the tick, by the folder of the files
// (ntdlib::dse::TickOf). Professor Layton and the Last Specter (BLF) is the one game with two ticks: its main part
// sets the slow tick of 16.715 ms for the files in its archive lt4_main_<language>.fa, and London Life starts the
// driver again with the tick of 10 ms for its files in ll/. The anchors of ntdlib's table ran the game's code.
//
// A small cartridge image from ntdlib's builders gives the game code BLFE, a song with its banks in a folder of
// lt4_main_en.fa, the same in ll/, and the same in a folder that the game's rows don't cover. Each sound and each bank
// takes its folder's tick, and the folder outside the rows takes the driver's default of 10 ms. The song of
// lt4_main_en.fa then has to play, read and convert as ntdlib does at 16.715 ms: the model's output matches
// dse::Player run directly, the read's tempo matches the performer's, and the MIDI file and the bank's SoundFont match
// ntdlib's converter. A note's preview runs the bank's tick. Each result differs from the one at 10 ms. Another game
// code gives every file the tick of 10 ms. A file imported from the image keeps the game code through a save and a
// load, and a sound sent to another console converts at its folder's tick.

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/sequence_export/dse/convert.h"
#include "ntdlib/sequence_export/dse/performance.h"
#include "ntdlib/testing/dse_song_files.h"
#include "ntdlib/testing/nds_files.h"
#include "xsflib/testing/check.h"

#include "dse_playing.h"
#include "dse_ticks_fixtures.h"
#include "edit/other_console.h"
#include "format/archive.h"
#include "format/file.h"
#include "song/dse.h"
#include "song/midi_export.h"
#include "song/preview.h"
#include "song/sequence.h"

namespace
{

namespace files = ntdlib::testing::dse_song_files;
namespace dse_export = ntdlib::sequence_export::dse;
using files::Bytes;
using hokuto::song::DseSet;
using hokuto::song::Sequence;
using xsflib::testing::Check;

using hokuto::testing::dse_ticks::DirectMidi;
using hokuto::testing::dse_ticks::kSlowTick;
using hokuto::testing::dse_ticks::kTick;
using hokuto::testing::dse_ticks::SetOf;

// A song of a conductor at a tempo of 120 and a track that plays note 48 for 48 ticks, then note 55 for 48.
Bytes TestSong()
{
    const Bytes conductor = {0, 0, 0, 0, 0xA4, 0x78, 0x98};
    const Bytes track = files::Join({{1, 1, 0, 0},
                                     {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x64},
                                     files::Note(0x7F, 0, 0, 0x30, 1),
                                     {0x92, 0x30},
                                     files::Note(0x60, 7, 0, 0x30, 1),
                                     {0x92, 0x30, 0x98}});
    return files::SongFile({conductor, track});
}

// An image of the game `game_code` with a song, its bank and the main bank in each of `folders`.
std::vector<uint8_t> TestImage(const std::string& game_code, const std::vector<std::string>& folders)
{
    ntdlib::testing::nds_files::Image image;
    image.game_code = game_code;
    for (const std::string& folder : folders)
    {
        image.files.push_back({folder + "bgm.swd", files::MainBankFile()});
        image.files.push_back({folder + "bgm0001.smd", TestSong()});
        image.files.push_back({folder + "bgm0001.swd", files::BankFile()});
    }

    return ntdlib::testing::nds_files::BuildImage(image).first;
}

// The number of the sound in `folder`, or a number past the sounds.
uint32_t SoundIn(const DseSet& set, const std::string& folder)
{
    uint32_t sound = 0;
    while (sound < set.sounds.size() && set.sounds[sound].folder != folder)
    {
        sound++;
    }

    return sound;
}

// The tempo of the performer's record of sound `sound` with a tick of `tick`, as the read keeps it.
std::vector<std::pair<uint32_t, double>> DirectTempo(const DseSet& set, uint32_t sound, int tick)
{
    const dse_export::Performance performance =
        dse_export::Perform(set.sounds[sound].Files(), {.loops = 1, .build = set.build, .tick_microseconds = tick});
    std::vector<std::pair<uint32_t, double>> tempo;
    for (const auto& change : performance.tempo)
    {
        tempo.push_back({static_cast<uint32_t>(change.tick), static_cast<double>(change.bpm)});
    }

    return tempo;
}

std::vector<std::pair<uint32_t, double>> ReadTempo(const DseSet& set, uint32_t sound)
{
    const Sequence sequence = hokuto::song::ReadSequence(set, sound);
    std::vector<std::pair<uint32_t, double>> tempo;
    for (const auto& change : sequence.tempo)
    {
        tempo.push_back({static_cast<uint32_t>(change.tick), static_cast<double>(change.bpm)});
    }

    return tempo;
}

// The first frames of a preview of program 0 of `bank`.
std::vector<float> Previewed(const hokuto::song::DseBank& bank)
{
    hokuto::song::DseNotePreview preview(bank, 0, ntdlib::dse::Build::kLater);
    std::vector<float> rendered(2 * 48000);
    rendered.resize(2 * preview.Render(rendered.data(), rendered.size() / 2));

    return rendered;
}

// Each sound and each bank takes the tick of its folder.
void TestTicks()
{
    const std::vector<uint8_t> image = TestImage("BLFE", {"lt4_main_en.fa/bgm/", "ll/bgm/", "lt4/bgm/"});
    const std::shared_ptr<const DseSet> set = hokuto::song::LoadDseSet(image);

    Check(set->warnings.empty(), "every file reads");
    SEQLIB_REQUIRE(set->sounds.size() == 3 && set->ticks.size() == 3);
    SEQLIB_REQUIRE(SoundIn(*set, "lt4_main_en.fa/bgm/") < 3 && SoundIn(*set, "ll/bgm/") < 3 &&
                   SoundIn(*set, "lt4/bgm/") < 3);
    Check(hokuto::song::DseSoundTick(*set, SoundIn(*set, "lt4_main_en.fa/bgm/")) == kSlowTick,
          "the main game's song takes the slow tick");
    Check(hokuto::song::DseSoundTick(*set, SoundIn(*set, "ll/bgm/")) == kTick, "London Life's song takes 10 ms");
    Check(hokuto::song::DseSoundTick(*set, SoundIn(*set, "lt4/bgm/")) == kTick,
          "a folder outside the game's rows takes the default tick");
    Check(hokuto::song::DseSoundTick(*set, 9) == kTick, "a number past the sounds takes the default tick");

    // The two banks of lt4_main_en.fa take the slow tick, and the four of the other folders 10 ms.
    SEQLIB_REQUIRE(set->banks.size() == 6);
    const auto slow = [](const hokuto::song::DseBank& bank)
    {
        return bank.tick_microseconds == kSlowTick;
    };
    const auto default_tick = [](const hokuto::song::DseBank& bank)
    {
        return bank.tick_microseconds == kTick;
    };
    Check(std::ranges::count_if(set->banks, slow) == 2 && std::ranges::count_if(set->banks, default_tick) == 4,
          "each bank takes its folder's tick");

    // A set of the image's files takes the ticks of the game whose code it's given, and 10 ms without a code.
    Check(hokuto::song::DseGameCodeOf(image) == "BLFE", "the image's game code");
    Check(hokuto::song::DseGameCodeOf(Bytes(16, 0)).empty(), "no game code for bytes that aren't an image");
    const std::vector<uint8_t> set_bytes = SetOf(image);
    const std::shared_ptr<const DseSet> from_set =
        hokuto::song::LoadDseSet(set_bytes, ntdlib::dse::Build::kLater, hokuto::song::DseGameCodeOf(image));
    const std::shared_ptr<const DseSet> without_code =
        hokuto::song::LoadDseSet(set_bytes, ntdlib::dse::Build::kLater, {});
    Check(from_set->ticks == set->ticks, "a set takes the game's ticks");
    Check(std::ranges::all_of(without_code->ticks, [](int tick) { return tick == kTick; }),
          "a set without a game code takes 10 ms");

    // Another game takes 10 ms everywhere.
    const std::shared_ptr<const DseSet> other =
        hokuto::song::LoadDseSet(TestImage("ATST", {"lt4_main_en.fa/bgm/", "ll/bgm/"}));
    Check(std::ranges::all_of(other->ticks, [](int tick) { return tick == kTick; }) &&
              std::ranges::all_of(other->banks,
                                  [](const hokuto::song::DseBank& bank) { return bank.tick_microseconds == kTick; }),
          "another game's files take 10 ms");
}

// The song of lt4_main_en.fa plays, reads and converts at the slow tick, and its bank converts and previews at it.
void TestSlowTick()
{
    const std::shared_ptr<const DseSet> set = hokuto::song::LoadDseSet(TestImage("BLFE", {"lt4_main_en.fa/bgm/"}));
    SEQLIB_REQUIRE(set->sounds.size() == 1 && set->banks.size() == 2);

    const std::vector<int16_t> played = hokuto::testing::ModelledDse(set, 0);
    const std::vector<int16_t> slow = hokuto::testing::DirectDse(*set, 0, kSlowTick);
    Check(hokuto::testing::StartsDse(played, slow), "the model plays what the player plays at the slow tick");
    Check(!hokuto::testing::StartsDse(played, hokuto::testing::DirectDse(*set, 0, kTick)),
          "and not what it plays at 10 ms");

    Check(ReadTempo(*set, 0) == DirectTempo(*set, 0, kSlowTick),
          "the read's tempo is the performer's at the slow tick");
    Check(ReadTempo(*set, 0) != DirectTempo(*set, 0, kTick), "and not the performer's at 10 ms");

    const std::vector<uint8_t> midi = hokuto::song::ConvertToMidi(*set, 0, "Hokuto").midi;
    Check(midi == DirectMidi(*set, 0, kSlowTick), "the MIDI file is the converter's at the slow tick");
    Check(midi != DirectMidi(*set, 0, kTick), "and not the converter's at 10 ms");

    // The bank of the song: the one with programs. The main bank contains only samples.
    const std::vector<uint32_t> program_banks = hokuto::song::DseProgramBanks(*set);
    SEQLIB_REQUIRE(program_banks.size() == 1);
    const hokuto::song::DseBank& bank = set->banks[program_banks[0]];
    const std::vector<uint32_t> programs = hokuto::song::DseBankPrograms(*bank.bank);
    const std::string name = hokuto::song::DseBankName(*bank.bank);
    const ntdlib::sequence_export::BankConversion converted =
        hokuto::song::ConvertDseBank(*set, program_banks[0], programs);
    const ntdlib::sequence_export::BankConversion slow_bank =
        dse_export::ConvertBank(*bank.bank, bank.main_bank.get(), programs, kSlowTick, name, "Hokuto");
    const ntdlib::sequence_export::BankConversion bank_at_10 =
        dse_export::ConvertBank(*bank.bank, bank.main_bank.get(), programs, kTick, name, "Hokuto");
    SEQLIB_REQUIRE(converted.sound_fonts.size() == 1 && slow_bank.sound_fonts.size() == 1 &&
                   bank_at_10.sound_fonts.size() == 1);
    Check(converted.sound_fonts[0].sf2 == slow_bank.sound_fonts[0].sf2, "the bank's SoundFont at the slow tick");
    Check(converted.sound_fonts[0].sf2 != bank_at_10.sound_fonts[0].sf2, "and not at 10 ms");

    hokuto::song::DseBank at_10 = bank;
    at_10.tick_microseconds = kTick;
    const std::vector<float> previewed = Previewed(bank);
    Check(!previewed.empty() && previewed != Previewed(at_10), "a note's preview runs the bank's tick");
}

// A file imported from the image keeps the game code in its cartridge image through a save and a load. Its song in
// lt4_main_en.fa goes to the Wii U at the slow tick: as from the file before the save, and unlike a song of another
// game at 10 ms.
void TestSavedFile()
{
    const hokuto::format::File file = hokuto::format::ImportDseImage(TestImage("BLFE", {"lt4_main_en.fa/bgm/"}));
    const hokuto::format::File other = hokuto::format::ImportDseImage(TestImage("ATST", {"lt4_main_en.fa/bgm/"}));

    const hokuto::format::File loaded = hokuto::format::Read(hokuto::format::Write(file));
    const hokuto::edit::OtherConsoleSong sent = hokuto::edit::ForOtherConsole(loaded, 0, ntdlib::sar::Flavor::kCafe);

    SEQLIB_REQUIRE(loaded.native && loaded.native->cartridge);
    Check(hokuto::song::DseGameCodeOf(*loaded.native->cartridge->image) == "BLFE", "the saved file's game code");
    Check(sent.file == hokuto::edit::ForOtherConsole(file, 0, ntdlib::sar::Flavor::kCafe).file,
          "a saved file's song converts as before the save");
    Check(sent.file != hokuto::edit::ForOtherConsole(other, 0, ntdlib::sar::Flavor::kCafe).file, "and not at 10 ms");
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("dse_ticks",
                                    []
                                    {
                                        TestTicks();
                                        TestSlowTick();
                                        TestSavedFile();
                                    });
}
