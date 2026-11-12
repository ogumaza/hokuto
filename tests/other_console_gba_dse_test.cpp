// SPDX-License-Identifier: MIT

// Tests the conversions of GBA songs and DSE songs to the other consoles through timelines. A song of the GBA converts
// to the Wii U as a timeline, and on to the Switch, the 3DS and the Wii. Each has to play the GBA's notes at their
// times, and loop where the GBA loops.

#include "driver_test_support.h"
#include "other_console_fixtures.h"

namespace
{

using hokuto::testing::TakeRead;

// A song of the GBA converts to the Wii U, and on to the Switch, the 3DS and the Wii. The new file contains the song as
// a timeline of NintendoWare's MIDI converter. ntdlib's performer has to play each note with its key and level within
// 10 ms of the time at which the GBA plays it, and loop where the GBA loops. MP2K plays its tempo of 150 slower than it
// reads, and the warnings say that the tempo moves between whole tempos. The file has to come back from both forms. A
// song without notes is refused.
void TestGba()
{
    const std::vector<uint8_t> bytes = GbaCart();
    const File file = format::ImportGbaRom(bytes);
    const std::shared_ptr<const song::GbaRom> rom = song::LoadGbaRom(bytes);
    const song::Sequence gba = song::ReadSequence(*rom, 0);
    std::string name = song::GbaSongName(*rom, 0);
    std::ranges::replace(name, ' ', '_');

    const hokuto::edit::OtherConsoleSong wii_u = hokuto::edit::ForOtherConsole(file, 0, sar::Flavor::kCafe);
    XSFLIB_CHECK(wii_u.file.form == format::ArchiveForm::kModel && FlavorOf(*wii_u.file.native) == sar::Flavor::kCafe &&
                 wii_u.file.engine == format::Engine::kWiiU);
    XSFLIB_CHECK(wii_u.file.timelines && wii_u.file.timelines->contains(0));
    XSFLIB_CHECK(wii_u.file.native->tables->sounds.size() == 1 &&
                 wii_u.file.native->tables->sounds[0].name == "SEQ_" + name &&
                 wii_u.file.native->tables->sounds[0].pan_curve == 3);
    const auto alternates = [](const std::string& warning)
    {
        return warning.starts_with("NintendoWare tempos must be whole BPM values.") &&
               warning.find("alternates between the whole tempos below and above") != std::string::npos;
    };
    XSFLIB_CHECK(std::ranges::any_of(wii_u.warnings, alternates));
    XSFLIB_CHECK(PlaysAsGba(gba, song::ReadSequence(*ArchiveOf(wii_u.file), 0)));
    XSFLIB_CHECK(ComesBack(wii_u.file));

    for (const sar::Flavor to : {sar::Flavor::kNx, sar::Flavor::kCtr})
    {
        const hokuto::edit::OtherConsoleSong next = hokuto::edit::ForOtherConsole(file, 0, to);
        XSFLIB_CHECK(FlavorOf(*next.file.native) == to && next.file.engine == format::EngineOf(to));
        XSFLIB_CHECK(PlaysAsGba(gba, song::ReadSequence(*ArchiveOf(next.file), 0)));
        XSFLIB_CHECK(ComesBack(next.file));
    }

    const hokuto::edit::OtherConsoleSong wii = hokuto::edit::ForWii(file, 0);
    XSFLIB_CHECK(wii.file.engine == format::Engine::kWii);
    XSFLIB_CHECK(PlaysAsGba(gba, song::ReadSequence(*WiiArchiveOf(wii.file), 0)));
    XSFLIB_CHECK(ComesBack(wii.file));

    const std::string silent = song::GbaSongName(*rom, 1) + " has no notes. The MIDI file would be empty.";
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::ForOtherConsole(file, 1, sar::Flavor::kCafe); }) ==
                 silent);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::ForWii(file, 1); }) == silent);

    // The conversion of a song without notes throws EmptyConversion. The command line counts such songs apart from
    // failures.
    bool empty = false;
    try
    {
        song::ConvertToMidi(*rom, 1, "Hokuto");
    }
    catch (const song::EmptyConversion&)
    {
        empty = true;
    }
    XSFLIB_CHECK(empty);
}

// A sound of DSE in a DS image, with two notes on one track.
struct DseFixture
{
    File file;
};

