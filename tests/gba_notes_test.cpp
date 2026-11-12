// SPDX-License-Identifier: MIT

// Tests the note edits of the GBA's MP2K songs (src/edit/notes.cpp). The song of a GBA cartridge accepts the edits by
// the rules of MusicPlayer2000's bytes. A note that the driver never starts leads to the stretch that ran its command
// (UnstartedPlaces in edit/commands.h), and accepts edits there. A note whose new length runs past the end of its track
// moves the end, unless that could change other notes.

#include "cart_bytes.h"
#include "notes_fixtures.h"

namespace
{

// A cartridge whose song 0 plays two tracks. Track 0 plays key 60 at tick 0, again at tick 24 with a command that
// leaves out its key and its velocity, and key 67 at tick 48, and ends at tick 96. Track 1 ties key 48 from tick 0 to
// tick 96. The bytes from 0x38000 on are 0xFF: the ROM's free space. With `fusion`, the cartridge contains Metroid
// Fusion's sequencer. The notes of that sequencer read a key and a velocity and no third byte.
std::vector<uint8_t> GbaRom(bool fusion = false)
{
    namespace mp2k_cart = ntdlib::testing::mp2k_cart;
    using enum ntdlib::mp2k::Command;

    mp2k_cart::Cart cart;
    if (fusion)
    {
        cart.WriteMetroid(16, true);
    }
    const uint32_t voices = cart.VoiceGroup({{0, mp2k_cart::Cart::Voice(0, cart.Wave(mp2k_cart::Saw(), 13379, 0))}});
    mp2k_cart::Track melody;
    melody.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Note(24, 60, 100).Wait(24).Note(24).Wait(24).Note(48, 67, 100);
    melody.Wait(48).Fine();
    mp2k_cart::Track tie;
    tie.Cmd(kCmdVoice, 0).Tie(48, 80).Wait(96).Eot(48).Fine();
    cart.Song({melody, tie}, voices);
    for (uint32_t a = 0x38000; a < 0x40000; a++)
    {
        cart.Put8(mp2k_cart::kRomBase + a, 0xFF);
    }

    const ntdlib::agb::Rom rom = cart.ToRom();
    return hokuto::testing::RomBytes(rom);
}

// The sequence of the file of a GBA ROM that contains song 0, and the song as ntdlib's performer plays it from the ROM.
struct GbaSong
{
    uint32_t sequence = 0;
    Sequence played;
};

// Song 0 of the ROM that `file` builds.
GbaSong GbaSongOf(const File& file)
{
    const std::shared_ptr<const hokuto::song::GbaRom> rom =
        hokuto::song::LoadGbaRom(hokuto::format::ExportArchive(file));
    return {hokuto::format::GbaSongSequence(file, rom->info.song_table, 0).value(),
            hokuto::song::ReadSequence(*rom, 0)};
}

// A new velocity changes the note's byte alone. The second note leaves out its velocity, and the driver would take the
// new one for it. Its command is then written in full, and it keeps its velocity. The ROM keeps its size.
void TestGbaKeepsState()
{
    const std::vector<uint8_t> rom = GbaRom();
    const File file = hokuto::format::ImportGbaRom(rom);
    const GbaSong song = GbaSongOf(file);
    XSFLIB_CHECK(hokuto::format::ExportArchive(file) == rom);
    XSFLIB_CHECK((GbaNotes(song.played, 0) ==
                  std::vector<std::vector<uint32_t>>{{0, 24, 60, 100}, {24, 24, 60, 100}, {48, 48, 67, 100}}));

    NoteEdit softer = EditOf(song.played, 0);
    softer.velocity = 90;
    const Edited edited = EditNotes(file, song.sequence, std::vector<NoteEdit>{softer});

    const std::vector<uint8_t> exported = hokuto::format::ExportArchive(edited.file);
    XSFLIB_CHECK(exported.size() == rom.size());
    XSFLIB_CHECK((GbaNotes(GbaSongOf(edited.file).played, 0) ==
                  std::vector<std::vector<uint32_t>>{{0, 24, 60, 90}, {24, 24, 60, 100}, {48, 48, 67, 100}}));
    const File again = hokuto::format::ImportGbaRom(exported);
    XSFLIB_CHECK(hokuto::format::ExportArchive(again) == exported);
}

// A longer note uses the code of 24 ticks and an extra tick in its third argument. A move of key 67 a tick earlier
// leaves a wait of 49 ticks after it. That wait splits into waits of 48 ticks and 1 tick. A removed note and a new note
// in the middle of a wait go in the same way. The other track plays as before.
void TestGbaNotes()
{
    const File file = hokuto::format::ImportGbaRom(GbaRom());
    const GbaSong song = GbaSongOf(file);
    NoteEdit longer = EditOf(song.played, 0);
    longer.length = 25;
    longer.velocity = 90;
    NoteEdit removed = EditOf(song.played, 1);
    removed.remove = true;
    NoteEdit earlier = EditOf(song.played, 2);
    earlier.shift = -1;
    const std::vector<NewNote> added = {
        {.stretch = earlier.stretch, .tick = 12, .key = 72, .velocity = 100, .length = 12}};

    const Edited edited = EditNotes(file, song.sequence, std::vector<NoteEdit>{longer, removed, earlier}, added);

    const GbaSong played = GbaSongOf(edited.file);
    XSFLIB_CHECK((GbaNotes(played.played, 0) ==
                  std::vector<std::vector<uint32_t>>{{0, 25, 60, 90}, {12, 12, 72, 100}, {47, 48, 67, 100}}));
    XSFLIB_CHECK(GbaNotes(played.played, 1) == GbaNotes(song.played, 1));
    const std::vector<uint8_t> exported = hokuto::format::ExportArchive(edited.file);
    XSFLIB_CHECK(hokuto::format::ExportArchive(hokuto::format::ImportGbaRom(exported)) == exported);

    // Each wait has the length of a code.
    const auto code_length = [](int64_t ticks)
    {
        for (int code = 0; code < 49; code++)
        {
            if (ntdlib::mp2k::ClockLength(code) == ticks)
            {
                return true;
            }
        }
        return false;
    };
    for (const ntdlib::commands::Command& command : edited.file.sequences[song.sequence]->items)
    {
        XSFLIB_CHECK(command.kind != Kind::kWait || code_length(command.value.value / ntdlib::commands::kOne));
    }
}

// A tie's velocity changes, but it can't move, change its length, transpose or go. Its limits give the reason, and so
// does the refusal of a move. A note can't last longer than 223 ticks without a tie.
void TestGbaRefusals()
{
    const File file = hokuto::format::ImportGbaRom(GbaRom());
    const GbaSong song = GbaSongOf(file);
    SEQLIB_REQUIRE(song.played.tracks.size() == 2 && song.played.tracks[1].notes.size() == 1);
    const Note& tied = song.played.tracks[1].notes[0];
    const NoteEdit tie{.command = tied.command, .stretch = song.played.tracks[1].stretches.at(tied.stretch)};
    const std::string kTieRefusal =
        "This note is tied and lasts until a later command ends it. Hokuto can't move, resize, transpose or remove "
        "tied notes yet. You can change the velocity.";
    const std::vector<NoteLimits> limits =
        LimitsOf(file, song.sequence, std::vector<NotePlace>{{.command = tie.command, .stretch = tie.stretch}});
    XSFLIB_CHECK(limits.size() == 1 && !limits[0].shift && !limits[0].length && limits[0].fixed == kTieRefusal);

    NoteEdit moved = tie;
    moved.shift = 1;
    NoteEdit longer = tie;
    longer.length = 48;
    NoteEdit higher = tie;
    higher.transpose = 1;
    NoteEdit removed = tie;
    removed.remove = true;
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>(
                     [&] { EditNotes(file, song.sequence, std::vector<NoteEdit>{moved}); }) == kTieRefusal);
    XSFLIB_CHECK(Refuses(file, song.sequence, moved) && Refuses(file, song.sequence, longer));
    XSFLIB_CHECK(Refuses(file, song.sequence, higher) && Refuses(file, song.sequence, removed));
    NoteEdit softer = tie;
    softer.velocity = 70;
    const Edited edited = EditNotes(file, song.sequence, std::vector<NoteEdit>{softer});
    XSFLIB_CHECK((GbaNotes(GbaSongOf(edited.file).played, 1) == std::vector<std::vector<uint32_t>>{{0, 96, 48, 70}}));

