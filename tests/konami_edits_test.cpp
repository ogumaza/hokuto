// SPDX-License-Identifier: MIT

// Tests the note and track edits of Konami's driver (src/edit/konami_notes.cpp and konami_tracks.cpp).

#include "ntdlib/konami/sequencer.h"

#include "konami_fixtures.h"

namespace
{

using hokuto::testing::LoopsOver;

// Where each sound of a file plays. The program finds each place the same way.
std::vector<std::optional<edit::SoundPlace>> PlacesOf(const File& file)
{
    const std::vector<std::optional<uint32_t>> sequences = {format::KonamiSongSequence(file, kRomBase + kSongTable, 0),
                                                            format::KonamiSongSequence(file, kRomBase + kSongTable, 1)};
    const std::vector<std::vector<uint32_t>> fields = {format::KonamiTrackFields(file, kRomBase + kSongTable, 0),
                                                       format::KonamiTrackFields(file, kRomBase + kSongTable, 1)};

    return edit::GbaSoundPlaces(sequences, fields);
}

// An edit of a note reaches the ROM's bytes: each case below names the bytes that change from the song's listing.
// The song's other bytes, and the other song, stay.
struct NoteEditsFixture
{
    std::vector<uint8_t> cart;
    File file;
    Sequence song0;
};

// The fixture of the tests of note edits in the first cart.
NoteEditsFixture MakeNoteEditsFixture()
{
    const std::vector<uint8_t> cart = FirstCart();
    const File file = format::ImportKonamiRom(cart);
    const Sequence song0 = ReadOf(file, 0);

    return {cart, file, song0};
}

// A new velocity, a move and a new length of a note change the bytes of the note and its rest.
void TestNoteVelocityAndTimes()
{
    const NoteEditsFixture fixture = MakeNoteEditsFixture();
    const std::vector<uint8_t>& cart = fixture.cart;
    const File& file = fixture.file;
    const Sequence& song0 = fixture.song0;
    const song::Track& voice = *TrackOf(song0, 4);
    const song::Note& first = voice.notes[0];
    const song::Stretch& stretch = voice.stretches.at(first.stretch);

    // A new velocity is the note's volume byte.
    std::vector<uint8_t> expected = cart;
    expected[kSong0 + 2] = 100;
    const edit::NoteEdit velocity{.command = first.command, .stretch = stretch, .velocity = 100};
    XSFLIB_CHECK(format::ExportArchive(edit::EditNotes(file, 0, std::span(&velocity, 1)).file) == expected);

    // A move of a tick takes the note's rest along: the wait before the note grows, and the wait after the rest
    // shrinks.
    expected = cart;
    expected[kSong0] = 1;
    expected[kSong0 + 6] = 1;
    const edit::NoteEdit moved{.command = first.command, .stretch = stretch, .shift = 1};
    const File moved_file = edit::EditNotes(file, 0, std::span(&moved, 1)).file;
    XSFLIB_CHECK(format::ExportArchive(moved_file) == expected);
    XSFLIB_CHECK(StartsOf(ReadOf(moved_file, 0), 4) == (std::vector<uint32_t>{1, 12}));

    // A new length moves the rest after the note.
    expected = cart;
    expected[kSong0 + 4] = 6;
    expected[kSong0 + 6] = 6;
    const edit::NoteEdit shorter{.command = first.command, .stretch = stretch, .length = 6u};
    const File shorter_file = edit::EditNotes(file, 0, std::span(&shorter, 1)).file;
    XSFLIB_CHECK(format::ExportArchive(shorter_file) == expected);
    XSFLIB_CHECK(TrackOf(ReadOf(shorter_file, 0), 4)->notes[0].length == 6);
}

// A transposition changes the byte of a note, and a removal leaves the waits.
void TestNoteTransposeAndRemove()
{
    const NoteEditsFixture fixture = MakeNoteEditsFixture();
    const std::vector<uint8_t>& cart = fixture.cart;
    const File& file = fixture.file;
    const Sequence& song0 = fixture.song0;
    const song::Track& voice = *TrackOf(song0, 4);
    const song::Note& first = voice.notes[0];
    const song::Note& second = voice.notes[1];
    const song::Stretch& stretch = voice.stretches.at(first.stretch);
    std::vector<uint8_t> expected;

    // A transposition moves a sample's semitones in the note's byte. A note without that byte takes it.
    expected = cart;
    expected[kSong0 + 14] = 5;
    const edit::NoteEdit up{.command = second.command, .stretch = voice.stretches.at(second.stretch), .transpose = 3};
    const File up_file = edit::EditNotes(file, 0, std::span(&up, 1)).file;
    XSFLIB_CHECK(format::ExportArchive(up_file) == expected);
    XSFLIB_CHECK(TrackOf(ReadOf(up_file, 0), 4)->notes[1].key == voice.notes[1].key + 3);

    // A removal leaves the wait after the note, and the note's rest.
    const edit::NoteEdit removed{.command = first.command, .stretch = stretch, .remove = true};
    const File removed_file = edit::EditNotes(file, 0, std::span(&removed, 1)).file;
    XSFLIB_CHECK(StartsOf(ReadOf(removed_file, 0), 4) == (std::vector<uint32_t>{12}));
    XSFLIB_CHECK(StartsOf(ReadOf(removed_file, 0), 0) == (std::vector<uint32_t>{0}));
    XSFLIB_CHECK(StartsOf(ReadOf(removed_file, 1), 5) == (std::vector<uint32_t>{0}));
}

// A new velocity of a note of a square wave.
void TestSquareNoteVelocity()
{
    const NoteEditsFixture fixture = MakeNoteEditsFixture();
    const std::vector<uint8_t>& cart = fixture.cart;
    const File& file = fixture.file;
    const Sequence& song0 = fixture.song0;
    std::vector<uint8_t> expected;

    // A note of a square wave has a volume up to 15, and the view's 127 is 15.
    expected = cart;
    expected[kSong0 + kFirstSquare + 3] = 0xD7;
    SEQLIB_REQUIRE(TrackOf(song0, 0) && TrackOf(song0, 0)->notes.size() > 0);
    const song::Note& low = TrackOf(song0, 0)->notes[0];
    const edit::NoteEdit full{
        .command = low.command, .stretch = TrackOf(song0, 0)->stretches.at(low.stretch), .velocity = 56};
    const std::vector<uint8_t> full_bytes = format::ExportArchive(edit::EditNotes(file, 0, std::span(&full, 1)).file);
    XSFLIB_CHECK(full_bytes == expected);
}

// A new note goes into the ROM, and the edited ROM plays on the model.
void TestNewNote()
{
    const NoteEditsFixture fixture = MakeNoteEditsFixture();
    const File& file = fixture.file;
    const Sequence& song0 = fixture.song0;

    // A new note goes in during the square wave's rest, from tick 30 for 3 ticks. The sequence's other notes stay, and
    // so does the loop.
    const song::Track& square = *TrackOf(song0, 0);
    const song::Stretch& square_stretch = square.stretches.at(square.notes[0].stretch);
    const edit::NewNote added = edit::NewKonamiNote(file, 0, square, square_stretch, 30, 62, 3, 3);
    XSFLIB_CHECK(added.key == 26);
    const File added_file = edit::EditNotes(file, 0, {}, std::span(&added, 1)).file;
    const Sequence with_new = ReadOf(added_file, 0);
    XSFLIB_CHECK(StartsAt(with_new, 0, {0, 30}));
    SEQLIB_REQUIRE(TrackOf(with_new, 0) && TrackOf(with_new, 0)->notes.size() > 1);
    XSFLIB_CHECK(TrackOf(with_new, 0)->notes[1].length == 3);
    XSFLIB_CHECK(TrackOf(with_new, 0)->notes[1].key == 62);
    XSFLIB_CHECK(StartsAt(with_new, 4, {0, 12}));
    XSFLIB_CHECK(LoopsOver(with_new, 12, 36));
    XSFLIB_CHECK(StartsOf(ReadOf(added_file, 1), 5) == (std::vector<uint32_t>{0}));

    // The edited ROM plays on the reference's model of the driver, and the file contains the edit in both forms.
    const std::shared_ptr<const KonamiRom> rom = song::LoadKonamiRom(format::ExportArchive(added_file));
    const auto sequence = std::make_shared<const Sequence>(with_new);
    SequenceModel model(rom, sequence);
    const Played played = Play(model, 60);
    const std::vector<int16_t> direct = DirectStrict(*rom, 0, 2048 * 70);
    SEQLIB_REQUIRE(!played.samples.empty() && played.samples.size() <= direct.size());
    XSFLIB_CHECK(std::equal(played.samples.begin(), played.samples.end(), direct.begin()));
    XSFLIB_CHECK(format::ExportArchive(format::Read(format::Write(added_file))) == format::ExportArchive(added_file));
    XSFLIB_CHECK(format::ExportArchive(format::ReadText(format::WriteText(added_file))) ==
                 format::ExportArchive(added_file));
}

// A note that moves past the start of the next note leaves its rest out: the rest would cut that note short. The
// first revision's song 0 with track 4 playing, before the loop's start:
//
//     +0: 00          wait 0
//     +1: A8 40 00    note of sample 0 at volume 64, at tick 0
//     +4: 03          wait 3
//     +5: E0          rest, at tick 3
//     +6: 02          wait 2
//     +7: A8 30 01    note of sample 1 at volume 48, at tick 5
//    +10: 05          wait 5
//    +11: E0          rest, at tick 10
//    +12: 02          wait 2
//    +13: F3 00 00    the loop's start, at tick 12
//    +16: 18          wait 24
//    +17: FF 01       loop the song, at tick 36
//    +19: 00          not reached
void TestMoveAcrossNote()
{
    std::vector<uint8_t> cart = FirstCart();
    const std::vector<uint8_t> voice = {0x00, 0xA8, 0x40, 0x00, 0x03, 0xE0, 0x02, 0xA8, 0x30, 0x01,
                                        0x05, 0xE0, 0x02, 0xF3, 0x00, 0x00, 0x18, 0xFF, 0x01, 0x00};
    std::ranges::copy(voice, cart.begin() + kSong0 + kFirstVoice);
    const File file = format::ImportKonamiRom(cart);
    const Sequence song0 = ReadOf(file, 0);
    SEQLIB_REQUIRE(StartsOf(song0, 4) == (std::vector<uint32_t>{0, 5}));
    const song::Track& voice_track = *TrackOf(song0, 4);
    const song::Note& first = voice_track.notes[0];
    const song::Stretch& stretch = voice_track.stretches.at(first.stretch);

    // 4 ticks later, the first note sounds until the second starts, and the second keeps its length.
    const edit::NoteEdit across{.command = first.command, .stretch = stretch, .shift = 4};
    const Sequence moved = ReadOf(edit::EditNotes(file, 0, std::span(&across, 1)).file, 0);
    SEQLIB_REQUIRE(StartsOf(moved, 4) == (std::vector<uint32_t>{4, 5}));
    XSFLIB_CHECK(TrackOf(moved, 4)->notes[0].length == 1);
    XSFLIB_CHECK(TrackOf(moved, 4)->notes[1].length == 5);

    // A tick later, the rest still comes before the second note, and moves with the first.
    const edit::NoteEdit closer{.command = first.command, .stretch = stretch, .shift = 1};
    const Sequence closer_song = ReadOf(edit::EditNotes(file, 0, std::span(&closer, 1)).file, 0);
    SEQLIB_REQUIRE(StartsOf(closer_song, 4) == (std::vector<uint32_t>{1, 5}));
    XSFLIB_CHECK(TrackOf(closer_song, 4)->notes[0].length == 3);
    XSFLIB_CHECK(TrackOf(closer_song, 4)->notes[1].length == 5);

    // Both notes 4 ticks later keep their lengths: the rest stays between them.
    const song::Note& second = voice_track.notes[1];
    const std::vector<edit::NoteEdit> both = {
        across, {.command = second.command, .stretch = voice_track.stretches.at(second.stretch), .shift = 4}};
    const Sequence later = ReadOf(edit::EditNotes(file, 0, both).file, 0);
    SEQLIB_REQUIRE(StartsOf(later, 4) == (std::vector<uint32_t>{4, 9}));
    XSFLIB_CHECK(TrackOf(later, 4)->notes[0].length == 3);
    XSFLIB_CHECK(TrackOf(later, 4)->notes[1].length == 3);
}

// The later revision's note that a call plays again: a new volume reaches its byte, for both places. A new note before
// the commands that the call plays moves the call's offset with them.
void TestCall()
{
    const std::vector<uint8_t> cart = LaterCart();
    const File file = format::ImportKonamiRom(cart);
    const Sequence song0 = ReadOf(file, 0);
    const song::Track& voice = *TrackOf(song0, 4);
    const song::Note& called = voice.notes[1];

    std::vector<uint8_t> expected = cart;
    expected[kSong0 + 10] = 0xAF;
    const edit::NoteEdit louder{
        .command = called.command, .stretch = voice.stretches.at(called.stretch), .velocity = 127};
    const File louder_file = edit::EditNotes(file, 0, std::span(&louder, 1)).file;
    XSFLIB_CHECK(format::ExportArchive(louder_file) == expected);
    XSFLIB_CHECK(TrackOf(ReadOf(louder_file, 0), 4)->notes[2].velocity == 127);

    const song::Note& opening = voice.notes[0];
    const edit::NewNote added = edit::NewKonamiNote(file, 0, voice, voice.stretches.at(opening.stretch), 11, 70, 68, 1);
    const File added_file = edit::EditNotes(file, 0, {}, std::span(&added, 1)).file;
    const Sequence with_new = ReadOf(added_file, 0);
    XSFLIB_CHECK(StartsAt(with_new, 4, {0, 11, 16, 26}));
    XSFLIB_CHECK(LoopsOver(with_new, 12, 32));
    XSFLIB_CHECK(StartsAt(with_new, 0, {0}));

    // A new note that ends where the next new note starts takes no rest: the next note cuts it. It would cut the next
    // note in that note's tick. Here a note of 3 ticks runs into a note of 1 tick. Pasted notes do the same.
    const std::array<edit::NewNote, 2> run{
        edit::NewKonamiNote(file, 0, voice, voice.stretches.at(opening.stretch), 5, 70, 68, 3),
        edit::NewKonamiNote(file, 0, voice, voice.stretches.at(opening.stretch), 8, 70, 68, 1)};
    const Sequence with_run = ReadOf(edit::EditNotes(file, 0, {}, run).file, 0);
    XSFLIB_CHECK(StartsAt(with_run, 4, {0, 5, 8, 16, 26}));
    SEQLIB_REQUIRE(TrackOf(with_run, 4) && TrackOf(with_run, 4)->notes.size() > 2);
    XSFLIB_CHECK(TrackOf(with_run, 4)->notes[1].length == 3);
    XSFLIB_CHECK(TrackOf(with_run, 4)->notes[2].length == 1);

    // The driver reads no delay after a call's last command. A shorter note there moves that command, and the delay
    // after the call makes up for it: the note lasts 3 ticks in both places, and the song still loops at tick 32.
    const edit::NoteEdit shorter{.command = called.command, .stretch = voice.stretches.at(called.stretch), .length = 3};
    const File shorter_file = edit::EditNotes(file, 0, std::span(&shorter, 1)).file;
    const Sequence with_shorter = song::ReadSequence(*song::LoadKonamiRom(format::ExportArchive(shorter_file)), 0);
    XSFLIB_CHECK(StartsAt(with_shorter, 4, {0, 16, 26}));
    XSFLIB_CHECK(LoopsOver(with_shorter, 12, 32));
    SEQLIB_REQUIRE(TrackOf(with_shorter, 4) && TrackOf(with_shorter, 4)->notes.size() > 2);
    XSFLIB_CHECK(TrackOf(with_shorter, 4)->notes[1].length == 3);
    XSFLIB_CHECK(TrackOf(with_shorter, 4)->notes[2].length == 3);

    // A pasted note of a sample keeps its sample and its semitones. A note of a PSG track has neither.
    const edit::KonamiSampleNote sample{.sample = 1, .semitones = 2};
    const edit::NewNote pasted =
        edit::NewKonamiNote(file, 0, voice, voice.stretches.at(opening.stretch), 11, 70, 68, 1, sample);
    XSFLIB_CHECK(pasted.sample == uint16_t{1});
    XSFLIB_CHECK(pasted.key == 2);
    const song::Track& square = *TrackOf(song0, 0);
    const edit::NewNote square_note =
        edit::NewKonamiNote(file, 0, square, square.stretches.at(square.notes[0].stretch), 11, 70, 68, 1, sample);
    XSFLIB_CHECK(!square_note.sample);

    // The driver's bytes stop a command's values at what they hold.
    XSFLIB_CHECK(edit::KonamiLongestWait(konami::Revision::kUltimateMasters) == 0xFFFF);
    XSFLIB_CHECK(edit::KonamiLongestWait(konami::Revision::kWct2004) == 0x1FFF);
    XSFLIB_CHECK(edit::KonamiLongestWait(konami::Revision::kEternalDuelist) == 0xFFF);
}

// One edit of a call's last command and of the note after the call. The later revision's song 0 with track 4 playing,
// before the square wave's track:
//
//     +0: 00          wait 0
//     +1: A8 00       note of sample 0 at volume 8, at tick 0
//     +3: 04          wait 4
//     +4: E0          rest, at tick 4
//     +5: 02          wait 2
//     +6: 9F 00 00 02 call of 2 commands from +0, at tick 6: a note at tick 6 and a rest at tick 10
//    +10: 04          wait 4, or 1
//    +11: A5 01       note of sample 1 at volume 5, at tick 14, or 11
//    +13: 06          wait 6
//    +14: E0          rest, at tick 20, or 17
//    +15: 00          wait 0
//    +16: FF          stop the song, at tick 20, or 17
//    +17: 00 00 00 00 not reached
//
// The edit makes the first note 3 ticks long, and moves the note at +11 a tick earlier. The rest at +4 then comes a
// tick sooner. The call plays it too, and returns at tick 9. The delay after the call grows by a tick, and the moved
// note sounds at tick 13. Where that delay lasts a tick, the moved note comes right after the call. A new delay of a
// tick then goes in before the note. The note sounds at tick 10. Either way, the bytes keep their length.
void TestCallAndMovedNote()
{
    for (const uint8_t delay : {uint8_t{4}, uint8_t{1}})
    {
        std::vector<uint8_t> cart = LaterCart();
        const std::vector<uint8_t> voice = {0x00, 0xA8, 0x00, 0x04, 0xE0, 0x02, 0x9F, 0x00, 0x00, 0x02, delay,
                                            0xA5, 0x01, 0x06, 0xE0, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00};
        std::ranges::copy(voice, cart.begin() + kSong0);
        const File file = format::ImportKonamiRom(cart);
        const Sequence song0 = ReadOf(file, 0);
        const uint32_t moved_from = 10u + delay;
        SEQLIB_REQUIRE(StartsOf(song0, 4) == (std::vector<uint32_t>{0, 6, moved_from}));
        const song::Track& track = *TrackOf(song0, 4);
        const song::Note& first = track.notes[0];
        const song::Note& after_call = track.notes[2];
        const std::vector<edit::NoteEdit> edits = {
            {.command = first.command, .stretch = track.stretches.at(first.stretch), .length = 3u},
            {.command = after_call.command, .stretch = track.stretches.at(after_call.stretch), .shift = -1}};

        const File edited = edit::EditNotes(file, 0, edits).file;

        std::vector<uint8_t> expected = cart;
        expected[kSong0 + 3] = 3;
        expected[kSong0 + 5] = 3;
        expected[kSong0 + 15] = 1;
        XSFLIB_CHECK(format::ExportArchive(edited) == expected);
        const Sequence after = ReadOf(edited, 0);
        SEQLIB_REQUIRE(StartsOf(after, 4) == (std::vector<uint32_t>{0, 6, moved_from - 1}));
        XSFLIB_CHECK(TrackOf(after, 4)->notes[1].length == 3);
    }
}

// A volume starts a PSG track's note again. The arrangement shows such a note. The edits keep it. The later revision's
// song 0 with track 0 playing a square wave:
//
//     +0: 00          wait 0
//     +1: D3 18       note 24 at volume 3, at tick 0
//     +3: 06          wait 6
//     +4: C2          volume 2, at tick 6: note 24 starts again
//     +5: 04          wait 4
//     +6: E0          rest, at tick 10
//     +7: 02          wait 2
//     +8: D3 1A       note 26 at volume 3, at tick 12
//    +10: 04          wait 4
//    +11: E0          rest, at tick 16
//    +12: 01          wait 1
//    +13: C3          volume 3, at tick 17: note 26 starts again
//    +14: 05          wait 5
//    +15: E0          rest, at tick 22
//    +16: 00          wait 0
//    +17: FF          stop the song, at tick 22
//
// Every other track shares the bytes at +18: 00 FD.
void TestNoteStartedAgain()
{
    std::vector<uint8_t> cart = LaterCart();
    const std::vector<uint8_t> song = {0x00, 0xD3, 0x18, 0x06, 0xC2, 0x04, 0xE0, 0x02, 0xD3, 0x1A,
                                       0x04, 0xE0, 0x01, 0xC3, 0x05, 0xE0, 0x00, 0xFF, 0x00, 0xFD};
    std::ranges::fill_n(cart.begin() + kSong0, static_cast<std::ptrdiff_t>(kLaterSong0.size()), uint8_t{0});
    std::ranges::copy(song, cart.begin() + kSong0);
    PutEntry(cart, 0, kSong0, 18, 0, 0);

    const File file = format::ImportKonamiRom(cart);
    const Sequence song0 = ReadOf(file, 0);
    SEQLIB_REQUIRE(StartsOf(song0, 0) == (std::vector<uint32_t>{0, 6, 12, 17}));
    const song::Track& square = *TrackOf(song0, 0);
    SEQLIB_REQUIRE(square.notes[1].length == 4 && square.notes[3].length == 5);

    const song::Note& first = square.notes[0];
    const song::Note& second = square.notes[2];
    const song::Stretch& stretch = square.stretches.at(first.stretch);
    const auto lengths = [](const Sequence& sequence)
    {
        std::vector<uint32_t> found;
        const song::Track* track = TrackOf(sequence, 0);
        XSFLIB_CHECK(track != nullptr);
        for (const song::Note& note : track ? track->notes : std::vector<song::Note>{})
        {
            found.push_back(note.length);
        }

        return found;
    };

    // The rest at +6 ends the note that the volume starts again, not note 24. A move of note 24 leaves the rest in
    // place. Only the waits on each side of the note change.
    const edit::NoteEdit later{.command = first.command, .stretch = stretch, .shift = 1};
    const File moved = edit::EditNotes(file, 0, std::span(&later, 1)).file;
    std::vector<uint8_t> expected = cart;
    expected[kSong0] = 1;
    expected[kSong0 + 3] = 5;
    XSFLIB_CHECK(format::ExportArchive(moved) == expected);
    XSFLIB_CHECK(StartsOf(ReadOf(moved, 0), 0) == (std::vector<uint32_t>{1, 6, 12, 17}));
    XSFLIB_CHECK(lengths(ReadOf(moved, 0)) == (std::vector<uint32_t>{5, 4, 4, 5}));

    // A shorter note 24 takes a rest of its own before the volume. The note can't last past the volume.
    const edit::NoteEdit shorter{.command = first.command, .stretch = stretch, .length = 4u};
    const Sequence with_shorter = ReadOf(edit::EditNotes(file, 0, std::span(&shorter, 1)).file, 0);
    XSFLIB_CHECK(StartsOf(with_shorter, 0) == (std::vector<uint32_t>{0, 6, 12, 17}));
    XSFLIB_CHECK(lengths(with_shorter) == (std::vector<uint32_t>{4, 4, 4, 5}));
    const edit::NoteEdit longer{.command = first.command, .stretch = stretch, .length = 7u};
    XSFLIB_CHECK(!ErrorOf<std::invalid_argument>([&] { edit::EditNotes(file, 0, std::span(&longer, 1)); }).empty());

    // Note 26 a tick later would take its rest to tick 17, after the volume there. The rest would end the note that
    // the volume starts. The edit removes the rest. Note 26 then sounds until the volume starts it again.
    const edit::NoteEdit second_later{
        .command = second.command, .stretch = square.stretches.at(second.stretch), .shift = 1};
    const Sequence with_second = ReadOf(edit::EditNotes(file, 0, std::span(&second_later, 1)).file, 0);
    XSFLIB_CHECK(StartsOf(with_second, 0) == (std::vector<uint32_t>{0, 6, 13, 17}));
    XSFLIB_CHECK(lengths(with_second) == (std::vector<uint32_t>{6, 4, 4, 5}));

    // A new note at tick 16 is refused, with the key of note 26 too. The volume at tick 17 would start the new note
    // again. The note at tick 17 would then name the new note's command and show its velocity.
    const edit::NewNote added = edit::NewKonamiNote(file, 0, square, stretch, 16, 62, 100, 3);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::EditNotes(file, 0, {}, std::span(&added, 1)); }) ==
                 "Can't add a note there. It would change another note on the track.");

    // The song with such a note written into its bytes: DC 1A at tick 16, a note of key 26 at volume 12. In the read,
    // the note at tick 17 names the new note's command and shows its velocity. Before, it named the command of note 26.
    std::vector<uint8_t> written = song;
    written.insert(written.begin() + 12, {0x00, 0xDC, 0x1A});
    std::ranges::copy(written, cart.begin() + kSong0);
    PutEntry(cart, 0, kSong0, 21, 0, 0);
    const Sequence with_written = ReadOf(format::ImportKonamiRom(cart), 0);
    SEQLIB_REQUIRE(StartsOf(with_written, 0) == (std::vector<uint32_t>{0, 6, 12, 16, 17}));
    const song::Track& written_square = *TrackOf(with_written, 0);
    XSFLIB_CHECK(square.notes[3].command == square.notes[2].command);
    XSFLIB_CHECK(square.notes[3].velocity == 25);
    XSFLIB_CHECK(written_square.notes[4].command == written_square.notes[3].command);
    XSFLIB_CHECK(written_square.notes[4].command != written_square.notes[2].command);
    XSFLIB_CHECK(written_square.notes[4].velocity == 102);
}

