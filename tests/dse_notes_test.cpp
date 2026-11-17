// SPDX-License-Identifier: MIT

// Tests the note edits of DSE's songs (src/edit/notes.cpp). A song of DSE's files accepts the edits by the rules of
// DSE's bytes.

#include "ntdlib/dse/player.h"

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
            "This sound has no bank in slot 1.");
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

// A DS image whose sound effect loads two banks: the song's, 0x7F00, in slot 0, and a copy of it with the id 0x7E00 in
// slot 1. The effect's track `track` follows its 4 bytes of preamble.
File TwoBankImage(const ntdlib::testing::dse_song_files::Bytes& track)
{
    namespace dse_files = ntdlib::testing::dse_song_files;
    namespace nds_files = ntdlib::testing::nds_files;
    dse_files::Bytes other = dse_files::BankFile();
    other[0x0E] = 0x00;
    other[0x0F] = 0x7E;
    nds_files::Image image;
    image.files = {{"SOUND/BGM/bgm.swd", dse_files::MainBankFile()},
                   {"SOUND/BGM/bgm0001.smd", dse_files::SongFile({{0, 0, 0, 0, 0xA4, 0x78, 0x98}})},
                   {"SOUND/BGM/bgm0001.swd", dse_files::BankFile()},
                   {"SOUND/SE/se.swd", other},
                   {"SOUND/SE/se.sed",
                    dse_files::EffectsFile({dse_files::Effect{.tracks = {dse_files::Join({{0, 0, 0, 0}, track})},
                                                              .banks = {0x7F00, 0x7E00}}},
                                           0x0101)}};

    return hokuto::format::ImportDseImage(nds_files::BuildImage(image).first);
}

// The bank slot and the program of each note of the first track of sound 1 of `file`.
std::vector<std::pair<int, uint32_t>> NoteInstruments(const File& file)
{
    const DseSong song = DseSongOf(file, 1);
    std::vector<std::pair<int, uint32_t>> instruments;
    XSFLIB_CHECK(!song.played.tracks.empty());
    if (song.played.tracks.empty())
    {
        return instruments;
    }

    for (const Note& note : song.played.tracks[0].notes)
    {
        instruments.emplace_back(note.bank_slot, note.program);
    }

    return instruments;
}

// The DSE commands of sequence `sequence` of `file` that select a whole bank id (0xA8), by their ids.
std::vector<uint16_t> WholeBankSelects(const File& file, uint32_t sequence)
{
    std::vector<uint16_t> ids;
    for (const ntdlib::commands::Command& command : file.sequences.at(sequence)->items)
    {
        if (command.kind == Kind::kOther && command.encoding.opcode == ntdlib::dse::kBank && command.bytes.size() == 2)
        {
            ids.push_back(static_cast<uint16_t>(command.bytes[0] << 8 | command.bytes[1]));
        }
    }

    return ids;
}

// A DSE track selects another bank that its sound loads. The notes of program 0 take bank 0x7E00 of slot 1. A bank
// select before the program change of each run of them selects it. One before the next program change selects bank
// 0x7F00 again, and the track's end requires none. The note of program 1 between them keeps its bank. Every note keeps
// its tick, key, length and velocity. A new program goes in with the new bank, and a second edit takes the notes back.
// A slot without a bank is refused.
void TestDseOtherBank()
{
    namespace dse_files = ntdlib::testing::dse_song_files;
    using Instruments = std::vector<std::pair<int, uint32_t>>;
    const File file = TwoBankImage(dse_files::Join({{0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00},
                                                    dse_files::Note(0x60, 0, 0, 0x18, 1),
                                                    {0x92, 0x18, 0xAC, 0x01},
                                                    dse_files::Note(0x60, 2, 0, 0x18, 1),
                                                    {0x92, 0x18, 0xAC, 0x00},
                                                    dse_files::Note(0x60, 4, 0, 0x18, 1),
                                                    {0x92, 0x18, 0x98}}));
    const DseSong song = DseSongOf(file, 1);
    SEQLIB_REQUIRE(song.played.tracks.size() == 1);
    const hokuto::song::Track& track = song.played.tracks[0];
    XSFLIB_CHECK((NoteInstruments(file) == Instruments{{0, 0}, {0, 1}, {0, 0}}));

    const Edited edited =
        hokuto::edit::EditInstrument(file, song.sequence, track.number, track.stretches, {0, 0}, {1, 0});
    XSFLIB_CHECK((NoteInstruments(edited.file) == Instruments{{1, 0}, {0, 1}, {1, 0}}));
    XSFLIB_CHECK(GbaNotes(DseSongOf(edited.file, 1).played, 0) == GbaNotes(song.played, 0));
    XSFLIB_CHECK((WholeBankSelects(edited.file, song.sequence) == std::vector<uint16_t>{0x7E00, 0x7F00, 0x7E00}));

    const Edited both =
        hokuto::edit::EditInstrument(file, song.sequence, track.number, track.stretches, {0, 0}, {1, 3});
    XSFLIB_CHECK((NoteInstruments(both.file) == Instruments{{1, 3}, {0, 1}, {1, 3}}));
    // Program 3 plays a wave that the main bank lacks, and its notes end elsewhere. They keep their ticks and keys.
    const auto starts = [](const Sequence& played)
    {
        std::vector<std::pair<uint32_t, uint32_t>> notes;
        for (const Note& note : played.tracks.at(0).notes)
        {
            notes.emplace_back(note.start, note.key);
        }
        return notes;
    };
    XSFLIB_CHECK(starts(DseSongOf(both.file, 1).played) == starts(song.played));

    const DseSong moved = DseSongOf(edited.file, 1);
    const hokuto::song::Track& again = moved.played.tracks.at(0);
    const Edited back =
        hokuto::edit::EditInstrument(edited.file, moved.sequence, again.number, again.stretches, {1, 0}, {0, 0});
    XSFLIB_CHECK((NoteInstruments(back.file) == Instruments{{0, 0}, {0, 1}, {0, 0}}));

    XSFLIB_CHECK(
        ErrorOf<std::invalid_argument>(
            [&]
            { hokuto::edit::EditInstrument(file, song.sequence, track.number, track.stretches, {0, 0}, {2, 0}); }) ==
        "This sound has no bank in slot 2.");
}