// The fixture of the tests of a sound of DSE.
DseFixture MakeDseFixture()
{
    namespace dse_files = ntdlib::testing::dse_song_files;
    namespace nds_files = ntdlib::testing::nds_files;
    using dse_files::Join;
    const dse_files::Bytes conductor = {0, 0, 0, 0, 0xA4, 0x78, 0x98};
    const dse_files::Bytes track = Join({{1, 1, 0, 0},
                                         {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x7F},
                                         dse_files::Note(0x60, 0, 0, 0x18, 1),
                                         {0x92, 0x18},
                                         dse_files::Note(0x50, 7, 0, 0x18, 1),
                                         {0x92, 0x18, 0x98}});
    nds_files::Image image;
    image.files = {{"SOUND/BGM/bgm.swd", dse_files::MainBankFile()},
                   {"SOUND/BGM/bgm0001.smd", dse_files::SongFile({conductor, track})},
                   {"SOUND/BGM/bgm0001.swd", dse_files::BankFile()}};
    const File file = format::ImportDseImage(nds_files::BuildImage(image).first);

    return {file};
}

// Key 48 at 0 and key 55 at 24 ticks of the song's 48 a quarter, at 120 beats a minute: 250 ms.
void CheckDseNotes(const std::vector<std::tuple<double, uint8_t, uint8_t>>& notes)
{
    SEQLIB_REQUIRE(notes.size() == 2);
    XSFLIB_CHECK(std::get<1>(notes[0]) == 48 && std::get<2>(notes[0]) == 0x60);
    XSFLIB_CHECK(std::get<1>(notes[1]) == 55 && std::get<2>(notes[1]) == 0x50);
    XSFLIB_CHECK(std::abs(std::get<0>(notes[1]) - std::get<0>(notes[0]) - 250.0) < 2.0);
}

// A sound of DSE converts to the Wii U through ntdlib's converter, and on from there. Each console plays the sound's
// notes, with their keys and velocities, at their times.
void TestDseToConsoles()
{
    const DseFixture fixture = MakeDseFixture();
    const File& file = fixture.file;

    for (const sar::Flavor to : {sar::Flavor::kCafe, sar::Flavor::kNx, sar::Flavor::kCtr})
    {
        const hokuto::edit::OtherConsoleSong sent = hokuto::edit::ForOtherConsole(file, 0, to);
        XSFLIB_CHECK(FlavorOf(*sent.file.native) == to);
        XSFLIB_CHECK(sent.file.native->tables->sounds.at(0).name == "SEQ_bgm0001");
        CheckDseNotes(TimedNotes(song::ReadSequence(*ArchiveOf(sent.file), 0)));
    }
    const hokuto::edit::OtherConsoleSong wii = hokuto::edit::ForWii(file, 0);
    XSFLIB_CHECK(wii.file.native->engine == format::Engine::kWii);
    CheckDseNotes(TimedNotes(
        song::ReadSequence(*song::LoadWiiArchive(format::ExportArchive(hokuto::edit::FoldTimelines(wii.file)),
                                                 "SEQ_bgm0001", ntdlib::rvl::snd::kRsarOptions),
                           0)));
    const hokuto::edit::OtherConsoleSong ds = hokuto::edit::ForDs(file, 0);
    XSFLIB_CHECK(ds.file.native->engine == format::Engine::kDs);
    CheckDseNotes(TimedNotes(song::ReadSequence(*song::LoadDsArchive(format::ExportArchive(ds.file)), 0)));
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::ForDs(file, 99); }) ==
                 "The image has no DSE sound 99.");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::ForOtherConsole(file, 99, sar::Flavor::kCafe); }) ==
                 "The image has no DSE sound 99.");
}

// The engines that the mixer's switch offers to a sound of DSE.
void TestDseEngines()
{
    const DseFixture fixture = MakeDseFixture();
    const File& file = fixture.file;

    // The mixer's switch offers the engines of the later consoles and of the DS, and DSE only to a sound of DSE.
    XSFLIB_CHECK(!hokuto::edit::EngineRefused(file, format::Engine::kDse));
    for (const format::Engine engine : {format::Engine::kSwitch, format::Engine::kWiiU, format::Engine::k3ds,
                                        format::Engine::kWii, format::Engine::kDs})
    {
        XSFLIB_CHECK(!hokuto::edit::EngineRefused(file, engine));
    }
    XSFLIB_CHECK(hokuto::edit::EngineRefused(file, format::Engine::kGba) ==
                 "Can't play sounds of other consoles on the GBA engine.");
    XSFLIB_CHECK(hokuto::edit::EngineRefused(TestFile(sar::Flavor::kNx), format::Engine::kDse) ==
                 "Can't play sounds of other consoles on the DSE engine.");
}