// A frame in which the driver starts or stops a channel: the frame, the volume and the note. A rest stops the channel,
// and has kRest as its volume.
using ChannelStart = std::array<int, 3>;
constexpr int kRest = -1;

// The frames in which ntdlib's model of the driver starts or stops the channel of track 0 of song 0 of `file`, among
// its first `frames` frames.
std::vector<ChannelStart> ChannelStartsOf(const File& file, int frames)
{
    std::vector<ChannelStart> starts;
    const std::shared_ptr<const KonamiRom> rom = song::LoadKonamiRom(format::ExportArchive(file));
    XSFLIB_CHECK(rom != nullptr);
    if (!rom)
    {
        return starts;
    }

    const std::unique_ptr<konami::Sequencer> sequencer = konami::Sequencer::Create(rom->rom, rom->info, 0);
    for (int frame = 0; frame < frames; frame++)
    {
        const konami::TrackOutput output = sequencer->Step()[0];
        if (output.trig)
        {
            starts.push_back({frame, (output.flags & konami::kOutStop) != 0 ? kRest : output.vol, output.pitch / 32});
        }
    }

    return starts;
}

// The later revision's song 0 with `song` as its bytes. Every track but track 0 runs the bytes at `others`.
File FileWithSong(const std::vector<uint8_t>& song, uint32_t others)
{
    std::vector<uint8_t> cart = LaterCart();
    std::ranges::fill_n(cart.begin() + kSong0, static_cast<std::ptrdiff_t>(kLaterSong0.size()), uint8_t{0});
    std::ranges::copy(song, cart.begin() + kSong0);
    PutEntry(cart, 0, kSong0, others, 0, 0);

    return format::ImportKonamiRom(cart);
}

// The later revision's song 0 with `song` as its bytes. Track 0 runs them from the start, track 4 from `voice`, and
// every other track from `others`.
File FileWithVoice(const std::vector<uint8_t>& song, uint32_t others, uint32_t voice)
{
    std::vector<uint8_t> cart = LaterCart();
    std::ranges::fill_n(cart.begin() + kSong0, static_cast<std::ptrdiff_t>(kLaterSong0.size()), uint8_t{0});
    std::ranges::copy(song, cart.begin() + kSong0);
    PutEntry(cart, 0, kSong0, others, 4, voice, 0);

    return format::ImportKonamiRom(cart);
}

// The first revision's song 0 with `song` as its bytes. Every track but track 0 runs the bytes at `others`. In this
// revision, FD ends a track for good, FE ends it until the song loops, and FF 01 loops the song.
File FirstFileWithSong(const std::vector<uint8_t>& song, uint32_t others)
{
    std::vector<uint8_t> cart = FirstCart();
    std::ranges::fill_n(cart.begin() + kSong0, static_cast<std::ptrdiff_t>(kFirstSong0.size()), uint8_t{0});
    std::ranges::copy(song, cart.begin() + kSong0);
    PutEntry(cart, 0, kSong0, others, 0, 0);

    return format::ImportKonamiRom(cart);
}

// The lengths of the notes of track 0 of `sequence`.
std::vector<uint32_t> LengthsOf(const Sequence& sequence)
{
    std::vector<uint32_t> found;
    const song::Track* track = TrackOf(sequence, 0);
    XSFLIB_CHECK(track != nullptr);
    for (const song::Note& note : track ? track->notes : std::vector<song::Note>{})
    {
        found.push_back(note.length);
    }

    return found;
}

// The message of the refusal of `edit`, an edit of song 0 of `file`. Empty when the edit goes in.
std::string RefusalOf(const File& file, const edit::NoteEdit& edit)
{
    return ErrorOf<std::invalid_argument>([&] { edit::EditNotes(file, 0, std::span(&edit, 1)); });
}