    NoteEdit too_long = EditOf(song.played, 2);
    too_long.length = 224;
    NoteEdit longest = EditOf(song.played, 2);
    longest.length = 223;
    XSFLIB_CHECK(Refuses(file, song.sequence, too_long) && !Refuses(file, song.sequence, longest));
}

// A cartridge whose song 0 plays one track: key 60 at tick 0 with the code of 24 ticks, its velocity and a byte of 50,
// then a wait of 24 ticks and the end. The SDK's notes read the byte as 50 ticks more. With `fusion`, the cartridge
// contains Metroid Fusion's sequencer. The sequencer reads the byte as the key of a second note of the same code and
// velocity. The bytes from 0x38000 on are 0xFF: the ROM's free space.
std::vector<uint8_t> GbaThirdByteRom(bool fusion)
{
    namespace mp2k_cart = ntdlib::testing::mp2k_cart;
    using enum ntdlib::mp2k::Command;

    mp2k_cart::Cart cart;
    if (fusion)
    {
        cart.WriteMetroid(16, true);
    }
    const uint32_t voices = cart.VoiceGroup({{0, mp2k_cart::Cart::Voice(0, cart.Wave(mp2k_cart::Saw(), 13379, 0))}});
    cart.Song({mp2k_cart::Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Note(24, 60, 100, 50).Wait(24).Fine()}, voices);
    for (uint32_t a = 0x38000; a < 0x40000; a++)
    {
        cart.Put8(mp2k_cart::kRomBase + a, 0xFF);
    }

    const ntdlib::agb::Rom rom = cart.ToRom();
    return hokuto::testing::RomBytes(rom);
}

