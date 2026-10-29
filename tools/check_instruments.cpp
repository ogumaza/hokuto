// SPDX-License-Identifier: MIT

// Checks the edit of a track's instrument (src/edit/instruments.h) on a sound archive of the Wii, the 3DS, the Wii U or
// the Switch. The inspector's Bank and Program menus make this edit. The tool imports the archive into a Hokuto file
// the way the program does, and ntdlib's performer reads each sequence sound. Up to four sounds of each sequence file
// take part, spread over the sounds that play the file. The track with the most notes of each such sound takes two
// edits, each in a separate version of the file:
//
// - The notes that play the instrument of the track's first note take the next program of the same bank.
// - Those notes take the bank in the next slot of the sound that holds one. They keep their program where that bank has
//   it, and take its first program otherwise.
//
// A sound of the Wii plays one bank, and takes the first edit alone.
//
// The edit has to change nothing but the programs of program changes and the slots of bank selects. It can put in
// nothing but program changes, bank selects and a jump past them. ntdlib's performer then reads every sound of the
// sequence file again from the exported archive. The edited track has to play its notes as before: at the same ticks,
// with the same keys and velocities. Its notes that played the old instrument have to play the new one, and its other
// notes their instruments as before. An instrument can lack a region for the key or the velocity of a note. A key group
// can cut a note in the tick that it starts. The performer then leaves the note out. The tool counts apart the edits
// after which the track leaves out notes of the new instrument, and those after which it plays more of them.
//
// The engine sets where some notes end, such as a note that a one-shot wave ends, or a note that lasts to the end of
// the performance. The new instrument can end them elsewhere. The tool counts such notes apart. From the first end that
// moves, a sound's tracks can play otherwise. A note of length 0 on a track whose notes wait for their length holds the
// track until its sound ends. The random numbers follow the time, and the sound can end elsewhere. The tool counts
// apart the tracks that play as before up to that end, and otherwise after it. Every other track of the file's sounds
// has to play as before, unless it runs a command that the edit changed, or the command that opened the edited track.
// Such a track shares commands with the edited track, and is counted apart.
//
// In some sounds, the instrument determines more. A track that ties its notes or plays them monophonically plays a
// legato on a sounding channel. The performer shows a legato as part of the note that it continues. Where the new
// instrument has no channel sounding there, the legato starts a note of its own. A note of length 0 on a track whose
// notes wait for their length holds the track until the note ends, and the new instrument ends it elsewhere. The track
// then goes on at another time, and its random numbers and the tracks that it signals follow. The tool counts apart the
// tracks that play otherwise in a sound with such a track. Many of the 3DS's sounds have one.
//
// ntdlib's performer finds a sound's loop where every track's state repeats. After an edit, it can find the loop a
// whole number of passes earlier or later, and it stops at the end of the loop that it finds. The tool then compares
// the notes up to the earlier end, and counts such reads.
//
// The tool also checks what the inspector lists before an edit (src/edit/reach.h). Every other track of the edited
// sound that plays otherwise has to be among the tracks that the edited track's reach lists, unless it plays otherwise
// only from the first end that moves. Every other sound with a track that plays otherwise has to be among the reach's
// sounds. The tool counts the listed tracks and sounds, and those that played otherwise. It also gives the time of the
// slowest reach.
//
// The edit refuses some tracks, such as a track whose program change also selects the program of notes from another
// bank slot. The tool counts the refusals by their messages. It prints a line for each problem, and a summary. It
// returns 1 when a track plays otherwise, or when an edit, a read or the export fails. With a sound's name, only that
// sound takes part.
//
// With --model, the file goes into the model form before the edits (format::InModelForm), and the archive that the
// model builds has to be the game's. Of the files that the edits make, the first 64 and every 64th after them then have
// to save in both forms and open again as they were (tools/checked_archive.h).
//
// usage: hokuto_check_instruments [--model] <archive> [sound]

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "xsflib/console.h"
#include "xsflib/files.h"

#include "checked_archive.h"
#include "edit/bytecode.h"
#include "edit/instruments.h"
#include "edit/reach.h"
#include "edit/replace.h"
#include "format/file.h"
#include "loop_passes.h"
#include "song/archive.h"
#include "song/sequence.h"