// A PSG note command at volume 0 starts no note. A volume command after it starts the note, at the note command's key
// and the volume command's volume (ntdlib src/konami/sequencer_wct2004.cpp, Op::kPsgNote and Op::kVolume). The
// arrangement's note names the volume command, and the edits change the two commands. The later revision's song 0 with
// track 0 playing a square wave:
//
//     +0: 00          wait 0
//     +1: D3 18       note 24 at volume 3, at tick 0
//     +3: 04          wait 4
//     +4: D0 1A       note 26 at volume 0, at tick 4: note 24 ends, and nothing sounds
//     +6: 02          wait 2
//     +7: C2          volume 2, at tick 6: note 26 starts
//     +8: 04          wait 4
//     +9: E0          rest, at tick 10
//    +10: 02          wait 2
//    +11: D3 1C       note 28 at volume 3, at tick 12
//    +13: 04          wait 4
//    +14: E0          rest, at tick 16
//    +15: 00          wait 0
//    +16: FF          stop the song, at tick 16
//
// Every other track shares the bytes at +17: 00 FD.
void TestNoteStartedByVolume()
{
    const std::vector<uint8_t> song = {0x00, 0xD3, 0x18, 0x04, 0xD0, 0x1A, 0x02, 0xC2, 0x04, 0xE0,
                                       0x02, 0xD3, 0x1C, 0x04, 0xE0, 0x00, 0xFF, 0x00, 0xFD};
    const File file = FileWithSong(song, 17);
    const std::vector<uint8_t> cart = format::ExportArchive(file);
    using Starts = std::vector<ChannelStart>;
    const Starts last = {{12, 3, 28}, {16, kRest, 28}};
    const auto with_last = [&last](Starts starts)
    {
        starts.insert(starts.end(), last.begin(), last.end());
        return starts;
    };
    XSFLIB_CHECK(ChannelStartsOf(file, 17) == with_last({{0, 3, 24}, {4, 0, 26}, {6, 2, 26}, {10, kRest, 26}}));

    // The note at tick 6 shows volume 2 of 15 and names the volume command.
    const Sequence song0 = ReadOf(file, 0);
    SEQLIB_REQUIRE(StartsOf(song0, 0) == (std::vector<uint32_t>{0, 6, 12}));
    const song::Track& square = *TrackOf(song0, 0);
    const song::Note& note = square.notes[1];
    const song::Stretch& stretch = square.stretches.at(note.stretch);
    XSFLIB_CHECK(note.command == 7);
    XSFLIB_CHECK(note.velocity == 17);
    XSFLIB_CHECK(note.key == edit::kKonamiLowestPsgKey + 26);
    XSFLIB_CHECK(LengthsOf(song0) == (std::vector<uint32_t>{4, 4, 4}));
    XSFLIB_CHECK(edit::CommandKey(file, 0, note.command) == 26);

    // A new velocity changes the volume command.
    const edit::NoteEdit louder{.command = note.command, .stretch = stretch, .velocity = 42};
    const File with_velocity = edit::EditNotes(file, 0, std::span(&louder, 1)).file;
    std::vector<uint8_t> expected = cart;
    expected[kSong0 + 7] = 0xC5;
    XSFLIB_CHECK(format::ExportArchive(with_velocity) == expected);
    XSFLIB_CHECK(ChannelStartsOf(with_velocity, 17) ==
                 with_last({{0, 3, 24}, {4, 0, 26}, {6, 5, 26}, {10, kRest, 26}}));
    XSFLIB_CHECK(TrackOf(ReadOf(with_velocity, 0), 0)->notes.at(1).velocity == 42);

    // A new key changes the note command. Note 28 sets the key of what follows.
    const edit::NoteEdit higher{.command = note.command, .stretch = stretch, .transpose = 3};
    const File with_key = edit::EditNotes(file, 0, std::span(&higher, 1)).file;
    expected = cart;
    expected[kSong0 + 5] = 0x1D;
    XSFLIB_CHECK(format::ExportArchive(with_key) == expected);
    XSFLIB_CHECK(ChannelStartsOf(with_key, 17) == with_last({{0, 3, 24}, {4, 0, 29}, {6, 2, 29}, {10, kRest, 29}}));
    XSFLIB_CHECK(TrackOf(ReadOf(with_key, 0), 0)->notes.at(1).key == edit::kKonamiLowestPsgKey + 29);

    // A move takes both commands and the rest along. Note 24 then ends a tick later.
    const edit::NoteEdit later{.command = note.command, .stretch = stretch, .shift = 1};
    const File moved = edit::EditNotes(file, 0, std::span(&later, 1)).file;
    expected = cart;
    expected[kSong0 + 3] = 5;
    expected[kSong0 + 10] = 1;
    XSFLIB_CHECK(format::ExportArchive(moved) == expected);
    XSFLIB_CHECK(ChannelStartsOf(moved, 17) == with_last({{0, 3, 24}, {5, 0, 26}, {7, 2, 26}, {11, kRest, 26}}));
    XSFLIB_CHECK(StartsOf(ReadOf(moved, 0), 0) == (std::vector<uint32_t>{0, 7, 12}));
    XSFLIB_CHECK(LengthsOf(ReadOf(moved, 0)) == (std::vector<uint32_t>{5, 4, 4}));

    // The note command stops at the start of its section. It can't reach the tick of note 24: that note would end at
    // once. The volume command can't reach the tick of note 28: note 28 would end the moved note at once.
    const std::optional<edit::ShiftRange> range = edit::ShiftRangeOf(file, 0, note.command, stretch);
    SEQLIB_REQUIRE(range);
    XSFLIB_CHECK(range->earlier == 4);
    const std::string past =
        "Can't move this note past another note or a rest on the track. Move or remove that one "
        "first.";
    XSFLIB_CHECK(!RefusalOf(file, {.command = note.command, .stretch = stretch, .shift = -5}).empty());
    XSFLIB_CHECK(RefusalOf(file, {.command = note.command, .stretch = stretch, .shift = -4}) == past);
    XSFLIB_CHECK(RefusalOf(file, {.command = note.command, .stretch = stretch, .shift = 6}) == past);
    const edit::NoteEdit earlier{.command = note.command, .stretch = stretch, .shift = -3};
    const File moved_earlier = edit::EditNotes(file, 0, std::span(&earlier, 1)).file;
    XSFLIB_CHECK(ChannelStartsOf(moved_earlier, 17) == with_last({{0, 3, 24}, {1, 0, 26}, {3, 2, 26}, {7, kRest, 26}}));

    // A removal takes the volume command out. The note command still ends note 24 at tick 4.
    const edit::NoteEdit gone{.command = note.command, .stretch = stretch, .remove = true};
    const File removed = edit::EditNotes(file, 0, std::span(&gone, 1)).file;
    XSFLIB_CHECK(ChannelStartsOf(removed, 17) == with_last({{0, 3, 24}, {4, 0, 26}, {10, kRest, 26}}));
    XSFLIB_CHECK(StartsOf(ReadOf(removed, 0), 0) == (std::vector<uint32_t>{0, 12}));
    XSFLIB_CHECK(LengthsOf(ReadOf(removed, 0)) == (std::vector<uint32_t>{4, 4}));

    // A new length moves the rest after the volume command. The note can't last past note 28.
    const edit::NoteLimits limits =
        edit::LimitsOf(file, 0, std::vector<edit::NotePlace>{{.command = note.command, .stretch = stretch}}).front();
    XSFLIB_CHECK(limits.length == 4u);
    XSFLIB_CHECK(limits.fixed_length.empty());
    const edit::NoteEdit longer{.command = note.command, .stretch = stretch, .length = 5u};
    const File lengthened = edit::EditNotes(file, 0, std::span(&longer, 1)).file;
    expected = cart;
    expected[kSong0 + 8] = 5;
    expected[kSong0 + 10] = 1;
    XSFLIB_CHECK(format::ExportArchive(lengthened) == expected);
    XSFLIB_CHECK(ChannelStartsOf(lengthened, 17) == with_last({{0, 3, 24}, {4, 0, 26}, {6, 2, 26}, {11, kRest, 26}}));
    XSFLIB_CHECK(LengthsOf(ReadOf(lengthened, 0)) == (std::vector<uint32_t>{4, 5, 4}));
    XSFLIB_CHECK(!RefusalOf(file, {.command = note.command, .stretch = stretch, .length = 7u}).empty());

    // A new note after it copies the note command's key byte, and so does the audition of a key.
    const edit::NewNote added =
        edit::NewKonamiNote(file, 0, square, stretch, 8, edit::kKonamiLowestPsgKey + 28, 100, 1);
    XSFLIB_CHECK(added.key == 28);
    const std::shared_ptr<const KonamiRom> rom = song::LoadKonamiRom(cart);
    SEQLIB_REQUIRE(rom);
    const std::optional<song::DriverAudition> audition = song::KonamiNoteAudition(
        *rom, *file.sequences.at(0), song::ReadSequence(*rom, 0), 0, static_cast<int>(note.program), note.key + 1, 100);
    SEQLIB_REQUIRE(audition);
    XSFLIB_CHECK(audition->note.key == 27);
}

// The two commands can run in one tick. The note then starts at the note command's tick, and a move keeps both
// commands in one tick. No note command follows the two here. A volume command of another part of the track could
// start a note at the note command's key, and a new key is refused:
//
//     +0: 00          wait 0
//     +1: D3 18       note 24 at volume 3, at tick 0
//     +3: 04          wait 4
//     +4: D0 1A       note 26 at volume 0, at tick 4
//     +6: 00          wait 0
//     +7: C2          volume 2, at tick 4: note 26 starts
//     +8: 04          wait 4
//     +9: E0          rest, at tick 8
//    +10: 08          wait 8
//    +11: FF          stop the song, at tick 16
//
// Every other track shares the bytes at +12: 00 FD.
void TestNoteStartedByVolumeInOneTick()
{
    const std::vector<uint8_t> song = {0x00, 0xD3, 0x18, 0x04, 0xD0, 0x1A, 0x00,
                                       0xC2, 0x04, 0xE0, 0x08, 0xFF, 0x00, 0xFD};
    const File file = FileWithSong(song, 12);
    using Starts = std::vector<ChannelStart>;
    XSFLIB_CHECK(ChannelStartsOf(file, 16) == (Starts{{0, 3, 24}, {4, 2, 26}, {8, kRest, 26}}));

    const Sequence song0 = ReadOf(file, 0);
    SEQLIB_REQUIRE(StartsOf(song0, 0) == (std::vector<uint32_t>{0, 4}));
    const song::Track& square = *TrackOf(song0, 0);
    const song::Note& note = square.notes[1];
    const song::Stretch& stretch = square.stretches.at(note.stretch);
    XSFLIB_CHECK(note.command == 7);
    XSFLIB_CHECK(note.velocity == 17);
    XSFLIB_CHECK(note.key == edit::kKonamiLowestPsgKey + 26);

    const edit::NoteEdit later{.command = note.command, .stretch = stretch, .shift = 2};
    const File moved = edit::EditNotes(file, 0, std::span(&later, 1)).file;
    XSFLIB_CHECK(ChannelStartsOf(moved, 16) == (Starts{{0, 3, 24}, {6, 2, 26}, {10, kRest, 26}}));
    XSFLIB_CHECK(StartsOf(ReadOf(moved, 0), 0) == (std::vector<uint32_t>{0, 6}));
    XSFLIB_CHECK(LengthsOf(ReadOf(moved, 0)) == (std::vector<uint32_t>{6, 4}));

    XSFLIB_CHECK(RefusalOf(file, {.command = note.command, .stretch = stretch, .transpose = 2}) ==
                 "Another section of the track can take its key from this note's command. Can't transpose it.");
    XSFLIB_CHECK(edit::LimitsOf(file, 0, std::vector<edit::NotePlace>{{.command = note.command, .stretch = stretch}})
                     .front()
                     .fixed_key ==
                 "Another section of the track can take its key from this note's command. Can't transpose it.");
}

// A rest between the two commands stays when the note moves. The volume command can't move to before the rest: the
// rest would end the moved note.
//
//     +0: 00          wait 0
//     +1: D3 18       note 24 at volume 3, at tick 0
//     +3: 06          wait 6
//     +4: D0 1A       note 26 at volume 0, at tick 6
//     +6: 01          wait 1
//     +7: E0          rest, at tick 7
//     +8: 03          wait 3
//     +9: C2          volume 2, at tick 10: note 26 starts
//    +10: 04          wait 4
//    +11: E0          rest, at tick 14
//    +12: 02          wait 2
//    +13: FF          stop the song, at tick 16
//
// Every other track shares the bytes at +14: 00 FD.
void TestNoteStartedByVolumeAfterRest()
{
    const std::vector<uint8_t> song = {0x00, 0xD3, 0x18, 0x06, 0xD0, 0x1A, 0x01, 0xE0,
                                       0x03, 0xC2, 0x04, 0xE0, 0x02, 0xFF, 0x00, 0xFD};
    const File file = FileWithSong(song, 14);
    using Starts = std::vector<ChannelStart>;
    XSFLIB_CHECK(ChannelStartsOf(file, 16) ==
                 (Starts{{0, 3, 24}, {6, 0, 26}, {7, kRest, 26}, {10, 2, 26}, {14, kRest, 26}}));

    const Sequence song0 = ReadOf(file, 0);
    SEQLIB_REQUIRE(StartsOf(song0, 0) == (std::vector<uint32_t>{0, 10}));
    const song::Track& square = *TrackOf(song0, 0);
    const song::Note& note = square.notes[1];
    const song::Stretch& stretch = square.stretches.at(note.stretch);
    XSFLIB_CHECK(note.command == 9);

    // 3 ticks earlier, the volume command runs in the tick of the rest, after it.
    const edit::NoteEdit earlier{.command = note.command, .stretch = stretch, .shift = -3};
    const File moved = edit::EditNotes(file, 0, std::span(&earlier, 1)).file;
    XSFLIB_CHECK(ChannelStartsOf(moved, 16) == (Starts{{0, 3, 24}, {3, 0, 26}, {7, 2, 26}, {11, kRest, 26}}));
    XSFLIB_CHECK(StartsOf(ReadOf(moved, 0), 0) == (std::vector<uint32_t>{0, 7}));
    XSFLIB_CHECK(LengthsOf(ReadOf(moved, 0)) == (std::vector<uint32_t>{3, 4}));

    XSFLIB_CHECK(RefusalOf(file, {.command = note.command, .stretch = stretch, .shift = -4}) ==
                 "Can't move this note past another note or a rest on the track. Move or remove that one first.");
}