// The reader plays a sound of DSE on the engine of another console.
void TestDseReader()
{
    const DseFixture fixture = MakeDseFixture();
    const File& file = fixture.file;

    // The reader plays the sound's conversion on another console's engine, and the mixer's mutes follow its tracks.
    using hokuto::song::SequenceReader;
    const std::shared_ptr<const song::DseSet> set =
        song::LoadDseSet(format::ExportArchive(file), ntdlib::dse::Build::kLater);
    SequenceReader reader;
    reader.Request(set, 0,
                   [&file]
                   {
                       const hokuto::edit::OtherConsoleSong converted =
                           hokuto::edit::ForEngine(file, 0, format::Engine::kWiiU);
                       return SequenceReader::EngineSound{
                           .played = song::LoadArchive(format::ExportArchive(converted.file), "test")};
                   });
    std::optional<SequenceReader::Result> read;
    read = TakeRead(reader);
    SEQLIB_REQUIRE(read && read->sequence && read->dse == set && read->engine_sequence && read->engine);
    CheckDseNotes(TimedNotes(*read->engine_sequence));
    const std::array<uint32_t, 32> tracks = song::TracksOfConversion(*read->sequence, *read->engine_sequence);
    XSFLIB_CHECK(tracks[1] != 0);

    // The DS's engine plays the conversion in the same way.
    reader.Request(set, 0,
                   [&file]
                   {
                       const hokuto::edit::OtherConsoleSong converted =
                           hokuto::edit::ForEngine(file, 0, format::Engine::kDs);
                       XSFLIB_CHECK(converted.file.engine == format::Engine::kDs);
                       SequenceReader::EngineSound sound;
                       sound.played = song::LoadDsArchive(format::ExportArchive(converted.file));
                       return sound;
                   });
    read.reset();
    read = TakeRead(reader);
    SEQLIB_REQUIRE(read && read->sequence && read->dse == set && read->engine_sequence && read->engine &&
                   read->engine->played.Ds());
    CheckDseNotes(TimedNotes(*read->engine_sequence));
    XSFLIB_CHECK(song::TracksOfConversion(*read->sequence, *read->engine_sequence)[1] != 0);

    // A conversion that fails leaves the sound on DSE, with the conversion's message.
    reader.Request(set, 0, []() -> SequenceReader::EngineSound { throw std::invalid_argument("No conversion."); });
    read.reset();
    read = TakeRead(reader);
    SEQLIB_REQUIRE(read && read->sequence);
    XSFLIB_CHECK(read->engine_error == "No conversion." && !read->engine && !read->engine_sequence);
}

// A sequence of `timebase` ticks a quarter note, with notes at the ticks and keys of each numbered track.
song::Sequence SongOfNotes(int timebase,
                           std::initializer_list<std::pair<int, std::vector<std::pair<uint32_t, uint8_t>>>> tracks)
{
    song::Sequence sequence;
    sequence.timebase = timebase;
    for (const auto& [number, notes] : tracks)
    {
        song::Track track;
        track.number = number;
        for (const auto& [start, key] : notes)
        {
            track.notes.push_back(song::Note{.start = start, .key = key});
        }
        sequence.tracks.push_back(std::move(track));
    }

    return sequence;
}

// The tracks of a conversion match the tracks of the source by their notes, in quarter notes: a source's track without
// notes matches none, a track of two channels matches two, and tracks that play the same notes match one each. A
// conversion's track that shares no note matches none.
void TestTracksOfConversion()
{
    const song::Sequence shown = SongOfNotes(24, {{0, {{0, 60}, {24, 62}}},
                                                  {1, {}},
                                                  {2, {{0, 40}, {12, 41}}},
                                                  {3, {{0, 72}, {48, 74}}},
                                                  {4, {{0, 72}, {48, 74}}},
                                                  {5, {{96, 50}}}});
    const song::Sequence played = SongOfNotes(48, {{0, {{0, 60}, {48, 62}}},
                                                   {1, {{0, 40}}},
                                                   {2, {{24, 41}}},
                                                   {3, {{0, 72}, {96, 74}}},
                                                   {4, {{0, 72}, {96, 74}}},
                                                   {5, {{192, 50}}},
                                                   {6, {{0, 90}}},
                                                   {7, {}}});
    const std::array<uint32_t, 32> tracks = song::TracksOfConversion(shown, played);
    XSFLIB_CHECK(tracks[0] == 1 << 0);
    XSFLIB_CHECK(tracks[1] == 0);
    XSFLIB_CHECK(tracks[2] == (1 << 1 | 1 << 2));
    XSFLIB_CHECK(tracks[3] == 1 << 3);
    XSFLIB_CHECK(tracks[4] == 1 << 4);
    XSFLIB_CHECK(tracks[5] == 1 << 5);
    for (std::size_t i = 6; i < tracks.size(); i++)
    {
        XSFLIB_CHECK(tracks[i] == 0);
    }

    // A conversion that starts its notes up to a few ticks early. Track 7 plays 24 ticks after track 6, and track 8
    // echoes track 6 two ticks after it. Each track of the conversion is nearest the track it comes from. A note more
    // than an eighth of a quarter note off, or with another key, matches no track.
    const song::Sequence moved_from = SongOfNotes(48, {{6, {{96, 60}, {120, 62}, {144, 64}}},
                                                       {7, {{120, 60}, {144, 62}, {168, 64}}},
                                                       {8, {{98, 60}, {122, 62}, {146, 64}}}});
    const song::Sequence moved = SongOfNotes(48, {{0, {{95, 60}, {119, 62}, {142, 64}}},
                                                  {1, {{118, 60}, {143, 62}, {167, 64}}},
                                                  {2, {{98, 60}, {122, 62}, {145, 64}}},
                                                  {3, {{180, 64}}},
                                                  {4, {{96, 61}}}});
    const std::array<uint32_t, 32> moved_tracks = song::TracksOfConversion(moved_from, moved);
    XSFLIB_CHECK(moved_tracks[6] == 1 << 0);
    XSFLIB_CHECK(moved_tracks[7] == 1 << 1);
    XSFLIB_CHECK(moved_tracks[8] == 1 << 2);

    // A song of DSE can number its tracks past 15. Track 17 keeps a place apart from track 1.
    const song::Sequence many = SongOfNotes(48, {{1, {{0, 60}}}, {17, {{0, 70}}}});
    const song::Sequence many_played = SongOfNotes(48, {{0, {{0, 60}}}, {1, {{0, 70}}}});
    const std::array<uint32_t, 32> many_tracks = song::TracksOfConversion(many, many_played);
    XSFLIB_CHECK(many_tracks[1] == 1 << 0);
    XSFLIB_CHECK(many_tracks[17] == 1 << 1);
}