// The first `size` bytes of track 0 of song 0 in the ROM that `file` builds, or none where the song can't be read.
std::vector<uint8_t> GbaTrackBytes(const File& file, std::size_t size)
{
    const std::shared_ptr<const hokuto::song::GbaRom> rom =
        hokuto::song::LoadGbaRom(hokuto::format::ExportArchive(file));
    ntdlib::mp2k::SongHeader header;
    if (!ntdlib::mp2k::ReadSong(rom->rom, rom->info, 0, header) || header.tracks.empty())
    {
        return {};
    }

    const uint8_t* start = rom->rom.Ptr(header.tracks[0]);
    return std::vector<uint8_t>(start, start + size);
}

// A cartridge whose song 0 plays `track` with a saw wave's voice. The bytes from 0x38000 on are 0xFF: the ROM's free
// space.
std::vector<uint8_t> GbaTrackRom(const ntdlib::testing::mp2k_cart::Track& track)
{
    namespace mp2k_cart = ntdlib::testing::mp2k_cart;

    mp2k_cart::Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, mp2k_cart::Cart::Voice(0, cart.Wave(mp2k_cart::Saw(), 13379, 0))}});
    cart.Song({track}, voices);
    for (uint32_t a = 0x38000; a < 0x40000; a++)
    {
        cart.Put8(mp2k_cart::kRomBase + a, 0xFF);
    }

    const ntdlib::agb::Rom rom = cart.ToRom();
    return hokuto::testing::RomBytes(rom);
}

// The edit of the note of track 0 of `sequence` that plays `key`.
NoteEdit EditOfKey(const Sequence& sequence, int key)
{
    for (const Note& note : sequence.tracks.at(0).notes)
    {
        if (note.key == key)
        {
            return NoteEdit{.command = note.command, .stretch = sequence.tracks[0].stretches.at(note.stretch)};
        }
    }

    throw std::invalid_argument("no note of that key");
}