namespace
{

namespace edit = hokuto::edit;
namespace song = hokuto::song;
using hokuto::format::File;
using hokuto::tools::CheckedArchive;
using hokuto::tools::EarlierEnd;

// The sounds of a sequence file that take part.
constexpr std::size_t kMostSounds = 4;

// The edits: another program of the bank, and another bank.
constexpr std::size_t kEdits = 2;
constexpr std::array<const char*, kEdits> kEditNames = {"program", "bank"};

// What the check counted, for the summary.
struct Totals
{
    std::size_t sequences = 0;
    std::size_t sounds = 0;
    std::size_t unreadable = 0;
    std::size_t taking_part = 0;
    std::array<std::size_t, kEdits> edits{};
    std::array<std::size_t, kEdits> without{};
    std::array<std::size_t, kEdits> fewer{};
    std::array<std::size_t, kEdits> more{};
    std::array<std::size_t, kEdits> notes{};
    std::array<std::size_t, kEdits> lengths{};
    std::array<std::size_t, kEdits> later{};
    std::array<std::size_t, kEdits> follow{};
    std::size_t sharing = 0;
    std::size_t reads = 0;
    std::size_t loops_apart = 0;
    std::size_t problems = 0;
    std::map<std::string, std::size_t> refusals;
    std::size_t listed_tracks = 0;
    std::size_t listed_sounds = 0;
    std::size_t reached_tracks = 0;
    std::size_t reached_sounds = 0;
    std::size_t unlisted = 0;
    double slowest_reach = 0.0; // seconds
};

// The archive as the tool reads it: its sounds, where each plays in the file, each sequence sound as ntdlib's performer
// read it, the sounds of each sequence file, the programs of the banks that the tool has read, and what an edit of each
// track of the sounds that took part reaches.
struct Archive
{
    std::string name;
    CheckedArchive archive;
    File file;
    std::vector<std::optional<edit::SoundPlace>> places;
    std::map<uint32_t, song::Sequence> songs;
    std::map<uint32_t, std::vector<uint32_t>> by_sequence;
    std::map<uint32_t, std::vector<uint32_t>> programs;
    std::map<uint32_t, std::vector<edit::TrackReach>> reaches;
};

// A note as the tool compares it: its start, key, velocity, bank slot and program.
using PlayedNote = std::tuple<uint32_t, uint8_t, uint8_t, int, int>;

// The notes of `track` as the tool compares them.
std::vector<PlayedNote> NotesOf(const song::Track& track)
{
    std::vector<PlayedNote> notes;
    for (const song::Note& note : track.notes)
    {
        notes.emplace_back(note.start, note.key, note.velocity, note.bank_slot, static_cast<int>(note.program));
    }

    return notes;
}

// The number of notes of `before` that end elsewhere in `after`. Both tracks play the same notes.
std::size_t LengthsChanged(const song::Track& before, const song::Track& after)
{
    std::size_t changed = 0;
    for (std::size_t i = 0; i < before.notes.size() && i < after.notes.size(); i++)
    {
        if (before.notes[i].length != after.notes[i].length)
        {
            changed++;
        }
    }

    return changed;
}

// A track of a sound as the tool compares it, before and after the edit: the notes that it played, the notes that it
// has to play, the notes that it plays, and how many notes at the start match. The edited track has to play the new
// instrument in place of the old one.
struct Comparison
{
    const song::Track* before = nullptr;
    const song::Track* after = nullptr; // nullptr where the track plays no more
    bool edited = false;
    std::size_t mapped = 0;
    std::vector<PlayedNote> expected;
    std::vector<PlayedNote> played;
    std::size_t same = 0;
};

// Whether `fewer` is `more` without some of its notes, each of which plays `to`.
bool WithoutNotesOf(const std::vector<PlayedNote>& fewer, const std::vector<PlayedNote>& more, const edit::Program& to)
{
    if (fewer.size() >= more.size())
    {
        return false;
    }

    std::size_t next = 0;
    for (const PlayedNote& note : more)
    {
        if (next < fewer.size() && note == fewer[next])
        {
            next++;
        }
        else if (std::get<3>(note) != to.slot || std::get<4>(note) != to.number)
        {
            return false;
        }
    }

    return next == fewer.size();
}

// The number of notes at the start of `played` that match those of `expected`.
std::size_t SameNotes(const std::vector<PlayedNote>& expected, const std::vector<PlayedNote>& played)
{
    std::size_t same = 0;
    while (same < expected.size() && same < played.size() && expected[same] == played[same])
    {
        same++;
    }

    return same;
}

// The first tick where a note of `comparison` that matches ends elsewhere: the earlier of its two ends. Nothing where
// none does.
std::optional<uint32_t> MovedEnd(const Comparison& comparison)
{
    std::optional<uint32_t> moved;
    for (std::size_t i = 0; comparison.after != nullptr && i < comparison.same; i++)
    {
        const song::Note& before = comparison.before->notes[i];
        const uint32_t length = comparison.after->notes[i].length;
        if (before.length != length)
        {
            const uint32_t end = before.start + std::min(before.length, length);
            moved = moved ? std::min(*moved, end) : end;
        }
    }

    return moved;
}

// The tick of the first note that differs in `comparison`, on either side.
uint32_t DifferenceTick(const Comparison& comparison)
{
    uint32_t tick = UINT32_MAX;
    if (comparison.same < comparison.expected.size())
    {
        tick = std::get<0>(comparison.expected[comparison.same]);
    }
    if (comparison.same < comparison.played.size())
    {
        tick = std::min(tick, std::get<0>(comparison.played[comparison.same]));
    }

    return tick;
}

// How `played` differs from `expected`: their counts, and the first note that differs.
std::string Difference(const std::vector<PlayedNote>& expected, const std::vector<PlayedNote>& played)
{
    const std::size_t first = SameNotes(expected, played);

    const auto text = [](const std::vector<PlayedNote>& notes, std::size_t index)
    {
        if (index >= notes.size())
        {
            return std::string("none");
        }

        const auto& [start, key, velocity, slot, program] = notes[index];
        return "key " + std::to_string(key) + " at " + std::to_string(start) + ", velocity " +
               std::to_string(velocity) + ", slot " + std::to_string(slot) + ", program " + std::to_string(program);
    };

    return std::to_string(expected.size()) + " notes expected and " + std::to_string(played.size()) + " played, note " +
           std::to_string(first) + " " + text(expected, first) + " expected and " + text(played, first) + " played";
}

// The track numbered `number` of `sound`, or nullptr.
const song::Track* TrackNumbered(const song::Sequence& sound, int number)
{
    for (const song::Track& track : sound.tracks)
    {
        if (track.number == number)
        {
            return &track;
        }
    }

    return nullptr;
}

// The track of `sound` with the most notes, the lowest first. Nothing for a sound without notes.
const song::Track* BusiestTrack(const song::Sequence& sound)
{
    const song::Track* busiest = nullptr;
    for (const song::Track& track : sound.tracks)
    {
        if (!track.notes.empty() && (busiest == nullptr || track.notes.size() > busiest->notes.size()))
        {
            busiest = &track;
        }
    }

    return busiest;
}

// The programs of the bank in slot `slot` of sound `sound`. A slot without a bank, and a bank that can't be read, have
// none.
const std::vector<uint32_t>& ProgramsIn(Archive& archive, uint32_t sound, int slot)
{
    static const std::vector<uint32_t> kNone;
    const std::vector<std::optional<uint32_t>> banks = archive.archive.BanksOf(sound);
    if (slot < 0 || static_cast<std::size_t>(slot) >= banks.size() || !banks[static_cast<std::size_t>(slot)])
    {
        return kNone;
    }

    const uint32_t bank = *banks[static_cast<std::size_t>(slot)];
    const auto found = archive.programs.find(bank);
    if (found != archive.programs.end())
    {
        return found->second;
    }

    std::vector<uint32_t> programs;
    try
    {
        programs = archive.archive.ProgramsOf(bank);
    }
    catch (const std::exception&)
    {
        programs.clear();
    }

    return archive.programs.emplace(bank, std::move(programs)).first->second;
}

// The instrument that the edit `kind` gives the notes of `from` in sound `sound`: the next program of the same bank, or
// the bank in the next slot that holds one. Nothing where the sound has no such instrument.
std::optional<edit::Program> NewInstrument(Archive& archive, uint32_t sound, const edit::Program& from,
                                           std::size_t kind)
{
    if (kind == 0)
    {
        const std::vector<uint32_t>& programs = ProgramsIn(archive, sound, from.slot);
        const auto next = std::upper_bound(programs.begin(), programs.end(), static_cast<uint32_t>(from.number));
        const uint32_t program = next != programs.end() ? *next : programs.empty() ? 0 : programs.front();
        if (programs.empty() || static_cast<int>(program) == from.number)
        {
            return std::nullopt;
        }

        return edit::Program{from.slot, static_cast<int>(program)};
    }

    for (int step = 1; step < 4; step++)
    {
        const int slot = (from.slot + step) % 4;
        const std::vector<uint32_t>& programs = ProgramsIn(archive, sound, slot);
        if (programs.empty())
        {
            continue;
        }

        const bool kept = std::find(programs.begin(), programs.end(), from.number) != programs.end();
        return edit::Program{slot, kept ? from.number : static_cast<int>(programs.front())};
    }

    return std::nullopt;
}

// What an edit of the instrument of track `index` of sound `sound` reaches: the tracks and sounds that the inspector
// lists. Nothing where the reach can't be found.
std::optional<edit::Reach> ReachOf(Archive& archive, uint32_t sound, std::size_t index, Totals& totals)
{
    auto found = archive.reaches.find(sound);
    if (found == archive.reaches.end())
    {
        const auto start = std::chrono::steady_clock::now();
        std::vector<edit::TrackReach> reach;
        try
        {
            reach = edit::ReachOfTracks(archive.file, archive.places, sound, archive.songs.at(sound));
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s: the reach can't be found: %s\n", archive.name.c_str(),
                        archive.songs.at(sound).name.c_str(), error.what());
        }
        const std::chrono::duration<double> taken = std::chrono::steady_clock::now() - start;
        totals.slowest_reach = std::max(totals.slowest_reach, taken.count());
        found = archive.reaches.emplace(sound, std::move(reach)).first;
    }