// Two volume commands after one note command at volume 0 start two notes at its key. A new velocity, a removal and a
// new length change one note. A move or a new key would change the other note too, and is refused.
//
//     +0: 00          wait 0
//     +1: D0 1A       note 26 at volume 0, at tick 0
//     +3: 02          wait 2
//     +4: C2          volume 2, at tick 2: note 26 starts
//     +5: 04          wait 4
//     +6: C3          volume 3, at tick 6: note 26 starts again
//     +7: 04          wait 4
//     +8: E0          rest, at tick 10
//     +9: 02          wait 2
//    +10: D3 1C       note 28 at volume 3, at tick 12
//    +12: 04          wait 4
//    +13: E0          rest, at tick 16
//    +14: 00          wait 0
//    +15: FF          stop the song, at tick 16
//
// Every other track shares the bytes at +16: 00 FD.
void TestNotesOfOneSilentCommand()
{
    const std::vector<uint8_t> song = {0x00, 0xD0, 0x1A, 0x02, 0xC2, 0x04, 0xC3, 0x04, 0xE0,
                                       0x02, 0xD3, 0x1C, 0x04, 0xE0, 0x00, 0xFF, 0x00, 0xFD};
    const File file = FileWithSong(song, 16);
    const std::vector<uint8_t> cart = format::ExportArchive(file);
    using Starts = std::vector<ChannelStart>;
    const Starts last = {{12, 3, 28}, {16, kRest, 28}};
    const auto with_last = [&last](Starts starts)
    {
        starts.insert(starts.end(), last.begin(), last.end());
        return starts;
    };
    XSFLIB_CHECK(ChannelStartsOf(file, 17) == with_last({{0, 0, 26}, {2, 2, 26}, {6, 3, 26}, {10, kRest, 26}}));

    const Sequence song0 = ReadOf(file, 0);
    SEQLIB_REQUIRE(StartsOf(song0, 0) == (std::vector<uint32_t>{2, 6, 12}));
    const song::Track& square = *TrackOf(song0, 0);
    const song::Note& first = square.notes[0];
    const song::Note& second = square.notes[1];
    const song::Stretch& stretch = square.stretches.at(first.stretch);
    XSFLIB_CHECK(first.command == 4 && second.command == 6);
    XSFLIB_CHECK(first.velocity == 17 && second.velocity == 25);
    XSFLIB_CHECK(first.key == edit::kKonamiLowestPsgKey + 26 && second.key == first.key);

    // A new velocity of each note changes its volume command alone.
    for (const song::Note* note : {&first, &second})
    {
        const edit::NoteEdit louder{.command = note->command, .stretch = stretch, .velocity = 42};
        std::vector<uint8_t> expected = cart;
        expected[kSong0 + note->command] = 0xC5;
        XSFLIB_CHECK(format::ExportArchive(edit::EditNotes(file, 0, std::span(&louder, 1)).file) == expected);
    }

    // A move of either note, in either direction, and a new key are refused.
    const std::string move = "This note takes its key from the same command as another note. Can't move it.";
    for (const song::Note* note : {&first, &second})
    {
        XSFLIB_CHECK(RefusalOf(file, {.command = note->command, .stretch = stretch, .shift = 1}) == move);
        XSFLIB_CHECK(RefusalOf(file, {.command = note->command, .stretch = stretch, .shift = -1}) == move);
        XSFLIB_CHECK(RefusalOf(file, {.command = note->command, .stretch = stretch, .transpose = 1}) ==
                     "This note takes its key from the same command as another note. Can't transpose it.");
    }

    // A removal of the first note leaves the second at its key and its tick.
    const edit::NoteEdit first_gone{.command = first.command, .stretch = stretch, .remove = true};
    const File without_first = edit::EditNotes(file, 0, std::span(&first_gone, 1)).file;
    XSFLIB_CHECK(ChannelStartsOf(without_first, 17) == with_last({{0, 0, 26}, {6, 3, 26}, {10, kRest, 26}}));
    XSFLIB_CHECK(StartsOf(ReadOf(without_first, 0), 0) == (std::vector<uint32_t>{6, 12}));

    // A removal of the second note lets the first sound up to the rest, like any note before a removed note.
    const edit::NoteEdit second_gone{.command = second.command, .stretch = stretch, .remove = true};
    const File without_second = edit::EditNotes(file, 0, std::span(&second_gone, 1)).file;
    XSFLIB_CHECK(ChannelStartsOf(without_second, 17) == with_last({{0, 0, 26}, {2, 2, 26}, {10, kRest, 26}}));
    XSFLIB_CHECK(LengthsOf(ReadOf(without_second, 0)) == (std::vector<uint32_t>{8, 4}));

    // A shorter first note takes a rest before the second volume command. The second note still starts at note 26.
    const edit::NoteEdit shorter{.command = first.command, .stretch = stretch, .length = 2u};
    const File shortened = edit::EditNotes(file, 0, std::span(&shorter, 1)).file;
    XSFLIB_CHECK(ChannelStartsOf(shortened, 17) ==
                 with_last({{0, 0, 26}, {2, 2, 26}, {4, kRest, 26}, {6, 3, 26}, {10, kRest, 26}}));
    XSFLIB_CHECK(LengthsOf(ReadOf(shortened, 0)) == (std::vector<uint32_t>{2, 4, 4}));
    XSFLIB_CHECK(!RefusalOf(file, {.command = first.command, .stretch = stretch, .length = 5u}).empty());
}

// The loop's start comes between the note command at volume 0 and the volume command. Another pass can reach the
// volume command after another note command, and the note takes no edit:
//
//     +0: 00          wait 0
//     +1: D3 18       note 24 at volume 3, at tick 0
//     +3: 04          wait 4
//     +4: D0 1A       note 26 at volume 0, at tick 4
//     +6: 02          wait 2
//     +7: F3          the loop's start, at tick 6
//     +8: 00          wait 0
//     +9: C2          volume 2, at tick 6: note 26 starts
//    +10: 04          wait 4
//    +11: E0          rest, at tick 10
//    +12: 06          wait 6
//    +13: FD          the end, at tick 16
//
// Every other track shares the bytes at +14: 00 FD.
void TestNoteStartedByVolumeAfterLoopStart()
{
    const std::vector<uint8_t> song = {0x00, 0xD3, 0x18, 0x04, 0xD0, 0x1A, 0x02, 0xF3,
                                       0x00, 0xC2, 0x04, 0xE0, 0x06, 0xFD, 0x00, 0xFD};
    const File file = FileWithSong(song, 14);
    using Starts = std::vector<ChannelStart>;
    XSFLIB_CHECK(ChannelStartsOf(file, 16) == (Starts{{0, 3, 24}, {4, 0, 26}, {6, 2, 26}, {10, kRest, 26}}));

    const Sequence song0 = ReadOf(file, 0);
    SEQLIB_REQUIRE(StartsOf(song0, 0) == (std::vector<uint32_t>{0, 6}));
    const song::Track& square = *TrackOf(song0, 0);
    const song::Note& note = square.notes[1];
    const song::Stretch& stretch = square.stretches.at(note.stretch);
    XSFLIB_CHECK(note.command == 9);
    XSFLIB_CHECK(note.velocity == 17);
    XSFLIB_CHECK(note.key == edit::kKonamiLowestPsgKey + 26);

    const std::string far =
        "This note's key comes from a command in another section of the track. Edit its commands "
        "in the List Editor.";
    XSFLIB_CHECK(RefusalOf(file, {.command = note.command, .stretch = stretch, .velocity = 42}) == far);
    XSFLIB_CHECK(RefusalOf(file, {.command = note.command, .stretch = stretch, .transpose = 1}) == far);
    XSFLIB_CHECK(RefusalOf(file, {.command = note.command, .stretch = stretch, .shift = 1}) == far);
    XSFLIB_CHECK(RefusalOf(file, {.command = note.command, .stretch = stretch, .remove = true}) == far);
    XSFLIB_CHECK(RefusalOf(file, {.command = note.command, .stretch = stretch, .length = 3u}) == far);
    const edit::NoteLimits limits =
        edit::LimitsOf(file, 0, std::vector<edit::NotePlace>{{.command = note.command, .stretch = stretch}}).front();
    XSFLIB_CHECK(!limits.length);
    XSFLIB_CHECK(limits.fixed_length == far);

    // The audition finds the note command in the stretch that ran the volume command.
    const std::shared_ptr<const KonamiRom> rom = song::LoadKonamiRom(format::ExportArchive(file));
    SEQLIB_REQUIRE(rom);
    const std::optional<song::DriverAudition> audition = song::KonamiNoteAudition(
        *rom, *file.sequences.at(0), song::ReadSequence(*rom, 0), 0, static_cast<int>(note.program), note.key, 100);
    SEQLIB_REQUIRE(audition);
    XSFLIB_CHECK(audition->note.key == 26);
}

// A volume command starts a PSG note at the key of the last note command that the track ran. An edit that puts another
// note command there would change the key of a note that it doesn't name, and is refused: a new note, a moved note, a
// note command moved past its volume command, and a removed note command. A new command of the same key is refused too:
// the note of the volume command would name it. An edit right outside the two commands goes in. The songs are those of
// TestNoteStartedByVolume, where the note command at tick 4 is silent and the volume command runs at tick 6, of
// TestNoteStartedAgain, where the volume command at tick 6 starts note 24 again, and of
// TestNoteStartedByVolumeAfterLoopStart, where the loop's start lies between the two commands.
void TestVolumeStartsKeepTheirKeys()
{
    using Starts = std::vector<ChannelStart>;
    const std::string add = "Can't add a note there. It would change another note on the track.";
    const std::string move = "Can't move this note there. It would change another note on the track.";
    const std::string remove = "Can't remove this note. A volume after it starts it again. Remove that volume first.";
    const auto refusal_of = [](const File& file, std::span<const edit::NewNote> added)
    {
        return ErrorOf<std::invalid_argument>([&] { edit::EditNotes(file, 0, {}, added); });
    };
    const auto starts_with = [](const File& file, std::span<const edit::NewNote> added)
    {
        return ChannelStartsOf(edit::EditNotes(file, 0, {}, added).file, 16);
    };
    const auto starts_after = [](const File& file, const edit::NoteEdit& edit)
    {
        return ChannelStartsOf(edit::EditNotes(file, 0, std::span(&edit, 1)).file, 16);
    };
    const int key = edit::kKonamiLowestPsgKey + 30;

    // After a silent note command.
    const File silent = FileWithSong({0x00, 0xD3, 0x18, 0x04, 0xD0, 0x1A, 0x02, 0xC2, 0x04, 0xE0, 0x02, 0xD3, 0x1C,
                                      0x04, 0xE0, 0x00, 0xFF, 0x00, 0xFD},
                                     17);
    const Sequence silent_song = ReadOf(silent, 0);
    const song::Track& square = *TrackOf(silent_song, 0);
    const song::Stretch& stretch = square.stretches.at(0);
    const auto new_note = [&](uint32_t tick, int at_key, uint32_t length)
    {
        return std::vector<edit::NewNote>{edit::NewKonamiNote(silent, 0, square, stretch, tick, at_key, 100, length)};
    };
    XSFLIB_CHECK(refusal_of(silent, new_note(4, key, 1)) == add);
    XSFLIB_CHECK(refusal_of(silent, new_note(5, key, 1)) == add);

    // A new note of key 26 is refused too. The note of the volume command would name the new note's command, and show
    // its velocity.
    XSFLIB_CHECK(refusal_of(silent, new_note(5, edit::kKonamiLowestPsgKey + 26, 1)) == add);
    XSFLIB_CHECK(starts_with(silent, new_note(3, key, 1)) ==
                 (Starts{{0, 3, 24}, {3, 12, 30}, {4, 0, 26}, {6, 2, 26}, {10, kRest, 26}, {12, 3, 28}}));

    // A new note at the tick of the volume command goes in after that command, and takes the place of its note.
    XSFLIB_CHECK(starts_with(silent, new_note(6, key, 1)) ==
                 (Starts{{0, 3, 24}, {4, 0, 26}, {6, 12, 30}, {7, kRest, 30}, {10, kRest, 30}, {12, 3, 28}}));
    XSFLIB_CHECK(starts_with(silent, new_note(8, key, 2)) ==
                 (Starts{{0, 3, 24}, {4, 0, 26}, {6, 2, 26}, {8, 12, 30}, {10, kRest, 30}, {12, 3, 28}}));

    // Note 24 and note 28 can't move to between the two commands. Right outside them, the moves go in.
    const song::Note& first = square.notes.front();
    const song::Note& last = square.notes.back();
    const auto moved = [&](const song::Note& note, int32_t shift)
    {
        return edit::NoteEdit{.command = note.command, .stretch = square.stretches.at(note.stretch), .shift = shift};
    };
    XSFLIB_CHECK(RefusalOf(silent, moved(first, 4)) == move);
    XSFLIB_CHECK(RefusalOf(silent, moved(first, 5)) == move);
    XSFLIB_CHECK(RefusalOf(silent, moved(last, -7)) == move);
    XSFLIB_CHECK(RefusalOf(silent, moved(last, -8)) == move);
    XSFLIB_CHECK(starts_after(silent, moved(first, 3)) ==
                 (Starts{{0, kRest, 0}, {3, 3, 24}, {4, 0, 26}, {6, 2, 26}, {10, kRest, 26}, {12, 3, 28}}));
    XSFLIB_CHECK(starts_after(silent, moved(last, -5)) ==
                 (Starts{{0, 3, 24}, {4, 0, 26}, {6, 2, 26}, {7, 3, 28}, {10, kRest, 28}, {11, kRest, 28}}));

    // Without note 24, the volume command still follows the silent note command.
    XSFLIB_CHECK(starts_after(silent, {.command = first.command, .stretch = stretch, .remove = true}) ==
                 (Starts{{0, kRest, 0}, {4, 0, 26}, {6, 2, 26}, {10, kRest, 26}, {12, 3, 28}}));

    // After an audible note command.
    const File again = FileWithSong({0x00, 0xD3, 0x18, 0x06, 0xC2, 0x04, 0xE0, 0x02, 0xD3, 0x1A,
                                     0x04, 0xE0, 0x01, 0xC3, 0x05, 0xE0, 0x00, 0xFF, 0x00, 0xFD},
                                    18);
    const Sequence again_song = ReadOf(again, 0);
    const song::Track& again_square = *TrackOf(again_song, 0);
    const song::Stretch& again_stretch = again_square.stretches.at(0);
    const auto again_note = [&](uint32_t tick, int at_key, uint32_t length)
    {
        return edit::NewKonamiNote(again, 0, again_square, again_stretch, tick, at_key, 100, length);
    };
    XSFLIB_CHECK(ChannelStartsOf(again, 16) == (Starts{{0, 3, 24}, {6, 2, 24}, {10, kRest, 24}, {12, 3, 26}}));
    XSFLIB_CHECK(refusal_of(again, std::vector{again_note(3, key, 1)}) == add);

    // A new note with the key of note 24 is refused too: the volume command would start the new note again.
    XSFLIB_CHECK(refusal_of(again, std::vector{again_note(3, edit::kKonamiLowestPsgKey + 24, 1)}) == add);
    XSFLIB_CHECK(starts_with(again, std::vector{again_note(8, key, 2)}) ==
                 (Starts{{0, 3, 24}, {6, 2, 24}, {8, 12, 30}, {10, kRest, 30}, {12, 3, 26}}));

    // So are two new notes where the second one has the key of note 24.
    XSFLIB_CHECK(
        refusal_of(again, std::vector{again_note(3, key, 1), again_note(4, edit::kKonamiLowestPsgKey + 24, 1)}) == add);

    // Note 24 can't move past the volume command that starts it again, and note 26 can't move to before it. Neither
    // note can go: its volume command would start the key of another note.
    const song::Note& held = again_square.notes[0];
    const song::Note& other = again_square.notes[2];
    const auto again_moved = [&](const song::Note& note, int32_t shift)
    {
        return edit::NoteEdit{
            .command = note.command, .stretch = again_square.stretches.at(note.stretch), .shift = shift};
    };
    XSFLIB_CHECK(RefusalOf(again, again_moved(held, 7)) == move);
    XSFLIB_CHECK(RefusalOf(again, again_moved(other, -7)) == move);
    XSFLIB_CHECK(starts_after(again, again_moved(held, 5)) ==
                 (Starts{{0, kRest, 0}, {5, 3, 24}, {6, 2, 24}, {10, kRest, 24}, {12, 3, 26}}));
    XSFLIB_CHECK(RefusalOf(again, {.command = held.command, .stretch = again_stretch, .remove = true}) == remove);
    XSFLIB_CHECK(RefusalOf(again, {.command = other.command, .stretch = again_stretch, .remove = true}) == remove);

    // The loop's start between the two commands: the new note would be the last note command before the loop.
    const File looped = FileWithSong(
        {0x00, 0xD3, 0x18, 0x04, 0xD0, 0x1A, 0x02, 0xF3, 0x00, 0xC2, 0x04, 0xE0, 0x06, 0xFD, 0x00, 0xFD}, 14);
    const Sequence looped_song = ReadOf(looped, 0);
    const song::Track& looped_square = *TrackOf(looped_song, 0);
    const song::Stretch& looped_stretch = looped_square.stretches.at(looped_square.notes[0].stretch);
    const auto looped_note = [&](uint32_t tick, int at_key)
    {
        return std::vector{edit::NewKonamiNote(looped, 0, looped_square, looped_stretch, tick, at_key, 100, 1)};
    };
    XSFLIB_CHECK(refusal_of(looped, looped_note(5, key)) == add);

    XSFLIB_CHECK(refusal_of(looped, looped_note(5, edit::kKonamiLowestPsgKey + 26)) == add);
    XSFLIB_CHECK(starts_with(looped, looped_note(2, key)) ==
                 (Starts{{0, 3, 24}, {2, 12, 30}, {3, kRest, 30}, {4, 0, 26}, {6, 2, 26}, {10, kRest, 26}}));
    XSFLIB_CHECK(
        RefusalOf(looped, {.command = looped_square.notes[0].command, .stretch = looped_stretch, .shift = 5}) == move);
}