// The channel at each note of the first track of sound 1 of `file`, as ntdlib's DSE driver plays them in the first 20
// seconds. A looped track plays several passes.
std::vector<ntdlib::dse::ChannelView> NoteChannels(const File& file)
{
    const std::shared_ptr<const hokuto::song::DseSet> set =
        hokuto::song::LoadDseSet(hokuto::format::ExportArchive(file), ntdlib::dse::Build::kLater);
    ntdlib::dse::Player player({.build = set->build});
    std::vector<ntdlib::dse::ChannelView> channels;
    ntdlib::dse::SequenceHooks hooks;
    hooks.on_note = [&player, &channels](const ntdlib::dse::NoteEvent& note)
    {
        if (note.track == 0)
        {
            channels.push_back(player.Driver().GetChannel(note.channel));
        }
    };
    player.Driver().SetHooks(std::move(hooks));
    const ntdlib::dse::SequenceFiles files = set->sounds.at(1).Files();
    player.LoadBanks(files);
    XSFLIB_CHECK(player.Start(files));
    for (int tick = 0; tick < 2000; tick++)
    {
        player.RunTick();
    }

    return channels;
}

// The bend range and the volume LFO (LFO 1) of the channel at each note (NoteChannels).
std::vector<std::pair<uint8_t, ntdlib::dse::LfoValues>> ChannelSettings(const File& file)
{
    std::vector<std::pair<uint8_t, ntdlib::dse::LfoValues>> settings;
    for (const ntdlib::dse::ChannelView& channel : NoteChannels(file))
    {
        settings.emplace_back(channel.bend_range, channel.lfos[1]);
    }

    return settings;
}

// The bank of the channel at each note (NoteChannels).
std::vector<uint16_t> NoteBanks(const File& file)
{
    std::vector<uint16_t> banks;
    for (const ntdlib::dse::ChannelView& channel : NoteChannels(file))
    {
        banks.push_back(channel.bank);
    }

    return banks;
}

// A bank select selects the channel's program again, and that resets the channel's bend range and LFOs. The new select
// goes before the program change that the notes follow. The bend range (0xDB) and the volume LFO (0xE4) after the
// program change then still reach the notes of the new bank. A run whose notes follow a repeat's start has no such
// command before it. Its new select would reset the track's bend range, and the edit refuses it.
void TestDseOtherBankSettings()
{
    namespace dse_files = ntdlib::testing::dse_song_files;
    using Instruments = std::vector<std::pair<int, uint32_t>>;
    const File file = TwoBankImage(dse_files::Join({{0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xDB, 0x0C},
                                                    {0xE4, 0x10, 0x00, 0x64, 0x00, 0x01},
                                                    dse_files::Note(0x60, 0, 0, 0x18, 1),
                                                    {0x92, 0x18, 0xAC, 0x01},
                                                    dse_files::Note(0x60, 2, 0, 0x18, 1),
                                                    {0x92, 0x18, 0x98}}));
    const DseSong song = DseSongOf(file, 1);
    SEQLIB_REQUIRE(song.played.tracks.size() == 1);
    const hokuto::song::Track& track = song.played.tracks[0];
    const auto before = ChannelSettings(file);
    SEQLIB_REQUIRE(before.size() >= 2);
    XSFLIB_CHECK(before[0].first == 12 && before[0].second.type == 1 && before[0].second.amplitude == 0x10);

    const Edited edited =
        hokuto::edit::EditInstrument(file, song.sequence, track.number, track.stretches, {0, 0}, {1, 0});
    XSFLIB_CHECK((NoteInstruments(edited.file) == Instruments{{1, 0}, {0, 1}}));
    XSFLIB_CHECK(ChannelSettings(edited.file) == before);

    const File repeated = TwoBankImage(dse_files::Join({{0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xDB, 0x0C, 0x9C, 0x02},
                                                        dse_files::Note(0x60, 0, 0, 0x18, 1),
                                                        {0x92, 0x18, 0x9D, 0x98}}));
    const DseSong looped = DseSongOf(repeated, 1);
    SEQLIB_REQUIRE(looped.played.tracks.size() == 1);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>(
                     [&]
                     {
                         hokuto::edit::EditInstrument(repeated, looped.sequence, looped.played.tracks[0].number,
                                                      looped.played.tracks[0].stretches, {0, 0}, {1, 0});
                     }) ==
                 "Can't change the bank. A new bank select would reset this track's bend range, LFOs and "
                 "envelope. Remove these commands in the List Editor first: Bend Range.");
}