    if (index >= found->second.size())
    {
        return std::nullopt;
    }

    return found->second[index].instrument;
}

// Whether `list` holds `value`.
template <typename T>
bool Listed(const std::vector<T>& list, T value)
{
    return std::find(list.begin(), list.end(), value) != list.end();
}

// The items of `sequence` that a track ran in `stretches`.
std::set<uint32_t> ItemsRun(const ntdlib::commands::Sequence& sequence, const song::Console& console,
                            const std::vector<song::Stretch>& stretches)
{
    std::set<uint32_t> items;
    const std::vector<uint32_t> offsets = edit::Offsets(sequence, console);
    for (const song::Stretch& stretch : stretches)
    {
        const std::optional<uint32_t> first = edit::ItemAtOffset(offsets, stretch.first);
        const std::optional<uint32_t> last = edit::ItemAtOffset(offsets, stretch.last);
        if (!first || !last)
        {
            continue;
        }

        for (uint32_t item = *first; item <= *last; item++)
        {
            items.insert(item);
        }
    }

    return items;
}

// Whether a track of `song`, a sound that plays `sequence`, ran a command after which the instrument determines which
// notes play and when: a tie or monophonic play turned on, or a note of length 0 or of a length that the sequence sets
// with the note wait on.
bool FollowsInstrument(const ntdlib::commands::Sequence& sequence, const song::Console& console,
                       const song::Sequence& song)
{
    using ntdlib::commands::Kind;
    using ntdlib::commands::Source;
    const std::vector<uint32_t> offsets = edit::Offsets(sequence, console);
    for (const song::Track& track : song.tracks)
    {
        for (const song::Stretch& stretch : track.stretches)
        {
            const std::optional<uint32_t> first = edit::ItemAtOffset(offsets, stretch.first);
            const std::optional<uint32_t> last = edit::ItemAtOffset(offsets, stretch.last);
            if (!first || !last)
            {
                continue;
            }

            bool note_wait = stretch.note_wait;
            for (uint32_t item = *first; item <= *last; item++)
            {
                const ntdlib::commands::Command& command = sequence.items[item];
                const bool on = command.value.source != Source::kValue || command.value.value != 0;
                const bool decided = command.value.source != Source::kValue || command.value.value <= 0;
                if ((command.kind == Kind::kTie || command.kind == Kind::kMonophonic) && on)
                {
                    return true;
                }
                if (command.kind == Kind::kNoteWait)
                {
                    note_wait = on;
                }
                else if (command.kind == Kind::kNote && note_wait && decided)
                {
                    return true;
                }
            }
        }
    }

    return false;
}

