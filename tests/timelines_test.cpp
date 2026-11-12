// SPDX-License-Identifier: MIT

// Tests the timelines of NintendoWare's MIDI converter (src/edit/timelines.cpp): moves, loops, ends and tracks.

#include "converter_fixtures.h"
#include "sound_places.h"

namespace
{

using hokuto::testing::PlacesOf;
using hokuto::testing::PlayedSequence;
using xsflib::testing::ErrorOf;

// A file that doesn't say makes the converted song a timeline without subroutines, and the other sequence a program. A
// save holds the song with its subroutines. A timeline whose hints don't give the song back opens as a program. The
// switch between the modes gives each form back, and a sequence that isn't a converted song is refused as a timeline.
// An edit of a timeline settles, and one of a program stays as the edit left it. A moved loop keeps each note's place
// among the notes, and stays where it moved after the settle.
void TestTimelines()
{
    // A file that doesn't say makes the converted song a timeline, and the other sequence a program.
    File file;
    file.sequences = hokuto::format::ShareSequences({MainSong(), Sequence{}});
    const ConverterHints kHints{{{192, 1}}, {}};
    const Sequence unfolded = LayOutConverted(MainSong(), kHints, false);
    const File opened = hokuto::edit::OpenTimelines(file);
    XSFLIB_CHECK(opened.timelines && opened.timelines->size() == 1 && opened.timelines->at(0) == kHints);
    XSFLIB_CHECK(*opened.sequences[0] == unfolded && opened.sequences[1]->items.empty());
    XSFLIB_CHECK(hokuto::edit::IsTimeline(opened, 0) && !hokuto::edit::IsTimeline(opened, 1));

    // A save holds the song with its subroutines. A timeline whose hints don't give the song back is a program.
    const File folded = hokuto::edit::FoldTimelines(opened);
    XSFLIB_CHECK(*folded.sequences[0] == MainSong() && folded.timelines == opened.timelines);
    File other = file;
    other.timelines.emplace();
    (*other.timelines)[0] = {{{144, 1}}, {}};
    const File program = hokuto::edit::OpenTimelines(other);
    XSFLIB_CHECK(program.timelines && program.timelines->empty() && *program.sequences[0] == MainSong());

    // The switch between the modes.
    const File as_program = hokuto::edit::AsProgram(opened, 0);
    XSFLIB_CHECK(*as_program.sequences[0] == MainSong() && !hokuto::edit::IsTimeline(as_program, 0));
    XSFLIB_CHECK(hokuto::edit::AsTimeline(as_program, 0) == opened);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::AsTimeline(opened, 1); })
                     .starts_with("This sequence can be edited only as a program."));

    // An edit of the timeline settles. One of a program stays as the edit left it.
    File edited = opened;
    edited.sequences[0] = Without(unfolded, 7);
    const Settled settled = hokuto::edit::SettleTimelines(opened, edited);
    XSFLIB_CHECK(settled.file.sequences[0]->items.size() == 66 && settled.items.at(0).at(7) == 6u);
    File programs = as_program;
    programs.sequences[0] = Without(MainSong(), 7);
    XSFLIB_CHECK(hokuto::edit::SettleTimelines(as_program, programs).items.empty());

    // The loop moves, and each note keeps its place among the notes.
    const Edited moved = hokuto::edit::MoveLoop(opened, 0, {192, 576});
    XSFLIB_CHECK((hokuto::edit::TimelineLoop(moved.file, 0) == ConvertedLoop{192, 576}));
    XSFLIB_CHECK(moved.items[5] == 5u && moved.items[35] == 36u && !moved.items[43]);

    // The program settles the moved loop as it settles every edit. The loop stays where it moved.
    for (const ConvertedLoop loop : {ConvertedLoop{192, 576}, ConvertedLoop{384, 960}, ConvertedLoop{432, 768}})
    {
        const Edited moved_loop = hokuto::edit::MoveLoop(opened, 0, loop);
        const Settled settled_loop = hokuto::edit::SettleTimelines(opened, moved_loop.file);
        XSFLIB_CHECK(hokuto::edit::TimelineLoop(settled_loop.file, 0) == loop);
        XSFLIB_CHECK(*settled_loop.file.sequences[0] == *moved_loop.file.sequences[0]);
    }
}

// An edit of the main song's timeline that sets the rest after track 0's first note to 0 ticks, and makes the next rest
// 96 ticks long. The settled layout leaves out the rest of 0 ticks, and the rest leads to the note after it.
void TestZeroWait()
{
    // The rest at item 6 lasts 0 ticks. The notes after it keep their ticks.
    File file;
    file.sequences = hokuto::format::ShareSequences({MainSong(), Sequence{}});
    const File opened = hokuto::edit::OpenTimelines(file);
    Sequence zero = *opened.sequences[0];
    zero.items[6] = Of(Kind::kWait, 0);
    zero.items[8] = Of(Kind::kWait, 96);
    File edited = opened;
    edited.sequences[0] = zero;

    std::optional<Settled> settled;
    const std::string refusal =
        ErrorOf<std::invalid_argument>([&] { settled = hokuto::edit::SettleTimelines(opened, edited); });

    XSFLIB_CHECK(refusal.empty() && settled && settled->file.sequences[0]->items.size() == 67);
    if (settled)
    {
        const std::vector<std::optional<uint32_t>>& items = settled->items.at(0);
        XSFLIB_CHECK(items.at(5) == 5u && items.at(6) == 6u && items.at(7) == 6u && items.at(8) == 7u);
        XSFLIB_CHECK(settled->file.sequences[0]->items[6].key == 62 && WaitAt(*settled->file.sequences[0], 7) == 96);
    }
}

// A sequence file of the Switch that holds `song`.
std::vector<uint8_t> SongFile(const Sequence& song)
{
    const ntdlib::nwsnd::Encoded encoded =
        ntdlib::nwsnd::EncodeSequence(song, hokuto::song::GrammarOf(ntdlib::sar::Flavor::kNx));
    std::vector<std::pair<std::string, uint32_t>> labels;
    for (const Label& label : song.labels)
    {
        labels.emplace_back(label.name, encoded.offsets.at(label.item));
    }

    return sar_files::Sequence(false, encoded.bytes, labels);
}

// The commands of `sequence` with its labels in the order of their names.
Sequence Sorted(Sequence sequence)
{
    std::stable_sort(sequence.labels.begin(), sequence.labels.end(),
                     [](const Label& a, const Label& b) { return a.name < b.name; });

    return sequence;
}

