// SPDX-License-Identifier: MIT

// Tests the imports of MIDI files (src/edit/midi_imports.h). A MIDI file sets each controller that the import supports.
// The song has to contain each setting at its tick, in the units of ntdlib's commands, converted from the value of the
// MIDI specification. An event between two of the song's ticks has to move to the nearer one, and the warnings have to
// count it. A file whose events all fall on the ticks of a timebase of 96 has to use that timebase. Of 18 channels with
// notes, the first 16 have to stay, and the warnings have to name the others. Each console then imports a small MIDI
// file through the path of File > Import > Sequence…: ReadIncomingMidi for the Switch, the Wii U, the 3DS and the Wii,
// and DsFileOfMidi for the DS. ntdlib's performer has to play the notes that the MIDI file contains: their ticks, keys,
// velocities, lengths and programs, with the bank slot that control change 0 selects. On the three later consoles, the
// new sequence has to open as a timeline. The pairing of a drum note has to set the program and the key of the note, on
// the later consoles and on the DS, and the other notes of the channel have to keep the file's program.

#include "edit/midi_imports.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sdat/cut.h"
#include "ntdlib/sequence_export/filter.h"
#include "ntdlib/sequence_export/settings.h"
#include "ntdlib/testing/rsar_files.h"
#include "ntdlib/testing/sar_files.h"
#include "ntdlib/testing/sdat_files.h"
#include "seqlib/midi/builder.h"
#include "xsflib/testing/check.h"

#include "edit/bytecode.h"
#include "edit/converter.h"
#include "edit/ds_new_items.h"
#include "edit/new_items.h"
#include "edit/timelines.h"
#include "format/archive.h"
#include "format/file.h"
#include "format/model.h"
#include "song/archive.h"
#include "song/sequence.h"