// The items of `before` that `edited` changed, by their indices before the edit: those that it removed, and those whose
// commands differ once their targets follow the items. A command that the edit put in before an item counts that item
// as changed.
std::set<uint32_t> ChangedItems(const ntdlib::commands::Sequence& before, const ntdlib::commands::Sequence& after,
                                const edit::Edited& edited)
{
    std::set<uint32_t> changed;
    std::set<uint32_t> kept;
    for (uint32_t item = 0; item < before.items.size(); item++)
    {
        const std::optional<uint32_t> moved = item < edited.items.size() ? edited.items[item] : std::nullopt;
        if (!moved || *moved >= after.items.size())
        {
            changed.insert(item);
            continue;
        }

        kept.insert(*moved);
        ntdlib::commands::Command command = before.items[item];
        const bool target = ntdlib::commands::HasTarget(command.kind) && !command.target_outside;
        if (target && command.target < edited.items.size() && edited.items[command.target])
        {
            command.target = *edited.items[command.target];
        }
        if (command != after.items[*moved])
        {
            changed.insert(item);
        }
    }

    // A new command leads to the item after it.
    for (uint32_t item = 0; item < after.items.size(); item++)
    {
        if (kept.contains(item))
        {
            continue;
        }

        for (uint32_t old = 0; old < before.items.size(); old++)
        {
            if (edited.items[old] && *edited.items[old] > item)
            {
                changed.insert(old);
                break;
            }
        }
    }

    return changed;
}