// A replacement of a sound's sequence in a timeline gives the program's commands and places. A saved file holds the
// same. SEQ_MAIN and SEQ_TWIN play the main song, from its start and from track 1's label. SEQ_ALONE plays a copy of it
// alone.
void TestReplaced()
{
    constexpr uint32_t kTwin = 1;
    constexpr uint32_t kAlone = 2;
    const Sequence song = MainSong();
    const ntdlib::nwsnd::Encoded encoded =
        ntdlib::nwsnd::EncodeSequence(song, hokuto::song::GrammarOf(ntdlib::sar::Flavor::kNx));
    const auto track_1 = std::find_if(song.labels.begin(), song.labels.end(),
                                      [](const Label& label) { return label.name == "SMF_T_Track_1"; });
    SEQLIB_REQUIRE(track_1 != song.labels.end());
    sar_files::SoundSpec main;
    main.name = "SEQ_MAIN";
    main.start_offset = 0;
    sar_files::SoundSpec twin = main;
    twin.name = "SEQ_TWIN";
    twin.start_offset = encoded.offsets.at(track_1->item);
    sar_files::SoundSpec alone = main;
    alone.name = "SEQ_ALONE";
    alone.file = 1;
    sar_files::ArchiveSpec spec;
    spec.sounds = {main, twin, alone};
    spec.players = {{"PLAYER_TEST", 1}};
    spec.files = {{SongFile(song), "", false}, {SongFile(song), "", false}};

    // The program opens both copies of the song as timelines.
    const File file = hokuto::format::ImportSwitchArchive(sar_files::Archive(false, spec));
    const File opened = hokuto::edit::OpenTimelines(file);
    XSFLIB_CHECK(hokuto::edit::IsTimeline(opened, 0) && hokuto::edit::IsTimeline(opened, 1));
    const std::vector<std::optional<SoundPlace>> places = PlacesOf(opened);
    const std::vector<std::optional<SoundPlace>> program_places = PlacesOf(file);

    // The song's file offers the sounds' starts at its items.
    const Sequence same = hokuto::edit::ReadIncoming(SongFile(song)).sequence;
    const Sequence other = hokuto::edit::ReadIncoming(SongFile(MainSong(true))).sequence;
    const std::vector<hokuto::edit::StartingSound> starts = hokuto::edit::SoundStarts(opened, places, same);
    SEQLIB_REQUIRE(starts.size() == 3);
    XSFLIB_CHECK(starts == hokuto::edit::SoundStarts(file, program_places, same));
    XSFLIB_CHECK(starts[1].item == track_1->item);

    // The file's commands from a sound's start change nothing.
    const auto replaced = [](const File& from, const std::vector<std::optional<SoundPlace>>& at, uint32_t sound,
                             const Sequence& incoming, uint32_t start)
    {
        return hokuto::edit::ReplaceSequence(from, at, sound, incoming, start, {}, "Other", 0);
    };
    for (const hokuto::edit::StartingSound& start : starts)
    {
        const File held = replaced(opened, places, start.sound, same, start.item);
        XSFLIB_CHECK(held.sequences == opened.sequences && held.timelines == opened.timelines);
        XSFLIB_CHECK(held.native->fields == opened.native->fields);
    }

    // Other commands replace SEQ_ALONE's whole. The sequence goes in as a program, and becomes a timeline again.
    const File whole = replaced(opened, places, kAlone, other, 0);
    XSFLIB_CHECK(!hokuto::edit::IsTimeline(whole, 1) && *whole.sequences[1] == other);
    const Settled settled = hokuto::edit::SettleTimelines(opened, whole, std::vector<uint32_t>{1});
    XSFLIB_CHECK(settled.replaced == std::vector<uint32_t>{1} && settled.items.empty());
    const File decided = hokuto::edit::WithNewModes(settled.file, settled.replaced);
    XSFLIB_CHECK(hokuto::edit::IsTimeline(decided, 1) && hokuto::edit::IsTimeline(decided, 0));
    const File program_whole = replaced(file, program_places, kAlone, other, 0);
    XSFLIB_CHECK(hokuto::format::ExportArchive(hokuto::edit::FoldTimelines(decided)) ==
                 hokuto::format::ExportArchive(program_whole));

    // SEQ_TWIN shares its file. The new commands go after the song's, and the timeline holds them. A saved file then
    // holds the program's commands and places, with the labels in the order of their names.
    const File shared = replaced(opened, places, kTwin, other, 0);
    const Settled settled_shared = hokuto::edit::SettleTimelines(opened, shared);
    XSFLIB_CHECK(settled_shared.replaced.empty() && hokuto::edit::IsTimeline(settled_shared.file, 0));
    const File saved = hokuto::edit::FoldTimelines(settled_shared.file);
    const File program_shared = replaced(file, program_places, kTwin, other, 0);
    XSFLIB_CHECK(*saved.sequences[0] == Sorted(*program_shared.sequences[0]));
    XSFLIB_CHECK(saved.native->fields == program_shared.native->fields);
    XSFLIB_CHECK(hokuto::edit::SettleTimelines(opened, shared, std::vector<uint32_t>{0}).replaced ==
                 std::vector<uint32_t>{0});
}

// The sounds of SongsArchive.
constexpr uint32_t kMain = 0;
constexpr uint32_t kOpening = 1;