namespace
{

namespace midi = seqlib::midi;
namespace export_ = ntdlib::sequence_export;
namespace sar = ntdlib::sar;
namespace sar_files = ntdlib::testing::sar_files;
namespace rsar_files = ntdlib::testing::rsar_files;
namespace ds_files = ntdlib::testing::sdat_files;
namespace edit = hokuto::edit;
namespace format = hokuto::format;
namespace song = hokuto::song;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Sequence;

// The settings of the track that starts at label `label`, by kind and index: each setting's tick and value.
using Setting = std::tuple<uint32_t, Kind, uint8_t, int64_t>;
std::vector<Setting> SettingsAt(const Sequence& sequence, const std::string& label)
{
    uint32_t item = 0;
    for (const auto& known : sequence.labels)
    {
        if (known.name == label)
        {
            item = known.item;
        }
    }
    std::vector<Setting> settings;
    uint32_t tick = 0;
    for (; item < sequence.items.size(); ++item)
    {
        const Command& command = sequence.items[item];
        if (command.kind == Kind::kWait)
        {
            tick += static_cast<uint32_t>(command.value.value / kOne);
        }
        else if (command.kind == Kind::kEnd || command.kind == Kind::kJump)
        {
            break;
        }
        else if (command.kind != Kind::kNote && command.kind != Kind::kNoteWait && command.kind != Kind::kTempo)
        {
            settings.emplace_back(tick, command.kind, command.index, command.value.value);
        }
    }

    return settings;
}

bool Holds(const std::vector<Setting>& settings, uint32_t tick, Kind kind, int64_t value, uint8_t index = 0)
{
    return std::ranges::find(settings, Setting(tick, kind, index, value)) != settings.end();
}

bool WarnsOf(const std::vector<std::string>& warnings, const std::string& text)
{
    return std::ranges::any_of(warnings,
                               [&](const std::string& warning) { return warning.find(text) != std::string::npos; });
}

// A MIDI file of 96 ticks a quarter note in which every setting that an import supports comes a quarter note after the
// last, on channel 1, before a note at the end.
std::vector<uint8_t> Controllers()
{
    midi::FileBuilder file(96);
    midi::TrackBuilder& track = file.AddTrack();
    uint32_t tick = 0;
    const auto next = [&tick]
    {
        tick += 96;
        return tick;
    };
    track.NoteOn(0, 0, 60, 100);
    track.NoteOff(96, 0, 60);
    track.Control(next(), 0, 1, 64);
    track.Control(next(), 0, 5, 20);
    track.Control(next(), 0, 65, 127);
    track.Control(next(), 0, 65, 10);
    track.Control(next(), 0, 72, 30);
    track.Control(next(), 0, 73, 40);
    track.Control(next(), 0, 74, 32);
    track.Control(next(), 0, 75, 50);
    track.Control(next(), 0, 84, 72);
    track.Control(next(), 0, 103, 16);
    track.Control(next(), 0, 104, 9);
    track.Control(next(), 0, 126, 1);
    track.Control(next(), 0, 127, 0);
    track.Control(next(), 0, 101, 0);
    track.Control(tick, 0, 100, 2);
    track.Control(tick, 0, 6, 66);
    track.Control(tick, 0, 38, 0);
    track.Control(next(), 0, 76, 3);
    track.NoteOn(next(), 0, 62, 90);
    track.NoteOff(tick + 96, 0, 62);

    return file.Encode();
}

// With `imports`, the control changes of the MIDI file become NintendoWare's settings on track 0, one every 48 ticks:
// the modulation depth, the portamento, the envelope, the low-pass filter, the biquad filter's value, monophonic mode
// and the transpose. No event moves. The song warns of a control change that it leaves out and of a value that it can't
// finish. Without `imports`, the same file leaves them out.
void TestControllers()
{
    const edit::MidiSong song = edit::ImportedSong(Controllers(), "song");
    XSFLIB_CHECK(song.moved == 0);
    const std::vector<Setting> settings = SettingsAt(song.timeline, "SMF_song_Track_0");
    uint32_t tick = 0;
    const auto next = [&tick]
    {
        tick += 48;
        return tick;
    };
    XSFLIB_CHECK(Holds(settings, next(), Kind::kLfoDepth, 64 * (kOne / 128)));
    XSFLIB_CHECK(Holds(settings, next(), Kind::kPortamentoTime, 20 * kOne));
    XSFLIB_CHECK(Holds(settings, next(), Kind::kPortamento, kOne));
    XSFLIB_CHECK(Holds(settings, next(), Kind::kPortamento, 0));
    XSFLIB_CHECK(Holds(settings, next(), Kind::kRelease, 30 * kOne));
    XSFLIB_CHECK(Holds(settings, next(), Kind::kAttack, 40 * kOne));
    XSFLIB_CHECK(Holds(settings, next(), Kind::kLowPass, -32 * (kOne / 64)));
    XSFLIB_CHECK(Holds(settings, next(), Kind::kDecay, 50 * kOne));
    XSFLIB_CHECK(Holds(settings, next(), Kind::kPortamentoFrom, 72 * kOne));
    XSFLIB_CHECK(Holds(settings, next(), Kind::kLowPass, -16 * (kOne / 64)));
    XSFLIB_CHECK(Holds(settings, next(), Kind::kBiquadValue, 9 * kOne));
    XSFLIB_CHECK(Holds(settings, next(), Kind::kMonophonic, kOne));
    XSFLIB_CHECK(Holds(settings, next(), Kind::kMonophonic, 0));
    XSFLIB_CHECK(Holds(settings, next(), Kind::kTranspose, 200 * kOne));

    // What stays out, and what the song can't finish from the file.
    XSFLIB_CHECK(WarnsOf(song.warnings, "1 event left out: control change 76"));
    XSFLIB_CHECK(WarnsOf(song.warnings, "the biquad filter value"));
    XSFLIB_CHECK(!WarnsOf(song.warnings, "control change 1."));

    // Without `imports`, the same file leaves them out.
    const edit::MidiSong plain = edit::SongOfMidi(Controllers(), "song");
    XSFLIB_CHECK(WarnsOf(plain.warnings, "control change 1."));
    const std::vector<Setting> none = SettingsAt(plain.timeline, "SMF_song_Track_0");
    XSFLIB_CHECK(std::ranges::none_of(none, [](const Setting& s) { return std::get<1>(s) == Kind::kLfoDepth; }));
}

// Control change 102 lowers a SoundFont's cutoff by 75 cents a step, from 13,500 cents. The song uses the low-pass
// whose cutoff is nearest. Control change 103 in the same file sets the low-pass itself, and 102 then stays out.
void TestFilterController()
{
    const auto song_of = [](bool both)
    {
        midi::FileBuilder file(48);
        midi::TrackBuilder& track = file.AddTrack();
        track.Control(0, 0, 102, 0);
        track.Control(48, 0, 102, 40);
        if (both)
        {
            track.Control(96, 0, 103, 8);
        }
        track.NoteOn(144, 0, 60, 100);
        track.NoteOff(192, 0, 60);
        return edit::ImportedSong(file.Encode(), "song");
    };

    // The cutoff of value 40, against the cutoffs in the table for the low-pass steps next to the step of the song.
    const std::vector<Setting> settings = SettingsAt(song_of(false).timeline, "SMF_song_Track_0");
    XSFLIB_CHECK(Holds(settings, 0, Kind::kLowPass, 0));
    const auto chosen =
        std::ranges::find_if(settings, [](const Setting& setting)
                             { return std::get<0>(setting) == 48 && std::get<1>(setting) == Kind::kLowPass; });
    SEQLIB_REQUIRE(chosen != settings.end());
    const int64_t low_pass = 64 + std::get<3>(*chosen) / (kOne / 64);
    export_::Settings switch_settings;
    switch_settings.engine = export_::Engine::kSwitch;
    const auto cutoff_of = [&](int64_t held)
    {
        const int step = export_::TrackFilters(static_cast<int>(held), 0, 0, switch_settings).low_pass;
        return step < 0
                   ? INT_MAX
                   : std::abs(int{export_::kSwitchFilters.low_pass_cutoff[static_cast<std::size_t>(step)]} - 10500);
    };
    XSFLIB_CHECK(low_pass > 0 && low_pass < 63);
    XSFLIB_CHECK(cutoff_of(low_pass) <= cutoff_of(low_pass - 1) && cutoff_of(low_pass) <= cutoff_of(low_pass + 1));

    const std::vector<Setting> exact = SettingsAt(song_of(true).timeline, "SMF_song_Track_0");
    XSFLIB_CHECK(Holds(exact, 96, Kind::kLowPass, -8 * (kOne / 64)));
    XSFLIB_CHECK(std::ranges::none_of(exact, [](const Setting& setting) { return std::get<0>(setting) == 48; }));
}

// A MIDI file of `division` ticks a quarter note, with notes at the ticks of `starts`, each a quarter note long.
std::vector<uint8_t> NotesAt(uint16_t division, const std::vector<uint32_t>& starts)
{
    midi::FileBuilder file(division);
    midi::TrackBuilder& track = file.AddTrack();
    for (const uint32_t start : starts)
    {
        track.NoteOn(start, 0, 60, 100);
        track.NoteOff(start + division, 0, 60);
    }

    return file.Encode();
}

void TestTicks()
{
    // On the ticks of 48: no event moves.
    XSFLIB_CHECK(edit::ImportedSong(NotesAt(480, {0, 480, 490}), "song").moved == 0);

    // On the ticks of 96 only: the song uses that timebase, and no event moves.
    const edit::MidiSong fine = edit::ImportedSong(NotesAt(96, {0, 1, 97}), "song");
    XSFLIB_CHECK(fine.moved == 0);
    XSFLIB_CHECK(std::ranges::any_of(fine.timeline.items, [](const Command& command)
                                     { return command.kind == Kind::kTimebase && command.value.value == 96 * kOne; }));

    // Between the ticks of 96 too: the song keeps 48 ticks a quarter note, and both notes move to tick 0. The events
    // of both notes move, the note-off included.
    const edit::MidiSong moved = edit::ImportedSong(NotesAt(480, {1, 4}), "song");
    XSFLIB_CHECK(moved.moved == 4);
    XSFLIB_CHECK(WarnsOf(moved.warnings, "4 events fall between ticks"));

    // With the DS's one timebase, the note at 3/480 is nearest tick 0 of 48, and the note at 7/480 nearest tick 1.
    const edit::MidiSong nearest = edit::ImportedSong(NotesAt(480, {3, 7}), "song", 48);
    uint32_t tick = 0;
    std::vector<uint32_t> notes;
    uint32_t item = 0;
    for (const auto& known : nearest.timeline.labels)
    {
        if (known.name == "SMF_song_Track_0")
        {
            item = known.item;
        }
    }
    for (; item < nearest.timeline.items.size(); ++item)
    {
        const Command& command = nearest.timeline.items[item];
        if (command.kind == Kind::kWait)
        {
            tick += static_cast<uint32_t>(command.value.value / kOne);
        }
        else if (command.kind == Kind::kNote)
        {
            notes.push_back(tick);
        }
        else if (command.kind == Kind::kEnd)
        {
            break;
        }
    }
    XSFLIB_CHECK(notes == std::vector<uint32_t>({0, 1}));
}

// A note of the loop that outlasts a pass would sound under its next pass.
void TestLongNotesInLoop()
{
    midi::FileBuilder file(96);
    midi::TrackBuilder& track = file.AddTrack();
    track.Meta(96, 0x06, "loopStart");
    track.Meta(288, 0x06, "loopEnd");
    // Before the loop, over its end by less than a pass, and over its end by more than a pass.
    track.NoteOn(0, 0, 60, 100);
    track.NoteOff(1000, 0, 60);
    track.NoteOn(192, 0, 62, 100);
    track.NoteOff(341, 0, 62);
    track.NoteOn(240, 0, 64, 100);
    track.NoteOff(2000, 0, 64);

    const edit::MidiSong song = edit::ImportedSong(file.Encode(), "song", 96);
    std::map<int64_t, int64_t> lengths;
    for (const Command& command : song.timeline.items)
    {
        if (command.kind == Kind::kNote)
        {
            lengths[command.key] = command.value.value / kOne;
        }
    }
    XSFLIB_CHECK(lengths.size() == 3);
    XSFLIB_CHECK(lengths[60] == 1000);
    XSFLIB_CHECK(lengths[62] == 149);
    XSFLIB_CHECK(lengths[64] == 192);
    XSFLIB_CHECK(WarnsOf(song.warnings, "1 note lasts longer than one pass of the loop"));

    // A pass longer than every note changes no length and adds no warning.
    midi::FileBuilder short_notes(96);
    midi::TrackBuilder& other = short_notes.AddTrack();
    other.Meta(0, 0x06, "loopStart");
    other.Meta(960, 0x06, "loopEnd");
    other.NoteOn(0, 0, 60, 100);
    other.NoteOff(960, 0, 60);
    XSFLIB_CHECK(!WarnsOf(edit::ImportedSong(short_notes.Encode(), "song", 96).warnings, "lasts longer than one pass"));
}

void TestChannels()
{
    // 18 MIDI tracks of notes, and a channel without notes among them.
    midi::FileBuilder file(48);
    midi::TrackBuilder& controls = file.AddTrack();
    controls.Name("controls");
    controls.Control(0, 3, 7, 100);
    for (int t = 0; t < 18; ++t)
    {
        midi::TrackBuilder& track = file.AddTrack();
        track.Name("part" + std::to_string(t));
        track.NoteOn(0, 0, 60 + t, 100);
        track.NoteOff(48, 0, 60 + t);
    }
    const edit::MidiSong song = edit::ImportedSong(file.Encode(), "song");
    XSFLIB_CHECK(WarnsOf(song.warnings, "part16, part17"));
    XSFLIB_CHECK(!WarnsOf(song.warnings, "part15"));
    XSFLIB_CHECK(song.timeline.items.at(0).kind == Kind::kAllocateTracks);
    XSFLIB_CHECK(song.timeline.items.at(0).value.value == int64_t{0xFFFF} * kOne);
    const auto note_keys = [&](const std::string& label)
    {
        std::vector<int> keys;
        uint32_t item = 0;
        for (const auto& known : song.timeline.labels)
        {
            if (known.name == label)
            {
                item = known.item;
            }
        }
        for (; item < song.timeline.items.size() && song.timeline.items[item].kind != Kind::kEnd; ++item)
        {
            if (song.timeline.items[item].kind == Kind::kNote)
            {
                keys.push_back(song.timeline.items[item].key);
            }
        }
        return keys;
    };
    XSFLIB_CHECK(note_keys("SMF_song_Track_0") == std::vector<int>({60}));
    XSFLIB_CHECK(note_keys("SMF_song_Track_15") == std::vector<int>({75}));
}

// A file is a MIDI file by its MThd header. A RIFF file isn't one. A MIDI file without notes is refused.
void TestRefusals()
{
    XSFLIB_CHECK(edit::IsMidi(NotesAt(48, {0})));
    XSFLIB_CHECK(!edit::IsMidi(std::vector<uint8_t>{'M', 'T', 'h'}));
    XSFLIB_CHECK(!edit::IsMidi(std::vector<uint8_t>{'R', 'I', 'F', 'F', 0, 0}));
    midi::FileBuilder silent(48);
    silent.AddTrack().Control(0, 0, 7, 100);
    const bool refused = !xsflib::testing::ErrorOf<std::invalid_argument>(
                              [&]
                              {
                                  edit::ReadIncomingMidi(silent.Encode(), "silent.mid",
                                                         song::Console{.engine = hokuto::format::Engine::kSwitch});
                              })
                              .empty();
    XSFLIB_CHECK(refused);
}

// A MIDI file of two channels: two notes on the first, and a note after a bar on the second. With `banks`, the first
// channel plays program 2 of bank slot 1, and the second program 5 of slot 0.
std::vector<uint8_t> SmallSong(bool banks)
{
    midi::FileBuilder file(480);
    midi::TrackBuilder& first = file.AddTrack();
    first.Tempo(0, 500000);
    if (banks)
    {
        first.Bank(0, 0, 1);
        first.Program(0, 0, 2);
    }
    first.NoteOn(0, 0, 60, 100);
    first.NoteOff(480, 0, 60);
    first.NoteOn(960, 0, 64, 80);
    first.NoteOff(1200, 0, 64);
    midi::TrackBuilder& second = file.AddTrack();
    if (banks)
    {
        second.Program(0, 1, 5);
    }
    second.NoteOn(1920, 1, 72, 127);
    second.NoteOff(2160, 1, 72);

    return file.Encode();
}

// Checks the notes of `read`, a read of SmallSong in ticks of 48 to a quarter note.
void CheckSmallSong(const song::Sequence& read, bool banks)
{
    SEQLIB_REQUIRE(read.tracks.size() == 2);
    const std::vector<song::Note>& first = read.tracks[0].notes;
    const std::vector<song::Note>& second = read.tracks[1].notes;
    SEQLIB_REQUIRE(first.size() == 2 && second.size() == 1);
    SEQLIB_CHECK_EQ(first[0].start, 0u);
    SEQLIB_CHECK_EQ(first[0].length, 48u);
    SEQLIB_CHECK_EQ(first[0].key, 60);
    SEQLIB_CHECK_EQ(first[0].velocity, 100);
    SEQLIB_CHECK_EQ(first[1].start, 96u);
    SEQLIB_CHECK_EQ(first[1].length, 24u);
    SEQLIB_CHECK_EQ(first[1].key, 64);
    SEQLIB_CHECK_EQ(first[1].velocity, 80);
    SEQLIB_CHECK_EQ(second[0].start, 192u);
    SEQLIB_CHECK_EQ(second[0].length, 24u);
    SEQLIB_CHECK_EQ(second[0].key, 72);
    SEQLIB_CHECK_EQ(second[0].velocity, 127);
    if (banks)
    {
        SEQLIB_CHECK_EQ(first[0].program, 2u);
        SEQLIB_CHECK_EQ(first[0].bank_slot, 1);
        SEQLIB_CHECK_EQ(second[0].program, 5u);
        SEQLIB_CHECK_EQ(second[0].bank_slot, 0);
    }
}

void TestLaterConsole(format::Engine engine, sar::Flavor flavor)
{
    // A bank whose programs play a wave, in two bank slots.
    sar_files::Region region;
    region.original_key = 60;
    sar_files::VelocityTable velocities;
    velocities.entries = {{0, region}};
    sar_files::KeyTable keys;
    keys.entries = {{0, velocities}};
    sar_files::BankSpec bank;
    bank.waves = {{sar::ItemId(sar::kWaveArchiveItem, 0), 0}};
    bank.instruments.assign(6, keys);
    const format::File waves = edit::AddWaveArchive(
        format::NewFile(engine), "",
        sar_files::WaveArchive(flavor,
                               {sar_files::Wave(flavor, sar_files::ConstantWave(flavor == sar::Flavor::kCafe))}));
    const format::File banked =
        edit::AddBank(waves, "BANK_NEW", sar_files::Bank(flavor, bank), {{sar::ItemId(sar::kWaveArchiveItem, 0), 0}});

    // The song is imported the way File > Import > Sequence… imports it.
    const edit::Incoming incoming = edit::ReadIncomingMidi(SmallSong(true), "small song.mid", edit::ConsoleOf(banked));
    XSFLIB_CHECK(incoming.midi && !incoming.wii && incoming.flavor == flavor);
    const format::File added =
        edit::AddSequenceSound(banked, std::nullopt, "SEQ_SMALL", incoming.sequence, edit::kEmptySongStart, {},
                               std::vector<uint32_t>(2, sar::ItemId(sar::kBankItem, 0)));

    // The sequence is a converter's song. The program edits it as a timeline.
    const format::File opened = edit::WithNewModes(added, std::vector<uint32_t>{0});
    XSFLIB_CHECK(edit::IsTimeline(opened, 0));
    const format::File saved = edit::FoldTimelines(opened);
    CheckSmallSong(
        song::ReadSequence(
            *song::LoadArchive(format::ExportArchive(saved), "small.bfsar", format::ExportGroupFiles(saved)), 0),
        true);
}

// A MIDI file is imported into a file of the Wii as a sequence sound, and the archive that the file builds plays the
// small song.
void TestWii()
{
    const format::File file = edit::AddWiiBank(format::NewFile(format::Engine::kWii), "BANK_NEW",
                                               rsar_files::MakeNewBank(), rsar_files::MakeRwar());
    const song::Console console = edit::ConsoleOf(file);
    const edit::Incoming incoming = edit::ReadIncomingMidi(SmallSong(false), "small.mid", console);
    XSFLIB_CHECK(incoming.midi && incoming.wii && incoming.converted);
    const format::File added =
        edit::AddWiiSequenceSound(file, std::nullopt, "SEQ_SMALL", incoming.sequence, edit::kEmptySongStart, {});
    const auto archive =
        song::LoadWiiArchive(format::ExportArchive(added), "small.brsar", ntdlib::rvl::snd::kRsarOptions);
    CheckSmallSong(song::ReadSequence(*archive, 0, {}, {}, console.wii.value()), false);
}

// A song on MIDI's drum channel, 10, at 48 ticks a quarter note: program 1, notes 36 and 38 on a quarter note's start,
// and note 36 and note 42 later. Channel 1 plays note 36 too.
std::vector<uint8_t> DrumSong()
{
    midi::FileBuilder file(48);
    midi::TrackBuilder& track = file.AddTrack();
    track.Program(0, 9, 1);
    track.NoteOn(0, 9, 36, 100);
    track.NoteOn(0, 9, 38, 90);
    track.NoteOff(24, 9, 36);
    track.NoteOff(24, 9, 38);
    track.NoteOn(48, 9, 36, 80);
    track.NoteOff(72, 9, 36);
    track.NoteOn(96, 9, 42, 70);
    track.NoteOff(120, 9, 42);
    track.NoteOn(0, 0, 36, 60);
    track.NoteOff(24, 0, 36);

    return file.Encode();
}

// What the track that selects a program plays, in order: each program and each note's key,
// a note's key as its negative less 1.
std::vector<int> DrumCommands(const Sequence& sequence)
{
    std::vector<int> commands;
    bool drums = false;
    for (const Command& command : sequence.items)
    {
        if (command.kind == Kind::kProgram)
        {
            commands.push_back(static_cast<int>(command.value.value / kOne));
            drums = true;
        }
        else if (command.kind == Kind::kNote && drums)
        {
            commands.push_back(-1 - command.key);
        }
        else if (command.kind == Kind::kEnd)
        {
            drums = false;
        }
    }

    return commands;
}

void TestDrumNotes()
{
    // The file's notes on the drum channel, in order, and the slot that control change 0 selects before them.
    const edit::MidiDrums drums = edit::DrumNotesOf(DrumSong());
    XSFLIB_CHECK(drums.slot == 0);
    XSFLIB_CHECK((drums.notes == std::vector<int>{36, 38, 42}));
    midi::FileBuilder with_slot(48);
    with_slot.AddTrack().Bank(0, 9, 2);
    with_slot.AddTrack().NoteOn(48, 9, 40, 100);
    XSFLIB_CHECK(edit::DrumNotesOf(with_slot.Encode()).slot == 2);
    XSFLIB_CHECK(edit::DrumNotesOf(std::vector<uint8_t>{1, 2, 3}).notes.empty());

    // Without a pairing, each note plays as in the file.
    XSFLIB_CHECK((DrumCommands(edit::ImportedSong(DrumSong(), "song").timeline) ==
                  std::vector<int>{1, -1 - 36, -1 - 38, -1 - 36, -1 - 42}));

    // A pairing selects the note's program before it, and the note uses the key of the pairing. A note without a
    // pairing uses the file's program again.
    const format::MidiDrumNotes pairings = {{36, {.program = 3, .key = 60}}};
    XSFLIB_CHECK((DrumCommands(edit::ImportedSong(DrumSong(), "song", 96, pairings).timeline) ==
                  std::vector<int>{1, 3, -1 - 60, 1, -1 - 38, 3, -1 - 60, 1, -1 - 42}));

    // A note of the same key on another channel stays out of the pairings. The note's length stays.
    const edit::MidiSong song = edit::ImportedSong(DrumSong(), "song", 96, pairings);
    int thirty_sixes = 0;
    for (const Command& command : song.timeline.items)
    {
        thirty_sixes += command.kind == Kind::kNote && command.key == 36 ? 1 : 0;
    }
    XSFLIB_CHECK(thirty_sixes == 1);

    // A DS song uses the pairings too. In the new commands, the paired program replaces the program.
    const edit::DsMidiFile ds = edit::DsFileOfMidi(DrumSong(), "song.mid", {{{.slot = 0, .number = 1}, 7}}, pairings);
    const std::optional<Sequence> read = ntdlib::sdat::ReadSequenceFile(ds.file);
    SEQLIB_REQUIRE(read.has_value());
    XSFLIB_CHECK((DrumCommands(*read) == std::vector<int>{7, 3, -1 - 60, 7, -1 - 38, 3, -1 - 60, 7, -1 - 42}));

    // The rows of the windows read the song's commands before the programs' pairings.
    XSFLIB_CHECK((DrumCommands(ds.song) == std::vector<int>{1, 3, -1 - 60, 1, -1 - 38, 3, -1 - 60, 1, -1 - 42}));
}

// A drum note plays the program and the key of its pairing on the console's model of a later console.
void TestDrumNotesPlay()
{
    sar_files::Region region;
    region.original_key = 60;
    sar_files::VelocityTable velocities;
    velocities.entries = {{0, region}};
    sar_files::KeyTable keys;
    keys.entries = {{0, velocities}};
    sar_files::BankSpec bank;
    bank.waves = {{sar::ItemId(sar::kWaveArchiveItem, 0), 0}};
    bank.instruments.assign(6, keys);
    const sar::Flavor flavor = sar::Flavor::kNx;
    const format::File waves =
        edit::AddWaveArchive(format::NewFile(format::Engine::kSwitch), "",
                             sar_files::WaveArchive(flavor, {sar_files::Wave(flavor, sar_files::ConstantWave(false))}));
    const format::File banked =
        edit::AddBank(waves, "BANK_NEW", sar_files::Bank(flavor, bank), {{sar::ItemId(sar::kWaveArchiveItem, 0), 0}});

    const format::MidiDrumNotes pairings = {{36, {.program = 3, .key = 60}}};
    const edit::Incoming incoming = edit::ReadIncomingMidi(DrumSong(), "drums.mid", edit::ConsoleOf(banked), pairings);
    const format::File added =
        edit::AddSequenceSound(banked, std::nullopt, "SEQ_DRUMS", incoming.sequence, edit::kEmptySongStart, {},
                               std::vector<uint32_t>(1, sar::ItemId(sar::kBankItem, 0)));
    const format::File opened = edit::WithNewModes(added, std::vector<uint32_t>{0});
    XSFLIB_CHECK(edit::IsTimeline(opened, 0));
    const format::File saved = edit::FoldTimelines(opened);
    const song::Sequence read = song::ReadSequence(
        *song::LoadArchive(format::ExportArchive(saved), "drums.bfsar", format::ExportGroupFiles(saved)), 0);
    SEQLIB_REQUIRE(read.tracks.size() == 2);

    // The drum channel's notes: their ticks, keys and programs.
    std::vector<std::tuple<uint32_t, int, int>> heard;
    for (const song::Track& track : read.tracks)
    {
        for (const song::Note& note : track.notes)
        {
            if (note.velocity != 60)
            {
                heard.emplace_back(note.start, note.key, static_cast<int>(note.program));
            }
        }
    }
    std::ranges::sort(heard);
    XSFLIB_CHECK(
        (heard == std::vector<std::tuple<uint32_t, int, int>>{{0, 38, 1}, {0, 60, 3}, {48, 60, 3}, {96, 42, 1}}));
}

// A MIDI file is imported into a file of the DS as a sequence sound beside a new wave archive and bank, and the archive
// that the file builds plays the small song.
void TestDs()
{
    const edit::DsMidiFile midi = edit::DsFileOfMidi(SmallSong(false), "small.mid");
    const std::vector<uint8_t> waves =
        ds_files::WaveArchive({ds_files::Wave{.loop = 1, .loop_length = 2, .data = {0x40, 0x40, 0xC0, 0xC0}}});
    const std::vector<uint8_t> bank = ds_files::Bank({ds_files::Instrument{.type = 1, .region = {.wave = 0}}});
    const format::File with_waves = edit::AddDsWaveArchive(format::NewFile(format::Engine::kDs), "WAVES_NEW", waves);
    const format::File with_bank = edit::AddDsBank(with_waves, "BANK_NEW", bank, {0, 0xFFFF, 0xFFFF, 0xFFFF});
    const format::File added = edit::AddDsSequenceSound(with_bank, "SEQ_SMALL", midi.file, {.bank = 1});
    const std::shared_ptr<const song::DsArchive> archive = song::LoadDsArchive(format::ExportArchive(added), {});
    CheckSmallSong(song::ReadSequence(*archive, 0), false);
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("midi_imports",
                                    []
                                    {
                                        TestControllers();
                                        TestFilterController();
                                        TestTicks();
                                        TestLongNotesInLoop();
                                        TestChannels();
                                        TestRefusals();
                                        TestLaterConsole(format::Engine::kSwitch, sar::Flavor::kNx);
                                        TestLaterConsole(format::Engine::kWiiU, sar::Flavor::kCafe);
                                        TestLaterConsole(format::Engine::k3ds, sar::Flavor::kCtr);
                                        TestWii();
                                        TestDs();
                                        TestDrumNotes();
                                        TestDrumNotesPlay();
                                    });
}