// The first change that `edited` made to `before` other than a new program or slot from `to` in a program change or a
// bank select. A new command can be a program change, a bank select or a jump. A command that led to an item can lead
// to the new commands just before it. Empty where there's none.
std::string OtherChange(const ntdlib::commands::Sequence& before, const ntdlib::commands::Sequence& after,
                        const edit::Edited& edited, const edit::Program& to)
{
    using ntdlib::commands::Kind;
    std::vector<bool> kept(after.items.size(), false);
    for (uint32_t item = 0; item < before.items.size(); item++)
    {
        const std::optional<uint32_t> moved = item < edited.items.size() ? edited.items[item] : std::nullopt;
        if (!moved || *moved >= after.items.size())
        {
            return "item " + std::to_string(item) + " is gone";
        }
        kept[*moved] = true;
    }

    // The new value of a command that sets a part of the instrument.
    const auto value_of = [&to](Kind kind)
    {
        return int64_t{kind == Kind::kProgram ? to.number : to.slot} * ntdlib::commands::kOne;
    };

    for (uint32_t item = 0; item < before.items.size(); item++)
    {
        ntdlib::commands::Command command = before.items[item];
        const ntdlib::commands::Command& now = after.items[*edited.items[item]];
        const bool target = ntdlib::commands::HasTarget(command.kind) && !command.target_outside;
        if (target && command.target < edited.items.size() && edited.items[command.target])
        {
            const uint32_t moved = *edited.items[command.target];
            const bool new_between =
                now.target <= moved &&
                std::none_of(kept.begin() + now.target, kept.begin() + moved, [](bool old) { return old; });
            command.target = new_between ? now.target : moved;
        }
        if (command.kind == Kind::kProgram || command.kind == Kind::kBankSelect)
        {
            ntdlib::commands::Command changed = command;
            changed.value.value = value_of(command.kind);
            command = now == changed ? changed : command;
        }
        if (command != now)
        {
            return "item " + std::to_string(item) + " changed otherwise";
        }
    }

    for (uint32_t item = 0; item < after.items.size(); item++)
    {
        const ntdlib::commands::Command& command = after.items[item];
        const bool setter = (command.kind == Kind::kProgram || command.kind == Kind::kBankSelect) &&
                            command.value.value == value_of(command.kind);
        if (!kept[item] && !setter && command.kind != Kind::kJump)
        {
            return "item " + std::to_string(item) + " is new and neither sets the instrument nor jumps";
        }
    }

    return {};
}