// An archive of the Switch whose sound SEQ_MAIN plays the main song, and SEQ_OPENING the opening song. Their bank's
// instrument plays every key. With `at_start`, the sounds start at the songs' Start labels, and SEQ_MAIN's entry
// allocates both tracks. Otherwise the sounds start at the songs' first commands. Those commands allocate the tracks.
std::vector<uint8_t> SongsArchive(bool at_start, const Sequence& main_song = MainSong(),
                                  const Sequence& opening_song = OpeningSong())
{
    sar_files::WaveSpec wave;
    wave.encoding = 1;
    wave.loop = true;
    wave.sample_rate = 48000;
    wave.loop_end = 64;
    wave.channels = {std::vector<uint8_t>(128, 0x10)};
    sar_files::Region region;
    region.original_key = 60;
    sar_files::VelocityTable velocities;
    velocities.entries = {{0, region}};
    sar_files::KeyTable keys;
    keys.entries = {{0, velocities}};
    sar_files::BankSpec bank;
    bank.waves = {{0x05000000, 0}};
    bank.instruments = {keys};

    // A sound starts at the Start label's offset.
    const auto start_of = [](const Sequence& song)
    {
        const ntdlib::nwsnd::Encoded encoded =
            ntdlib::nwsnd::EncodeSequence(song, hokuto::song::GrammarOf(ntdlib::sar::Flavor::kNx));
        const auto start = std::find_if(song.labels.begin(), song.labels.end(),
                                        [](const Label& label) { return label.name.ends_with("_Start"); });
        return encoded.offsets.at(start->item);
    };

    sar_files::SoundSpec main;
    main.name = "SEQ_MAIN";
    main.volume = 127;
    main.banks = {0};
    sar_files::SoundSpec opening = main;
    opening.name = "SEQ_OPENING";
    opening.file = 1;
    if (at_start)
    {
        main.start_offset = start_of(main_song);
        main.allocate_track_flags = 3;
        opening.start_offset = start_of(opening_song);
    }
    sar_files::ArchiveSpec spec;
    spec.sounds = {main, opening};
    spec.banks = {{"BANK_TEST", 2}};
    spec.wave_archives = {{"WARC_TEST", 3}};
    spec.players = {{"PLAYER_TEST", 1}};
    spec.files = {{SongFile(main_song), "", false},
                  {SongFile(opening_song), "", false},
                  {sar_files::Bank(false, bank), "", false},
                  {sar_files::WaveArchive(false, {sar_files::Wave(false, wave)}), "", false}};

    return sar_files::Archive(false, spec);
}

// A timeline's notes move as in a MIDI file. SEQ_MAIN plays the main song and SEQ_OPENING the opening song. Track 0's
// last note of the intro, key 65 at tick 336, moves a beat later, to tick 384: the loop's start. It then plays in every
// pass of the loop, after key 60. A note can't start at or after the loop's end, at tick 768. The opening song has no
// loop. Its second note moves from tick 192 to 384, the track's old end. The track then lasts to the note's end.
void TestTimelineMoves()
{
    const File opened = hokuto::edit::OpenTimelines(hokuto::format::ImportSwitchArchive(SongsArchive(false)));
    XSFLIB_CHECK(hokuto::edit::IsTimeline(opened, 0) && hokuto::edit::IsTimeline(opened, 1));

    const auto starts = [](const hokuto::song::Sequence& sequence, int key)
    {
        std::vector<uint32_t> ticks;
        for (const hokuto::song::Note& note : sequence.tracks.at(0).notes)
        {
            if (note.key == key)
            {
                ticks.push_back(note.start);
            }
        }

        return ticks;
    };

    const auto edit_of = [](const hokuto::song::Sequence& sequence, uint32_t start, int32_t shift)
    {
        const hokuto::song::Track& track = sequence.tracks.at(0);
        const auto note = std::find_if(track.notes.begin(), track.notes.end(),
                                       [start](const hokuto::song::Note& n) { return n.start == start; });
        return hokuto::edit::NoteEdit{
            .command = note->command, .stretch = track.stretches.at(note->stretch), .shift = shift};
    };

    const hokuto::song::Sequence song = PlayedSequence(opened, 0);
    const hokuto::edit::NoteEdit later = edit_of(song, 336, 48);
    const std::vector<hokuto::edit::NoteLimits> limits = hokuto::edit::LimitsOf(
        opened, 0, std::vector<hokuto::edit::NotePlace>{{.command = later.command, .stretch = later.stretch}});
    XSFLIB_CHECK((limits.at(0).shift == hokuto::edit::ShiftRange{336, 431}));
    const Settled settled =
        hokuto::edit::SettleTimelines(opened, hokuto::edit::EditNotes(opened, 0, std::vector{later}).file);
    const hokuto::song::Sequence moved = PlayedSequence(hokuto::edit::FoldTimelines(settled.file), 0);
    XSFLIB_CHECK(moved.loop && moved.loop->start == 384 && moved.loop->end == 768);
    XSFLIB_CHECK((starts(moved, 65) == std::vector<uint32_t>{144, 384, 528, 720}));
    XSFLIB_CHECK(
        ErrorOf<std::invalid_argument>([&] { hokuto::edit::EditNotes(opened, 0, std::vector{edit_of(song, 720, 48)}); })
            .starts_with("A note can't start at or after the loop end."));

    const hokuto::song::Sequence alone = PlayedSequence(opened, 1);
    const Settled longer = hokuto::edit::SettleTimelines(
        opened, hokuto::edit::EditNotes(opened, 1, std::vector{edit_of(alone, 192, 192)}).file);
    const hokuto::song::Sequence lasting = PlayedSequence(hokuto::edit::FoldTimelines(longer.file), 1);
    XSFLIB_CHECK((starts(lasting, 60) == std::vector<uint32_t>{0, 384}));
    XSFLIB_CHECK(alone.notes_end == 384 && lasting.notes_end == 576 && lasting.length == alone.length + 192);
}

// A song whose track 0 loops over a bar of four notes from tick 0 to tick 384, and whose track 1 plays two notes and
// ends at tick 192.
Sequence EndingSong()
{
    const std::vector<Line> lines = {{{"SMF_E_Begin"}, Of(Kind::kAllocateTracks, 3)},
                                     {{"SMF_E_Start"}, OpenTrack(1), "SMF_E_Track_1"},
                                     {{"SMF_E_Track_0"}, Of(Kind::kNoteWait, 0)},
                                     {{"SMF_E_Track_0_LoopStart"}, Note(60, 96)},
                                     {{}, Of(Kind::kWait, 96)},
                                     {{}, Note(64, 96)},
                                     {{}, Of(Kind::kWait, 96)},
                                     {{}, Note(67, 96)},
                                     {{}, Of(Kind::kWait, 96)},
                                     {{}, Note(72, 96)},
                                     {{}, Of(Kind::kWait, 96)},
                                     {{}, Of(Kind::kJump), "SMF_E_Track_0_LoopStart"},
                                     {{"SMF_E_Track_1"}, Of(Kind::kNoteWait, 0)},
                                     {{}, Note(48, 96)},
                                     {{}, Of(Kind::kWait, 96)},
                                     {{}, Note(50, 96)},
                                     {{}, Of(Kind::kWait, 96)},
                                     {{}, Of(Kind::kEnd)},
                                     {{"SMF_E_End"}, Of(Kind::kEnd)}};
    return Build(lines);
}