// A call plays a volume command again. In the call, the volume command starts a note at the key of the last note
// command that the track ran before the call:
//
//     +0: 00          wait 0
//     +1: D3 18       note 24 at volume 3, at tick 0
//     +3: 04          wait 4
//     +4: C2          volume 2, at tick 4: starts note 24 again
//     +5: 04          wait 4
//     +6: E0          rest, at tick 8
//     +7: 02          wait 2
//     +8: D3 1C       note 28 at volume 3, at tick 10
//    +10: 02          wait 2
//    +11: E0          rest, at tick 12
//    +12: 00          wait 0
//    +13: 9F 03 00 02 call of 2 commands from +3, at tick 12: the volume command at tick 16 and the rest at tick 20.
//                     The volume command starts note 28 again there
//    +17: 04          wait 4
//    +18: D3 1D       note 29 at volume 3, at tick 24
//    +20: 04          wait 4
//    +21: E0          rest, at tick 28
//    +22: 00          wait 0
//    +23: FF          stop the song
//    +24: 00 FD       the other tracks
const std::vector<uint8_t> kCalledVolume = {0x00, 0xD3, 0x18, 0x04, 0xC2, 0x04, 0xE0, 0x02, 0xD3,
                                            0x1C, 0x02, 0xE0, 0x00, 0x9F, 0x03, 0x00, 0x02, 0x04,
                                            0xD3, 0x1D, 0x04, 0xE0, 0x00, 0xFF, 0x00, 0xFD};

// A call inside a call. The call at +7 plays one command, the call at +12. The driver keeps one return address, and
// goes on at +17 after the volume command:
//
//     +0: 00          wait 0
//     +1: D3 18       note 24 at volume 3, at tick 0
//     +3: 04          wait 4
//     +4: C2          volume 2, at tick 4
//     +5: 04          wait 4
//     +6: E0          rest, at tick 8
//     +7: 00          wait 0
//     +8: 9F 0C 00 01 call of 1 command from +12
//    +12: 00          wait 0
//    +13: 9F 03 00 01 call of 1 command from +3: the volume command
//    +17: 04          wait 4
//    +18: E0          rest
//    +19: 00          wait 0
//    +20: FF          stop the song
//    +21: 00 FD       the other tracks
const std::vector<uint8_t> kNestedCall = {0x00, 0xD3, 0x18, 0x04, 0xC2, 0x04, 0xE0, 0x00, 0x9F, 0x0C, 0x00, 0x01,
                                          0x00, 0x9F, 0x03, 0x00, 0x01, 0x04, 0xE0, 0x00, 0xFF, 0x00, 0xFD};

// A volume command that two tracks run. Track 0 runs it after note 24. The other tracks start at +3 and run it with no
// note command before it:
//
//     +0: 00          wait 0
//     +1: D3 18       note 24 at volume 3, at tick 0
//     +3: 04          wait 4
//     +4: C2          volume 2, at tick 4
//     +5: 04          wait 4
//     +6: E0          rest, at tick 8
//     +7: 00          wait 0
//     +8: FD          end
const std::vector<uint8_t> kSharedVolume = {0x00, 0xD3, 0x18, 0x04, 0xC2, 0x04, 0xE0, 0x00, 0xFD};

// A song that loops. On each later pass, the volume command after the loop's start starts a note at the key of the last
// note command of the pass before:
//
//     +0: 00          wait 0
//     +1: D3 18       note 24 at volume 3, at tick 0
//     +3: 04          wait 4
//     +4: F3          the loop's start, at tick 4
//     +5: 00          wait 0
//     +6: C2          volume 2, at tick 4: note 24 on the first pass, note 26 on the later ones
//     +7: 04          wait 4
//     +8: D3 1A       note 26 at volume 3, at tick 8
//    +10: 04          wait 4
//    +11: E0          rest, at tick 12
//    +12: 04          wait 4
//    +13: FE          loop the song, at tick 16
//    +14: 00 FD       the other tracks
const std::vector<uint8_t> kLoopedVolume = {0x00, 0xD3, 0x18, 0x04, 0xF3, 0x00, 0xC2, 0x04,
                                            0xD3, 0x1A, 0x04, 0xE0, 0x04, 0xFE, 0x00, 0xFD};

// The check of volume starts follows each track through its calls, into commands that tracks share and around the
// song's loop. An edit that changes the key of a volume start there is refused. An edit beside it that changes no key
// goes in, and ntdlib's model of the driver shows the same keys.
void TestVolumeStartsAcrossBlocks()
{
    using Starts = std::vector<ChannelStart>;
    const std::string add = "Can't add a note there. It would change another note on the track.";
    const auto refusal_of = [](const File& file, const edit::NewNote& note)
    {
        return ErrorOf<std::invalid_argument>([&] { edit::EditNotes(file, 0, {}, std::span(&note, 1)); });
    };
    const auto starts_with = [](const File& file, const edit::NewNote& note, int frames)
    {
        return ChannelStartsOf(edit::EditNotes(file, 0, {}, std::span(&note, 1)).file, frames);
    };
    // A new note of key `key` at `tick`, in the stretch of note `beside` of track 0.
    const auto new_note = [](const File& file, std::size_t beside, uint32_t tick, int key)
    {
        const Sequence song = ReadOf(file, 0);
        const song::Track& square = *TrackOf(song, 0);
        return edit::NewKonamiNote(file, 0, square, square.stretches.at(square.notes.at(beside).stretch), tick,
                                   edit::kKonamiLowestPsgKey + key, 100, 1);
    };

    // The call: a new note after note 28 would be the note that the volume command starts again in the call, whatever
    // its key. So would one before the volume command, where the track first runs it. A new note after the call goes
    // in: no volume command follows.
    const File called = FileWithSong(kCalledVolume, 24);
    const Starts called_starts = {{0, 3, 24},  {4, 2, 24},      {8, kRest, 24}, {10, 3, 28},    {12, kRest, 28},
                                  {16, 2, 28}, {20, kRest, 28}, {24, 3, 29},    {28, kRest, 29}};
    XSFLIB_CHECK(ChannelStartsOf(called, 32) == called_starts);
    XSFLIB_CHECK(refusal_of(called, new_note(called, 0, 2, 30)) == add);
    XSFLIB_CHECK(refusal_of(called, new_note(called, 2, 11, 30)) == add);
    XSFLIB_CHECK(refusal_of(called, new_note(called, 2, 11, 28)) == add);
    XSFLIB_CHECK(starts_with(called, new_note(called, 4, 26, 30), 32) == (Starts{{0, 3, 24},
                                                                                 {4, 2, 24},
                                                                                 {8, kRest, 24},
                                                                                 {10, 3, 28},
                                                                                 {12, kRest, 28},
                                                                                 {16, 2, 28},
                                                                                 {20, kRest, 28},
                                                                                 {24, 3, 29},
                                                                                 {26, 12, 30},
                                                                                 {27, kRest, 30},
                                                                                 {28, kRest, 30}}));

    // A new note of another key before note 28 changes nothing: the volume command still follows note 28 in the call.
    XSFLIB_CHECK(starts_with(called, new_note(called, 2, 9, 30), 32) == (Starts{{0, 3, 24},
                                                                                {4, 2, 24},
                                                                                {8, kRest, 24},
                                                                                {9, 12, 30},
                                                                                {10, 3, 28},
                                                                                {12, kRest, 28},
                                                                                {16, 2, 28},
                                                                                {20, kRest, 28},
                                                                                {24, 3, 29},
                                                                                {28, kRest, 29}}));

    // The shared commands: track 0 runs into them after its note. A new note at tick 2 goes into the shared commands,
    // and the other tracks run its command too. Their volume command had no note command before it. A new note of
    // either key is refused.
    const File shared = FileWithSong(kSharedVolume, 3);
    XSFLIB_CHECK(ChannelStartsOf(shared, 12) == (Starts{{0, 3, 24}, {4, 2, 24}, {8, kRest, 24}}));
    XSFLIB_CHECK(refusal_of(shared, new_note(shared, 0, 2, 30)) == add);
    XSFLIB_CHECK(refusal_of(shared, new_note(shared, 0, 2, 24)) == add);

    // The loop: on the second pass, the volume command starts note 26 again. A new note after note 26 would take that
    // place, whatever its key. A new note before note 26 changes nothing on either pass.
    const File looped = FileWithSong(kLoopedVolume, 14);
    XSFLIB_CHECK(
        ChannelStartsOf(looped, 26) ==
        (Starts{{0, 3, 24}, {4, 2, 24}, {8, 3, 26}, {12, kRest, 26}, {16, 2, 26}, {20, 3, 26}, {24, kRest, 26}}));
    XSFLIB_CHECK(refusal_of(looped, new_note(looped, 2, 13, 30)) == add);
    XSFLIB_CHECK(refusal_of(looped, new_note(looped, 2, 13, 26)) == add);
    XSFLIB_CHECK(starts_with(looped, new_note(looped, 2, 5, 30), 26) == (Starts{{0, 3, 24},
                                                                                {4, 2, 24},
                                                                                {5, 12, 30},
                                                                                {6, kRest, 30},
                                                                                {8, 3, 26},
                                                                                {12, kRest, 26},
                                                                                {16, 2, 26},
                                                                                {17, 12, 30},
                                                                                {18, kRest, 30},
                                                                                {20, 3, 26},
                                                                                {24, kRest, 26}}));

    // A volume command of volume 0 starts no note that sounds. A new note before it goes in. The channel starts at
    // volume 0 at the new note's key there, and nothing sounds:
    //
    //     +0: 00          wait 0
    //     +1: D3 18       note 24 at volume 3, at tick 0
    //     +3: 04          wait 4
    //     +4: C0          volume 0, at tick 4
    //     +5: 04          wait 4
    //     +6: D3 1C       note 28 at volume 3, at tick 8
    //     +8: 04          wait 4
    //     +9: E0          rest, at tick 12
    //    +10: 00          wait 0
    //    +11: FF          stop the song
    //    +12: 00 FD       the other tracks
    const File muted =
        FileWithSong({0x00, 0xD3, 0x18, 0x04, 0xC0, 0x04, 0xD3, 0x1C, 0x04, 0xE0, 0x00, 0xFF, 0x00, 0xFD}, 12);
    XSFLIB_CHECK(ChannelStartsOf(muted, 16) == (Starts{{0, 3, 24}, {4, 0, 24}, {8, 3, 28}, {12, kRest, 28}}));
    XSFLIB_CHECK(starts_with(muted, new_note(muted, 0, 2, 30), 16) ==
                 (Starts{{0, 3, 24}, {2, 12, 30}, {3, kRest, 30}, {4, 0, 30}, {8, 3, 28}, {12, kRest, 28}}));

    // A new length adds a rest inside a call's commands, before the volume command that ends them. The edit keeps
    // every command in its order and goes in without the walk. The call still plays the volume command:
    //
    //     +0: 00          wait 0
    //     +1: D3 18       note 24 at volume 3, at tick 0
    //     +3: 04          wait 4
    //     +4: D3 1A       note 26 at volume 3, at tick 4
    //     +6: 04          wait 4
    //     +7: C2          volume 2, at tick 8: starts note 26 again
    //     +8: 04          wait 4
    //     +9: E0          rest, at tick 12
    //    +10: 00          wait 0
    //    +11: 9F 03 00 02 call of 2 commands from +3, at tick 12: note 26 at tick 16 and the volume command at tick 20
    //    +15: 04          wait 4
    //    +16: E0          rest, at tick 24
    //    +17: 00          wait 0
    //    +18: FF          stop the song
    //    +19: 00 FD       the other tracks
    const File ranged = FileWithSong({0x00, 0xD3, 0x18, 0x04, 0xD3, 0x1A, 0x04, 0xC2, 0x04, 0xE0, 0x00,
                                      0x9F, 0x03, 0x00, 0x02, 0x04, 0xE0, 0x00, 0xFF, 0x00, 0xFD},
                                     19);
    XSFLIB_CHECK(
        ChannelStartsOf(ranged, 28) ==
        (Starts{{0, 3, 24}, {4, 3, 26}, {8, 2, 26}, {12, kRest, 26}, {16, 3, 26}, {20, 2, 26}, {24, kRest, 26}}));
    const Sequence ranged_song = ReadOf(ranged, 0);
    const song::Track& ranged_square = *TrackOf(ranged_song, 0);
    const edit::NoteEdit shorter{.command = ranged_square.notes[1].command,
                                 .stretch = ranged_square.stretches.at(ranged_square.notes[1].stretch),
                                 .length = 2u};
    XSFLIB_CHECK(ChannelStartsOf(edit::EditNotes(ranged, 0, std::span(&shorter, 1)).file, 28) ==
                 (Starts{{0, 3, 24},
                         {4, 3, 26},
                         {6, kRest, 26},
                         {8, 2, 26},
                         {12, kRest, 26},
                         {16, 3, 26},
                         {18, kRest, 26},
                         {20, 2, 26},
                         {24, kRest, 26}}));

    // A call inside a call: the check doesn't follow the track. An edit that adds a command is refused. A new velocity
    // keeps every command in its place, and goes in.
    const File nested = FileWithSong(kNestedCall, 21);
    XSFLIB_CHECK(ChannelStartsOf(nested, 20) ==
                 (Starts{{0, 3, 24}, {4, 2, 24}, {8, kRest, 24}, {12, 2, 24}, {16, kRest, 24}}));
    XSFLIB_CHECK(
        refusal_of(nested, new_note(nested, 0, 2, 24)) ==
        "Can't edit this song here. One of its tracks calls inside a call. Edit its commands in the List Editor.");
    XSFLIB_CHECK(edit::KonamiWalkOf(nested, 0).stopped.has_value());
    const Sequence nested_song = ReadOf(nested, 0);
    const song::Track& nested_square = *TrackOf(nested_song, 0);
    const edit::NoteEdit louder{.command = nested_square.notes[0].command,
                                .stretch = nested_square.stretches.at(nested_square.notes[0].stretch),
                                .velocity = 127};
    XSFLIB_CHECK(ChannelStartsOf(edit::EditNotes(nested, 0, std::span(&louder, 1)).file, 20) ==
                 (Starts{{0, 15, 24}, {4, 2, 24}, {8, kRest, 24}, {12, 2, 24}, {16, kRest, 24}}));
}