// The end of a GBA song's track cuts a note whose command lasts past it. Key 60 starts at tick 0 with the code of 24
// ticks. A wait of 12 ticks and the end follow, and the note sounds for 12 ticks. The note's limits say that the end
// cuts it. A new length of 36 ticks moves the end to the note's end, and so does the command's length of 24 ticks. The
// note then plays that long. The edit is refused where a move of the end could change another note:
//
// - Key 64 starts at tick 0 with the same code. The end cuts it too. A new length of both notes moves the end.
// - A tie of key 48 starts at tick 0, and no end of the tie comes before the track's end. A tie of key 48 that ends at
//   tick 6 leaves the end free to move.
// - The track plays a pattern before key 60. A note of the pattern could sound up to the end.
//
// A note that ends before the end of its track isn't cut.
void TestGbaNoteOverTheEnd()
{
    namespace mp2k_cart = ntdlib::testing::mp2k_cart;
    using enum ntdlib::mp2k::Command;
    const std::string kEndRefusal =
        "The track ends before this note does. Moving the end could change other notes too. To move it anyway, "
        "lengthen the wait before the End in the List Editor.";
    const auto opening = []
    {
        return mp2k_cart::Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0);
    };
    const auto played = [](const File& file)
    {
        return GbaNotes(GbaSongOf(file).played, 0);
    };
    const auto with_length = [](NoteEdit edit, uint32_t length)
    {
        edit.length = length;
        return edit;
    };

    const File file = hokuto::format::ImportGbaRom(GbaTrackRom(opening().Note(24, 60, 100).Wait(12).Fine()));
    const GbaSong song = GbaSongOf(file);
    XSFLIB_CHECK((GbaNotes(song.played, 0) == std::vector<std::vector<uint32_t>>{{0, 12, 60, 100}}));
    const NoteEdit note = EditOfKey(song.played, 60);
    const std::vector<NoteLimits> limits =
        LimitsOf(file, song.sequence, std::vector<NotePlace>{{.command = note.command, .stretch = note.stretch}});
    XSFLIB_CHECK(limits.size() == 1 && limits[0].cut_by_end && limits[0].length == 24u);
    for (const uint32_t length : {36u, 24u})
    {
        const Edited edited = EditNotes(file, song.sequence, std::vector<NoteEdit>{with_length(note, length)});
        XSFLIB_CHECK((played(edited.file) == std::vector<std::vector<uint32_t>>{{0, length, 60, 100}}));
        const std::vector<uint8_t> exported = hokuto::format::ExportArchive(edited.file);
        XSFLIB_CHECK(hokuto::format::ExportArchive(hokuto::format::ImportGbaRom(exported)) == exported);
    }

    const File chord =
        hokuto::format::ImportGbaRom(GbaTrackRom(opening().Note(24, 60, 100).Note(24, 64, 100).Wait(12).Fine()));
    const GbaSong both = GbaSongOf(chord);
    const NoteEdit low = with_length(EditOfKey(both.played, 60), 36);
    const NoteEdit high = with_length(EditOfKey(both.played, 64), 36);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { EditNotes(chord, both.sequence, std::vector<NoteEdit>{low}); }) ==
                 kEndRefusal);
    XSFLIB_CHECK((played(EditNotes(chord, both.sequence, std::vector<NoteEdit>{low, high}).file) ==
                  std::vector<std::vector<uint32_t>>{{0, 36, 60, 100}, {0, 36, 64, 100}}));

    const File held =
        hokuto::format::ImportGbaRom(GbaTrackRom(opening().Tie(48, 80).Note(24, 60, 100).Wait(12).Fine()));
    const GbaSong tied = GbaSongOf(held);
    const NoteEdit under_tie = with_length(EditOfKey(tied.played, 60), 36);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>(
                     [&] { EditNotes(held, tied.sequence, std::vector<NoteEdit>{under_tie}); }) == kEndRefusal);
    const File ended = hokuto::format::ImportGbaRom(
        GbaTrackRom(opening().Tie(48, 80).Wait(6).Eot(48).Note(24, 60, 100).Wait(12).Fine()));
    const GbaSong untied = GbaSongOf(ended);
    const NoteEdit after_tie = with_length(EditOfKey(untied.played, 60), 36);
    XSFLIB_CHECK((played(EditNotes(ended, untied.sequence, std::vector<NoteEdit>{after_tie}).file) ==
                  std::vector<std::vector<uint32_t>>{{0, 6, 48, 80}, {6, 36, 60, 100}}));

    // The pattern's place is known once the track before it is.
    const auto patterned = [&](std::size_t pattern)
    {
        mp2k_cart::Track built = opening();
        built.Patt(pattern).Note(24, 60, 100).Wait(12).Fine();
        const std::size_t here = built.Here();
        built.Wait(6).Raw({kCmdPend});
        return std::pair(built, here);
    };
    const File called = hokuto::format::ImportGbaRom(GbaTrackRom(patterned(patterned(0).second).first));
    const GbaSong after_call = GbaSongOf(called);
    XSFLIB_CHECK((GbaNotes(after_call.played, 0) == std::vector<std::vector<uint32_t>>{{6, 12, 60, 100}}));
    const NoteEdit after_pattern = with_length(EditOfKey(after_call.played, 60), 36);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>(
                     [&]
                     { EditNotes(called, after_call.sequence, std::vector<NoteEdit>{after_pattern}); }) == kEndRefusal);

    const File whole = hokuto::format::ImportGbaRom(GbaRom());
    const GbaSong uncut = GbaSongOf(whole);
    const NoteEdit first = EditOf(uncut.played, 0);
    XSFLIB_CHECK(
        !LimitsOf(whole, uncut.sequence, std::vector<NotePlace>{{.command = first.command, .stretch = first.stretch}})
             .at(0)
             .cut_by_end);
}