// In a song that loops, a note of a track that ends moves past the track's end up to the tick before the loop's end, as
// on a track that loops. Track 1's last note of the ending song, at tick 96, moves at most 287 ticks later. At tick
// 383, the track lasts to the note's end, and the song keeps its loop.
void TestEndingTrackMoves()
{
    const File opened =
        hokuto::edit::OpenTimelines(hokuto::format::ImportSwitchArchive(SongsArchive(false, EndingSong())));
    XSFLIB_CHECK(hokuto::edit::IsTimeline(opened, 0));
    XSFLIB_CHECK((hokuto::edit::TimelineLoop(opened, 0) == ConvertedLoop{0, 384}));
    const hokuto::song::Sequence song = PlayedSequence(opened, 0);
    const auto track = std::ranges::find(song.tracks, 1, &hokuto::song::Track::number);
    SEQLIB_REQUIRE(track != song.tracks.end() && track->notes.size() == 2);
    const hokuto::song::Note& last = track->notes.back();
    XSFLIB_CHECK(last.start == 96);

    const auto edit_of = [&](int32_t shift)
    {
        return hokuto::edit::NoteEdit{
            .command = last.command, .stretch = track->stretches.at(last.stretch), .shift = shift};
    };

    const std::vector<hokuto::edit::NoteLimits> limits = hokuto::edit::LimitsOf(
        opened, 0,
        std::vector<hokuto::edit::NotePlace>{{.command = last.command, .stretch = track->stretches.at(last.stretch)}});
    XSFLIB_CHECK(limits.size() == 1 && limits[0].shift && limits[0].shift->later == 287);

    XSFLIB_CHECK(
        ErrorOf<std::invalid_argument>([&] { hokuto::edit::EditNotes(opened, 0, std::vector{edit_of(288)}); }) ==
        "A note can't start at or after the loop end. The song jumps back to the loop start there.");

    const File settled =
        hokuto::edit::SettleTimelines(opened, hokuto::edit::EditNotes(opened, 0, std::vector{edit_of(287)}).file).file;
    XSFLIB_CHECK((hokuto::edit::TimelineLoop(settled, 0) == ConvertedLoop{0, 384}));
    const hokuto::song::Sequence moved =
        hokuto::edit::TimelineRead(settled, 0, PlayedSequence(hokuto::edit::FoldTimelines(settled), 0));
    const auto moved_track = std::ranges::find(moved.tracks, 1, &hokuto::song::Track::number);
    XSFLIB_CHECK(moved.loop && moved.loop->start == 0 && moved.loop->end == 384 && moved_track != moved.tracks.end() &&
                 moved_track->notes.size() == 2 && moved_track->notes.back().start == 383);
}

// A note that runs past the end of a timeline's track moves the end, and so does a note after the end. The opening song
// has no loop. Its track 0 plays two notes of 192 ticks and ends at tick 384, and a new track ends there too. A note
// drawn on the new track at tick 288 lasts 192 ticks. The track then lasts to the note's end at tick 480, and the song
// with it. A note drawn there for 96 ticks ends at tick 384, and the song keeps its end. Track 0's second note, at
// tick 192, sized to 288 ticks, also ends at tick 480 and moves the end.
void TestNotesOverTheEnd()
{
    const File opened = hokuto::edit::OpenTimelines(hokuto::format::ImportSwitchArchive(SongsArchive(true)));
    const hokuto::edit::TrackEdit added = hokuto::edit::AddTrack(
        opened, PlacesOf(opened), kOpening, PlayedSequence(opened, kOpening), hokuto::edit::Program{0, 0});
    const File with_track = hokuto::edit::SettleTimelines(opened, added.file).file;
    XSFLIB_CHECK(added.track == 1 && hokuto::edit::IsTimeline(with_track, kOpening));
    const hokuto::song::Sequence song = PlayedSequence(with_track, kOpening);
    XSFLIB_CHECK(song.notes_end == 384);
    const auto new_track = std::ranges::find(song.tracks, 1, &hokuto::song::Track::number);
    const auto first_track = std::ranges::find(song.tracks, 0, &hokuto::song::Track::number);
    SEQLIB_REQUIRE(new_track != song.tracks.end() && first_track != song.tracks.end() &&
                   first_track->notes.size() == 2);

    // The song after the edit, and the notes of track `number` there, at their starts and lengths.
    const auto edited = [&with_track](const hokuto::edit::Edited& edit)
    {
        return PlayedSequence(hokuto::edit::FoldTimelines(hokuto::edit::SettleTimelines(with_track, edit.file).file),
                              kOpening);
    };
    const auto notes_of = [](const hokuto::song::Sequence& sequence, int number)
    {
        std::vector<std::pair<uint32_t, uint32_t>> notes;
        for (const hokuto::song::Track& track : sequence.tracks)
        {
            for (const hokuto::song::Note& note : track.notes)
            {
                if (track.number == number)
                {
                    notes.emplace_back(note.start, note.length);
                }
            }
        }

        return notes;
    };
    const auto drawn = [&](uint32_t length)
    {
        const hokuto::edit::NewNote note{
            .stretch = new_track->stretches.at(0), .tick = 288, .key = 62, .length = length};
        return edited(hokuto::edit::EditNotes(with_track, kOpening, {}, std::vector{note}));
    };

    const hokuto::song::Sequence crossing = drawn(192);
    XSFLIB_CHECK((notes_of(crossing, 1) == std::vector<std::pair<uint32_t, uint32_t>>{{288, 192}}));
    XSFLIB_CHECK(crossing.notes_end == 480 && crossing.length == song.length + 96 && !crossing.loop);
    XSFLIB_CHECK(notes_of(crossing, 0) == notes_of(song, 0));

    const hokuto::song::Sequence inside = drawn(96);
    XSFLIB_CHECK((notes_of(inside, 1) == std::vector<std::pair<uint32_t, uint32_t>>{{288, 96}}));
    XSFLIB_CHECK(inside.notes_end == 384 && inside.length == song.length);

    const hokuto::song::Note& second = first_track->notes.back();
    XSFLIB_CHECK(second.start == 192 && second.length == 192);
    const hokuto::edit::NoteEdit longer{
        .command = second.command, .stretch = first_track->stretches.at(second.stretch), .length = 288};
    const hokuto::song::Sequence sized = edited(hokuto::edit::EditNotes(with_track, kOpening, std::vector{longer}));
    XSFLIB_CHECK((notes_of(sized, 0) == std::vector<std::pair<uint32_t, uint32_t>>{{0, 192}, {192, 288}}));
    XSFLIB_CHECK(sized.notes_end == 480 && sized.length == song.length + 96);
}

