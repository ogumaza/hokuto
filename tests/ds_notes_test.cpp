// SPDX-License-Identifier: MIT

// Tests the note edits of the DS's sequences (src/edit/notes.cpp). The sequence of a DS sound archive accepts the edits
// by the rules of NITRO-Composer's bytes. A note that the driver never starts leads to the stretch that ran its command
// (UnstartedPlaces in edit/commands.h), and accepts edits there. A note whose new length runs past the end of its track
// moves the end, unless that could change other notes.

#include "notes_fixtures.h"

namespace
{

// The bytes of a DS sequence file that plays two tracks. Track 0 plays key 60 at tick 0 for 48 ticks, and track 1 key
// 67 at tick 24 for 24 ticks. The tracks' notes wait for their length. The notes of a DS track do so when the track
// opens.
//
//    0: FE 03 00        allocate tracks 0 and 1
//    3: 93 01 10 00 00  open track 1 at 16
//    8: C1 64           volume 100
//   10: C0 20           pan 0x20
//   12: 3C 7F 30        note 60, velocity 127, 48 ticks
//   15: FF              end
//   16: C1 40           track 1: volume 64
//   18: 80 18           wait 24 ticks
//   20: 43 50 18        note 67, velocity 80, 24 ticks
//   23: FF              end
const std::vector<uint8_t> kDsTwoTracks = {0xFE, 0x03, 0x00, 0x93, 0x01, 0x10, 0x00, 0x00, 0xC1, 0x64, 0xC0, 0x20,
                                           0x3C, 0x7F, 0x30, 0xFF, 0xC1, 0x40, 0x80, 0x18, 0x43, 0x50, 0x18, 0xFF};

// A DS sound archive like the one that the DS's playback test builds. Its sequence file contains `sequence`. Sounds 0
// and 1 play the file, entry 2 has no sequence, and sound 3 is the one sequence of a sequence archive.
std::vector<uint8_t> DsArchive(const std::vector<uint8_t>& sequence = kDsTwoTracks)
{
    namespace sdat_files = ntdlib::testing::sdat_files;
    const sdat_files::Wave square{
        .loop = 1,
        .loop_length = 4,
        .data = {0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0}};
    constexpr uint16_t kNone = 0xFFFF;

    ntdlib::sdat::ArchiveModel model;
    model.sequences = {ntdlib::sdat::SequenceInfo{0, 0, 127, 64, 64, 0, 0, ""},
                       ntdlib::sdat::SequenceInfo{0, 0, 127, 64, 64, 0, 0, ""}, std::nullopt};
    model.sequence_archives = {ntdlib::sdat::SequenceArchiveInfo{1, "", {}}};
    model.banks = {ntdlib::sdat::BankInfo{2, {0, kNone, kNone, kNone}, ""}};
    model.wave_archives = {ntdlib::sdat::WaveArchiveInfo{3, ""}};
    model.players = {ntdlib::sdat::PlayerInfo{1, 0, 0, 0, ""}};
    model.files = {sdat_files::Sequence(sequence),
                   sdat_files::SequenceArchive({{.offset = 0}}, {0x3C, 0x7F, 0x18, 0xFF}),
                   sdat_files::Bank({{.type = 1, .region = {.wave = 0}}}), sdat_files::WaveArchive({square})};

    return ntdlib::sdat::BuildArchive(model);
}

// The sequence of a DS file that sound 0 plays, and sounds 0 and 1 as ntdlib's performer plays them from the archive
// that the file builds.
struct DsSong
{
    uint32_t sequence = 0;
    Sequence played;
    Sequence second;
};

DsSong DsSongOf(const File& file)
{
    const std::vector<uint8_t> sdat = hokuto::format::ExportArchive(file);
    const std::shared_ptr<const hokuto::song::DsArchive> archive = hokuto::song::LoadDsArchive(sdat);
    return {hokuto::format::DsSoundSequences(file, sdat).at(0).value(), hokuto::song::ReadSequence(*archive, 0),
            hokuto::song::ReadSequence(*archive, 1)};
}

// The INFO block's sounds lead to the sequence that their file became. The sequence archive's sound and the empty
// entry lead nowhere: the cut keeps a sequence archive among the archive's bytes. A sound starts at the first item of
// its sequence.
void TestDsSounds()
{
    const std::vector<uint8_t> sdat = DsArchive();
    const File file = hokuto::format::ImportDsArchive(sdat);
    const std::vector<std::optional<uint32_t>> sequences = hokuto::format::DsSoundSequences(file, sdat);
    XSFLIB_CHECK((sequences == std::vector<std::optional<uint32_t>>{0u, 0u, std::nullopt}));

    const std::vector<std::optional<hokuto::edit::SoundPlace>> places = hokuto::edit::DsSoundPlaces(sequences);
    SEQLIB_REQUIRE(places.size() == 3 && places[0] && places[1] && !places[2]);
    XSFLIB_CHECK(places[0]->sequence == 0 && !places[0]->start_field && places[0]->track_fields.empty());
    XSFLIB_CHECK((hokuto::edit::SharingStart(places, 0) == std::vector<uint32_t>{1}));
}

// A shorter, softer note, a new note in its wait and a note a tick earlier. The edits write the tracks' waits out, and
// each track still ends at tick 48. Both sounds of the file play the edits. The archive grows, and the file's other
// bytes follow it: the archive comes back from a new import.
void TestDsNotes()
{
    const std::vector<uint8_t> sdat = DsArchive();
    const File file = hokuto::format::ImportDsArchive(sdat);
    const DsSong song = DsSongOf(file);
    SEQLIB_REQUIRE(song.played.tracks.size() == 2);
    XSFLIB_CHECK((GbaNotes(song.played, 0) == std::vector<std::vector<uint32_t>>{{0, 48, 60, 127}}));
    XSFLIB_CHECK((GbaNotes(song.played, 1) == std::vector<std::vector<uint32_t>>{{24, 24, 67, 80}}));

    NoteEdit shorter = EditOf(song.played, 0);
    shorter.length = 40;
    shorter.velocity = 90;
    shorter.transpose = 2;
    const Note& second = song.played.tracks[1].notes.at(0);
    const NoteEdit earlier{
        .command = second.command, .stretch = song.played.tracks[1].stretches.at(second.stretch), .shift = -1};
    const std::vector<NewNote> added = {
        {.stretch = shorter.stretch, .tick = 12, .key = 72, .velocity = 100, .length = 12}};
    const Edited edited = EditNotes(file, song.sequence, std::vector<NoteEdit>{shorter, earlier}, added);

    const DsSong played = DsSongOf(edited.file);
    XSFLIB_CHECK(
        (GbaNotes(played.played, 0) == std::vector<std::vector<uint32_t>>{{0, 40, 62, 90}, {12, 12, 72, 100}}));
    XSFLIB_CHECK((GbaNotes(played.played, 1) == std::vector<std::vector<uint32_t>>{{23, 24, 67, 80}}));
    XSFLIB_CHECK(GbaNotes(played.second, 0) == GbaNotes(played.played, 0));

    const std::vector<uint8_t> exported = hokuto::format::ExportArchive(edited.file);
    XSFLIB_CHECK(exported.size() > sdat.size());
    XSFLIB_CHECK(hokuto::format::ExportArchive(hokuto::format::ImportDsArchive(exported)) == exported);
}

// On the DS, the end of a track cuts its notes too. Track 0 waits for its note of 48 ticks, then ends. A new length of
// 60 ticks writes the track's waits out and moves the end to the note's end. Both sounds of the file play the note
// that long. Track 1 plays as before. A track that ties its notes holds a note to the end, whatever its command's
// length: there, the edit is refused. Its bytes turn ties on (C8 01), then play key 60 for 48 ticks and end.
void TestDsNoteOverTheEnd()
{
    const File file = hokuto::format::ImportDsArchive(DsArchive());
    const DsSong song = DsSongOf(file);
    NoteEdit longer = EditOf(song.played, 0);
    longer.length = 60;

    const DsSong played = DsSongOf(EditNotes(file, song.sequence, std::vector<NoteEdit>{longer}).file);
    XSFLIB_CHECK((GbaNotes(played.played, 0) == std::vector<std::vector<uint32_t>>{{0, 60, 60, 127}}));
    XSFLIB_CHECK(GbaNotes(played.played, 1) == GbaNotes(song.played, 1));
    XSFLIB_CHECK(GbaNotes(played.second, 0) == GbaNotes(played.played, 0));

    const File tied = hokuto::format::ImportDsArchive(DsArchive({0xC8, 0x01, 0x3C, 0x7F, 0x30, 0xFF}));
    const DsSong tied_song = DsSongOf(tied);
    NoteEdit held = EditOf(tied_song.played, 0);
    held.length = 60;
    XSFLIB_CHECK(
        ErrorOf<std::invalid_argument>([&] { EditNotes(tied, tied_song.sequence, std::vector<NoteEdit>{held}); }) ==
        "The track ends before this note does. Moving the end could change other notes too. To move it "
        "anyway, lengthen the wait before the End in the List Editor.");
}

// The values that NITRO-Composer's bytes hold. A volume uses a byte, a transpose a signed one, and a tempo 16 signed
// bits from 1. A variable is one of the 32 that the driver keeps. No command has a time, and a send has no command.
// A send's setting is refused with the DS's message.
void TestDsValues()
{
    using hokuto::edit::ArgumentRange;
    using hokuto::edit::ValueRange;
    using ntdlib::commands::kOne;
    using ntdlib::commands::Source;
    const hokuto::song::Console ds{.engine = hokuto::format::Engine::kDs};

    ntdlib::commands::Command transpose;
    transpose.kind = Kind::kTranspose;
    ntdlib::commands::Command tempo;
    tempo.kind = Kind::kTempo;
    ntdlib::commands::Command send;
    send.kind = Kind::kSend;

    XSFLIB_CHECK((ArgumentRange(ds, 0, Volume(100), Source::kValue) == ValueRange{0, 255 * kOne}));
    XSFLIB_CHECK((ArgumentRange(ds, 0, transpose, Source::kValue) == ValueRange{-128 * 100 * kOne, 127 * 100 * kOne}));
    XSFLIB_CHECK((ArgumentRange(ds, 0, Volume(100), Source::kVariable) == ValueRange{0, 31}));
    XSFLIB_CHECK(!ArgumentRange(ds, 0, Volume(100), Source::kValue, true));
    XSFLIB_CHECK(!ArgumentRange(ds, 0, send, Source::kValue));
    const std::optional<ValueRange> tempos = ArgumentRange(ds, 0, tempo, Source::kValue);
    SEQLIB_REQUIRE(tempos.has_value());
    XSFLIB_CHECK(tempos->lowest > 0 && tempos->highest > tempos->lowest);

    const File file = hokuto::format::ImportDsArchive(DsArchive());
    const DsSong song = DsSongOf(file);
    send.value = ntdlib::commands::Argument{Source::kValue, 10 * kOne, 0};
    const std::vector<NewSetting> settings = {
        {.stretch = song.played.tracks[0].stretches.at(0), .tick = 0, .command = send}};
    const std::string refusal = ErrorOf<std::invalid_argument>([&] { AddSettings(file, song.sequence, settings); });
    XSFLIB_CHECK(refusal.starts_with("NITRO-Composer doesn't support this setting."));
}

// A DS archive with one sequence. Its track calls a subroutine twice: first with program 0, then with program 1.
// Program 0 plays a wave, and program 1 has no instrument. The subroutine's note then never starts in the second
// call.
//
//    0: C7 00        note wait off
//    2: 81 00        program 0
//    4: 95 0F 00 00  call 15, at tick 0
//    8: 81 01        program 1
//   10: 95 0F 00 00  call 15, at tick 24
//   14: FF           end
//   15: 3C 7F 18     note 60, velocity 127, 24 ticks
//   18: 80 18        wait 24 ticks
//   20: FD           return
std::vector<uint8_t> DsUnstartedArchive()
{
    namespace sdat_files = ntdlib::testing::sdat_files;
    const sdat_files::Bytes calls = {0xC7, 0x00, 0x81, 0x00, 0x95, 0x0F, 0x00, 0x00, 0x81, 0x01, 0x95,
                                     0x0F, 0x00, 0x00, 0xFF, 0x3C, 0x7F, 0x18, 0x80, 0x18, 0xFD};
    const sdat_files::Wave square{
        .loop = 1,
        .loop_length = 4,
        .data = {0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0}};
    constexpr uint16_t kNone = 0xFFFF;

    ntdlib::sdat::ArchiveModel model;
    model.sequences = {ntdlib::sdat::SequenceInfo{0, 0, 127, 64, 64, 0, 0, ""}};
    model.banks = {ntdlib::sdat::BankInfo{1, {0, kNone, kNone, kNone}, ""}};
    model.wave_archives = {ntdlib::sdat::WaveArchiveInfo{2, ""}};
    model.players = {ntdlib::sdat::PlayerInfo{1, 0, 0, 0, ""}};
    model.files = {sdat_files::Sequence(calls), sdat_files::Bank({{.type = 1, .region = {.wave = 0}}, {}}),
                   sdat_files::WaveArchive({square})};

    return ntdlib::sdat::BuildArchive(model);
}

// A note that the driver never started leads to the stretch that ran its command at the note's start. Both calls run
// the subroutine's note: the first at tick 0, the second at tick 24. Only the second never starts, and its place names
// the second call's stretch. An edit through the place transposes the note and sets a new velocity for it. The note of
// the first call plays the same command, and changes with it.
void TestDsUnstartedPlaces()
{
    constexpr uint32_t kSubroutine = 15;
    const std::vector<uint8_t> sdat = DsUnstartedArchive();
    const File file = hokuto::format::ImportDsArchive(sdat);
    const uint32_t sequence = hokuto::format::DsSoundSequences(file, sdat).at(0).value();

    // ntdlib's performer plays the sound of the archive that a file builds.
    const auto played = [](const File& of)
    {
        return hokuto::song::ReadSequence(*hokuto::song::LoadDsArchive(hokuto::format::ExportArchive(of)), 0);
    };

    const Sequence song = played(file);
    SEQLIB_REQUIRE(song.tracks.size() == 1);
    const hokuto::song::Track& track = song.tracks[0];
    SEQLIB_REQUIRE(track.notes.size() == 1 && track.unstarted.size() == 1);
    XSFLIB_CHECK(track.notes[0].start == 0 && track.notes[0].command == kSubroutine);
    XSFLIB_CHECK(track.unstarted[0].start == 24 && track.unstarted[0].command == kSubroutine);

    // The stretch of the second call.
    const auto second_call = std::ranges::find_if(
        track.stretches, [](const Stretch& stretch) { return stretch.first == kSubroutine && stretch.tick == 24; });
    SEQLIB_REQUIRE(second_call != track.stretches.end());
    XSFLIB_CHECK(second_call->entry == Entry::kCall);

    const std::vector<std::optional<NotePlace>> places = UnstartedPlaces(file, sequence, track);

    SEQLIB_REQUIRE(places.size() == 1 && places[0].has_value());
    XSFLIB_CHECK(places[0]->command == kSubroutine && places[0]->stretch == *second_call);

    const NoteEdit edit{.command = places[0]->command, .stretch = places[0]->stretch, .transpose = 2, .velocity = 90};
    const Sequence after = played(EditNotes(file, sequence, std::vector{edit}).file);

    SEQLIB_REQUIRE(after.tracks.size() == 1 && after.tracks[0].notes.size() == 1);
    SEQLIB_REQUIRE(after.tracks[0].unstarted.size() == 1);
    const Note& unstarted = after.tracks[0].unstarted[0];
    XSFLIB_CHECK(unstarted.start == 24 && unstarted.key == 62 && unstarted.velocity == 90);
    XSFLIB_CHECK(after.tracks[0].notes[0].key == 62 && after.tracks[0].notes[0].velocity == 90);
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("ds_notes",
                                    []
                                    {
                                        TestDsSounds();
                                        TestDsNotes();
                                        TestDsNoteOverTheEnd();
                                        TestDsValues();
                                        TestDsUnstartedPlaces();
                                    });
}
