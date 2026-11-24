// SPDX-License-Identifier: MIT

// Tests the levels at which games on DSE start their songs (format::DseGameStarts, song::DseSoundStart). A game's code
// starts some songs below full level, and DSE's driver then takes the volume level × 127 / 256 at once. ntdlib's
// dse::GameStarts gives the levels by the game's image and the song's path. Its rows come from runs of the games' code.
//
// A small cartridge image from ntdlib's builders gives a game code of the rows and three songs in SOUND/BGM/: one that
// the game starts at one level below full level and at full level in other scenes, one that it starts at full level
// and at another level, and one that no row lists. The import has to give the file the starts of ntdlib's rows: the
// first state as the start without a name, unless it is full level, and each other state as a start named by its
// suffix. The file keeps them through a save and a load. Playback, the read, the dry render, the MIDI export and a
// sound sent to another console have to start the song at its level: the model's output matches ntdlib's dse::Player
// run directly with dse::StartOf, and the MIDI file matches ntdlib's converter with the same start. Each differs from
// the song at full level. A file of an older version gets the starts when it opens, and another game's songs start at
// full level. Where ntdlib's profile of a game can't read the image's code, the import takes the rows of the game and
// the image opens with a note. So does a file of an older version of such an image. The test image takes the game code
// BEZJ, an Inazuma Eleven game whose levels ntdlib reads from its code, and holds none of the game's code.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/dse/game_starts.h"
#include "ntdlib/ntr/rom.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/dse/convert.h"
#include "ntdlib/testing/dse_song_files.h"
#include "ntdlib/testing/nds_files.h"
#include "xsflib/testing/check.h"

#include "dse_playing.h"
#include "edit/dse_files.h"
#include "edit/other_console.h"
#include "format/archive.h"
#include "format/dse_files.h"
#include "format/file.h"
#include "format/text.h"
#include "song/dry_render.h"
#include "song/dse.h"
#include "song/midi_export.h"
#include "song/sequence.h"