// A note in a pattern that the track plays just before its end. The pattern plays key 60 with the code of 24 ticks,
// then a wait of 12 ticks, and returns. The track then waits 0 or 6 ticks and ends, and the end cuts the note there.
// The note's stretch ends at the pattern's return, before the end. After a wait, a later stretch contains the end.
// Without one, the end runs in the song's last tick, and no stretch contains it. TrackEndOf returns the tick of the end
// either way. The note's limits, given that tick, say that the end cuts it. A new length past the end is refused: the
// notes of the stretches between could sound up to it. A new length of 6 ticks plays. Without the track's end, the edit
// can't tell, and the end cuts the longer note as before.
void TestGbaNoteOverALaterEnd()
{
    namespace mp2k_cart = ntdlib::testing::mp2k_cart;
    using enum ntdlib::mp2k::Command;

    for (const uint32_t rest : {0u, 6u})
    {
        // The pattern's place is known once the track before it is.
        const auto track = [rest](std::size_t pattern)
        {
            mp2k_cart::Track built;
            built.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Patt(pattern);
            if (rest > 0)
            {
                built.Wait(rest);
            }
            built.Fine();
            const std::size_t here = built.Here();
            built.Note(24, 60, 100).Wait(12).Raw({kCmdPend});
            return std::pair(built, here);
        };
        const File file = hokuto::format::ImportGbaRom(GbaTrackRom(track(track(0).second).first));
        const GbaSong song = GbaSongOf(file);
        SEQLIB_REQUIRE(!song.played.tracks.empty());
        const uint32_t ends = 12 + rest;
        XSFLIB_CHECK((GbaNotes(song.played, 0) == std::vector<std::vector<uint32_t>>{{0, ends, 60, 100}}));
        const std::vector<Stretch>& stretches = song.played.tracks[0].stretches;
        XSFLIB_CHECK(stretches.size() == (rest > 0 ? 3u : 2u));
        const std::optional<uint32_t> track_end = hokuto::edit::TrackEndOf(file, song.sequence, stretches);
        XSFLIB_CHECK(track_end == ends);

        NoteEdit note = EditOfKey(song.played, 60);
        XSFLIB_CHECK(note.stretch.entry == Entry::kCall);
        const std::vector<NoteLimits> limits = LimitsOf(
            file, song.sequence,
            std::vector<NotePlace>{{.command = note.command, .stretch = note.stretch},
                                   {.command = note.command, .stretch = note.stretch, .track_end = track_end}});
        XSFLIB_CHECK(limits.size() == 2 && !limits[0].cut_by_end && limits[1].cut_by_end);

        note.length = 36;
        NoteEdit bounded = note;
        bounded.track_end = track_end;
        XSFLIB_CHECK(
            ErrorOf<std::invalid_argument>([&] { EditNotes(file, song.sequence, std::vector<NoteEdit>{bounded}); }) ==
            "The track ends before this note does. Moving the end could change other notes too. To move it "
            "anyway, lengthen the wait before the End in the List Editor.");
        XSFLIB_CHECK((GbaNotes(GbaSongOf(EditNotes(file, song.sequence, std::vector<NoteEdit>{note}).file).played, 0) ==
                      std::vector<std::vector<uint32_t>>{{0, ends, 60, 100}}));
        bounded.length = 6;
        XSFLIB_CHECK((GbaNotes(GbaSongOf(EditNotes(file, song.sequence, std::vector<NoteEdit>{bounded}).file).played,
                               0) == std::vector<std::vector<uint32_t>>{{0, 6, 60, 100}}));
    }
}