// The first track that reaches the song's loop starts every track again. A longer track is cut short there. Here the
// other tracks loop the song at tick 8, and track 0 never plays note 26:
//
//     +0: 00          wait 0
//     +1: D3 18       note 24 at volume 3, at tick 0
//     +3: 04          wait 4
//     +4: C2          volume 2, at tick 4: starts note 24 again
//     +5: 08          wait 8
//     +6: D3 1A       note 26 at volume 3, at tick 12
//     +8: 04          wait 4
//     +9: E0          rest, at tick 16
//    +10: 00          wait 0
//    +11: FD          end until the song loops
//    +12: 08 FE       the other tracks: loop the song at tick 8
//
// The check follows each track to its end, and can't tell which note command a track ran last before such a cut. It
// refuses an edit of the song that adds, removes or moves a command. A new velocity goes in.
void TestLoopCutsTrack()
{
    using Starts = std::vector<ChannelStart>;
    const File file =
        FileWithSong({0x00, 0xD3, 0x18, 0x04, 0xC2, 0x08, 0xD3, 0x1A, 0x04, 0xE0, 0x00, 0xFD, 0x08, 0xFE}, 12);
    XSFLIB_CHECK(ChannelStartsOf(file, 20) == (Starts{{0, 3, 24}, {4, 2, 24}, {8, 3, 24}, {12, 2, 24}, {16, 3, 24}}));

    const Sequence song = ReadOf(file, 0);
    const song::Track& square = *TrackOf(song, 0);
    const song::Stretch& stretch = square.stretches.at(square.notes[0].stretch);
    const edit::NewNote added =
        edit::NewKonamiNote(file, 0, square, stretch, 5, edit::kKonamiLowestPsgKey + 30, 100, 1);
    XSFLIB_CHECK(
        ErrorOf<std::invalid_argument>([&] { edit::EditNotes(file, 0, {}, std::span(&added, 1)); }) ==
        "Can't edit this song here. The song loops before one of its tracks has played all its notes. Edit its "
        "commands in the List Editor.");
    const edit::KonamiWalked walked = edit::KonamiWalkOf(file, 0);
    XSFLIB_CHECK(walked.stopped.has_value() && walked.loop_cuts);

    const edit::NoteEdit louder{.command = square.notes[0].command, .stretch = stretch, .velocity = 127};
    XSFLIB_CHECK(ChannelStartsOf(edit::EditNotes(file, 0, std::span(&louder, 1)).file, 20) ==
                 (Starts{{0, 15, 24}, {4, 2, 24}, {8, 15, 24}, {12, 2, 24}, {16, 15, 24}}));

    // A new key and a new length go in too. They keep every note command and volume command at its time. The new
    // length puts a rest at tick 2, and the volume command at tick 4 still starts note 24.
    const edit::NoteEdit higher{.command = square.notes[0].command, .stretch = stretch, .transpose = 2};
    XSFLIB_CHECK(ChannelStartsOf(edit::EditNotes(file, 0, std::span(&higher, 1)).file, 20) ==
                 (Starts{{0, 3, 26}, {4, 2, 26}, {8, 3, 26}, {12, 2, 26}, {16, 3, 26}}));
    const edit::NoteEdit shorter{.command = square.notes[0].command, .stretch = stretch, .length = 2};
    XSFLIB_CHECK(ChannelStartsOf(edit::EditNotes(file, 0, std::span(&shorter, 1)).file, 20) ==
                 (Starts{{0, 3, 24},
                         {2, kRest, 24},
                         {4, 2, 24},
                         {8, 3, 24},
                         {10, kRest, 24},
                         {12, 2, 24},
                         {16, 3, 24},
                         {18, kRest, 24}}));

    // A song whose loop cuts no track before a note: track 0 rests from tick 6 to its end at tick 12, and the other
    // tracks loop the song at tick 8. A note added or moved to tick 9 would never play. The refusal names the edit:
    //
    //     +0: 00          wait 0
    //     +1: D3 18       note 24 at volume 3, at tick 0
    //     +3: 02          wait 2
    //     +4: C2          volume 2, at tick 2
    //     +5: 02          wait 2
    //     +6: D3 1A       note 26 at volume 3, at tick 4
    //     +8: 02          wait 2
    //     +9: E0          rest, at tick 6
    //    +10: 06          wait 6
    //    +11: E0          rest, at tick 12
    //    +12: 00          wait 0
    //    +13: FD          end until the song loops
    //    +14: 08 FE       the other tracks: loop the song at tick 8
    const File tail = FileWithSong(
        {0x00, 0xD3, 0x18, 0x02, 0xC2, 0x02, 0xD3, 0x1A, 0x02, 0xE0, 0x06, 0xE0, 0x00, 0xFD, 0x08, 0xFE}, 14);
    XSFLIB_CHECK(!edit::KonamiWalkOf(tail, 0).stopped.has_value());
    const Sequence tail_song = ReadOf(tail, 0);
    const song::Track& tail_square = *TrackOf(tail_song, 0);
    const song::Note& last = tail_square.notes[2];
    const song::Stretch& tail_stretch = tail_square.stretches.at(last.stretch);
    const edit::NewNote late =
        edit::NewKonamiNote(tail, 0, tail_square, tail_stretch, 9, edit::kKonamiLowestPsgKey + 30, 100, 1);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::EditNotes(tail, 0, {}, std::span(&late, 1)); }) ==
                 "Can't add a note there. The song loops before it would play.");
    XSFLIB_CHECK(RefusalOf(tail, {.command = last.command, .stretch = tail_stretch, .shift = 5}) ==
                 "Can't move this note there. The song loops before it would play.");
}

// What the refusal of a cut track prevents, and where the rule ends. In each song, the other tracks loop the song at
// tick 8.
void TestLoopCutEdges()
{
    using Starts = std::vector<ChannelStart>;
    const std::string cut =
        "Can't edit this song here. The song loops before one of its tracks has played all its "
        "notes. Edit its commands in the List Editor.";

    // A note of key 30 and 1 tick at `tick`, in the stretch of the first note of track 0.
    const auto new_note = [](const File& file, uint32_t tick)
    {
        const Sequence song = ReadOf(file, 0);
        const song::Track& square = *TrackOf(song, 0);
        return edit::NewKonamiNote(file, 0, square, square.stretches.at(square.notes.at(0).stretch), tick,
                                   edit::kKonamiLowestPsgKey + 30, 100, 1);
    };
    const auto refusal_of = [](const File& file, const edit::NewNote& note)
    {
        return ErrorOf<std::invalid_argument>([&] { edit::EditNotes(file, 0, {}, std::span(&note, 1)); });
    };

    // A volume command at the top of the loop of a cut track. Track 0 would loop the song at tick 16. The other tracks
    // loop it at tick 8, before note 28. The volume command then starts note 26 at tick 8:
    //
    //     +0: 00          wait 0
    //     +1: D3 18       note 24 at volume 3, at tick 0
    //     +3: 00          wait 0
    //     +4: F3          the loop's start, at tick 0
    //     +5: 00          wait 0
    //     +6: C2          volume 2, at tick 0: starts the note of the last note command that the track ran
    //     +7: 04          wait 4
    //     +8: D3 1A       note 26 at volume 3, at tick 4
    //    +10: 08          wait 8
    //    +11: D3 1C       note 28 at volume 3, at tick 12
    //    +13: 04          wait 4
    //    +14: E0          rest, at tick 16
    //    +15: 00          wait 0
    //    +16: FE          loop the song, at tick 16
    //    +17: 08 FE       the other tracks: loop the song at tick 8
    //
    // A walk to the track's end takes note 28 for the volume command of the second pass. A note added at tick 5 leaves
    // note 28 the last note command, and that walk lets it in. In the driver, the new note is the last one before
    // the cut, and the volume command starts it at tick 8 and at tick 16.
    const File harmed = FileWithSong({0x00, 0xD3, 0x18, 0x00, 0xF3, 0x00, 0xC2, 0x04, 0xD3, 0x1A, 0x08, 0xD3, 0x1C,
                                      0x04, 0xE0, 0x00, 0xFE, 0x08, 0xFE},
                                     17);
    XSFLIB_CHECK(ChannelStartsOf(harmed, 20) == (Starts{{0, 2, 24}, {4, 3, 26}, {8, 2, 26}, {12, 3, 26}, {16, 2, 26}}));
    const edit::KonamiWalked walked = edit::KonamiWalkOf(harmed, 0);
    XSFLIB_CHECK(walked.stopped.has_value() && walked.loop_cuts && !walked.later_loop_cuts);
    XSFLIB_CHECK(refusal_of(harmed, new_note(harmed, 5)) == cut);

    // The song with that note written into its bytes: DC 1E at tick 5 and a rest at tick 6.
    const File written = FileWithSong({0x00, 0xD3, 0x18, 0x00, 0xF3, 0x00, 0xC2, 0x04, 0xD3, 0x1A, 0x01, 0xDC,
                                       0x1E, 0x01, 0xE0, 0x06, 0xD3, 0x1C, 0x04, 0xE0, 0x00, 0xFE, 0x08, 0xFE},
                                      22);
    XSFLIB_CHECK(ChannelStartsOf(written, 20) == (Starts{{0, 2, 24},
                                                         {4, 3, 26},
                                                         {5, 12, 30},
                                                         {6, kRest, 30},
                                                         {8, 2, 30},
                                                         {12, 3, 26},
                                                         {13, 12, 30},
                                                         {14, kRest, 30},
                                                         {16, 2, 30}}));

    // A loop point after the cut. Track 0 never passes it, and starts every pass at its start. A walk to the track's
    // end would start the second pass after the loop point:
    //
    //     +0: 00          wait 0
    //     +1: D3 18       note 24 at volume 3, at tick 0
    //     +3: 04          wait 4
    //     +4: C2          volume 2, at tick 4
    //     +5: 08          wait 8
    //     +6: F3          the loop's start, at tick 12
    //     +7: 00          wait 0
    //     +8: E0          rest, at tick 12
    //     +9: 00          wait 0
    //    +10: FD          end until the song loops
    //    +11: 08 FE       the other tracks
    const File late_point =
        FileWithSong({0x00, 0xD3, 0x18, 0x04, 0xC2, 0x08, 0xF3, 0x00, 0xE0, 0x00, 0xFD, 0x08, 0xFE}, 11);
    XSFLIB_CHECK(ChannelStartsOf(late_point, 20) ==
                 (Starts{{0, 3, 24}, {4, 2, 24}, {8, 3, 24}, {12, 2, 24}, {16, 3, 24}}));
    XSFLIB_CHECK(refusal_of(late_point, new_note(late_point, 5)) == cut);

    // A note command at the very tick of the loop. The tracks run from 15 down to 0, and the loop comes before track 0
    // runs note 26. The volume command starts note 24 on every pass:
    //
    //     +0: 00          wait 0
    //     +1: D3 18       note 24 at volume 3, at tick 0
    //     +3: 00          wait 0
    //     +4: F3          the loop's start, at tick 0
    //     +5: 00          wait 0
    //     +6: C2          volume 2, at tick 0
    //     +7: 08          wait 8
    //     +8: D3 1A       note 26 at volume 3, at tick 8
    //    +10: 04          wait 4
    //    +11: E0          rest, at tick 12
    //    +12: 00          wait 0
    //    +13: FD          end until the song loops
    //    +14: 08 FE       the other tracks
    const File at_loop = FileWithSong(
        {0x00, 0xD3, 0x18, 0x00, 0xF3, 0x00, 0xC2, 0x08, 0xD3, 0x1A, 0x04, 0xE0, 0x00, 0xFD, 0x08, 0xFE}, 14);
    XSFLIB_CHECK(ChannelStartsOf(at_loop, 20) == (Starts{{0, 2, 24}, {8, 2, 24}, {16, 2, 24}}));
    XSFLIB_CHECK(refusal_of(at_loop, new_note(at_loop, 4)) == cut);

    // A track that ends for good before the loop, on the first revision: FD at tick 4. The loop doesn't start it
    // again, and its volume command never runs after note 24. A note added after note 24 goes in:
    //
    //     +0: 00          wait 0
    //     +1: C2          volume 2, at tick 0
    //     +2: 02          wait 2
    //     +3: D3 18       note 24 at volume 3, at tick 2
    //     +5: 02          wait 2
    //     +6: E0          rest, at tick 4
    //     +7: 00          wait 0
    //     +8: FD          end for good
    //     +9: 08 FF 01    the other tracks: loop the song at tick 8
    const File ended = FirstFileWithSong({0x00, 0xC2, 0x02, 0xD3, 0x18, 0x02, 0xE0, 0x00, 0xFD, 0x08, 0xFF, 0x01}, 9);
    XSFLIB_CHECK(ChannelStartsOf(ended, 20) == (Starts{{0, 2, 0}, {2, 3, 24}, {4, kRest, 24}}));
    XSFLIB_CHECK(!edit::KonamiWalkOf(ended, 0).stopped.has_value());
    const edit::NewNote after = new_note(ended, 3);
    XSFLIB_CHECK(ChannelStartsOf(edit::EditNotes(ended, 0, {}, std::span(&after, 1)).file, 20) ==
                 (Starts{{0, 2, 0}, {2, 3, 24}, {3, 12, 30}, {4, kRest, 30}}));
}

