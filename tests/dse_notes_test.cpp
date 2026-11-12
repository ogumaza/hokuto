// SPDX-License-Identifier: MIT

// Tests the note edits of DSE's songs (src/edit/notes.cpp). A song of DSE's files accepts the edits by the rules of
// DSE's bytes.

#include "notes_fixtures.h"

namespace
{

// A DS cartridge image with DSE's files: a song and a bank of sound effects. Track 0 of the song sets the tempo and
// ends. Track 1 plays key 48 at tick 0 for 48 ticks, then waits 48. Track 2 waits 24, plays key 55 for 24 and waits
// 24. Each of the two tracks selects the song's bank and program 0.
std::vector<uint8_t> DseImage()
{
    namespace dse_files = ntdlib::testing::dse_song_files;
    namespace nds_files = ntdlib::testing::nds_files;
    using dse_files::Bytes;
    using dse_files::Join;
    using dse_files::Note;
    const Bytes conductor = {0, 0, 0, 0, 0xA4, 0x78, 0x98};
    const Bytes first = Join({{1, 1, 0, 0},
                              {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x64},
                              Note(0x7F, 0, 0, 0x30, 1),
                              {0x92, 0x30, 0x98}});
    const Bytes second = Join({{2, 2, 0, 0},
                               {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0x92, 0x18},
                               Note(0x50, 7, 0, 0x18, 1),
                               {0x92, 0x18, 0x98}});
    const Bytes effect =
        Join({{0, 0, 0, 0, 0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00}, Note(0x60, 0, 0, 0x18, 1), {0x92, 0x18, 0x98}});

    nds_files::Image image;
    image.files = {{"SOUND/BGM/bgm.swd", dse_files::MainBankFile()},
                   {"SOUND/BGM/bgm0001.smd", dse_files::SongFile({conductor, first, second})},
                   {"SOUND/BGM/bgm0001.swd", dse_files::BankFile()},
                   {"SOUND/SE/se.sed", dse_files::EffectsFile({dse_files::Effect{.tracks = {effect}}}, 0x0101)}};

    return nds_files::BuildImage(image).first;
}

// The sequence of a file of DSE's files that sound `sound` plays, and the sound as ntdlib's performer plays it from the
// set that the file builds.
struct DseSong
{
    uint32_t sequence = 0;
    Sequence played;
};

DseSong DseSongOf(const File& file, uint32_t sound = 0)
{
    const std::vector<uint8_t> set = hokuto::format::ExportArchive(file);
    const std::shared_ptr<const hokuto::song::DseSet> loaded =
        hokuto::song::LoadDseSet(set, ntdlib::dse::Build::kLater);
    return {hokuto::format::DseSoundSequences(file, set, hokuto::song::DseSoundRefs(*loaded)).at(sound).value(),
            hokuto::song::ReadSequence(*loaded, sound)};
}

// The song and the sound effect lead to their sequences, and start at their first items. The offsets of a performance
// count from the file's start, but each note's command still names its item.
void TestDseSounds()
{
    const File file = hokuto::format::ImportDseImage(DseImage());
    const std::vector<uint8_t> set = hokuto::format::ExportArchive(file);
    const std::shared_ptr<const hokuto::song::DseSet> loaded =
        hokuto::song::LoadDseSet(set, ntdlib::dse::Build::kLater);
    const std::vector<std::optional<uint32_t>> sequences =
        hokuto::format::DseSoundSequences(file, set, hokuto::song::DseSoundRefs(*loaded));
    XSFLIB_CHECK((sequences == std::vector<std::optional<uint32_t>>{0u, 1u}));

    const std::vector<std::optional<hokuto::edit::SoundPlace>> places = hokuto::edit::DsSoundPlaces(sequences);
    SEQLIB_REQUIRE(places.size() == 2 && places[0] && places[1]);
    XSFLIB_CHECK(places[0]->sequence == 0 && !places[0]->start_field && places[0]->track_fields.empty());
    XSFLIB_CHECK(hokuto::edit::SharingStart(places, 0).empty());

    for (uint32_t sound = 0; sound < 2; sound++)
    {
        const DseSong song = DseSongOf(file, sound);
        const ntdlib::commands::Sequence& commands = *file.sequences.at(song.sequence);
        const std::vector<uint32_t> offsets = hokuto::edit::Offsets(commands, hokuto::edit::ConsoleOf(file));
        bool found = false;
        for (const hokuto::song::Track& track : song.played.tracks)
        {
            for (const Note& note : track.notes)
            {
                const std::optional<uint32_t> item = hokuto::edit::ItemAtOffset(offsets, note.command);
                SEQLIB_REQUIRE(item.has_value());
                XSFLIB_CHECK(commands.items[*item].kind == Kind::kNote && commands.items[*item].key == note.key);
                found = true;
            }
        }
        XSFLIB_CHECK(found);
    }
}

// A shorter, softer note, a new note in a wait and a note a tick earlier. The edits write the tracks' waits out, and
// each track still ends at its tick. New note 72 is out of reach of the octave before it, and starts its bytes with the
// octave's command. Its offset still names it, and it accepts an edit of its own. The set grows, and the file's other
// bytes follow it: the set comes back from a new import.
void TestDseNotes()
{
    const std::vector<uint8_t> image = DseImage();
    const File file = hokuto::format::ImportDseImage(image);
    const std::vector<uint8_t> set = hokuto::format::ExportArchive(file);
    const DseSong song = DseSongOf(file);
    SEQLIB_REQUIRE(song.played.tracks.size() == 3);
    const hokuto::song::Track& first = song.played.tracks[1];
    const hokuto::song::Track& second = song.played.tracks[2];
    XSFLIB_CHECK((GbaNotes(song.played, 1) == std::vector<std::vector<uint32_t>>{{0, 48, 48, 127}}));
    XSFLIB_CHECK((GbaNotes(song.played, 2) == std::vector<std::vector<uint32_t>>{{24, 24, 55, 80}}));

    NoteEdit shorter{.command = first.notes.at(0).command, .stretch = first.stretches.at(first.notes.at(0).stretch)};
    shorter.length = 40;
    shorter.velocity = 90;
    const NoteEdit earlier{
        .command = second.notes.at(0).command, .stretch = second.stretches.at(second.notes.at(0).stretch), .shift = -1};
    const std::vector<NewNote> added = {
        {.stretch = shorter.stretch, .tick = 12, .key = 72, .velocity = 100, .length = 12}};
    const Edited edited = EditNotes(file, song.sequence, std::vector<NoteEdit>{shorter, earlier}, added);

    const DseSong played = DseSongOf(edited.file);
    XSFLIB_CHECK(
        (GbaNotes(played.played, 1) == std::vector<std::vector<uint32_t>>{{0, 40, 48, 90}, {12, 12, 72, 100}}));
    XSFLIB_CHECK((GbaNotes(played.played, 2) == std::vector<std::vector<uint32_t>>{{23, 24, 55, 80}}));

    // The new note's edit finds its item past the octave's command.
    const Note& high = played.played.tracks[1].notes.at(1);
    NoteEdit softer{.command = high.command, .stretch = played.played.tracks[1].stretches.at(high.stretch)};
    softer.velocity = 50;
    const Edited again = EditNotes(edited.file, played.sequence, std::vector<NoteEdit>{softer});
    XSFLIB_CHECK((GbaNotes(DseSongOf(again.file).played, 1) ==
                  std::vector<std::vector<uint32_t>>{{0, 40, 48, 90}, {12, 12, 72, 50}}));

    const std::vector<uint8_t> exported = hokuto::format::ExportArchive(edited.file);
    XSFLIB_CHECK(exported.size() > set.size());
    XSFLIB_CHECK(hokuto::format::ExportArchive(hokuto::format::ImportDseSet(exported)) == exported);
}

// A DS cartridge image with DSE's song `tracks` and its banks.
std::vector<uint8_t> DseImageOf(const std::vector<ntdlib::testing::dse_song_files::Bytes>& tracks)
{
    namespace dse_files = ntdlib::testing::dse_song_files;
    namespace nds_files = ntdlib::testing::nds_files;
    nds_files::Image image;
    image.files = {{"SOUND/BGM/bgm.swd", dse_files::MainBankFile()},
                   {"SOUND/BGM/bgm0001.smd", dse_files::SongFile(tracks)},
                   {"SOUND/BGM/bgm0001.swd", dse_files::BankFile()}};

    return nds_files::BuildImage(image).first;
}

// A legato's command (0xC0) acts on the track's next note. A move of the note before it moves the command with it. The
// next note then still continues in the voices of that note, and the arrangement shows one note.
void TestDseLegato()
{
    namespace dse_files = ntdlib::testing::dse_song_files;
    using dse_files::Join;
    const dse_files::Bytes conductor = {0, 0, 0, 0, 0xA4, 0x78, 0x98};
    const dse_files::Bytes track = Join({{1, 1, 0, 0},
                                         {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x64},
                                         dse_files::Note(0x7F, 0, 0, 0x18, 1),
                                         {0xC0, 0x92, 0x18},
                                         dse_files::Note(0x7F, 0, 0, 0x18, 1),
                                         {0x92, 0x18, 0x98}});
    const File file = hokuto::format::ImportDseImage(DseImageOf({conductor, track}));
    const DseSong song = DseSongOf(file);
    SEQLIB_REQUIRE(song.played.tracks.size() == 2);
    const hokuto::song::Track& played = song.played.tracks[1];
    XSFLIB_CHECK((GbaNotes(song.played, 1) == std::vector<std::vector<uint32_t>>{{0, 48, 48, 127}}));

    const NoteEdit later{
        .command = played.notes.at(0).command, .stretch = played.stretches.at(played.notes.at(0).stretch), .shift = 1};
    const Edited edited = EditNotes(file, song.sequence, std::vector<NoteEdit>{later});
    XSFLIB_CHECK((GbaNotes(DseSongOf(edited.file).played, 1) == std::vector<std::vector<uint32_t>>{{1, 47, 48, 127}}));
    bool after_note = false;
    int legatos = 0;
    for (const ntdlib::commands::Command& command : edited.file.sequences.at(song.sequence)->items)
    {
        if (command.kind == Kind::kOther && command.encoding.opcode == 0xC0)
        {
            XSFLIB_CHECK(after_note);
            ++legatos;
        }
        after_note = command.kind == Kind::kNote;
    }
    XSFLIB_CHECK(legatos == 1);
}

// Tracks of one channel share their mix. An edit of one track's volume changes the commands of the channel, and the
// other track shows the new volume. The reach lists that track.
void TestDseChannelMix()
{
    namespace dse_files = ntdlib::testing::dse_song_files;
    using dse_files::Join;
    const dse_files::Bytes conductor = {0, 0, 0, 0, 0xA4, 0x78, 0x98};
    const dse_files::Bytes track = Join({{1, 0, 0, 0},
                                         {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x64},
                                         dse_files::Note(0x7F, 0, 0, 0x18, 1),
                                         {0x92, 0x18, 0x98}});
    const File file = hokuto::format::ImportDseImage(DseImageOf({conductor, track}));
    const DseSong song = DseSongOf(file);
    SEQLIB_REQUIRE(song.played.tracks.size() == 2);
    const hokuto::song::Track& first = song.played.tracks[0];
    const hokuto::song::Track& second = song.played.tracks[1];
    XSFLIB_CHECK(first.channel == 0 && second.channel == 0);
    XSFLIB_CHECK(hokuto::song::MixStretches(song.played, second).size() ==
                 first.stretches.size() + second.stretches.size());
    XSFLIB_CHECK(first.volume == second.volume);

    const std::vector<std::optional<hokuto::edit::SoundPlace>> places =
        hokuto::edit::DsSoundPlaces(std::vector<std::optional<uint32_t>>{song.sequence});
    const std::vector<hokuto::edit::TrackReach> reach = hokuto::edit::ReachOfTracks(file, places, 0, song.played);
    XSFLIB_CHECK(reach.at(1).mix.at(static_cast<std::size_t>(hokuto::song::Mix::kVolume)).tracks ==
                 std::vector<int>{0});

    const Edited edited =
        hokuto::edit::EditMix(file, song.sequence, second.number, hokuto::song::MixStretches(song.played, second),
                              {hokuto::song::Mix::kVolume, 100, 50});
    const DseSong after = DseSongOf(edited.file);
    SEQLIB_REQUIRE(after.played.tracks.size() == 2);
    XSFLIB_CHECK(!after.played.tracks[1].volume.empty() && after.played.tracks[1].volume.front().value == 50);
    XSFLIB_CHECK(after.played.tracks[0].volume == after.played.tracks[1].volume);
}

// A sound effect's channels start without a bank, and the driver loses a program that comes before a bank's select. A
// track that selects a bank and no program gets its new program after the select. It then plays that program.
void TestDseOpeningBank()
{
    namespace dse_files = ntdlib::testing::dse_song_files;
    namespace nds_files = ntdlib::testing::nds_files;
    const dse_files::Bytes effect = dse_files::Join(
        {{0, 0, 0, 0, 0xA9, 0x7F, 0xAA, 0x00}, dse_files::Note(0x60, 0, 0, 0x18, 1), {0x92, 0x18, 0x98}});
    nds_files::Image image;
    image.files = {{"SOUND/BGM/bgm.swd", dse_files::MainBankFile()},
                   {"SOUND/BGM/bgm0001.smd", dse_files::SongFile({{0, 0, 0, 0, 0xA4, 0x78, 0x98}})},
                   {"SOUND/BGM/bgm0001.swd", dse_files::BankFile()},
                   {"SOUND/SE/se.sed", dse_files::EffectsFile({dse_files::Effect{.tracks = {effect}}}, 0x0101)}};
    const File file = hokuto::format::ImportDseImage(nds_files::BuildImage(image).first);
    const DseSong song = DseSongOf(file, 1);
    SEQLIB_REQUIRE(song.played.tracks.size() == 1);
    const hokuto::song::Track& played = song.played.tracks[0];
    XSFLIB_CHECK(played.program == 0);

    const Edited edited = hokuto::edit::EditInstrument(file, 1, played.number, played.stretches, {0, 0}, {0, 1});
    const DseSong after = DseSongOf(edited.file, 1);
    SEQLIB_REQUIRE(after.played.tracks.size() == 1);
    XSFLIB_CHECK(after.played.tracks[0].program == 1);
    XSFLIB_CHECK(after.played.tracks[0].notes.size() == 1);
}

// A track without notes gets its instrument in its opening, and a first note gets one in the same edit. A channel of a
// song starts in the song's bank. A channel of a sound effect starts without a bank, and gets one with the program.
void TestDseOpeningInstrument()
{
    namespace dse_files = ntdlib::testing::dse_song_files;
    namespace nds_files = ntdlib::testing::nds_files;
    using hokuto::edit::EditNotesWithInstrument;
    using hokuto::edit::EditOpeningInstrument;
    using hokuto::edit::OpeningInstrument;
    const dse_files::Bytes empty = {0x92, 0x18, 0x98};
    nds_files::Image image;
    image.files = {
        {"SOUND/BGM/bgm.swd", dse_files::MainBankFile()},
        {"SOUND/BGM/bgm0001.smd",
         dse_files::SongFile({{0, 0, 0, 0, 0xA4, 0x78, 0x98}, dse_files::Join({{1, 1, 0, 0}, empty})})},
        {"SOUND/BGM/bgm0001.swd", dse_files::BankFile()},
        {"SOUND/SE/se.sed",
         dse_files::EffectsFile({dse_files::Effect{.tracks = {dse_files::Join({{0, 0, 0, 0}, empty})}}}, 0x0101)}};
    const File file = hokuto::format::ImportDseImage(nds_files::BuildImage(image).first);
    for (const uint32_t sound : {0u, 1u})
    {
        const DseSong song = DseSongOf(file, sound);
        SEQLIB_REQUIRE(!song.played.tracks.empty());
        const hokuto::song::Track& track = song.played.tracks.back();
        XSFLIB_CHECK(track.notes.empty());
        XSFLIB_CHECK(!OpeningInstrument(file, song.sequence, track));

        const File chosen = EditOpeningInstrument(file, song.sequence, track, {0, 1}).file;
        const DseSong opened = DseSongOf(chosen, sound);
        SEQLIB_REQUIRE(!opened.played.tracks.empty());
        const std::optional<hokuto::edit::Program> opening =
            OpeningInstrument(chosen, opened.sequence, opened.played.tracks.back());
        XSFLIB_CHECK(opening && opening->slot == 0 && opening->number == 1);

        // A second choice changes the program's command, and a note then plays it.
        const File again = EditOpeningInstrument(chosen, opened.sequence, opened.played.tracks.back(), {0, 0}).file;
        const DseSong changed = DseSongOf(again, sound);
        const std::optional<hokuto::edit::Program> second =
            OpeningInstrument(again, changed.sequence, changed.played.tracks.back());
        XSFLIB_CHECK(second && second->slot == 0 && second->number == 0);

        const NewNote note{.stretch = opened.played.tracks.back().stretches.at(0), .tick = 0, .key = 60, .length = 12};
        const File heard = EditNotes(chosen, opened.sequence, {}, std::vector{note}).file;
        const DseSong played = DseSongOf(heard, sound);
        SEQLIB_REQUIRE(!played.played.tracks.empty());
        SEQLIB_REQUIRE(played.played.tracks.back().notes.size() == 1);
        XSFLIB_CHECK(played.played.tracks.back().program == 1);

        const NewNote first{.stretch = track.stretches.at(0), .tick = 0, .key = 60, .length = 12};
        const File taken = EditNotesWithInstrument(file, song.sequence, track, std::vector{first}, {0, 1}).file;
        const DseSong took = DseSongOf(taken, sound);
        SEQLIB_REQUIRE(!took.played.tracks.empty());
        SEQLIB_REQUIRE(took.played.tracks.back().notes.size() == 1);
        XSFLIB_CHECK(took.played.tracks.back().program == 1);

        XSFLIB_CHECK(
            ErrorOf<std::invalid_argument>([&] { EditOpeningInstrument(file, song.sequence, track, {1, 0}); }) ==
            "Can't change the bank that a DSE track selects.");
    }
}

// A track without notes whose opening selects a program and no bank. Its channel starts without a bank. A note drawn
// there plays nothing (the driver's SelectProgram, 0x02073BC8). A first note gets a bank select before the program
// change, and the program change gets the program of the note. The note then sounds, and the opening contains one
// program change.
void TestDseProgramWithoutBank()
{
    namespace dse_files = ntdlib::testing::dse_song_files;
    namespace nds_files = ntdlib::testing::nds_files;
    using hokuto::edit::DseOpeningSelectsBank;
    using hokuto::edit::EditNotesWithInstrument;
    const dse_files::Bytes opening = {0xAC, 0x01, 0x92, 0x18, 0x98};
    nds_files::Image image;
    image.files = {
        {"SOUND/BGM/bgm.swd", dse_files::MainBankFile()},
        {"SOUND/BGM/bgm0001.smd",
         dse_files::SongFile({{0, 0, 0, 0, 0xA4, 0x78, 0x98}, dse_files::Join({{1, 1, 0, 0}, opening})})},
        {"SOUND/BGM/bgm0001.swd", dse_files::BankFile()},
        {"SOUND/SE/se.sed",
         dse_files::EffectsFile({dse_files::Effect{.tracks = {dse_files::Join({{0, 0, 0, 0}, opening})}}}, 0x0101)}};
    const File file = hokuto::format::ImportDseImage(nds_files::BuildImage(image).first);
    for (const uint32_t sound : {0u, 1u})
    {
        const DseSong song = DseSongOf(file, sound);
        SEQLIB_REQUIRE(!song.played.tracks.empty());
        const hokuto::song::Track& track = song.played.tracks.back();
        XSFLIB_CHECK(track.notes.empty());
        XSFLIB_CHECK(!DseOpeningSelectsBank(file, song.sequence, track));
        const std::optional<hokuto::edit::Program> shown = hokuto::edit::OpeningInstrument(file, song.sequence, track);
        XSFLIB_CHECK(shown && shown->slot == 0 && shown->number == 1);

        // A note alone starts no voice.
        const NewNote note{.stretch = track.stretches.at(0), .tick = 0, .key = 60, .length = 12};
        const DseSong silent = DseSongOf(EditNotes(file, song.sequence, {}, std::vector{note}).file, sound);
        SEQLIB_REQUIRE(!silent.played.tracks.empty());
        XSFLIB_CHECK(silent.played.tracks.back().notes.empty());
        XSFLIB_CHECK(silent.played.tracks.back().unstarted.size() == 1);

        const Edited edited = EditNotesWithInstrument(file, song.sequence, track, std::vector{note}, {0, 1});

        const DseSong heard = DseSongOf(edited.file, sound);
        SEQLIB_REQUIRE(!heard.played.tracks.empty());
        const hokuto::song::Track& after = heard.played.tracks.back();
        SEQLIB_REQUIRE(after.notes.size() == 1);
        XSFLIB_CHECK(after.unstarted.empty());
        XSFLIB_CHECK(after.program == 1);
        XSFLIB_CHECK(DseOpeningSelectsBank(edited.file, heard.sequence, after));
        std::size_t programs = 0;
        for (const ntdlib::commands::Command& command : edited.file.sequences.at(heard.sequence)->items)
        {
            programs += command.kind == Kind::kProgram ? 1 : 0;
        }
        XSFLIB_CHECK(programs == 1);
    }
}

// A DSE channel starts at volume 100. A track without a volume command gets a new command for an edit of it. A slide's
// target and a change's amount of volume move with the values, and a change's amount of pan stays.
void TestDseMixMoves()
{
    namespace dse_files = ntdlib::testing::dse_song_files;
    using dse_files::Join;
    const dse_files::Bytes conductor = {0, 0, 0, 0, 0xA4, 0x78, 0x98};
    const dse_files::Bytes plain = Join(
        {{1, 1, 0, 0}, {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00}, dse_files::Note(0x7F, 0, 0, 0x18, 1), {0x92, 0x18, 0x98}});
    const dse_files::Bytes moving = Join({{2, 2, 0, 0},
                                          {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x40, 0xE8, 0x40},
                                          dse_files::Note(0x7F, 0, 0, 0x18, 1),
                                          {0xE1, 0xF8, 0xE2, 0x10, 0x00, 0x20, 0xE9, 0x04, 0xEA, 0x10, 0x00, 0x50},
                                          {0x92, 0x18, 0x98}});
    const File file = hokuto::format::ImportDseImage(DseImageOf({conductor, plain, moving}));
    const DseSong song = DseSongOf(file);
    SEQLIB_REQUIRE(song.played.tracks.size() == 3);

    const hokuto::song::Track& first = song.played.tracks[1];
    XSFLIB_CHECK(hokuto::song::MixAt(first, hokuto::song::Mix::kVolume, 0) == hokuto::song::kDseDefaultVolume);
    const Edited opened = hokuto::edit::EditMix(file, song.sequence, first.number, first.stretches,
                                                {hokuto::song::Mix::kVolume, 100, 50});
    XSFLIB_CHECK(hokuto::song::MixAt(DseSongOf(opened.file).played.tracks[1], hokuto::song::Mix::kVolume, 0) == 50);

    const hokuto::song::Track& second = song.played.tracks[2];
    const auto opcodes = [](const File& edited, uint16_t opcode)
    {
        for (const ntdlib::commands::Command& command : edited.sequences.at(0)->items)
        {
            if (command.kind == Kind::kOther && command.encoding.opcode == opcode)
            {
                return command.bytes;
            }
        }
        return std::vector<uint8_t>();
    };
    const Edited volume = hokuto::edit::EditMix(file, song.sequence, second.number, second.stretches,
                                                {hokuto::song::Mix::kVolume, 64, 32});
    XSFLIB_CHECK(opcodes(volume.file, 0xE1) == std::vector<uint8_t>{0xFC});
    XSFLIB_CHECK((opcodes(volume.file, 0xE2) == std::vector<uint8_t>{0x10, 0x00, 0x10}));

    const Edited pan =
        hokuto::edit::EditMix(file, song.sequence, second.number, second.stretches, {hokuto::song::Mix::kPan, 0, 8});
    XSFLIB_CHECK(opcodes(pan.file, 0xE9) == std::vector<uint8_t>{0x04});
    XSFLIB_CHECK((opcodes(pan.file, 0xEA) == std::vector<uint8_t>{0x10, 0x00, 0x58}));
}

// The values that DSE's bytes hold. A volume and a pan stop at 0x7F in the driver, a tempo uses a byte from 1, and the
// length of a note up to 3 bytes. No command has a time, a random number or a variable. A send has no command. A track
// keeps its start and its end, and DSE has no timelines and no subroutines.
void TestDseValues()
{
    using hokuto::edit::ArgumentRange;
    using hokuto::edit::ValueRange;
    using ntdlib::commands::kOne;
    using ntdlib::commands::Source;
    const hokuto::song::Console dse{.engine = hokuto::format::Engine::kDse};

    ntdlib::commands::Command note;
    note.kind = Kind::kNote;
    ntdlib::commands::Command tempo;
    tempo.kind = Kind::kTempo;
    ntdlib::commands::Command pan;
    pan.kind = Kind::kPan;
    ntdlib::commands::Command send;
    send.kind = Kind::kSend;

    XSFLIB_CHECK((ArgumentRange(dse, 0, Volume(100), Source::kValue) == ValueRange{0, 127 * kOne}));
    XSFLIB_CHECK((ArgumentRange(dse, 0, note, Source::kValue) == ValueRange{0, 0xFFFFFF * kOne}));
    XSFLIB_CHECK((ArgumentRange(dse, 0, tempo, Source::kValue) == ValueRange{kOne, 255 * kOne}));
    XSFLIB_CHECK((ArgumentRange(dse, 0, pan, Source::kValue) == ValueRange{-64 * kOne, 63 * kOne}));
    XSFLIB_CHECK(!ArgumentRange(dse, 0, Volume(100), Source::kVariable));
    XSFLIB_CHECK(!ArgumentRange(dse, 0, Volume(100), Source::kRandom));
    XSFLIB_CHECK(!ArgumentRange(dse, 0, Volume(100), Source::kValue, true));
    XSFLIB_CHECK(!ArgumentRange(dse, 0, send, Source::kValue));

    const File file = hokuto::format::ImportDseImage(DseImage());
    const DseSong song = DseSongOf(file);
    send.value = ntdlib::commands::Argument{Source::kValue, 10 * kOne, 0};
    const std::vector<NewSetting> settings = {
        {.stretch = song.played.tracks[1].stretches.at(0), .tick = 0, .command = send}};
    const std::string skipped = ErrorOf<std::invalid_argument>([&] { AddSettings(file, song.sequence, settings); });
    XSFLIB_CHECK(skipped.starts_with("DSE doesn't support this setting."));

    // Item 0 starts track 0. Its end comes two items on.
    const ntdlib::commands::Sequence& commands = *file.sequences.at(song.sequence);
    const std::vector<uint32_t> offsets = hokuto::edit::Offsets(commands, hokuto::edit::ConsoleOf(file));
    SEQLIB_REQUIRE(commands.items.at(0).kind == Kind::kData && commands.items.at(2).kind == Kind::kEnd);
    const hokuto::edit::CommandPlace start{offsets[0], {}};
    const hokuto::edit::CommandPlace end{offsets[2], {}};
    const std::string before_start = ErrorOf<std::invalid_argument>(
        [&] { hokuto::edit::InsertCommand(file, song.sequence, start, true, Volume(100)); });
    XSFLIB_CHECK(before_start.starts_with("Can't insert a command before the start of a DSE track."));
    const std::string without_end =
        ErrorOf<std::invalid_argument>([&] { hokuto::edit::RemoveCommands(file, song.sequence, {&end, 1}); });
    XSFLIB_CHECK(without_end.starts_with("Can't remove the start or the end of a DSE track."));
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>(
                     [&] { hokuto::edit::InsertCommand(file, song.sequence, end, false, Volume(100)); })
                     .empty());

    const std::string timeline = ErrorOf<std::invalid_argument>([&] { hokuto::edit::AsTimeline(file, song.sequence); });
    XSFLIB_CHECK(timeline.starts_with("DSE sequences can be edited only as a program."));
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("dse_notes",
                                    []
                                    {
                                        TestDseSounds();
                                        TestDseNotes();
                                        TestDseLegato();
                                        TestDseChannelMix();
                                        TestDseOpeningBank();
                                        TestDseOpeningInstrument();
                                        TestDseProgramWithoutBank();
                                        TestDseMixMoves();
                                        TestDseValues();
                                    });
}
