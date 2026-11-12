// SPDX-License-Identifier: MIT

// Checks the edits of notes (src/edit/notes.h) on the songs of GBA ROMs that MusicPlayer2000 plays. The tool imports
// each ROM into a Hokuto file the way the program does (format::ImportGbaRom). Each song's sequence first has to come
// back unchanged from the rules of MusicPlayer2000's bytes without an edit (edit::Mp2kWritable). The tool then reads
// each song that plays (song::GbaSongs) with ntdlib's performer, and edits every note command that the song plays, in
// one version of the file. The commands take turns at four edits:
//
// - The note moves a tick later, or a tick earlier near the end of its block.
// - Its velocity goes one down, or one up from 1.
// - The note goes.
// - The note becomes a tick longer.
//
// A note that can't move or change its length changes its velocity instead: a tie, and a note of the longest length
// that MusicPlayer2000's bytes hold. So does a note whose song refuses its edit. The tool lists and counts each
// refusal. On Metroid Fusion's sequencer, a note reads no extra length (edit/mp2k.h): a longer note uses the next
// length that a code encodes. The tool then exports the ROM and checks two things:
//
// - The ROM, imported again, returns each edited sequence. A song that shrank can hold zeros after its items.
// - Each song plays the notes that it played before, each with its command's edit, and no other notes. The performer
//   reads the exported ROM.
//
// A ROM without free space has no room for a song that its edits make longer. The edits of those songs then stay out,
// and the tool counts the songs apart. With --filler, the songs are written to the filler runs of the ROM instead
// (format::NativeArchive::gba_filler), and every edit is kept.
//
// The second check counts some differences apart. A note can end sooner than its command says: its track or the song
// ends, another note cuts it, or its channel falls silent. A moved note can then end where it ended before, and a
// longer note at its old length or later, where its sample ends. The channel of a quieter note, such as a PSG channel,
// can fall silent sooner. A note that a move shifts to the end of the loop's first pass leaves the pass that the
// arrangement shows. A move can put a note on the tick of another note of the same key on its track, and the two then
// play as one. Two commands of one key on a track's tick play as one note too, and the note leads to one of them. A
// move or a removal of that command parts the two, and the other command's note then plays alone at the old place. The
// driver's channels play one note each, for every track: the DirectSound channels that the game's code assigns the song
// (ntdlib::mp2k::SongChannels), and the four PSG channels. A note that finds every channel of its kind playing takes
// one from a note of the same or a lower priority, or is dropped. A note whose time, before or after its edit, meets
// enough other notes of its kind to take every channel can therefore go, come, or end sooner. The notes in their
// releases hold channels too, and the check doesn't count them. A note that the edited song lists among the notes that
// the driver never starts therefore counts apart as well. A note that the channels now let through, or a note that the
// driver never started before, can play on the tick of another note of the same key on its track, and the two play as
// one, louder. When the performer marks another loop after the edits, the check compares only the notes before the
// earlier loop's end, cut there. Above a tick a frame, a note shorter than a frame's ticks starts and ends in one of
// the driver's frames at some ticks, and never sounds. Such a note can come or go when it moves or grows. A PSG
// channel's envelope moves in 16 steps, and a quiet note there can end in its first frames. Whether it sounds then
// depends on the state that the note before it leaves, and on the frame where it starts. A note of a PSG channel at
// half the full level or less, its velocity times the volume of its track, can therefore come or go.
//
// The tool also counts the songs whose notes collide more often on the driver's channels after the edits, and the
// notes that the edits cut short or keep from starting. An export of the ROM from the program lists them
// (song::AddedGbaCollisions). The function, given the two ROMs, has to find the same songs and notes.
//
// Before the edits, the tool works out what the editors show of each song the way the program does: the List Editor's
// rows of each track, the instrument of each track's first note and the marker row's places. A GBA song shows as a
// program, never as a timeline. A song whose view fails counts as a failure.
//
// The tool prints a line for each song that differs, and a summary. It returns 1 when a song differs, an edited
// sequence doesn't come back, or an edit, a read, a view or the export fails.
//
// usage: hokuto_check_gba_edits [--filler] <rom.gba>...