// A note whose command lasts past the end of its track in the timeline. A file saved before such notes moved the end
// can contain one. The opening song's second note, at tick 192, lasts 288 ticks, and its track ends at tick 384. The
// end cuts the note, and LimitsOf says so. The first note ends before the end. An edit of the note to the length that
// its command sets moves the end to tick 480, the note's end.
void TestNoteCutByTheEnd()
{
    const File opened = hokuto::edit::OpenTimelines(hokuto::format::ImportSwitchArchive(SongsArchive(true)));
    Sequence commands = *opened.sequences.at(kOpening);

    // The second note's command lasts 288 ticks.
    std::vector<Command*> notes;
    for (Command& command : commands.items)
    {
        if (command.kind == Kind::kNote)
        {
            notes.push_back(&command);
        }
    }
    SEQLIB_REQUIRE(notes.size() == 2);
    notes[1]->value.value = 288 * kOne;
    notes[1]->encoding.value_bytes = 0;
    File cut = opened;
    cut.sequences[kOpening] = commands;
    XSFLIB_CHECK(hokuto::edit::IsTimeline(cut, kOpening));

    // The song plays the second note up to the end.
    const hokuto::song::Sequence song = PlayedSequence(cut, kOpening);
    const auto track = std::ranges::find(song.tracks, 0, &hokuto::song::Track::number);
    SEQLIB_REQUIRE(track != song.tracks.end() && track->notes.size() == 2);
    const hokuto::song::Note& first = track->notes.front();
    const hokuto::song::Note& second = track->notes.back();
    XSFLIB_CHECK(second.start == 192 && second.length == 192 && song.notes_end == 384);
    const hokuto::edit::NotePlace first_place{.command = first.command, .stretch = track->stretches.at(first.stretch)};
    const hokuto::edit::NotePlace second_place{.command = second.command,
                                               .stretch = track->stretches.at(second.stretch)};

    const std::vector<hokuto::edit::NoteLimits> limits =
        hokuto::edit::LimitsOf(cut, kOpening, std::vector{first_place, second_place});
    const hokuto::edit::NoteEdit full{.command = second.command, .stretch = second_place.stretch, .length = 288};
    const hokuto::song::Sequence sized = PlayedSequence(
        hokuto::edit::FoldTimelines(
            hokuto::edit::SettleTimelines(cut, hokuto::edit::EditNotes(cut, kOpening, std::vector{full}).file).file),
        kOpening);

    SEQLIB_REQUIRE(limits.size() == 2);
    XSFLIB_CHECK(!limits[0].cut_by_end && limits[0].length == 192u);
    XSFLIB_CHECK(limits[1].cut_by_end && limits[1].length == 288u);
    const auto sized_track = std::ranges::find(sized.tracks, 0, &hokuto::song::Track::number);
    SEQLIB_REQUIRE(sized_track != sized.tracks.end() && sized_track->notes.size() == 2);
    XSFLIB_CHECK(sized_track->notes.back().start == 192 && sized_track->notes.back().length == 288);
    XSFLIB_CHECK(sized.notes_end == 480 && sized.length == song.length + 96);
}

// The ticks of each note of track `number` of `song`, with its place for LimitsOf and EditNotes.
std::vector<std::pair<uint32_t, hokuto::edit::NotePlace>> NotesOf(const hokuto::song::Sequence& song, int number)
{
    std::vector<std::pair<uint32_t, hokuto::edit::NotePlace>> notes;
    for (const hokuto::song::Track& track : song.tracks)
    {
        if (track.number != number)
        {
            continue;
        }

        for (const hokuto::song::Note& note : track.notes)
        {
            notes.emplace_back(note.start, hokuto::edit::NotePlace{.command = note.command,
                                                                   .stretch = track.stretches.at(note.stretch)});
        }
    }

    return notes;
}

// Whether EditNotes refuses to move the note at `place` of sequence `sequence` by `shift` ticks with `message`.
bool RefusesMove(const File& file, uint32_t sequence, const hokuto::edit::NotePlace& place, int32_t shift,
                 const std::string& message)
{
    return ErrorOf<std::invalid_argument>(
               [&]
               {
                   hokuto::edit::EditNotes(file, sequence,
                                           std::vector{hokuto::edit::NoteEdit{
                                               .command = place.command, .stretch = place.stretch, .shift = shift}});
               }) == message;
}