// The edit of `file`'s first track of sound 1 from slot 0 to slot 1, for program 0.
Edited OtherBankOf(const File& file)
{
    const DseSong song = DseSongOf(file, 1);
    const hokuto::song::Track& track = song.played.tracks.at(0);
    return hokuto::edit::EditInstrument(file, song.sequence, track.number, track.stretches, {0, 0}, {1, 0});
}

// A looped track starts each later pass with the bank that it ends with. A run of program 0 at the end of the loop
// takes a select back to bank 0x7F00 at the track's end, and every pass plays program 1 in that bank. With a bend range
// in the track, that select would reset it, and the edit refuses it. A loop of program 0 alone takes the new bank
// before its point, and needs no select back. Its bend range then stays. An envelope command blocks the edit too. A
// track whose loop starts with another program than it ends with refuses a select after the loop's point.
void TestDseOtherBankLoop()
{
    namespace dse_files = ntdlib::testing::dse_song_files;
    const auto track = [](std::initializer_list<uint8_t> opening)
    {
        return dse_files::Join({dse_files::Bytes(opening),
                                {0x99, 0xAC, 0x01},
                                dse_files::Note(0x60, 2, 0, 0x18, 1),
                                {0x92, 0x18, 0xAC, 0x00},
                                dse_files::Note(0x60, 0, 0, 0x18, 1),
                                {0x92, 0x18, 0x98}});
    };
    const File looped = TwoBankImage(track({0xA9, 0x7F, 0xAA, 0x00}));
    const std::vector<uint16_t> before = NoteBanks(looped);
    SEQLIB_REQUIRE(before.size() >= 4);
    const std::vector<uint16_t> after = NoteBanks(OtherBankOf(looped).file);
    SEQLIB_REQUIRE(after.size() == before.size());
    for (std::size_t i = 0; i < after.size(); i++)
    {
        XSFLIB_CHECK(after[i] == (i % 2 == 0 ? 0x7F00 : 0x7E00));
    }

    const File bent = TwoBankImage(track({0xA9, 0x7F, 0xAA, 0x00, 0xDB, 0x0C}));
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { OtherBankOf(bent); }) ==
                 "Can't change the bank. A new bank select would reset this track's bend range, LFOs and envelope. "
                 "Remove these commands in the List Editor first: Bend Range.");

    // The List Editor can't add an envelope command again. The message names the command, and gives no remedy.
    const File enveloped = TwoBankImage(track({0xA9, 0x7F, 0xAA, 0x00, 0xB2, 0x10, 0xDB, 0x0C}));
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { OtherBankOf(enveloped); }) ==
                 "Can't change the bank. A new bank select would reset this track's bend range, LFOs and envelope. "
                 "These commands set them: Opcode 0xB2, Bend Range.");

    // The loop starts with program 0, and the track ends with program 1. A later pass would run a select after the
    // loop's point with program 1, and the edit refuses it.
    const File changing = TwoBankImage(dse_files::Join({{0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00},
                                                        dse_files::Note(0x60, 0, 0, 0x18, 1),
                                                        {0x92, 0x18, 0x99},
                                                        dse_files::Note(0x60, 2, 0, 0x18, 1),
                                                        {0x92, 0x18, 0xAC, 0x01},
                                                        dse_files::Note(0x60, 4, 0, 0x18, 1),
                                                        {0x92, 0x18, 0x98}}));
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { OtherBankOf(changing); }) ==
                 "Can't change the bank. This track's loop starts with another instrument than the track ends with.");

    const File alone = TwoBankImage(dse_files::Join({{0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xDB, 0x0C, 0x99},
                                                     dse_files::Note(0x60, 0, 0, 0x18, 1),
                                                     {0x92, 0x18},
                                                     dse_files::Note(0x60, 2, 0, 0x18, 1),
                                                     {0x92, 0x18, 0x98}}));
    const Edited edited = OtherBankOf(alone);
    XSFLIB_CHECK((WholeBankSelects(edited.file, DseSongOf(alone, 1).sequence) == std::vector<uint16_t>{0x7E00}));
    const std::vector<ntdlib::dse::ChannelView> played = NoteChannels(edited.file);
    SEQLIB_REQUIRE(played.size() >= 4);
    for (const ntdlib::dse::ChannelView& channel : played)
    {
        XSFLIB_CHECK(channel.bank == 0x7E00 && channel.bend_range == 12);
    }
}