// A song with an intro. Track 4 passes its loop point at tick 8 and loops the song at tick 16. Each later loop comes 8
// ticks after the one before. Track 0 has no loop point, and starts every pass at its start:
//
//     +0: 00          wait 0
//     +1: C2          volume 2, at tick 0: starts the note of the last note command that the track ran
//     +2: 02          wait 2
//     +3: D3 18       note 24 at volume 3, at tick 2
//     +5: 08          wait 8
//     +6: D3 1A       note 26 at volume 3, at tick 10
//     +8: 02          wait 2
//     +9: FD          end until the song loops, at tick 12
//    +10: 08          track 4: wait 8
//    +11: F3 00 00    the loop's start, at tick 8
//    +14: 08          wait 8
//    +15: FE          loop the song, at tick 16
//    +16: 00 FD       the other tracks
//
// The first loop cuts track 0 after its end. The volume command at tick 16 starts note 26. The later loops cut track 0
// at its tick 8, before note 26, and the volume command at tick 24 starts note 24. A note added between the two notes
// would be the one that it starts there. The check follows each pass to the track's end and can't tell. It doesn't
// follow the song.
void TestLaterLoopCutsTrack()
{
    using Starts = std::vector<ChannelStart>;
    const File file = FileWithVoice(
        {0x00, 0xC2, 0x02, 0xD3, 0x18, 0x08, 0xD3, 0x1A, 0x02, 0xFD, 0x08, 0xF3, 0x00, 0x00, 0x08, 0xFE, 0x00, 0xFD},
        16, 10);
    XSFLIB_CHECK(ChannelStartsOf(file, 36) == (Starts{{0, 2, 0},
                                                      {2, 3, 24},
                                                      {10, 3, 26},
                                                      {12, kRest, 26},
                                                      {16, 2, 26},
                                                      {18, 3, 24},
                                                      {24, 2, 24},
                                                      {26, 3, 24},
                                                      {32, 2, 24},
                                                      {34, 3, 24}}));
    const edit::KonamiWalked walked = edit::KonamiWalkOf(file, 0);
    XSFLIB_CHECK(walked.stopped.has_value() && walked.loop_cuts && walked.later_loop_cuts);

    const Sequence song = ReadOf(file, 0);
    const song::Track& square = *TrackOf(song, 0);
    const edit::NewNote added = edit::NewKonamiNote(file, 0, square, square.stretches.at(square.notes[1].stretch), 5,
                                                    edit::kKonamiLowestPsgKey + 30, 100, 1);
    XSFLIB_CHECK(
        ErrorOf<std::invalid_argument>([&] { edit::EditNotes(file, 0, {}, std::span(&added, 1)); }) ==
        "Can't edit this song here. The song loops before one of its tracks has played all its notes. Edit its "
        "commands in the List Editor.");

    // The song with that note written into its bytes: DC 1E at tick 5 and a rest at tick 6. The volume command at
    // tick 24 starts key 30.
    const File written = FileWithVoice({0x00, 0xC2, 0x02, 0xD3, 0x18, 0x03, 0xDC, 0x1E, 0x01, 0xE0, 0x04, 0xD3,
                                        0x1A, 0x02, 0xFD, 0x08, 0xF3, 0x00, 0x00, 0x08, 0xFE, 0x00, 0xFD},
                                       21, 15);
    XSFLIB_CHECK(ChannelStartsOf(written, 28) == (Starts{{0, 2, 0},
                                                         {2, 3, 24},
                                                         {5, 12, 30},
                                                         {6, kRest, 30},
                                                         {10, 3, 26},
                                                         {12, kRest, 26},
                                                         {16, 2, 26},
                                                         {18, 3, 24},
                                                         {21, 12, 30},
                                                         {22, kRest, 30},
                                                         {24, 2, 30},
                                                         {26, 3, 24}}));

    // The same song with note 26 at tick 7, before the later loops' cut at tick 8. No loop cuts the track, and the
    // volume command at tick 24 starts note 26.
    const File early = FileWithVoice(
        {0x00, 0xC2, 0x02, 0xD3, 0x18, 0x05, 0xD3, 0x1A, 0x05, 0xFD, 0x08, 0xF3, 0x00, 0x00, 0x08, 0xFE, 0x00, 0xFD},
        16, 10);
    XSFLIB_CHECK(ChannelStartsOf(early, 28) == (Starts{{0, 2, 0},
                                                       {2, 3, 24},
                                                       {7, 3, 26},
                                                       {12, kRest, 26},
                                                       {16, 2, 26},
                                                       {18, 3, 24},
                                                       {23, 3, 26},
                                                       {24, 2, 26},
                                                       {26, 3, 24}}));
    XSFLIB_CHECK(!edit::KonamiWalkOf(early, 0).stopped.has_value());

    // An edit that puts a note command at tick 9 makes the later loops cut the track. The note would play at tick 9
    // and never again. The song with note 26 written at tick 9 shows it: no note starts at tick 25, and the volume
    // command at tick 24 starts note 24.
    const Sequence early_song = ReadOf(early, 0);
    const song::Track& early_square = *TrackOf(early_song, 0);
    const song::Note& second = early_square.notes[2];
    const song::Stretch& stretch = early_square.stretches.at(second.stretch);
    XSFLIB_CHECK(second.start == 7 && second.key == edit::kKonamiLowestPsgKey + 26);
    XSFLIB_CHECK(RefusalOf(early, {.command = second.command, .stretch = stretch, .shift = 2}) ==
                 "Can't move this note there. It wouldn't play after the song loops.");
    const edit::NewNote late =
        edit::NewKonamiNote(early, 0, early_square, stretch, 9, edit::kKonamiLowestPsgKey + 30, 100, 1);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::EditNotes(early, 0, {}, std::span(&late, 1)); }) ==
                 "Can't add a note there. It wouldn't play after the song loops.");
    const File moved = FileWithVoice(
        {0x00, 0xC2, 0x02, 0xD3, 0x18, 0x07, 0xD3, 0x1A, 0x03, 0xFD, 0x08, 0xF3, 0x00, 0x00, 0x08, 0xFE, 0x00, 0xFD},
        16, 10);
    XSFLIB_CHECK(
        ChannelStartsOf(moved, 28) ==
        (Starts{
            {0, 2, 0}, {2, 3, 24}, {9, 3, 26}, {12, kRest, 26}, {16, 2, 26}, {18, 3, 24}, {24, 2, 24}, {26, 3, 24}}));
}

// On the builds that store a volume in a byte, a PSG channel starts at the low 4 bits of the volume of its track. The
// volume is stored in bits 12 to 15 of the 16-bit register of the channel (ntdlib src/konami/output_stages.cpp,
// StorePsg). A volume of a PSG track therefore ends at 15, and a volume of a sampled track at 127. Song 0 of the first
// revision with track 0 playing a square wave:
//
//     +0: 00          wait 0
//     +1: D3 18       note 24 at volume 3, at tick 0
//     +3: 04          wait 4
//     +4: D0 1A       note 26 at volume 0, at tick 4: note 24 ends, and nothing sounds
//     +6: 02          wait 2
//     +7: C2          volume 2, at tick 6: note 26 starts
//     +8: 04          wait 4
//     +9: E0          rest, at tick 10
//    +10: 00          wait 0
//    +11: FD          end
//
// Every other track shares the bytes at +10.
void TestPsgVolumeRange()
{
    using ntdlib::commands::Command;
    using ntdlib::commands::Kind;
    using ntdlib::commands::kOne;
    using ntdlib::commands::Source;
    using Starts = std::vector<ChannelStart>;
    const File file = FirstFileWithSong({0x00, 0xD3, 0x18, 0x04, 0xD0, 0x1A, 0x02, 0xC2, 0x04, 0xE0, 0x00, 0xFD}, 10);
    XSFLIB_CHECK(ChannelStartsOf(file, 12) == (Starts{{0, 3, 24}, {4, 0, 26}, {6, 2, 26}, {10, kRest, 26}}));
    const song::Console console = edit::ConsoleOf(file);
    SEQLIB_REQUIRE(konami::UltimateMasters(*song::KonamiRevisionOf(console.driver)));

    // The note at tick 6 names the volume command.
    const Sequence song0 = ReadOf(file, 0);
    const song::Track& square = *TrackOf(song0, 0);
    const song::Note& note = square.notes[1];
    const std::vector<Command>& items = file.sequences[0]->items;
    const auto volume = std::ranges::find(items, Kind::kVolume, &Command::kind);
    SEQLIB_REQUIRE(note.command == 7 && volume != items.end());
    for (int track = 0; track < konami::kTracks; track++)
    {
        const int64_t most = konami::IsPsgTrack(track) ? 15 : 127;
        XSFLIB_CHECK(
            (edit::ArgumentRange(console, track, *volume, Source::kValue) == edit::ValueRange{0, most * kOne}));
    }

    // Volume 15 on the PSG track: the driver starts the channel at 15.
    Command loudest = *volume;
    loudest.value.value = 15 * kOne;
    const edit::CommandPlace place{.command = note.command, .stretch = square.stretches.at(note.stretch)};
    XSFLIB_CHECK(ChannelStartsOf(edit::ChangeCommand(file, 0, place, loudest).file, 12) ==
                 (Starts{{0, 3, 24}, {4, 0, 26}, {6, 15, 26}, {10, kRest, 26}}));
}

// The check of volume starts walks a track's commands (edit::KonamiWalkOf). The reader takes the order from
// a run of ntdlib's model of the driver (song::KonamiVolumeStarts). For each note that a volume command starts in the
// read, the walk has to give the same note command.
void TestWalkAgreesWithReader()
{
    // Whether the walk gives each of the read's notes that a volume command starts, at least `least` of them.
    const auto agree = [](const File& file, uint32_t sound, std::size_t least)
    {
        const std::shared_ptr<const KonamiRom> rom = song::LoadKonamiRom(format::ExportArchive(file));
        const edit::KonamiWalked walked = edit::KonamiWalkOf(file, sound);
        if (!rom || walked.stopped)
        {
            return false;
        }

        const std::vector<song::KonamiVolumeStart> read = song::KonamiVolumeStarts(*rom, sound);
        const auto walks = [&](const song::KonamiVolumeStart& start)
        {
            return std::ranges::binary_search(walked.starts, start);
        };
        return read.size() >= least && std::ranges::all_of(read, walks);
    };

    for (const std::vector<uint8_t>& cart : {FirstCart(), LaterCart()})
    {
        const File file = format::ImportKonamiRom(cart);
        XSFLIB_CHECK(agree(file, 0, 0));
        XSFLIB_CHECK(agree(file, 1, 0));
    }

    // The songs of TestVolumeStartsKeepTheirKeys, then a call, shared commands and a loop.
    XSFLIB_CHECK(agree(FileWithSong({0x00, 0xD3, 0x18, 0x04, 0xD0, 0x1A, 0x02, 0xC2, 0x04, 0xE0, 0x02, 0xD3, 0x1C, 0x04,
                                     0xE0, 0x00, 0xFF, 0x00, 0xFD},
                                    17),
                       0, 1));
    XSFLIB_CHECK(agree(FileWithSong({0x00, 0xD3, 0x18, 0x06, 0xC2, 0x04, 0xE0, 0x02, 0xD3, 0x1A,
                                     0x04, 0xE0, 0x01, 0xC3, 0x05, 0xE0, 0x00, 0xFF, 0x00, 0xFD},
                                    18),
                       0, 2));
    XSFLIB_CHECK(
        agree(FileWithSong(
                  {0x00, 0xD3, 0x18, 0x04, 0xD0, 0x1A, 0x02, 0xF3, 0x00, 0xC2, 0x04, 0xE0, 0x06, 0xFD, 0x00, 0xFD}, 14),
              0, 1));
    XSFLIB_CHECK(agree(FileWithSong(kCalledVolume, 24), 0, 2));
    XSFLIB_CHECK(agree(FileWithSong(kSharedVolume, 3), 0, 1));
    XSFLIB_CHECK(agree(FileWithSong(kLoopedVolume, 14), 0, 1));
}

// The volume lane moves a volume command as a setting. A volume command of a PSG track that would run before its note
// command, or after the next one, would start a note at another key. Such a move is refused. A move that keeps the
// volume command after its note command goes in. The songs are those of TestVolumeStartsKeepTheirKeys.
void TestMovedVolumeKeepsItsKey()
{
    using Starts = std::vector<ChannelStart>;
    const std::string refusal = "Can't move this volume there. It would start another note again.";
    const auto refusal_of = [](const File& file, const song::Track& track, const edit::SettingMove& move)
    {
        return ErrorOf<std::invalid_argument>([&]
                                              { edit::MoveSettings(file, 0, std::span(&move, 1), track.stretches); });
    };

    // The volume command at +7 runs at tick 6, after the silent note command at tick 4.
    const File silent = FileWithSong({0x00, 0xD3, 0x18, 0x04, 0xD0, 0x1A, 0x02, 0xC2, 0x04, 0xE0, 0x02, 0xD3, 0x1C,
                                      0x04, 0xE0, 0x00, 0xFF, 0x00, 0xFD},
                                     17);
    const Sequence silent_song = ReadOf(silent, 0);
    const song::Track& square = *TrackOf(silent_song, 0);
    const song::Stretch& stretch = square.stretches.at(0);
    XSFLIB_CHECK(refusal_of(silent, square, {.command = 7, .stretch = stretch, .shift = -4}) == refusal);
    XSFLIB_CHECK(refusal_of(silent, square, {.command = 7, .stretch = stretch, .shift = -2}) == refusal);
    const edit::SettingMove earlier{.command = 7, .stretch = stretch, .shift = -1};
    XSFLIB_CHECK(ChannelStartsOf(edit::MoveSettings(silent, 0, std::span(&earlier, 1), square.stretches).file, 16) ==
                 (Starts{{0, 3, 24}, {4, 0, 26}, {5, 2, 26}, {10, kRest, 26}, {12, 3, 28}}));

    // The volume command at +4 runs at tick 6 and starts note 24 again. Note 26 starts at tick 12.
    const File again = FileWithSong({0x00, 0xD3, 0x18, 0x06, 0xC2, 0x04, 0xE0, 0x02, 0xD3, 0x1A,
                                     0x04, 0xE0, 0x01, 0xC3, 0x05, 0xE0, 0x00, 0xFF, 0x00, 0xFD},
                                    18);
    const Sequence again_song = ReadOf(again, 0);
    const song::Track& again_square = *TrackOf(again_song, 0);
    const song::Stretch& again_stretch = again_square.stretches.at(0);
    XSFLIB_CHECK(refusal_of(again, again_square, {.command = 4, .stretch = again_stretch, .shift = 7}) == refusal);
    const edit::SettingMove later{.command = 4, .stretch = again_stretch, .shift = 2};
    XSFLIB_CHECK(ChannelStartsOf(edit::MoveSettings(again, 0, std::span(&later, 1), again_square.stretches).file, 16) ==
                 (Starts{{0, 3, 24}, {8, 2, 24}, {10, kRest, 24}, {12, 3, 26}}));
}

// A song whose loop takes no time hangs the driver: after the song loop, its frame never ends.
void TestLoopWithoutWait()
{
    const File file = format::ImportKonamiRom(FirstCart());
    const Sequence song0 = ReadOf(file, 0);
    SEQLIB_REQUIRE(TrackOf(song0, 4));
    const song::Stretch stretch = TrackOf(song0, 4)->stretches.back();

    // The wait of 24 ticks at +15 is the only time that track 4's loop takes. Without the rest at +16, it still waits.
    const std::vector<edit::CommandPlace> wait = {edit::CommandPlace{15, stretch}};
    const std::vector<edit::CommandPlace> rest = {edit::CommandPlace{16, stretch}};
    const File hanging = edit::RemoveCommands(file, 0, wait).file;
    const File still_waits = edit::RemoveCommands(file, 0, rest).file;
    XSFLIB_CHECK(!ErrorOf<std::invalid_argument>([&] { edit::RefuseLoopsWithoutWaits(file, hanging); }).empty());
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::RefuseLoopsWithoutWaits(file, still_waits); }).empty());
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::RefuseLoopsWithoutWaits(file, file); }).empty());

    // An edit of a song that already contains such a loop isn't refused for it. A version that shares the sequence
    // isn't looked at. The new version therefore contains a copy.
    File again = hanging;
    again.sequences[0] = ntdlib::commands::Sequence(*hanging.sequences[0]);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::RefuseLoopsWithoutWaits(hanging, again); }).empty());

    // ntdlib's sequencer stops such a song: the read and the model both end.
    XSFLIB_CHECK(ReadOf(hanging, 0).tracks.size() == song0.tracks.size());
    const std::vector<uint8_t> built = format::ExportArchive(hanging);
    const std::shared_ptr<const KonamiRom> rom = song::LoadKonamiRom(built);
    konami::ArchivePlayer player(rom->rom, rom->info);
    player.Start(0);
    std::vector<int16_t> samples;
    for (int frame = 0; frame < 8; ++frame)
    {
        player.RunFrame(samples);
    }

    // The commands of a call count where the call stands. In the later revision's song 0, a call's commands hold the
    // waits at +9 and +12. A wait inside the call keeps the track out of the count, and no wait leaves its 2 places:
    // the track's start and the item after its loop point. Track 0's commands end, and hold no such place.
    const File later = format::ImportKonamiRom(LaterCart());
    ntdlib::commands::Sequence commands = *later.sequences[0];
    const std::vector<std::vector<uint32_t>> tracks = konami::TrackEntries(
        *later.native->pieces, later.native->fields, static_cast<konami::Revision>(later.native->driver->revision));
    const auto first = std::ranges::find_if(tracks, [](const std::vector<uint32_t>& each) { return !each.empty(); });
    SEQLIB_REQUIRE(first != tracks.end());
    std::vector<uint32_t> waits;
    for (uint32_t item = 0; item < commands.items.size(); ++item)
    {
        if (commands.items[item].kind == ntdlib::commands::Kind::kWait && commands.items[item].value.value != 0)
        {
            waits.push_back(item);
        }
    }
    const std::vector<uint32_t> offsets =
        konami::ItemOffsets(commands, static_cast<konami::Revision>(later.native->driver->revision));
    for (const uint32_t item : waits)
    {
        if (offsets[item] != 12)
        {
            commands.items[item].value.value = 0;
        }
    }
    XSFLIB_CHECK(edit::KonamiLoopsWithoutWaits(commands, *first) == 0);
    for (const uint32_t item : waits)
    {
        commands.items[item].value.value = 0;
    }
    XSFLIB_CHECK(edit::KonamiLoopsWithoutWaits(commands, *first) == 2);
}