// Paste Track opens a pasted track where the sound starts, before a header's waits. The waited song in SEQ_MAIN's place
// takes the opening song's track 0. That track ends. The waited song's tracks still loop from tick 17 to 209. The
// pasted track's note at tick 192 moves up to tick 208, like a note of a looping track. The ending song in SEQ_MAIN's
// place takes the waited song's track 1. That track loops from tick 17 to 209, and the ending song's track 0 from tick
// 0 to 384. The song then has no single loop. Its looping tracks have each gone back once by tick 384. Track 1's last
// note, at tick 96, moves up to tick 383 and stays in the arrangement there. The waited song with its track 1 pasted
// again moves its loop. Each track counts the loop's ticks from where the header starts it.
void TestPastedLoops()
{
    const std::string kLoopEnd =
        "A note can't start at or after the loop end. The song jumps back to the loop start there.";

    const auto paste = [](const File& opened, int track)
    {
        const std::vector<std::optional<SoundPlace>> places = PlacesOf(opened);
        const hokuto::edit::CopiedTrack copied =
            hokuto::edit::CopyTrack(opened, places, kOpening, PlayedSequence(opened, kOpening), track);
        const hokuto::edit::TrackEdit pasted =
            hokuto::edit::PasteTrack(opened, places, kMain, PlayedSequence(opened, kMain), copied);
        return std::pair(hokuto::edit::SettleTimelines(opened, pasted.file).file, pasted.track);
    };

    const File waited =
        hokuto::edit::OpenTimelines(hokuto::format::ImportSwitchArchive(SongsArchive(true, WaitedSong())));
    const auto [with_ending, ending_track] = paste(waited, 0);
    XSFLIB_CHECK(hokuto::edit::IsTimeline(with_ending, kMain));
    XSFLIB_CHECK((hokuto::edit::TimelineLoop(with_ending, kMain) == ConvertedLoop{17, 209}));
    const auto pasted_notes = NotesOf(PlayedSequence(with_ending, kMain), ending_track);
    XSFLIB_CHECK(pasted_notes.size() == 2 && pasted_notes.back().first == 192);
    if (pasted_notes.size() == 2)
    {
        const hokuto::edit::NotePlace& last = pasted_notes.back().second;
        const auto limits = hokuto::edit::LimitsOf(with_ending, kMain, std::span(&last, 1));
        XSFLIB_CHECK(limits.at(0).shift && limits.at(0).shift->later == 16);
        XSFLIB_CHECK(RefusesMove(with_ending, kMain, last, 17, kLoopEnd));
    }

    const File ending = hokuto::edit::OpenTimelines(
        hokuto::format::ImportSwitchArchive(SongsArchive(false, EndingSong(), WaitedSong())));
    const auto [with_loop, looping_track] = paste(ending, 1);
    XSFLIB_CHECK(hokuto::edit::IsTimeline(with_loop, kMain) && !hokuto::edit::TimelineLoop(with_loop, kMain));
    const hokuto::edit::SongEnd song_end = hokuto::edit::TimelineSongEnd(with_loop, kMain);
    XSFLIB_CHECK(song_end.loops && song_end.tick == 384u);
    const auto track_1 = NotesOf(PlayedSequence(with_loop, kMain), 1);
    XSFLIB_CHECK(looping_track == 2 && track_1.size() == 2 && track_1.back().first == 96);
    if (track_1.size() == 2)
    {
        const hokuto::edit::NotePlace& last = track_1.back().second;
        const auto limits = hokuto::edit::LimitsOf(with_loop, kMain, std::span(&last, 1));
        XSFLIB_CHECK(limits.at(0).shift && limits.at(0).shift->later == 287);
        XSFLIB_CHECK(RefusesMove(with_loop, kMain, last, 288, kLoopEnd));
        const std::vector moves{hokuto::edit::NoteEdit{.command = last.command, .stretch = last.stretch, .shift = 287}};
        const File edited = hokuto::edit::EditNotes(with_loop, kMain, moves).file;
        const File moved = hokuto::edit::SettleTimelines(with_loop, edited).file;
        const auto moved_notes = NotesOf(PlayedSequence(moved, kMain), 1);
        XSFLIB_CHECK(moved_notes.size() == 2 && moved_notes.back().first == 383);
    }

    const File twice = hokuto::edit::OpenTimelines(
        hokuto::format::ImportSwitchArchive(SongsArchive(false, WaitedSong(), WaitedSong())));
    const auto [with_copy, copy_track] = paste(twice, 1);
    XSFLIB_CHECK(copy_track == 2 && (hokuto::edit::TimelineLoop(with_copy, kMain) == ConvertedLoop{17, 209}));
    const Edited moved_loop = hokuto::edit::MoveLoop(with_copy, kMain, {113, 209});
    const File settled_loop = hokuto::edit::SettleTimelines(with_copy, moved_loop.file).file;
    XSFLIB_CHECK((hokuto::edit::TimelineLoop(settled_loop, kMain) == ConvertedLoop{113, 209}));
}

// Tracks that loop at other ticks. The ending song in SEQ_MAIN's place takes the main song's track 0. That track loops
// from tick 384 to 768. The ending song's track 0 loops from tick 0 to 384. Both loops last 384 ticks. The song
// therefore repeats from tick 384 to 768. Track 1 ends at tick 192. Its last note, at tick 96, moves up to tick 767.
// The note then plays in the first pass of the song's loop. ntdlib's performer finds the loop a pass later. The
// arrangement still shows the loop from tick 384 to 768, with the note in it.
//
// The waited song in SEQ_MAIN's place takes the ending song's track 0. The track's loop lasts 384 ticks, twice the
// waited song's. ntdlib's performer finds the waited song's loop, from tick 17 to 209. The arrangement shows the pasted
// track's notes up to tick 209. Without the loop that the arrangement shows, the pasted track's note at tick 192 moves
// up to tick 383, before its track's loop ends. With it, the note moves up to tick 208. It stays in the arrangement
// there, and the loop stays.
void TestOtherLoops()
{
    const auto paste = [](const File& opened, int track)
    {
        const std::vector<std::optional<SoundPlace>> places = PlacesOf(opened);
        const hokuto::edit::CopiedTrack copied =
            hokuto::edit::CopyTrack(opened, places, kOpening, PlayedSequence(opened, kOpening), track);
        const hokuto::edit::TrackEdit pasted =
            hokuto::edit::PasteTrack(opened, places, kMain, PlayedSequence(opened, kMain), copied);
        return hokuto::edit::SettleTimelines(opened, pasted.file).file;
    };

    const auto shown = [](const File& file)
    {
        return hokuto::edit::TimelineRead(file, kMain, PlayedSequence(hokuto::edit::FoldTimelines(file), kMain));
    };

    const File ending = paste(
        hokuto::edit::OpenTimelines(hokuto::format::ImportSwitchArchive(SongsArchive(false, EndingSong(), MainSong()))),
        0);
    XSFLIB_CHECK(hokuto::edit::IsTimeline(ending, kMain) && !hokuto::edit::TimelineLoop(ending, kMain));
    XSFLIB_CHECK((SongLoopsOf(*ending.sequences[kMain]) == std::vector<ConvertedLoop>{{0, 384}, {384, 768}}));
    XSFLIB_CHECK((hokuto::edit::TimelineSongLoop(ending, kMain) == ConvertedLoop{384, 768}));
    const hokuto::song::Sequence before = shown(ending);
    XSFLIB_CHECK(before.loop && before.loop->start == 384 && before.loop->end == 768);
    const auto track_1 = NotesOf(before, 1);
    XSFLIB_CHECK(track_1.size() == 2 && track_1.back().first == 96);
    if (track_1.size() == 2 && before.loop)
    {
        const hokuto::edit::NotePlace& last = track_1.back().second;
        const auto limits = hokuto::edit::LimitsOf(ending, kMain, std::span(&last, 1), before.loop->end);
        XSFLIB_CHECK(limits.at(0).shift && limits.at(0).shift->later == 671);

        const File moved =
            hokuto::edit::SettleTimelines(
                ending, hokuto::edit::EditNotes(ending, kMain,
                                                std::vector{hokuto::edit::NoteEdit{
                                                    .command = last.command, .stretch = last.stretch, .shift = 671}})
                            .file)
                .file;

        const hokuto::song::Sequence after = shown(moved);
        const auto moved_notes = NotesOf(after, 1);
        XSFLIB_CHECK(after.loop && after.loop->start == 384 && after.loop->end == 768);
        XSFLIB_CHECK(moved_notes.size() == 2 && moved_notes.back().first == 767);
    }

    const File waited = paste(hokuto::edit::OpenTimelines(
                                  hokuto::format::ImportSwitchArchive(SongsArchive(false, WaitedSong(), EndingSong()))),
                              0);
    XSFLIB_CHECK(!hokuto::edit::TimelineLoop(waited, kMain) && !hokuto::edit::TimelineSongLoop(waited, kMain));
    XSFLIB_CHECK((SongLoopsOf(*waited.sequences[kMain]) == std::vector<ConvertedLoop>{{17, 209}, {17, 209}, {0, 384}}));
    const hokuto::song::Sequence waited_before = shown(waited);
    XSFLIB_CHECK(waited_before.loop && waited_before.loop->start == 17 && waited_before.loop->end == 209);
    const auto pasted = NotesOf(waited_before, 2);
    XSFLIB_CHECK(pasted.size() == 3 && pasted.back().first == 192);
    if (pasted.size() == 3 && waited_before.loop)
    {
        const hokuto::edit::NotePlace& last = pasted.back().second;
        const auto alone = hokuto::edit::LimitsOf(waited, kMain, std::span(&last, 1));
        const auto limits = hokuto::edit::LimitsOf(waited, kMain, std::span(&last, 1), waited_before.loop->end);
        XSFLIB_CHECK(alone.at(0).shift && alone.at(0).shift->later == 191);
        XSFLIB_CHECK(limits.at(0).shift && limits.at(0).shift->later == 16);

        const File moved =
            hokuto::edit::SettleTimelines(
                waited, hokuto::edit::EditNotes(waited, kMain,
                                                std::vector{hokuto::edit::NoteEdit{
                                                    .command = last.command, .stretch = last.stretch, .shift = 16}})
                            .file)
                .file;

        const hokuto::song::Sequence after = shown(moved);
        const auto moved_notes = NotesOf(after, 2);
        XSFLIB_CHECK(after.loop && after.loop->start == 17 && after.loop->end == 209);
        XSFLIB_CHECK(moved_notes.size() == 3 && moved_notes.back().first == 208);
    }
}