namespace
{

namespace files = ntdlib::testing::dse_song_files;
namespace dse_export = ntdlib::sequence_export::dse;
using files::Bytes;
using hokuto::format::File;
using hokuto::format::SoundStart;
using hokuto::song::DseSet;
using hokuto::song::Sequence;
using xsflib::testing::Check;

// The game code of the rows: Pokémon Mystery Dungeon: Explorers of Sky.
constexpr const char* kGame = "C2SE";

// The songs: bgm0086 starts at 180 and at full level, bgm0125 at full level and at 190, and no row lists bgm0001.
constexpr const char* kLowSong = "SOUND/BGM/bgm0086.smd";
constexpr const char* kStatesSong = "SOUND/BGM/bgm0125.smd";
constexpr const char* kFullSong = "SOUND/BGM/bgm0001.smd";

// A song of a conductor at a tempo of 120 and a track that plays note 48 for 48 ticks, then note 55 for 48.
Bytes TestSong()
{
    const Bytes conductor = {0, 0, 0, 0, 0xA4, 0x78, 0x98};
    const Bytes track = files::Join({{1, 1, 0, 0},
                                     {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x7F},
                                     files::Note(0x7F, 0, 0, 0x30, 1),
                                     {0x92, 0x30},
                                     files::Note(0x60, 7, 0, 0x30, 1),
                                     {0x92, 0x30, 0x98}});
    return files::SongFile({conductor, track});
}

// An image of the game `game_code` with the main bank and the three songs with their banks.
std::vector<uint8_t> TestImage(const std::string& game_code)
{
    ntdlib::testing::nds_files::Image image;
    image.game_code = game_code;
    image.files.push_back({"SOUND/BGM/bgm.swd", files::MainBankFile()});
    for (const std::string song : {"bgm0001", "bgm0086", "bgm0125"})
    {
        image.files.push_back({"SOUND/BGM/" + song + ".smd", TestSong()});
        image.files.push_back({"SOUND/BGM/" + song + ".swd", files::BankFile()});
    }

    return ntdlib::testing::nds_files::BuildImage(image).first;
}

// The number of the sound at `path`, or a number past the sounds.
uint32_t SoundAt(const DseSet& set, const std::string& path)
{
    uint32_t sound = 0;
    while (sound < set.sounds.size() && set.sounds[sound].ref.path != path)
    {
        sound++;
    }

    return sound;
}

// The starts that ntdlib's rows give the song at `path` of sound `sound` in the file's form: each state but a first
// state at full level.
std::vector<SoundStart> RowStarts(uint32_t sound, const std::string& path)
{
    std::vector<SoundStart> starts;
    for (const ntdlib::dse::SongState& state : ntdlib::dse::SongStatesOf(kGame, path))
    {
        if (!state.suffix.empty() || state.level != ntdlib::dse::kFullLevel)
        {
            starts.push_back({.sound = sound, .name = state.suffix, .dse_level = state.level});
        }
    }

    return starts;
}

// The output of ntdlib's dse::Player run directly for sound `sound`, with the song started in `state`.
std::vector<int16_t> Direct(const DseSet& set, uint32_t sound, const ntdlib::dse::SongState& state)
{
    return hokuto::testing::DirectDse(set, sound, hokuto::song::DseSoundTick(set, sound), ntdlib::dse::StartOf(state));
}

// The MIDI file of ntdlib's converter for sound `sound`, with the song started in `state`.
std::vector<uint8_t> DirectMidi(const DseSet& set, uint32_t sound, const ntdlib::dse::SongState& state)
{
    const dse_export::PerformOptions options{.build = set.build,
                                             .tick_microseconds = hokuto::song::DseSoundTick(set, sound),
                                             .song_start = ntdlib::dse::StartOf(state)};
    return dse_export::ConvertSequence(set.sounds[sound].Files(), hokuto::song::DseSoundName(set, sound), options,
                                       "Hokuto")
        .midi;
}

// The largest magnitude of the dry render of 2 s of sound `sound`.
float DryPeak(const std::shared_ptr<const DseSet>& set, uint32_t sound)
{
    const auto sequence = std::make_shared<const Sequence>(hokuto::song::ReadSequence(*set, sound));
    const std::atomic<bool> cancel = false;
    const hokuto::song::DryRender render = hokuto::song::DryOf(set, sequence).render(2.0, cancel);
    float peak = 0.0f;
    for (const float sample : render.samples)
    {
        peak = std::max(peak, std::abs(sample));
    }

    return peak;
}

// The import gives the file the starts of ntdlib's rows, and the file keeps them through a save and a load in either
// form. A song that no row lists gets no start. Another game's songs get none.
void TestImport()
{
    const std::vector<uint8_t> image = TestImage(kGame);
    const File file = hokuto::format::ImportDseImage(image);
    const std::shared_ptr<const DseSet> set = hokuto::song::LoadDseSet(image);
    SEQLIB_REQUIRE(set->sounds.size() == 3);
    const uint32_t low = SoundAt(*set, kLowSong);
    const uint32_t states = SoundAt(*set, kStatesSong);
    const uint32_t full = SoundAt(*set, kFullSong);
    SEQLIB_REQUIRE(low < 3 && states < 3 && full < 3);

    std::vector<SoundStart> expected = RowStarts(low, kLowSong);
    const std::vector<SoundStart> state_starts = RowStarts(states, kStatesSong);
    expected.insert(expected.end(), state_starts.begin(), state_starts.end());
    std::ranges::sort(expected, {}, &SoundStart::sound);

    Check(RowStarts(low, kLowSong).size() == 2 && RowStarts(low, kLowSong)[0].dse_level == 180 &&
              state_starts.size() == 1 && state_starts[0].name == "_LEVEL190" && RowStarts(full, kFullSong).empty(),
          "the rows of the songs");
    std::vector<SoundStart> imported = file.starts;
    std::ranges::stable_sort(imported, {}, &SoundStart::sound);
    Check(imported == expected, "the import gives the rows' starts");
    Check(hokuto::format::Read(hokuto::format::Write(file)).starts == file.starts &&
              hokuto::format::ReadText(hokuto::format::WriteText(file)).starts == file.starts,
          "the file keeps them through a save and a load");

    Check(hokuto::song::DseSoundState(*set, low).level == 180 &&
              hokuto::song::DseSoundState(*set, states).level == 256 &&
              hokuto::song::DseSoundState(*set, full).level == 256,
          "each song of the image starts in its first state");
    const std::shared_ptr<const DseSet> of_file = hokuto::edit::DseSetOfFile(file);
    Check(of_file->states == set->states, "the file's set starts its songs as the image's set does");

    Check(hokuto::format::ImportDseImage(TestImage("ATST")).starts.empty(), "another game's songs start at full level");
}

// The levels of an image whose code ntdlib's profile can't read come from the rows of the game, and the image opens
// with a note that says so. An image of the rows' game opens without it.
void TestUnreadLevels()
{
    const std::vector<uint8_t> image = TestImage("BEZJ");
    const ntdlib::ntr::Rom rom = ntdlib::ntr::Rom::Parse(image);
    const hokuto::format::DseLevels levels = hokuto::format::ReadDseLevels(rom);
    const std::optional<std::string> note = hokuto::format::DseLevelsNote(rom);
    const std::shared_ptr<const DseSet> set = hokuto::song::LoadDseSet(image);
    const std::vector<std::string> notes = hokuto::song::DseOpenNotes(image, *set);

    Check(!levels.read && !levels.problem.empty() && note.has_value(), "ntdlib's profile can't read the test image");
    Check(note && std::ranges::find(notes, *note) != notes.end(), "the image opens with the note");
    Check(hokuto::format::ImportDseImage(image).starts.empty(), "the rows of the game give the test's songs no level");
    const std::vector<uint8_t> listed = TestImage(kGame);
    Check(!hokuto::format::DseLevelsNote(ntdlib::ntr::Rom::Parse(listed)).has_value(),
          "an image of the rows' game gives no note");
}

// The song that starts at 180 plays, renders dry, converts and goes to another console at that level. The new file of
// the other console keeps no level, and comes back from both forms.
void TestLevel()
{
    const std::vector<uint8_t> image = TestImage(kGame);
    const File file = hokuto::format::ImportDseImage(image);
    File at_full = file;
    at_full.starts.clear();
    const std::shared_ptr<const DseSet> set = hokuto::edit::DseSetOfFile(file);
    const std::shared_ptr<const DseSet> full_set = hokuto::edit::DseSetOfFile(at_full);
    const uint32_t low = SoundAt(*set, kLowSong);
    SEQLIB_REQUIRE(low < set->sounds.size());
    const ntdlib::dse::SongState state = hokuto::song::DseSoundState(*set, low);
    const ntdlib::dse::SongState full_state;

    const std::vector<int16_t> played = hokuto::testing::ModelledDse(set, low);
    Check(hokuto::testing::StartsDse(played, Direct(*set, low, state)),
          "the model plays what the player plays at the song's level");
    Check(!hokuto::testing::StartsDse(played, Direct(*set, low, full_state)), "and not what it plays at full level");
    Check(hokuto::testing::StartsDse(hokuto::testing::ModelledDse(full_set, low), Direct(*set, low, full_state)),
          "a file without the start plays at full level");

    const float peak = DryPeak(set, low);
    const float full_peak = DryPeak(full_set, low);
    Check(peak > 0.0f && peak < 0.75f * full_peak, "the dry render plays the song lower");

    const std::vector<uint8_t> midi = hokuto::song::ConvertToMidi(*set, low, "Hokuto").midi;
    Check(midi == DirectMidi(*set, low, state), "the MIDI file is the converter's at the song's level");
    Check(midi != DirectMidi(*set, low, full_state), "and not the converter's at full level");

    const hokuto::edit::OtherConsoleSong sent = hokuto::edit::ForOtherConsole(file, low, ntdlib::sar::Flavor::kCafe);
    Check(sent.file != hokuto::edit::ForOtherConsole(at_full, low, ntdlib::sar::Flavor::kCafe).file,
          "a song sent to another console goes at its level");
    Check(sent.file.starts.empty(), "the new file keeps no level, and no start that sets nothing");
    Check(hokuto::format::Read(hokuto::format::Write(sent.file)) == sent.file &&
              hokuto::format::ReadText(hokuto::format::WriteText(sent.file)) == sent.file,
          "the new file comes back from both forms");
}

// A file of version 1 of the format keeps no starts. It gets the game's starts when it opens, by its cartridge image.
void TestOlderFile()
{
    const File file = hokuto::format::ImportDseImage(TestImage(kGame));
    File without = file;
    without.starts.clear();
    std::vector<uint8_t> older = hokuto::format::Write(without);
    older[8] = 1;
    older[10] = 2;

    const File opened = hokuto::format::WithOlderStarts(hokuto::format::Read(older), older);
    Check(opened.starts == file.starts, "a file of version 1 gets the game's starts");
}

// A file of version 1 whose image ntdlib's profile can't read opens with the note of DseLevelsNote. A file of version
// 2, a file without its cartridge, a file of another engine and a file of an image whose levels ntdlib reads get no
// note.
void TestOlderNote()
{
    const std::vector<uint8_t> image = TestImage("BEZJ");
    const File file = hokuto::format::ImportDseImage(image);
    const std::vector<uint8_t> current = hokuto::format::Write(file);
    std::vector<uint8_t> older = current;
    older[8] = 1;
    older[10] = 2;
    const File read = hokuto::format::Read(older);
    const std::optional<std::string> note = hokuto::format::DseLevelsNote(ntdlib::ntr::Rom::Parse(image));
    SEQLIB_REQUIRE(note.has_value());

    Check(hokuto::format::OlderDseLevelsNote(read, older) == note, "a file of version 1 gets the note");
    Check(!hokuto::format::OlderDseLevelsNote(file, current), "a file of version 2 gets none");
    File without = read;
    without.native.Edit([](hokuto::format::NativeArchive& native) { native.cartridge.reset(); });
    Check(!hokuto::format::OlderDseLevelsNote(without, older), "a file without its cartridge gets none");
    File other = read;
    other.engine = hokuto::format::Engine::kSwitch;
    Check(!hokuto::format::OlderDseLevelsNote(other, older), "a file of another engine gets none");

    std::vector<uint8_t> listed = hokuto::format::Write(hokuto::format::ImportDseImage(TestImage(kGame)));
    listed[8] = 1;
    listed[10] = 2;
    Check(!hokuto::format::OlderDseLevelsNote(hokuto::format::Read(listed), listed),
          "a file of an image whose levels ntdlib reads gets none");
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("dse_starts",
                                    []
                                    {
                                        TestImport();
                                        TestUnreadLevels();
                                        TestLevel();
                                        TestOlderFile();
                                        TestOlderNote();
                                    });
}