// On Metroid Fusion's sequencer, a note reads no third byte: a byte below 0x80 after its velocity starts the next
// command (the read of a note's arguments that ntdlib's WriteMetroid writes). The file of such a cart reads the two
// notes that the sequencer plays. A new velocity of the first note writes it with its key and its velocity alone. The
// second note is then written in full, to keep its velocity. On a plain cart, the same bytes play one note with an
// extra length, and the note with the new velocity keeps its third byte. On the sequencer, the 48-tick note of key 67
// can be set to 44 ticks, the length of a code, and the song plays no other note. A note of 47 ticks would require 3
// more ticks in a third byte. The sequencer would read that byte as a note of key 3. Hokuto therefore refuses a length
// of 47 ticks there. A plain cart accepts it with the extra length.
void TestGbaFusionNotes()
{
    using enum ntdlib::mp2k::Command;
    const File fusion = hokuto::format::ImportGbaRom(GbaThirdByteRom(true));
    const File plain = hokuto::format::ImportGbaRom(GbaThirdByteRom(false));
    const GbaSong two = GbaSongOf(fusion);
    const GbaSong one = GbaSongOf(plain);
    NoteEdit softer = EditOf(two.played, 0);
    softer.velocity = 90;
    NoteEdit plain_softer = EditOf(one.played, 0);
    plain_softer.velocity = 90;
    const File melody = hokuto::format::ImportGbaRom(GbaRom(true));
    const File plain_melody = hokuto::format::ImportGbaRom(GbaRom());
    const GbaSong song = GbaSongOf(melody);
    const GbaSong plain_song = GbaSongOf(plain_melody);
    NoteEdit shorter = EditOf(song.played, 2);
    shorter.length = 47;
    NoteEdit code = shorter;
    code.length = 44;
    NoteEdit plain_shorter = EditOf(plain_song.played, 2);
    plain_shorter.length = 47;

    const Edited edited = EditNotes(fusion, two.sequence, std::vector<NoteEdit>{softer});
    const Edited plain_edited = EditNotes(plain, one.sequence, std::vector<NoteEdit>{plain_softer});
    const std::string refusal =
        ErrorOf<std::invalid_argument>([&] { EditNotes(melody, song.sequence, std::vector<NoteEdit>{shorter}); });
    const Edited coded = EditNotes(melody, song.sequence, std::vector<NoteEdit>{code});
    const Edited extra = EditNotes(plain_melody, plain_song.sequence, std::vector<NoteEdit>{plain_shorter});

    const auto n24 = static_cast<uint8_t>(kCmdTie + 24);
    const auto w24 = static_cast<uint8_t>(kCmdWait + 24);
    XSFLIB_CHECK(fusion.native->gba_note_arguments == 2 && plain.native->gba_note_arguments == 3);
    XSFLIB_CHECK((GbaNotes(two.played, 0) == std::vector<std::vector<uint32_t>>{{0, 24, 60, 100}, {0, 24, 50, 100}}));
    XSFLIB_CHECK((GbaNotes(GbaSongOf(edited.file).played, 0) ==
                  std::vector<std::vector<uint32_t>>{{0, 24, 60, 90}, {0, 24, 50, 100}}));
    XSFLIB_CHECK((GbaTrackBytes(edited.file, 12) ==
                  std::vector<uint8_t>{kCmdTempo, 75, kCmdVoice, 0, n24, 60, 90, n24, 50, 100, w24, kCmdFine}));
    XSFLIB_CHECK((GbaTrackBytes(plain_edited.file, 10) ==
                  std::vector<uint8_t>{kCmdTempo, 75, kCmdVoice, 0, n24, 60, 90, 50, w24, kCmdFine}));
    XSFLIB_CHECK(refusal.find("Notes in this game can last 1 to 24 ticks") == 0);
    XSFLIB_CHECK((GbaNotes(GbaSongOf(coded.file).played, 0) ==
                  std::vector<std::vector<uint32_t>>{{0, 24, 60, 100}, {24, 24, 60, 100}, {48, 44, 67, 100}}));
    XSFLIB_CHECK((GbaNotes(GbaSongOf(extra.file).played, 0) ==
                  std::vector<std::vector<uint32_t>>{{0, 24, 60, 100}, {24, 24, 60, 100}, {48, 47, 67, 100}}));
}