// A track whose notes have several volumes takes the fader's change as a factor, rounded to the nearest. A sample track
// of the first revision has volumes up to 127, and a track of the later one up to 15. A new pan goes in before the
// track's first note.
void TestMix()
{
    const std::vector<uint8_t> first = FirstCart();
    const File file = format::ImportKonamiRom(first);
    const Sequence song0 = ReadOf(file, 0);
    std::vector<uint8_t> expected = first;
    expected[kSong0 + 2] = 74;
    expected[kSong0 + 12] = 56; // 48 * 74 / 64 is 55.5
    const edit::MixEdit louder{.setting = song::Mix::kVolume, .from = 64, .to = 74};
    SEQLIB_REQUIRE(TrackOf(song0, 4));
    const File louder_file = edit::EditMix(file, 0, 4, TrackOf(song0, 4)->stretches, louder).file;
    XSFLIB_CHECK(format::ExportArchive(louder_file) == expected);

    const edit::MixEdit panned{.setting = song::Mix::kPan, .from = 0, .to = 32};
    const File panned_file = edit::EditMix(file, 0, 4, TrackOf(song0, 4)->stretches, panned).file;
    const Sequence with_pan = ReadOf(panned_file, 0);
    SEQLIB_REQUIRE(StartsOf(with_pan, 4) == StartsOf(song0, 4));
    SEQLIB_REQUIRE(!TrackOf(with_pan, 4)->pan.empty());
    XSFLIB_CHECK(TrackOf(with_pan, 4)->pan.back().value > 0);
    XSFLIB_CHECK(StartsOf(ReadOf(panned_file, 1), 5) == (std::vector<uint32_t>{0}));

    const std::vector<uint8_t> later = LaterCart();
    const File later_file = format::ImportKonamiRom(later);
    const Sequence later_song = ReadOf(later_file, 0);
    expected = later;
    expected[kSong0 + 1] = 0xAA;
    expected[kSong0 + 10] = 0xA6; // 5 * 10 / 8 is 6.25
    const edit::MixEdit raised{.setting = song::Mix::kVolume, .from = 8, .to = 10};
    SEQLIB_REQUIRE(TrackOf(later_song, 4));
    XSFLIB_CHECK(format::ExportArchive(
                     edit::EditMix(later_file, 0, 4, TrackOf(later_song, 4)->stretches, raised).file) == expected);
}

// On the first revision, a track of the wave channel selects a wave by the duty's command, with a byte of 4 plus the
// wave. A later revision has a command for a wave. A new selection takes the command of the revision. A track whose
// commands start no note has no volume to change.
void TestEmptyTracks()
{
    using ntdlib::commands::Kind;
    const auto wave_commands = [](const File& file, uint8_t index)
    {
        const Sequence song0 = ReadOf(file, 0);
        const File edited = edit::EditOpeningInstrument(file, 0, *TrackOf(song0, 2), edit::Program{0, 3}).file;
        const std::vector<uint8_t> rom = format::ExportArchive(edited);
        XSFLIB_CHECK(rom.size() >= 0x1000);
        const File back = format::ImportKonamiRom(rom);
        std::size_t found = 0;

        // A song that grows moves past the others, and the import counts it after them.
        for (const auto& sequence : back.sequences)
        {
            for (const ntdlib::commands::Command& command : sequence->items)
            {
                if (command.kind == Kind::kProgram && command.value.value == 3 * ntdlib::commands::kOne)
                {
                    XSFLIB_CHECK(command.index == index);
                    found++;
                }
            }
        }
        XSFLIB_CHECK(found == 1);
    };
    wave_commands(format::ImportKonamiRom(FirstCart()), 0);
    wave_commands(format::ImportKonamiRom(LaterCart()), 1);

    const File file = format::ImportKonamiRom(FirstCart());
    const Sequence song0 = ReadOf(file, 0);
    SEQLIB_REQUIRE(TrackOf(song0, 1) != nullptr);
    const edit::MixEdit raised{.setting = song::Mix::kVolume, .from = 0, .to = 4};
    XSFLIB_CHECK(
        ErrorOf<std::invalid_argument>([&] { edit::EditMix(file, 0, 1, TrackOf(song0, 1)->stretches, raised); })
            .find("no volume") != std::string::npos);

    // Track 1 runs the wait of 0 and the end that the other empty tracks run too. It takes no note, wherever the note
    // starts, and the refusal says how to fill it.
    const song::Track& empty = *TrackOf(song0, 1);
    SEQLIB_REQUIRE(empty.notes.empty() && !empty.stretches.empty());
    for (const uint32_t tick : {0U, 12U})
    {
        XSFLIB_CHECK(ErrorOf<std::invalid_argument>(
                         [&] { edit::NewKonamiNote(file, 0, empty, empty.stretches.front(), tick, 60, 100, 6); }) ==
                     edit::kKonamiNoRoom);
    }
}

// On the revision without a mixer, a track's command can play a note on the next track. The command then belongs to
// that track: to its volume and its instrument, not to the track that has it. The bytes below are for that revision: a
// wait, a note of sample 3 on the track itself, a wait, a note of sample 5 on the next track, a wait and an end.
void TestNotesOnTheNextTrack()
{
    using ntdlib::commands::Command;
    using ntdlib::commands::Kind;
    const song::Console console{.engine = hokuto::format::Engine::kKonami,
                                .driver = &song::KonamiRules(konami::Revision::kDungeonDiceMonsters)};
    const auto note = [](uint16_t sample, uint8_t index)
    {
        Command command;
        command.kind = Kind::kNote;
        command.index = index;
        command.velocity = 8 * ntdlib::commands::kOne;
        command.sample = sample;
        return command;
    };
    const auto wait = []
    {
        Command command;
        command.kind = Kind::kWait;
        return command;
    };
    Command end;
    end.kind = Kind::kEnd;
    ntdlib::commands::Sequence commands;
    commands.items = {wait(), note(3, 0), wait(), note(5, 1), wait(), end};
    const std::vector<uint32_t> offsets = edit::Offsets(commands, console);
    const std::array<song::Stretch, 1> stretches = {song::Stretch{.first = offsets[0], .last = offsets[5]}};

    const auto setters =
        [&](edit::Program program, std::span<const song::Stretch> ran, std::span<const song::Stretch> previous)
    {
        return edit::InstrumentSetters(commands, offsets, ran, program, console, previous).items;
    };
    XSFLIB_CHECK(setters({0, 3}, stretches, {}) == (std::set<uint32_t>{1}));
    XSFLIB_CHECK(setters({0, 5}, stretches, {}).empty());
    XSFLIB_CHECK(setters({0, 5}, {}, stretches) == (std::set<uint32_t>{3}));
    XSFLIB_CHECK(edit::MixSetters(commands, offsets, stretches, song::Mix::kVolume, false, true).items ==
                 (std::set<uint32_t>{1}));
    XSFLIB_CHECK(edit::MixSetters(commands, offsets, {}, song::Mix::kVolume, false, true, stretches).items ==
                 (std::set<uint32_t>{3}));

    // A note of this driver gives its track's rows no tick. Another command can start it, and the waits give the tick
    // of each command.
    song::Note later_note;
    later_note.command = offsets[1];
    later_note.start = 100;
    later_note.start_gives_tick = false;
    const std::array<song::Note, 1> notes = {later_note};
    XSFLIB_CHECK(edit::CommandRows(commands, offsets, stretches, notes, std::nullopt).at(1).tick == 0u);
}

// A setting at the start of a block runs after a note that the commands before the block run in that tick. A volume
// there can take the place of the note's start. The setting therefore stops a tick later.
void TestSettingAfterNote()
{
    using ntdlib::commands::Kind;
    const File file = format::ImportKonamiRom(LaterCart());
    const Sequence song0 = ReadOf(file, 0);
    const song::Track& square = *TrackOf(song0, 0);
    XSFLIB_CHECK(std::ranges::none_of(square.notes, &song::Note::start_gives_tick));

    // The square wave's note runs at tick 0, and a wait of 12 ticks follows it.
    edit::NewSetting setting{.stretch = square.stretches.at(0), .tick = 6};
    setting.command.kind = Kind::kVolume;
    setting.command.value.value = 3 * ntdlib::commands::kOne;
    const File with_volume = edit::AddSettings(file, 0, std::span(&setting, 1)).file;
    const std::vector<uint32_t> offsets = edit::Offsets(
        *with_volume.sequences[0], song::Console{.engine = hokuto::format::Engine::kKonami,
                                                 .driver = &song::KonamiRules(konami::Revision::kWct2004)});
    const auto& items = with_volume.sequences[0]->items;
    uint32_t wait = 0;
    uint32_t volume = 0;
    for (uint32_t i = 0; i < items.size(); i++)
    {
        if (items[i].kind == Kind::kVolume && items[i].value.value == 3 * ntdlib::commands::kOne)
        {
            volume = i;
            wait = i - 1;
            break;
        }
    }
    XSFLIB_CHECK(volume != 0);
    XSFLIB_CHECK(items[wait].kind == Kind::kWait);

    const song::Stretch stretch = TrackOf(ReadOf(with_volume, 0), 0)->stretches.at(0);
    XSFLIB_CHECK(edit::ShiftRangeOf(with_volume, 0, offsets[volume], stretch)->earlier == 6);

    const File labelled = edit::AddLabel(with_volume, 0, offsets[wait], "after_the_note").file;
    XSFLIB_CHECK(edit::ShiftRangeOf(labelled, 0, offsets[volume], stretch)->earlier == 5);
}

// A song has a field for each track, and each track plays one channel. A new track therefore has no place. A copy
// goes to the first track of the same kind of channel that runs no note. A removal leaves the track without notes, and
// a track that loops the song stays.
void TestTracks()
{
    const File file = format::ImportKonamiRom(FirstCart());
    const std::vector<std::optional<edit::SoundPlace>> places = PlacesOf(file);
    const Sequence song0 = ReadOf(file, 0);
    SEQLIB_REQUIRE(places[0] && places[0]->track_fields.size() == 16);

    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::AddTrack(file, places, 0, song0, std::nullopt); })
                     .find("Square 2") != std::string::npos);

    const edit::TrackEdit copy = edit::DuplicateTrack(file, places, 0, 4);
    XSFLIB_CHECK(copy.track == 5);
    const Sequence with_copy = ReadOf(copy.file, 0);
    XSFLIB_CHECK(StartsOf(with_copy, 5) == StartsOf(song0, 4));
    XSFLIB_CHECK(StartsOf(with_copy, 4) == StartsOf(song0, 4));
    XSFLIB_CHECK(StartsOf(with_copy, 0) == StartsOf(song0, 0));

    const edit::TrackEdit square_copy = edit::DuplicateTrack(file, places, 0, 0);
    XSFLIB_CHECK(square_copy.track == 1);
    XSFLIB_CHECK(StartsOf(ReadOf(square_copy.file, 0), 1) == StartsOf(song0, 0));

    const File without = edit::RemoveTrack(file, places, 0, 0);
    const Sequence without_square = ReadOf(without, 0);
    XSFLIB_CHECK(StartsOf(without_square, 0).empty());
    XSFLIB_CHECK(StartsOf(without_square, 4) == StartsOf(song0, 4));
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::RemoveTrack(file, places, 0, 4); }) != "");

    // A track of the other song is placed on the first track that plays samples and runs no note, at its tick. The copy
    // has no loop point: the driver starts it again from its start each time the song loops.
    const Sequence song1 = ReadOf(file, 1);
    const edit::CopiedTrack copied = edit::CopyTrack(file, places, 1, song1, 5);
    XSFLIB_CHECK(copied.driver == format::Engine::kKonami);
    const edit::TrackEdit pasted = edit::PasteTrack(file, places, 0, song0, copied);
    XSFLIB_CHECK(pasted.track == 5);
    const Sequence with_paste = ReadOf(pasted.file, 0);
    XSFLIB_CHECK(StartsAt(with_paste, 5, {0, 36}));
    SEQLIB_REQUIRE(TrackOf(with_paste, 5) && TrackOf(with_paste, 5)->notes.size() > 0);
    XSFLIB_CHECK(TrackOf(with_paste, 5)->notes[0].length == 8);
    XSFLIB_CHECK(StartsAt(with_paste, 4, {0, 12}));
    XSFLIB_CHECK(LoopsOver(with_paste, 12, 36));
    XSFLIB_CHECK(StartsOf(ReadOf(pasted.file, 1), 5) == (std::vector<uint32_t>{0}));
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("konami_edits",
                                    []
                                    {
                                        TestNoteVelocityAndTimes();
                                        TestNoteTransposeAndRemove();
                                        TestSquareNoteVelocity();
                                        TestNewNote();
                                        TestMoveAcrossNote();
                                        TestCall();
                                        TestCallAndMovedNote();
                                        TestNoteStartedAgain();
                                        TestNoteStartedByVolume();
                                        TestNoteStartedByVolumeInOneTick();
                                        TestNoteStartedByVolumeAfterRest();
                                        TestNotesOfOneSilentCommand();
                                        TestNoteStartedByVolumeAfterLoopStart();
                                        TestVolumeStartsKeepTheirKeys();
                                        TestVolumeStartsAcrossBlocks();
                                        TestLoopCutsTrack();
                                        TestLoopCutEdges();
                                        TestLaterLoopCutsTrack();
                                        TestPsgVolumeRange();
                                        TestWalkAgreesWithReader();
                                        TestMovedVolumeKeepsItsKey();
                                        TestLoopWithoutWait();
                                        TestMix();
                                        TestEmptyTracks();
                                        TestNotesOnTheNextTrack();
                                        TestSettingAfterNote();
                                        TestTracks();
                                    });
}