// The reader returns a song of the GBA and its conversion for the engine of each later console. Each track of the song
// has a track of the conversion, at the same quarter notes with the same keys. A conversion that fails leaves the song,
// with the conversion's message.
void TestGbaOnEngines()
{
    using hokuto::song::SequenceReader;

    const std::vector<uint8_t> bytes = GbaCart();
    const File file = format::ImportGbaRom(bytes);
    const std::shared_ptr<const song::GbaRom> rom = song::LoadGbaRom(bytes);
    const auto wait = [](SequenceReader& reader)
    {
        std::optional<SequenceReader::Result> read;
        read = TakeRead(reader);
        return read;
    };

    for (const format::Engine engine :
         {format::Engine::kSwitch, format::Engine::kWiiU, format::Engine::k3ds, format::Engine::kWii})
    {
        SequenceReader reader;
        reader.Request(rom, 0,
                       [&file, engine]
                       {
                           hokuto::edit::OtherConsoleSong converted = hokuto::edit::ForEngine(file, 0, engine);
                           const std::vector<uint8_t> built = format::ExportArchive(converted.file);
                           if (engine == format::Engine::kWii)
                           {
                               return SequenceReader::EngineSound{
                                   .played = song::LoadWiiArchive(built, "test", ntdlib::rvl::snd::kRsarOptions),
                                   .wii_build = format::BuildOfWiiArchive(*converted.file.native)};
                           }
                           return SequenceReader::EngineSound{.played = song::LoadArchive(built, "test")};
                       });
        const std::optional<SequenceReader::Result> read = wait(reader);
        SEQLIB_REQUIRE(read && read->sequence && read->gba == rom && read->engine_sequence && read->engine);
        XSFLIB_CHECK(read->engine_error.empty());

        const song::Sequence& shown = *read->sequence;
        const song::Sequence& played = *read->engine_sequence;
        const std::array<uint32_t, 32> tracks = song::TracksOfConversion(shown, played);
        XSFLIB_CHECK(tracks[0] != 0 && tracks[1] != 0 && (tracks[0] & tracks[1]) == 0);
        for (const song::Track& track : shown.tracks)
        {
            const auto first = std::ranges::find_if(played.tracks, [&](const song::Track& other)
                                                    { return (tracks[track.number] >> other.number & 1) != 0; });
            SEQLIB_REQUIRE(first != played.tracks.end());
            for (const song::Note& note : track.notes)
            {
                const uint32_t tick =
                    note.start * static_cast<uint32_t>(played.timebase) / static_cast<uint32_t>(shown.timebase);
                XSFLIB_CHECK(std::ranges::any_of(first->notes, [&](const song::Note& other)
                                                 { return other.start == tick && other.key == note.key; }));
            }
        }
    }

    SequenceReader reader;
    reader.Request(rom, 0, []() -> SequenceReader::EngineSound { throw std::invalid_argument("No conversion."); });
    const std::optional<SequenceReader::Result> read = wait(reader);
    SEQLIB_REQUIRE(read && read->sequence);
    XSFLIB_CHECK(read->engine_error == "No conversion." && !read->engine && !read->engine_sequence);
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("other_console_gba_dse",
                                    []
                                    {
                                        TestGba();
                                        TestDseToConsoles();
                                        TestDseEngines();
                                        TestDseReader();
                                        TestTracksOfConversion();
                                        TestGbaOnEngines();
                                    });
}