// A cartridge whose song 0 plays two tracks on the square wave of PSG channel 2. Track 0's note at tick 0 keeps the
// channel until its next note at tick 24. Track 1's note at tick 12 meets the channel that an earlier track uses at the
// same priority, and never starts. The bytes from 0x38000 on are 0xFF: the ROM's free space.
std::vector<uint8_t> GbaUnstartedRom()
{
    namespace mp2k_cart = ntdlib::testing::mp2k_cart;
    using enum ntdlib::mp2k::Command;

    mp2k_cart::Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, mp2k_cart::Cart::Voice(2, 2, 60, {0, 0, 15, 0})}});
    mp2k_cart::Track first;
    first.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Note(48, 60, 100).Wait(24);
    first.Note(24, 62, 100).Wait(24).Fine();
    mp2k_cart::Track second;
    second.Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Wait(12).Note(12, 64, 90).Wait(36).Fine();
    cart.Song({first, second}, voices);
    for (uint32_t a = 0x38000; a < 0x40000; a++)
    {
        cart.Put8(mp2k_cart::kRomBase + a, 0xFF);
    }

    const ntdlib::agb::Rom rom = cart.ToRom();
    return hokuto::testing::RomBytes(rom);
}

// On the GBA, track 1's note that never starts leads to its track's one stretch. A move of 6 ticks and a new velocity
// through its place reach the note. At tick 18, track 0 still keeps the channel, and the note still never starts.
void TestGbaUnstartedPlaces()
{
    const File file = hokuto::format::ImportGbaRom(GbaUnstartedRom());
    const GbaSong song = GbaSongOf(file);
    SEQLIB_REQUIRE(song.played.tracks.size() == 2);
    const hokuto::song::Track& track = song.played.tracks[1];
    SEQLIB_REQUIRE(track.notes.empty() && track.unstarted.size() == 1 && track.stretches.size() == 1);
    XSFLIB_CHECK(track.unstarted[0].start == 12 && track.unstarted[0].key == 64 && track.unstarted[0].velocity == 90);

    const std::vector<std::optional<NotePlace>> places = UnstartedPlaces(file, song.sequence, track);

    SEQLIB_REQUIRE(places.size() == 1 && places[0].has_value());
    XSFLIB_CHECK(places[0]->command == track.unstarted[0].command && places[0]->stretch == track.stretches[0]);

    const NoteEdit edit{.command = places[0]->command, .stretch = places[0]->stretch, .shift = 6, .velocity = 70};
    const GbaSong after = GbaSongOf(EditNotes(file, song.sequence, std::vector{edit}).file);

    SEQLIB_REQUIRE(after.played.tracks.size() == 2 && after.played.tracks[1].unstarted.size() == 1);
    const Note& moved = after.played.tracks[1].unstarted[0];
    XSFLIB_CHECK(moved.start == 18 && moved.length == 12 && moved.key == 64 && moved.velocity == 70);
    XSFLIB_CHECK(after.played.tracks[1].notes.empty() && GbaNotes(after.played, 0) == GbaNotes(song.played, 0));
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("gba_notes",
                                    []
                                    {
                                        TestGbaKeepsState();
                                        TestGbaNotes();
                                        TestGbaRefusals();
                                        TestGbaNoteOverTheEnd();
                                        TestGbaNoteOverALaterEnd();
                                        TestGbaFusionNotes();
                                        TestGbaUnstartedPlaces();
                                    });
}