// Makes edit `kind` of the busiest track of sound `sound`, in a version of the file, and checks every sound of the
// sound's sequence file.
void EditSound(Archive& archive, uint32_t sound, std::size_t kind, Totals& totals)
{
    const song::Sequence& before = archive.songs.at(sound);
    const song::Track* track = BusiestTrack(before);
    if (track == nullptr)
    {
        return;
    }

    const edit::Program from{track->notes.front().bank_slot, static_cast<int>(track->notes.front().program)};
    const std::optional<edit::Program> to = NewInstrument(archive, sound, from, kind);
    if (!to)
    {
        ++totals.without[kind];
        return;
    }

    // The edit.
    const uint32_t sequence = archive.places[sound]->sequence;
    const std::string what = before.name + "'s " + track->name + " (" + kEditNames[kind] + ")";
    edit::Edited edited;
    try
    {
        edited = edit::EditInstrument(archive.file, sequence, track->number, track->stretches, from, *to);
    }
    catch (const std::invalid_argument& error)
    {
        ++totals.refusals[error.what()];
        return;
    }
    catch (const std::exception& error)
    {
        std::printf("%s: %s fails: %s\n", archive.name.c_str(), what.c_str(), error.what());
        ++totals.problems;
        return;
    }
    ++totals.edits[kind];

    // The sounds of the file, read again from the exported archive.
    const song::Console console = edit::ConsoleOf(archive.file);
    const ntdlib::commands::Sequence& old_commands = *archive.file.sequences[sequence];

    // The items that the edit changed and the edited track ran. Another track that runs one of them shares it.
    const std::set<uint32_t> ran_by_track = ItemsRun(old_commands, console, track->stretches);
    std::set<uint32_t> changed;
    for (const uint32_t item : ChangedItems(old_commands, *edited.file.sequences[sequence], edited))
    {
        if (ran_by_track.contains(item))
        {
            changed.insert(item);
        }
    }
    const std::string other_change = OtherChange(old_commands, *edited.file.sequences[sequence], edited, *to);
    if (!other_change.empty())
    {
        std::printf("%s: %s changes the sequence otherwise: %s\n", archive.name.c_str(), what.c_str(),
                    other_change.c_str());
        ++totals.problems;
        return;
    }
    const std::optional<edit::Reach> reach =
        ReachOf(archive, sound, static_cast<std::size_t>(track - before.tracks.data()), totals);
    if (!reach)
    {
        ++totals.problems;
        return;
    }
    totals.listed_tracks += reach->tracks.size();
    totals.listed_sounds += reach->sounds.size();
    CheckedArchive built;
    try
    {
        hokuto::tools::CheckSaved(edited.file, true);
        built = hokuto::tools::BuildChecked(edited.file, archive.name);
    }
    catch (const std::exception& error)
    {
        std::printf("%s: the file after %s can't be exported: %s\n", archive.name.c_str(), what.c_str(), error.what());
        ++totals.problems;
        return;
    }

    for (const uint32_t other : archive.by_sequence.at(sequence))
    {
        const song::Sequence& was = archive.songs.at(other);
        song::Sequence after;
        try
        {
            after = built.Read(other);
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s can't be read after %s: %s\n", archive.name.c_str(), was.name.c_str(), what.c_str(),
                        error.what());
            ++totals.problems;
            continue;
        }
        ++totals.reads;
        const bool follows = FollowsInstrument(old_commands, console, was);
        const std::optional<uint32_t> cut = EarlierEnd(was, after);
        totals.loops_apart += cut ? 1 : 0;

        // Each track plays as before. The edited track's notes of the old instrument play the new one. The notes that
        // match at the start show the first end that moves.
        std::vector<Comparison> comparisons;
        std::optional<uint32_t> moved;
        for (const song::Track& old_track : was.tracks)
        {
            Comparison& comparison = comparisons.emplace_back();
            comparison.before = &old_track;
            comparison.after = TrackNumbered(after, old_track.number);
            comparison.edited = other == sound && old_track.number == track->number;
            comparison.expected = NotesOf(old_track);
            for (PlayedNote& note : comparison.expected)
            {
                if (comparison.edited && std::get<3>(note) == from.slot && std::get<4>(note) == from.number)
                {
                    std::get<3>(note) = to->slot;
                    std::get<4>(note) = to->number;
                    comparison.mapped++;
                }
            }
            if (comparison.after != nullptr)
            {
                comparison.played = NotesOf(*comparison.after);
            }
            if (cut)
            {
                for (std::vector<PlayedNote>* notes : {&comparison.expected, &comparison.played})
                {
                    std::erase_if(*notes, [&cut](const PlayedNote& note) { return std::get<0>(note) >= *cut; });
                }
            }
            comparison.same = SameNotes(comparison.expected, comparison.played);
            const std::optional<uint32_t> end = MovedEnd(comparison);
            moved = end && (!moved || *end < *moved) ? end : moved;
        }

        // The tracks that play otherwise, and the other sounds with such tracks, are in the edited track's reach. From
        // the first end that moves, a track of the edited sound can play otherwise outside it.
        bool other_changed = false;
        for (const Comparison& comparison : comparisons)
        {
            if (comparison.edited || (comparison.after != nullptr && comparison.played == comparison.expected))
            {
                continue;
            }

            const int number = comparison.before->number;
            if (other != sound)
            {
                other_changed = true;
            }
            else if (Listed(reach->tracks, number))
            {
                ++totals.reached_tracks;
            }
            else if (!follows && (!moved || DifferenceTick(comparison) < *moved))
            {
                std::printf("%s: %s's %s plays otherwise after %s, but the reach leaves the track out\n",
                            archive.name.c_str(), was.name.c_str(), comparison.before->name.c_str(), what.c_str());
                ++totals.unlisted;
            }
        }
        if (other_changed && Listed(reach->sounds, other))
        {
            ++totals.reached_sounds;
        }
        else if (other_changed)
        {
            std::printf("%s: %s plays otherwise after %s, but the reach leaves the sound out\n", archive.name.c_str(),
                        was.name.c_str(), what.c_str());
            ++totals.unlisted;
        }

        for (const Comparison& comparison : comparisons)
        {
            const song::Track& old_track = *comparison.before;
            if (comparison.after != nullptr && comparison.played == comparison.expected)
            {
                totals.notes[kind] += comparison.mapped;
                totals.lengths[kind] += LengthsChanged(old_track, *comparison.after);
                continue;
            }

            // The edited track can leave out notes of the new instrument, or play notes that the old instrument left
            // out.
            const std::vector<PlayedNote>& played = comparison.played;
            const std::vector<PlayedNote>& expected = comparison.expected;
            if (comparison.edited && comparison.after != nullptr && WithoutNotesOf(played, expected, *to))
            {
                ++totals.fewer[kind];
                continue;
            }
            if (comparison.edited && comparison.after != nullptr && WithoutNotesOf(expected, played, *to))
            {
                ++totals.more[kind];
                continue;
            }

            // A track that runs the changed commands of the edited track shares them.
            const std::set<uint32_t> ran = ItemsRun(old_commands, console, old_track.stretches);
            const auto shares = [&changed](uint32_t item)
            {
                return changed.contains(item);
            };
            if (!comparison.edited && std::any_of(ran.begin(), ran.end(), shares))
            {
                ++totals.sharing;
                continue;
            }

            // From the first end that moves, a track can play otherwise.
            if (moved && DifferenceTick(comparison) >= *moved)
            {
                ++totals.later[kind];
                continue;
            }

            // In a sound with legatos, or with notes that wait until the track's notes have ended, the instrument
            // determines which notes play and when.
            if (follows)
            {
                ++totals.follow[kind];
                continue;
            }

            std::printf("%s: %s's %s plays otherwise after %s: %s\n", archive.name.c_str(), was.name.c_str(),
                        old_track.name.c_str(), what.c_str(), Difference(expected, played).c_str());
            ++totals.problems;
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> arguments = xsflib::console::Arguments(argc, argv);
    hokuto::tools::TakeModelOption(arguments);
    if (arguments.size() < 2 || arguments.size() > 3)
    {
        std::fprintf(stderr, "usage: hokuto_check_instruments [--model] <archive> [sound]\n");
        return 2;
    }
    const std::string only = arguments.size() > 2 ? arguments[2] : std::string();

    // The archive and the group files beside it make a Hokuto file.
    const std::filesystem::path path = xsflib::PathFromUtf8(arguments[1]);
    Archive archive;
    archive.name = xsflib::Utf8(path.filename());
    try
    {
        std::vector<uint8_t> bytes;
        xsflib::ReadFile(path, bytes);
        archive.archive = hokuto::tools::OpenChecked(path, bytes);
        archive.file = hokuto::tools::CheckedForm(hokuto::tools::ImportChecked(bytes, archive.archive), bytes);
        archive.places = hokuto::tools::PlacesOfChecked(archive.file, bytes, archive.archive);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", archive.name.c_str(), error.what());
        return 2;
    }

    // The sequence sounds with a place, by their sequence.
    Totals totals;
    for (uint32_t sound = 0; sound < archive.places.size(); sound++)
    {
        if (!archive.places[sound])
        {
            continue;
        }

        try
        {
            archive.songs.emplace(sound, archive.archive.Read(sound));
            archive.by_sequence[archive.places[sound]->sequence].push_back(sound);
            ++totals.sounds;
        }
        catch (const std::exception&)
        {
            ++totals.unreadable;
        }
    }

    // Up to four sounds of each sequence, spread over the sounds that play it, or the named sound.
    std::vector<uint32_t> chosen;
    for (const auto& [sequence, sounds] : archive.by_sequence)
    {
        ++totals.sequences;
        const std::size_t count = std::min(sounds.size(), kMostSounds);
        for (std::size_t i = 0; i < count; i++)
        {
            const uint32_t sound = sounds[i * sounds.size() / count];
            if (only.empty() || archive.songs.at(sound).name == only)
            {
                chosen.push_back(sound);
            }
        }
    }
    totals.taking_part = chosen.size();

    for (std::size_t kind = 0; kind < kEdits; kind++)
    {
        for (const uint32_t sound : chosen)
        {
            EditSound(archive, sound, kind, totals);
        }
    }

    for (const auto& [message, count] : totals.refusals)
    {
        std::printf("%s: refused %zu times: %s\n", archive.name.c_str(), count, message.c_str());
    }
    hokuto::tools::PrintSaves(archive.name);
    std::printf(
        "%s: %zu sequences, %zu sounds read, %zu unreadable, %zu taking part. %zu edits of the program, %zu tracks "
        "without another program, %zu edits with fewer notes, %zu edits with more notes, %zu notes with a new program, "
        "%zu notes ending elsewhere, %zu tracks playing otherwise after an end moves, %zu in sounds with legatos or "
        "notes that wait for the track's notes. %zu edits of the bank, %zu tracks without another bank, %zu edits with "
        "fewer notes, %zu edits with more notes, %zu notes with a new bank, %zu notes ending elsewhere, %zu tracks "
        "playing otherwise after an end moves, %zu in sounds with legatos or notes that wait for the track's notes. "
        "%zu tracks sharing commands. %zu reads, %zu of them with the loop a whole number of passes away. %zu "
        "problems. The reaches list %zu tracks and %zu sounds. %zu of the tracks and %zu of the sounds played "
        "otherwise. %zu tracks and sounds played otherwise outside the reach. The slowest reach took %.1f ms.\n",
        archive.name.c_str(), totals.sequences, totals.sounds, totals.unreadable, totals.taking_part, totals.edits[0],
        totals.without[0], totals.fewer[0], totals.more[0], totals.notes[0], totals.lengths[0], totals.later[0],
        totals.follow[0], totals.edits[1], totals.without[1], totals.fewer[1], totals.more[1], totals.notes[1],
        totals.lengths[1], totals.later[1], totals.follow[1], totals.sharing, totals.reads, totals.loops_apart,
        totals.problems, totals.listed_tracks, totals.listed_sounds, totals.reached_tracks, totals.reached_sounds,
        totals.unlisted, totals.slowest_reach * 1000.0);

    return totals.problems == 0 && totals.unlisted == 0 ? 0 : 1;
}