#include "check_gba_edits.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/mp2k/commands.h"
#include "ntdlib/mp2k/driver.h"
#include "ntdlib/mp2k/sequencer.h"
#include "ntdlib/mp2k/song.h"
#include "xsflib/console.h"
#include "xsflib/files.h"

#include "app/list_edits.h"
#include "app/markers.h"
#include "checked_archive.h"
#include "edit/bytecode.h"
#include "edit/commands.h"
#include "edit/instruments.h"
#include "edit/mp2k.h"
#include "edit/notes.h"
#include "edit/timelines.h"
#include "format/archive.h"
#include "format/file.h"
#include "notes_apart.h"
#include "song/gba.h"
#include "song/sequence.h"

namespace
{

using hokuto::edit::NoteEdit;
using hokuto::format::File;
using hokuto::song::Note;
using hokuto::song::Sequence;
using hokuto::tools::gba_edits::Change;
using hokuto::tools::gba_edits::ChannelOf;
using hokuto::tools::gba_edits::Compare;
using hokuto::tools::gba_edits::Plan;
using hokuto::tools::gba_edits::Totals;
using ntdlib::commands::Command;
using ntdlib::commands::kOne;

// The item of each offset in `sequence`'s bytes.
std::map<uint32_t, uint32_t> ItemsByOffset(const File& file, uint32_t sequence)
{
    const std::vector<uint32_t> offsets =
        hokuto::edit::Offsets(*file.sequences[sequence], hokuto::edit::ConsoleOf(file));
    std::map<uint32_t, uint32_t> items;
    for (uint32_t item = 0; item + 1 < offsets.size(); item++)
    {
        items.emplace(offsets[item], item);
    }

    return items;
}

// Whether sequence `sequence` of `file` comes back unchanged from Mp2kWritable.
bool Writable(const File& file, uint32_t sequence)
{
    hokuto::edit::Edited edited{file, {}, {}};
    for (uint32_t item = 0; item < file.sequences[sequence]->items.size(); item++)
    {
        edited.items.push_back(item);
    }

    return *hokuto::edit::Mp2kWritable(std::move(edited), sequence).file.sequences[sequence] ==
           *file.sequences[sequence];
}

// The length that the longer edit sets for a note of `ticks`: a tick more, or on Metroid Fusion's sequencer
// (`short_notes`) the next length that a code encodes. Nothing past the longest note that the bytes hold without a tie.
std::optional<uint32_t> Longer(uint32_t ticks, bool short_notes)
{
    for (int code = 1; code <= 48; code++)
    {
        const auto length = static_cast<uint32_t>(ntdlib::mp2k::ClockLength(code));
        if (short_notes && length > ticks)
        {
            return length;
        }
    }
    if (short_notes || ticks + 1 > hokuto::edit::kLongestMp2kNote)
    {
        return std::nullopt;
    }

    return ticks + 1;
}

// The plans for the note commands of `song`, sequence `sequence` of `file`, by their offsets. On Metroid Fusion's
// sequencer (`short_notes`), a longer note uses the next length that a code encodes.
std::map<uint32_t, Plan> Plans(const File& file, uint32_t sequence, const Sequence& song, bool short_notes)
{
    const std::map<uint32_t, uint32_t> items = ItemsByOffset(file, sequence);
    // As in the Key Editor, a longer note can't pass the end of its track.
    std::map<uint32_t, hokuto::edit::NotePlace> by_command;
    for (const hokuto::song::Track& track : song.tracks)
    {
        const std::optional<uint32_t> track_end = hokuto::edit::TrackEndOf(file, sequence, track.stretches);
        for (const Note& note : track.notes)
        {
            by_command.emplace(note.command, hokuto::edit::NotePlace{.command = note.command,
                                                                     .stretch = track.stretches.at(note.stretch),
                                                                     .track_end = track_end});
        }
    }

    std::vector<hokuto::edit::NotePlace> places;
    for (const auto& [offset, place] : by_command)
    {
        places.push_back(place);
    }
    const std::vector<hokuto::edit::NoteLimits> limits = hokuto::edit::LimitsOf(file, sequence, places);

    std::map<uint32_t, Plan> plans;
    std::size_t turn = 0;
    for (std::size_t i = 0; i < places.size(); i++)
    {
        const Command& command = file.sequences[sequence]->items.at(items.at(places[i].command));
        Plan plan;
        plan.edit.command = places[i].command;
        plan.edit.stretch = places[i].stretch;
        plan.edit.track_end = places[i].track_end;
        plan.ticks = static_cast<uint32_t>(command.value.value / kOne);
        plan.velocity = static_cast<int>(command.velocity / kOne);
        const hokuto::edit::NoteLimits& limit = limits[i];
        switch (turn++ % 4)
        {
        case 0:
            if (limit.shift && limit.shift->later >= 1)
            {
                plan.change = Change::kMove;
                plan.edit.shift = 1;
            }
            else if (limit.shift && limit.shift->earlier >= 1)
            {
                plan.change = Change::kMove;
                plan.edit.shift = -1;
            }
            break;
        case 2:
            if (limit.shift)
            {
                plan.change = Change::kRemove;
                plan.edit.remove = true;
            }
            break;
        case 3:
            if (const std::optional<uint32_t> longer = limit.length ? Longer(*limit.length, short_notes) : std::nullopt;
                limit.shift && longer)
            {
                plan.change = Change::kLonger;
                plan.edit.length = *longer;
            }
            break;
        default:
            break;
        }
        if (plan.change == Change::kVelocity)
        {
            plan.edit.velocity = plan.velocity > 1 ? plan.velocity - 1 : plan.velocity + 1;
        }
        plans.emplace(places[i].command, std::move(plan));
    }

    return plans;
}

// The edits of `plans`.
std::vector<NoteEdit> EditsOf(const std::map<uint32_t, Plan>& plans)
{
    std::vector<NoteEdit> edits;
    for (const auto& [offset, plan] : plans)
    {
        edits.push_back(plan.edit);
    }

    return edits;
}

// Makes the edits of `plans` to sequence `sequence` of `file`. An edit that the sequence refuses becomes a change of
// velocity, with a line. Returns the file after the edits.
File Edit(const File& file, uint32_t sequence, const std::string& name, std::map<uint32_t, Plan>& plans, Totals& totals)
{
    try
    {
        return hokuto::edit::EditNotes(file, sequence, EditsOf(plans)).file;
    }
    catch (const std::invalid_argument&)
    {
    }

    for (auto& [offset, plan] : plans)
    {
        if (plan.change == Change::kVelocity)
        {
            continue;
        }
        try
        {
            hokuto::edit::EditNotes(file, sequence, std::span(&plan.edit, 1));
        }
        catch (const std::invalid_argument& error)
        {
            std::printf("%s: the command at 0x%X refuses its edit: %s\n", name.c_str(), offset, error.what());
            totals.refused++;
            plan.change = Change::kVelocity;
            plan.edit = NoteEdit{.command = plan.edit.command, .stretch = plan.edit.stretch};
            plan.edit.velocity = plan.velocity > 1 ? plan.velocity - 1 : plan.velocity + 1;
        }
    }

    return hokuto::edit::EditNotes(file, sequence, EditsOf(plans)).file;
}

// Whether `got`, a sequence that a cut of the exported ROM returns, contains `want`: its bytes start with those of
// `want`, where a note reads up to `note_arguments` bytes. A song that shrank leaves zeros in its place up to its
// header, and the cut reads them as data after the items of the song (ntdlib::mp2k::Join).
bool Holds(const ntdlib::commands::Sequence& want, const ntdlib::commands::Sequence& got, int note_arguments)
{
    const std::vector<uint8_t> wanted = ntdlib::mp2k::EncodeSequence(want, ntdlib::agb::kRomBase, note_arguments).bytes;
    const std::vector<uint8_t> held = ntdlib::mp2k::EncodeSequence(got, ntdlib::agb::kRomBase, note_arguments).bytes;
    return held.size() >= wanted.size() && std::equal(wanted.begin(), wanted.end(), held.begin()) &&
           std::all_of(held.begin() + static_cast<std::ptrdiff_t>(wanted.size()), held.end(),
                       [](uint8_t b) { return b == 0; });
}

// Works out the way the program does what the editors show of `song`, sequence `sequence` of `file`: the List Editor's
// rows of each track, the instrument of each track's first note, the marker row's places and whether the song draws
// random numbers. A GBA song opens as a program: AsTimeline has to refuse it. Prints a line for each problem, and
// returns their number.
std::size_t CheckViews(const std::string& name, const File& file, uint32_t sequence, const Sequence& song)
{
    std::size_t problems = 0;
    const auto attempt = [&](const char* what, const auto& work)
    {
        try
        {
            work();
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s throws: %s\n", name.c_str(), what, error.what());
            problems++;
        }
    };
    for (const hokuto::song::Track& track : song.tracks)
    {
        attempt("MakeListTrack", [&] { hokuto::app::MakeListTrack(file, sequence, song, track.number); });
        attempt("OpeningInstrument", [&] { hokuto::edit::OpeningInstrument(file, sequence, track); });
    }
    attempt("LogicMarkers", [&] { hokuto::app::LogicMarkers(file, sequence, song); });
    attempt("DrawsRandom", [&] { hokuto::edit::DrawsRandom(file, sequence, song); });
    attempt("IsTimeline",
            [&]
            {
                if (hokuto::edit::IsTimeline(file, sequence) || hokuto::edit::TimelineLoop(file, sequence))
                {
                    throw std::logic_error("the song shows as a timeline");
                }
            });
    try
    {
        hokuto::edit::AsTimeline(file, sequence);
        std::printf("%s: AsTimeline takes the song\n", name.c_str());
        problems++;
    }
    catch (const std::invalid_argument&)
    {
    }
    catch (const std::exception& error)
    {
        std::printf("%s: AsTimeline throws: %s\n", name.c_str(), error.what());
        problems++;
    }

    return problems;
}

// An edited song: its sequence, how it played before, and the plans of its edits.
struct Edited
{
    int song = 0;
    uint32_t sequence = 0;
    Sequence before;
    std::map<uint32_t, Plan> plans;
};

// Checks that every sequence of `file`, the ROM `rom_name`, comes back without an edit.
void CheckUnedited(const File& file, const std::string& rom_name, Totals& totals)
{
    for (uint32_t sequence = 0; sequence < file.sequences.size(); sequence++)
    {
        totals.sequences++;
        if (!Writable(file, sequence))
        {
            std::printf("%s: sequence %u doesn't come back from MusicPlayer2000's rules without an edit\n",
                        rom_name.c_str(), sequence);
            totals.unwritable++;
        }
    }
}

// Edits each song of `rom` that plays, once for each sequence, in `current`, and returns the edits. `file` is the ROM's
// Hokuto file.
std::vector<Edited> EditSongs(const File& file, const hokuto::song::GbaRom& rom, const std::string& rom_name,
                              File& current, Totals& totals)
{
    std::vector<Edited> edited;
    std::set<uint32_t> seen;
    for (const int song : hokuto::song::GbaSongs(rom))
    {
        const std::optional<uint32_t> sequence = hokuto::format::GbaSongSequence(file, rom.info.song_table, song);
        if (!sequence || !seen.insert(*sequence).second)
        {
            continue;
        }

        const std::string name = rom_name + ": " + hokuto::song::GbaSongName(rom, song);
        try
        {
            Edited entry{song, *sequence, hokuto::song::ReadSequence(rom, song), {}};
            totals.failed += CheckViews(name, current, *sequence, entry.before);
            entry.plans = Plans(current, *sequence, entry.before, rom.info.metroid_short_notes);
            if (entry.plans.empty())
            {
                continue;
            }
            current = Edit(current, *sequence, name, entry.plans, totals);
            edited.push_back(std::move(entry));
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s\n", name.c_str(), error.what());
            totals.failed++;
        }
    }

    return edited;
}

// The Hokuto file and the ROM that the export of an edited ROM produces.
struct Exported
{
    File again;
    std::shared_ptr<const hokuto::song::GbaRom> played;
};

// Exports `current`, `file` with the edits of `edited`, or nothing when the export fails. Where the ROM has no room for
// the songs that their edits make longer, their edits leave `edited` and `current`.
std::optional<Exported> ExportEdits(const File& file, const std::string& rom_name, File& current,
                                    std::vector<Edited>& edited, Totals& totals)
{
    std::vector<uint8_t> exported;
    File again;
    std::shared_ptr<const hokuto::song::GbaRom> played;
    try
    {
        try
        {
            exported = hokuto::format::ExportArchive(current);
        }
        catch (const std::invalid_argument& error)
        {
            const std::vector<uint32_t> moved = hokuto::format::MovedSongs(current);
            if (moved.empty() || std::string(error.what()).find("32 MiB") == std::string::npos)
            {
                throw;
            }

            const std::set<uint32_t> outgrown(moved.begin(), moved.end());
            std::printf("%s: %zu songs outgrow their places, and the ROM has no room for them. Their edits stay out.\n",
                        rom_name.c_str(), outgrown.size());
            totals.outgrown += outgrown.size();
            std::erase_if(edited, [&outgrown](const Edited& entry) { return outgrown.contains(entry.sequence); });
            current = file;
            for (const Edited& entry : edited)
            {
                current = hokuto::edit::EditNotes(current, entry.sequence, EditsOf(entry.plans)).file;
            }
            exported = hokuto::format::ExportArchive(current);
        }
        again = hokuto::format::ImportGbaRom(exported);
        played = hokuto::song::LoadGbaRom(exported);
    }
    catch (const std::exception& error)
    {
        std::printf("%s: the export fails: %s\n", rom_name.c_str(), error.what());
        totals.failed++;
        return std::nullopt;
    }

    return Exported{std::move(again), std::move(played)};
}

// Checks each edit of `edited` in `played`, the exported ROM, and `again`, its Hokuto file: the edited sequence comes
// back, the song plays the edits, and the export counts the collisions that the edits add. `current` is the edited
// file, and `rom` the ROM before the edits.
void CheckExportedEdits(const hokuto::song::GbaRom& rom, const File& current, const File& again,
                        const hokuto::song::GbaRom& played, const std::vector<Edited>& edited,
                        const std::string& rom_name, Totals& totals)
{
    std::vector<int> songs;
    std::vector<hokuto::song::AddedCollisions> added;
    for (const Edited& entry : edited)
    {
        const std::string name = rom_name + ": " + hokuto::song::GbaSongName(rom, entry.song);
        totals.songs++;
        songs.push_back(entry.song);
        for (const auto& [offset, plan] : entry.plans)
        {
            totals.commands++;
            totals.moved += plan.change == Change::kMove;
            totals.velocities += plan.change == Change::kVelocity;
            totals.removed += plan.change == Change::kRemove;
            totals.longer += plan.change == Change::kLonger;
        }
        const std::optional<uint32_t> sequence =
            hokuto::format::GbaSongSequence(again, played.info.song_table, entry.song);
        if (!sequence || !Holds(*current.sequences[entry.sequence], *again.sequences[*sequence],
                                ntdlib::mp2k::NoteArguments(played.info)))
        {
            std::printf("%s: the exported ROM doesn't give back the edited sequence%s\n", name.c_str(),
                        sequence ? "" : ": the cut leaves it among the ROM's bytes");
            totals.wrong_sequences++;
        }
        try
        {
            const Sequence after = hokuto::song::ReadSequence(played, entry.song);
            ntdlib::mp2k::SongHeader header;
            ntdlib::mp2k::ReadSong(rom.rom, rom.info, entry.song, header);
            const ChannelOf channel_of = [&rom, &header](const Note& note)
            {
                return hokuto::tools::GbaPsgChannel(rom.rom, header.voices, note.program, note.key);
            };
            const int direct_channels = ntdlib::mp2k::SongChannels(rom.rom, rom.info, entry.song);
            totals.differ += Compare(name, entry.before, after, entry.plans, channel_of, direct_channels, totals);

            const hokuto::song::Collisions then = hokuto::song::CollisionsOf(entry.before);
            const hokuto::song::Collisions now = hokuto::song::CollisionsOf(after);
            const hokuto::song::Collisions more{now.cut > then.cut ? now.cut - then.cut : 0,
                                                now.unstarted > then.unstarted ? now.unstarted - then.unstarted : 0};
            if (more.cut > 0 || more.unstarted > 0)
            {
                added.push_back({entry.song, more});
                totals.colliding++;
                totals.added_cut += more.cut;
                totals.added_unstarted += more.unstarted;
            }
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s\n", name.c_str(), error.what());
            totals.failed++;
        }
    }

    if (hokuto::song::AddedGbaCollisions(rom, played, songs) != added)
    {
        std::printf("%s: the export's count of added collisions differs from the songs' reads\n", rom_name.c_str());
        totals.failed++;
    }
}

// Checks one ROM.
void CheckRom(const std::filesystem::path& path, Totals& totals)
{
    const std::string rom_name = xsflib::Utf8(path);
    std::vector<uint8_t> bytes;
    if (!xsflib::ReadFile(path, bytes))
    {
        std::printf("%s: can't be read\n", rom_name.c_str());
        totals.failed++;
        return;
    }

    File file;
    std::shared_ptr<const hokuto::song::GbaRom> rom;
    try
    {
        file = hokuto::tools::WithFillerChoice(hokuto::format::ImportGbaRom(bytes));
        rom = hokuto::song::LoadGbaRom(bytes);
    }
    catch (const xsflib::FormatError&)
    {
        return; // no MP2K, or no song that plays
    }
    totals.roms++;

    // Without an edit, every sequence comes back as it is.
    CheckUnedited(file, rom_name, totals);

    // Each song that plays, once for each sequence.
    File current = file;
    std::vector<Edited> edited = EditSongs(file, *rom, rom_name, current, totals);

    // The exported ROM returns the edited sequences, and plays the edits. A ROM without free space has no room for the
    // songs that their edits make longer. Their edits then stay out, and the others go in again.
    const std::optional<Exported> exported = ExportEdits(file, rom_name, current, edited, totals);
    if (!exported)
    {
        return;
    }

    CheckExportedEdits(*rom, current, exported->again, *exported->played, edited, rom_name, totals);
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> arguments = xsflib::console::Arguments(argc, argv);
    hokuto::tools::TakeFillerOption(arguments);
    if (arguments.size() < 2)
    {
        std::fprintf(stderr, "usage: hokuto_check_gba_edits [--filler] <rom.gba>...\n");
        return 2;
    }

    Totals totals;
    for (std::size_t i = 1; i < arguments.size(); i++)
    {
        CheckRom(xsflib::PathFromUtf8(arguments[i]), totals);
    }

    std::printf(
        "%zu ROMs, %zu sequences, %zu don't come back without an edit. %zu songs, %zu notes, %zu commands: "
        "%zu moved, %zu new velocities, %zu removed, %zu longer, %zu refused. %zu edited sequences don't come "
        "back. %zu songs differ, %zu failures. Apart: %zu notes that end otherwise than their commands say, %zu "
        "notes on the tick of another note of the same key, %zu notes whose channels other notes "
        "may take or keep, %zu notes that can start and end in one of the driver's frames, %zu quiet notes of the PSG "
        "channels, %zu notes moved out of the loop's first pass, %zu loops moved, %zu songs without room for their "
        "edits. %zu songs collide more often: %zu notes that later notes cut short, %zu that never start.\n",
        totals.roms, totals.sequences, totals.unwritable, totals.songs, totals.notes, totals.commands, totals.moved,
        totals.velocities, totals.removed, totals.longer, totals.refused, totals.wrong_sequences, totals.differ,
        totals.failed, totals.lengths, totals.together, totals.contested, totals.brief, totals.quiet, totals.past_loop,
        totals.loops, totals.outgrown, totals.colliding, totals.added_cut, totals.added_unstarted);

    return totals.unwritable + totals.wrong_sequences + totals.differ + totals.failed > 0 ? 1 : 0;
}