// A timeline takes a new track, a copy and a pasted track as NintendoWare's MIDI converter lays out a song of more
// tracks.
struct TimelineTracksFixture
{
    File opened;
    std::vector<std::optional<SoundPlace>> places;
    hokuto::song::Sequence main;
    hokuto::song::Sequence opening;
};

// The fixture of the tests of new, copied and pasted tracks in the songs' timelines.
TimelineTracksFixture MakeTimelineTracksFixture()
{
    const File opened = hokuto::edit::OpenTimelines(hokuto::format::ImportSwitchArchive(SongsArchive(true)));
    const std::vector<std::optional<SoundPlace>> places = PlacesOf(opened);
    const hokuto::song::Sequence main = PlayedSequence(opened, kMain);
    const hokuto::song::Sequence opening = PlayedSequence(opened, kOpening);
    return {opened, places, main, opening};
}

// The item that label `name` names in `sequence`. Nothing if `sequence` has no such label.
std::optional<uint32_t> LabelItem(const Sequence& sequence, const std::string& name)
{
    const auto label = std::find_if(sequence.labels.begin(), sequence.labels.end(),
                                    [&name](const Label& l) { return l.name == name; });
    return label == sequence.labels.end() ? std::nullopt : std::optional<uint32_t>(label->item);
}

// The starts and keys of the notes of track `number` of `song`.
std::vector<std::pair<uint32_t, int>> NoteKeysOf(const hokuto::song::Sequence& song, int number)
{
    std::vector<std::pair<uint32_t, int>> notes;
    for (const hokuto::song::Track& track : song.tracks)
    {
        if (track.number != number)
        {
            continue;
        }

        for (const hokuto::song::Note& note : track.notes)
        {
            notes.emplace_back(note.start, note.key);
        }
    }

    return notes;
}

// Whether `song` has track `number`.
bool HasTrack(const hokuto::song::Sequence& song, int number)
{
    return std::any_of(song.tracks.begin(), song.tracks.end(),
                       [number](const hokuto::song::Track& track) { return track.number == number; });
}

// A settled edit keeps the timeline, and the converter's rules read the save again. Each save holds the converter's
// layout.
File SettledEdit(const File& opened, const File& edited, uint32_t sequence)
{
    const Settled result = hokuto::edit::SettleTimelines(opened, edited);
    XSFLIB_CHECK(hokuto::edit::IsTimeline(result.file, sequence));
    const File saved = hokuto::edit::FoldTimelines(result.file);
    XSFLIB_CHECK(ReadConverted(*saved.sequences[sequence]).hints.has_value());

    return result.file;
}

// The sounds start at the songs' Start labels. In the opening song, the label names track 0's first command. The new
// track start takes the sound's start and the Start label there, and track 0 keeps its label.
void TestTimelineTracksOpening()
{
    const TimelineTracksFixture fixture = MakeTimelineTracksFixture();
    const File& opened = fixture.opened;
    const std::vector<std::optional<SoundPlace>>& places = fixture.places;
    const hokuto::song::Sequence& opening = fixture.opening;
    XSFLIB_CHECK(hokuto::edit::IsTimeline(opened, kMain) && hokuto::edit::IsTimeline(opened, kOpening));

    // The opening song's new track start takes the Start label, and the track's code goes before the End label.
    const hokuto::edit::TrackEdit added =
        hokuto::edit::AddTrack(opened, places, kOpening, opening, hokuto::edit::Program{0, 0});
    XSFLIB_CHECK(added.track == 1);
    const File with_track = SettledEdit(opened, added.file, kOpening);
    const Sequence& song = *with_track.sequences[kOpening];
    const std::optional<uint32_t> start = LabelItem(song, "SMF_O_Start");
    XSFLIB_CHECK(start && song.items.at(*start).kind == Kind::kOpenTrack && song.items.at(*start).index == 1);
    XSFLIB_CHECK(LabelItem(song, "SMF_O_Track_0") == std::optional<uint32_t>(*start + 1));
    const std::optional<uint32_t> track_1 = LabelItem(song, "SMF_O_Track_1");
    XSFLIB_CHECK(track_1 && song.items.at(*start).target == *track_1 && *track_1 < *LabelItem(song, "SMF_O_End"));
    const hokuto::song::Sequence with_added = PlayedSequence(hokuto::edit::FoldTimelines(with_track), kOpening);
    XSFLIB_CHECK(HasTrack(with_added, 1) && NoteKeysOf(with_added, 0) == NoteKeysOf(opening, 0));
}