// Two runs of program 0 with a program change between them that selects program 0 again. The select back after the
// first run and the select of the new bank before the second would meet at that program change. They cancel out, and
// the edit puts in one select. The track's end needs no select back.
void TestDseOtherBankCancel()
{
    namespace dse_files = ntdlib::testing::dse_song_files;
    const File file = TwoBankImage(dse_files::Join({{0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00},
                                                    dse_files::Note(0x60, 0, 0, 0x18, 1),
                                                    {0x92, 0x18, 0xAC, 0x00},
                                                    dse_files::Note(0x60, 2, 0, 0x18, 1),
                                                    {0x92, 0x18, 0x98}}));
    const Edited edited = OtherBankOf(file);
    XSFLIB_CHECK((WholeBankSelects(edited.file, DseSongOf(file, 1).sequence) == std::vector<uint16_t>{0x7E00}));
    XSFLIB_CHECK((NoteBanks(edited.file) == std::vector<uint16_t>{0x7E00, 0x7E00}));
}

// A repeat plays its notes again from the command after its start. The new bank select goes in after that start, and
// every pass plays the new bank. A track without notes takes the new bank in its opening.
void TestDseOtherBankRepeat()
{
    namespace dse_files = ntdlib::testing::dse_song_files;
    using Instruments = std::vector<std::pair<int, uint32_t>>;
    const File file = TwoBankImage(dse_files::Join({{0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0x9C, 0x02},
                                                    dse_files::Note(0x60, 0, 0, 0x18, 1),
                                                    {0x92, 0x18, 0x9D, 0xAC, 0x01},
                                                    dse_files::Note(0x60, 2, 0, 0x18, 1),
                                                    {0x92, 0x18, 0x98}}));
    const DseSong song = DseSongOf(file, 1);
    SEQLIB_REQUIRE(song.played.tracks.size() == 1);
    const hokuto::song::Track& track = song.played.tracks[0];
    const Instruments before = NoteInstruments(file);
    SEQLIB_REQUIRE(before.size() >= 3 && before.back() == std::make_pair(0, 1u));

    const Edited edited =
        hokuto::edit::EditInstrument(file, song.sequence, track.number, track.stretches, {0, 0}, {1, 0});
    Instruments expected(before.size() - 1, {1, 0});
    expected.emplace_back(0, 1);
    XSFLIB_CHECK(NoteInstruments(edited.file) == expected);
    XSFLIB_CHECK(GbaNotes(DseSongOf(edited.file, 1).played, 0) == GbaNotes(song.played, 0));

    const File empty = TwoBankImage({0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0x92, 0x18, 0x98});
    const DseSong quiet = DseSongOf(empty, 1);
    SEQLIB_REQUIRE(quiet.played.tracks.size() == 1);
    const File chosen = hokuto::edit::EditOpeningInstrument(empty, quiet.sequence, quiet.played.tracks[0], {1, 2}).file;
    const DseSong opened = DseSongOf(chosen, 1);
    const std::optional<hokuto::edit::Program> opening =
        hokuto::edit::OpeningInstrument(chosen, opened.sequence, opened.played.tracks.at(0));
    XSFLIB_CHECK(opening && opening->slot == 1 && opening->number == 2);

    const NewNote note{.stretch = quiet.played.tracks[0].stretches.at(0), .tick = 0, .key = 60, .length = 12};
    const File taken =
        hokuto::edit::EditNotesWithInstrument(empty, quiet.sequence, quiet.played.tracks[0], std::vector{note}, {1, 2})
            .file;
    XSFLIB_CHECK((NoteInstruments(taken) == Instruments{{1, 2}}));
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
                                        TestDseOtherBank();
                                        TestDseOtherBankRepeat();
                                        TestDseOtherBankSettings();
                                        TestDseOtherBankLoop();
                                        TestDseOtherBankCancel();
                                        TestDseMixMoves();
                                        TestDseValues();
                                    });
}