// In the main song, the new track's code goes after track 1's, and the track loops with the song.
void TestTimelineTracksMain()
{
    const TimelineTracksFixture fixture = MakeTimelineTracksFixture();
    const File& opened = fixture.opened;
    const std::vector<std::optional<SoundPlace>>& places = fixture.places;
    const hokuto::song::Sequence& main = fixture.main;

    // The main song's new track loops with the song.
    const hokuto::edit::TrackEdit looping = hokuto::edit::AddTrack(opened, places, kMain, main, std::nullopt);
    XSFLIB_CHECK(looping.track == 2);
    const File with_loop = SettledEdit(opened, looping.file, kMain);
    const Sequence& main_song = *with_loop.sequences[kMain];
    const std::optional<uint32_t> track_2 = LabelItem(main_song, "SMF_T_Track_2");
    XSFLIB_CHECK(track_2 && *LabelItem(main_song, "SMF_T_Track_1") < *track_2 &&
                 *track_2 < *LabelItem(main_song, "SMF_T_End"));
    XSFLIB_CHECK((LoopOfConverted(main_song) == ConvertedLoop{384, 768}));
    const hokuto::song::Sequence with_loop_read = PlayedSequence(hokuto::edit::FoldTimelines(with_loop), kMain);
    XSFLIB_CHECK(HasTrack(with_loop_read, 2) && with_loop_read.loop && with_loop_read.loop->start == 384 &&
                 with_loop_read.loop->end == 768);
    XSFLIB_CHECK(NoteKeysOf(with_loop_read, 0) == NoteKeysOf(main, 0) &&
                 NoteKeysOf(with_loop_read, 1) == NoteKeysOf(main, 1));
}

// A copied track and a track pasted from another song play their notes.
void TestTimelineTracksCopies()
{
    const TimelineTracksFixture fixture = MakeTimelineTracksFixture();
    const File& opened = fixture.opened;
    const std::vector<std::optional<SoundPlace>>& places = fixture.places;
    const hokuto::song::Sequence& main = fixture.main;
    const hokuto::song::Sequence& opening = fixture.opening;

    // A copy of the opening song's track 0 plays its notes.
    const hokuto::edit::TrackEdit copied = hokuto::edit::DuplicateTrack(opened, places, kOpening, 0);
    const hokuto::song::Sequence with_copy =
        PlayedSequence(hokuto::edit::FoldTimelines(SettledEdit(opened, copied.file, kOpening)), kOpening);
    XSFLIB_CHECK(copied.track == 1 && NoteKeysOf(with_copy, 1) == NoteKeysOf(opening, 0));

    // The main song's track 1 pasted into the opening song plays its notes, and its loop's label takes the opening
    // song's name of the track.
    const hokuto::edit::CopiedTrack track = hokuto::edit::CopyTrack(opened, places, kMain, main, 1);
    const hokuto::edit::TrackEdit pasted = hokuto::edit::PasteTrack(opened, places, kOpening, opening, track);
    const File with_paste = SettledEdit(opened, pasted.file, kOpening);
    XSFLIB_CHECK(LabelItem(*with_paste.sequences[kOpening], "SMF_O_Track_1_LoopStart").has_value());
    const hokuto::song::Sequence pasted_read = PlayedSequence(hokuto::edit::FoldTimelines(with_paste), kOpening);

    // The opening song's track 0 ends before the loop. ntdlib's performer then finds the loop a pass later.
    const std::vector<std::pair<uint32_t, int>> played = NoteKeysOf(pasted_read, 1);
    const std::vector<std::pair<uint32_t, int>> pass = NoteKeysOf(main, 1);
    XSFLIB_CHECK(pasted.track == 1 && played.size() >= pass.size() &&
                 std::equal(pass.begin(), pass.end(), played.begin()));
}

// The waited song in SEQ_MAIN's place. The timeline's loop counts the header's wait, and a drag of an edge moves it by
// the same ticks. The loop can't start before the tracks. A new track starts with the song's tracks, before the
// header's jump at tick 17, and loops with them.
void TestWaitedTimeline()
{
    const File opened =
        hokuto::edit::OpenTimelines(hokuto::format::ImportSwitchArchive(SongsArchive(true, WaitedSong())));
    XSFLIB_CHECK(hokuto::edit::IsTimeline(opened, kMain));
    XSFLIB_CHECK((hokuto::edit::TimelineLoop(opened, kMain) == ConvertedLoop{17, 209}));
    const hokuto::song::Sequence main = hokuto::edit::TimelineRead(opened, kMain, PlayedSequence(opened, kMain));
    XSFLIB_CHECK(main.loop && main.loop->start == 17 && main.loop->end == 209);

    const Edited moved = hokuto::edit::MoveLoop(opened, kMain, {113, 209});
    XSFLIB_CHECK((hokuto::edit::TimelineLoop(moved.file, kMain) == ConvertedLoop{113, 209}));
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::MoveLoop(opened, kMain, {10, 209}); }) ==
                 "The loop can't start before the tracks start.");

    const hokuto::edit::TrackEdit added = hokuto::edit::AddTrack(opened, PlacesOf(opened), kMain, main, std::nullopt);
    const File settled = hokuto::edit::SettleTimelines(opened, added.file).file;
    const Sequence& song = *settled.sequences[kMain];
    XSFLIB_CHECK(added.track == 2 && hokuto::edit::IsTimeline(settled, kMain));
    XSFLIB_CHECK(song.items.at(3).kind == Kind::kOpenTrack && song.items.at(3).index == 2 &&
                 song.items.at(4).kind == Kind::kJump);
    XSFLIB_CHECK((hokuto::edit::TimelineLoop(settled, kMain) == ConvertedLoop{17, 209}));
    const hokuto::song::Sequence read =
        hokuto::edit::TimelineRead(settled, kMain, PlayedSequence(hokuto::edit::FoldTimelines(settled), kMain));
    XSFLIB_CHECK(read.loop && read.loop->start == 17 && read.loop->end == 209 &&
                 std::ranges::any_of(read.tracks, [](const hokuto::song::Track& track) { return track.number == 2; }));
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("timelines",
                                    []
                                    {
                                        TestTimelines();
                                        TestZeroWait();
                                        TestReplaced();
                                        TestTimelineMoves();
                                        TestEndingTrackMoves();
                                        TestNotesOverTheEnd();
                                        TestNoteCutByTheEnd();
                                        TestTimelineTracksOpening();
                                        TestTimelineTracksMain();
                                        TestTimelineTracksCopies();
                                        TestWaitedTimeline();
                                        TestPastedLoops();
                                        TestOtherLoops();
                                    });
}
