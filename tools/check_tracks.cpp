// SPDX-License-Identifier: MIT

// Checks the edits of a sound's tracks (src/edit/tracks.h) on a sound archive of the Wii, the 3DS, the Wii U or the
// Switch, on a GBA ROM, or on a DS cartridge image. The arrangement's Project menu makes these edits. The tool imports
// the archive into a Hokuto file the way the program does, and ntdlib's performer reads each sequence sound. Up to four
// sounds of each sequence file then take part in four rounds, spread over the sounds that play the file. Each round
// makes its edits in one version of the file. ntdlib's performer then reads every sequence sound again from the
// exported archive.
//
// - Each sound gains a new track, with the instrument of its first note. The new track has to run without notes, and
//   the sound's other tracks have to play their notes as before. A sound that holds until the game stops it is counted
//   apart: the performer ends it 10 s after its last change, and the new track's end is a change. A note then goes into
//   the new track of the first such sound of each sequence file, at the loop's start with the new track or at the
//   start, with the key of the sound's first note. The sound has to play it there, and its other notes as before. Where
//   the driver starts the note, it plays the instrument of the sound's first note. A note that the driver never starts
//   gets a line, and counts apart: the driver can find no channel or voice for it, and a lost instrument leaves it
//   unstarted too. On the GBA, a later note of another track can take the new note's PSG channel before the new note
//   sounds. ntdlib's performer then records the new note nowhere (open work, item 218). Such a note gets a line, and
//   counts apart too. The arrangement ends a held note where the performer stops, and the new note's release can move
//   that place. A note that lasts to the end of a sound without a loop therefore matches whatever its length. ntdlib's
//   performer can find the loop a whole number of passes sooner, with its end at the new note's tick (open work, item
//   221). The read then ends before the note, and the tool counts the sound apart. The tool adds the note at the loop
//   start of the read with the new track. That read can find the loop a pass or more late. The note then falls within
//   the loop's body, and plays first a whole number of the loop's lengths sooner. The tool counts such a note apart.
//   Metroid Fusion's sequencer reads no extra length after a note (edit/mp2k.h). On that sequencer, the note lasts the
//   longest length that a note code has, up to the length of the first note.
// - Each sound gains a copy of its track with the most notes. The copy has to play that track's notes, and the sound's
//   other tracks have to play theirs as before. A track whose commands draw random numbers, use variables, run the
//   game's code or run only if a comparison held is counted apart. Its copy draws, compares and changes them again.
//   On Brownie Brown's driver, a copy of a track that turns square 1's sweep on, on a channel other than square 1,
//   plays without the sweep. A copy that plays otherwise there is counted apart.
// - Each sound loses its last track with notes, other than track 0. The sound's other tracks have to start their
//   notes as before, with their keys and velocities, up to the earlier end. A track that ran longer than the others or
//   looped decided where the sound ends or loops, and the last notes then last otherwise. A track that sets the tempo,
//   the timebase, the sound's volume or a variable, draws random numbers or runs the game's code is counted apart. The
//   others can play otherwise without it.
// - Each sound takes a paste of the track with the most notes of another sound: the next sound taking part that plays
//   another sequence file. The pasted track has to play that track's notes at the same ticks, with the same banks and
//   programs, up to the earlier end. The sound's other tracks have to play their notes as before, up to the earlier
//   end. A track whose commands draw random numbers, use variables, run the game's code or run only if a comparison
//   held is counted apart. So is a track that its sound starts more than once. The paste plays it once. So is a track
//   with a note of length 0: its wave sets how long the note lasts, and its ticks then follow the tempo. A sound of the
//   Wii plays one bank, and refuses a paste from a sound of another bank.
//
// A note of a tick can end in the frame where it starts, and the arrangement then leaves it out. A frame of the 3DS
// lasts 4.89 ms, and a tick can be shorter. A copy or a paste plays on another track, and the frames can show such a
// note there and hide it on the original track. The tool counts apart the copies and pastes that differ only in notes
// shorter than 2 ticks.
//
// On Brownie Brown's driver, the ticks follow the beat that ntdlib's conversion works out from the notes' starts
// (ntdlib's BeatGrid). A new note can move that beat. A note of one frame on another track can then fall within a
// tick, and the conversion leaves it out. The tool counts apart a new note after which only notes shorter than 2 ticks
// differ.
//
// The performer marks the loop on the first pass that the next pass repeats. A new track or a copy can change the state
// that it compares, and the loop can then start a whole number of passes earlier or later. The notes then have to match
// up to the earlier loop's end. A note that lasts to the loop's end in both reads matches whatever its length. The
// arrangement ends a note there when it still sounds where the performer stops. A note that lasts to the earlier loop's
// end in its read therefore matches a note that lasts at least that long in the read whose loop ends later. In each
// round, every sound that the round doesn't edit has to play as before: its tracks, its notes and its loop. The edits
// refuse some sounds and tracks, such as a sound whose commands can't move, a sound without a free track and a track
// that opens other tracks. The tool counts the refusals by their messages. It prints a line for each problem, and a
// summary. It returns 1 when a sound plays otherwise, or when an edit, a read or the export fails. With a sound's name,
// only that sound takes part.
//
// With --model, the file goes into the model form before the edits (format::InModelForm), and the archive that the
// model builds has to be the game's. Each file that the edits make then has to save in both forms and open again as it
// was (tools/checked_archive.h).
//
// A GBA song's header lists its tracks (ntdlib/mp2k/layout.h). Songs that share a header play the same tracks, and one
// of them takes part. The others have to play as the edited song plays. A new track, a copy and a paste take the
// header's next place. A removal can delete any track, and the tracks after it move up a number. The sound's other
// tracks then have to play as their old numbers played. A pasted track comes from a song of the same voice group: a GBA
// song uses one voice group. A copy of a track whose memory accesses jump counts apart. On the other consoles, a copy
// of a track that compares counts apart too. A ROM that fills its 32 MiB without free space has no room for a song
// whose header gains a track, or that a new note makes longer (tools::Roomless). With --filler, the ROM's filler runs
// can also have no room (tools::Outgrows). Such an edit stays out, and the tool counts it.
//
// On the GBA and the DS, the driver shares its channels among the tracks. A track's notes are the notes that its
// commands play: those that the driver started, and those that it never started (song::Track::unstarted). The lengths
// of the notes that never started, and of those that a later note cut, don't count. ntdlib's performer leaves out a
// note that started and stayed silent. A note that never started can therefore stand in one read alone, and the tool
// counts such notes. A header can list more tracks than the song's music player plays. A removal then brings the first
// track past the player's count into play, and the tool counts such removals. A PSG channel's envelope moves in 16
// steps, and a quiet note there, at half the full level or less, can end in its first frames. The note before it and
// the frame where it starts decide whether it sounds. A pasted track plays beside other tracks, and its quiet notes of
// the PSG channels can come or go. A paste that plays otherwise only in such notes counts apart. A song that lasts no
// time shows only its tracks with notes: the performer records no stretch for the others. A new track or a paste makes
// the song last, and its other tracks then show without notes. They don't count.
//
// On the DS, every sound of a sequence file starts at the file's first command, and one of them takes part. An edit of
// its tracks reaches the file's other sounds, and each of them has to play the edit as the edited sound plays it. A
// sound that shares the file with another bank plays a pasted track on that bank, and counts apart. A DS note whose
// length runs out in the tick where a later note claims its channel ends in that tick. Otherwise its release comes at
// the end of the frame, after the frame's ticks (ntdlib's performer). Its length can therefore differ by up to the
// ticks of a frame: the tempo over 240, rounded up (FrameTicks).
//
// DSE's driver shares its voices among the tracks. A note that finds no voice is among the notes that it never started,
// and the rules above for them hold. As on the GBA, a removal moves the later tracks up a number. The tracks of a
// driver channel share its volume, pan and program. A removal of one of them can change what the others play, and the
// tool counts it apart. As on the DS, a note whose wave runs out ends at the frame's end (FrameTicks). New tracks and
// copies take a free channel. A sound without one refuses them. A paste comes from a sound of the same timebase whose
// banks are all banks of the sound. Each DSE file names its banks by their ids.
//
// On Konami's driver, a track's number names its channel, and the edits refuse a new track. A copy and a paste go to a
// track without notes, and a removed track stays without notes. Two numbers of the song table can name one song, and a
// song can have a live stage's version (song::KonamiSounds). Another number of an edited song has to play as the edited
// number plays. A live stage's version compares only with an edited live stage's version, and counts apart otherwise.
//
// On Quintet's driver, a song keeps all 6 channels, and the number of a track names its channel. The edits refuse a new
// track, a copy and a paste. A removal turns the channel's notes into rests, and the removed track stays without notes.
//
// Nintendo R&D2's driver shares its voices among the tracks by priority, and ntdlib's performer marks no note of the
// driver as cut or never started. A new note, a copy, a paste or a removal changes how many voices the song's notes ask
// for. Other notes then end sooner or later, and a note can stay silent or sound. Where a comparison fails, the tool
// compares again without the notes' lengths, and lets any note stand alone in one read. An edit that matches that way
// counts apart, and the tool counts the notes that stood alone (VoicesDecide). A square, the wave or the noise plays
// one note at a time. The game code allots some songs fewer of the seven sample voices, down to one. A new note can
// then find its voice held by a track of a higher priority. A new note that stays silent where a note of another track
// occupies the same kind of voice counts apart (edit::Rd2VoiceTypeOf). As on Konami's driver, a song's versions play
// the song with the edit.
//
// On Brownie Brown's driver, the channels of a sound start at tick 1. A new note without a loop is added there. A new
// track uses the kind of channel of the track of its first note, with the settings of that track there. The new note
// copies the byte of the first note, and has to play at the key of the first note. The bytes of a note contain no
// velocity, and the note plays at 127. A sound whose read shows no track ends in its first frame. A new track there
// makes the sound last longer, and the sound's other tracks then show, without notes. The tool counts such a sound
// apart.
//
// A tune of Rare's driver keeps its count of tracks. The edits refuse a new track, a copy and a paste. As on Konami's
// driver, a removed track stays without notes (KeepsRemovedTracks). The tracks that play on a MIDI channel share its
// slots, and a note-off ends the channel's first note of its key, whichever track started it. A removal of a track
// whose notes share a channel with another track's notes can change what the other track plays. The tool counts such a
// removal apart. Tunes can share the bytes of a track: their track lists name items of one run. A removal deletes the
// run's notes, and every tune that plays the run loses them. Such a tune has to play its other tracks as before, and
// its track of the run without notes. A tune whose track runs only part of the run counts apart.
//
// On Krawall, a module keeps the channels of its header, and a track's number names its channel. The edits refuse a new
// track, a copy and a paste. A removal makes the channel's notes note offs in every pattern of the module, and the
// removed track stays without notes. The module's other songs play the same patterns. The removal applies to each of
// them too: it has to play no notes on the removed track, and its other tracks as before (Sharers). While more than 15
// of a module's channels play notes, ntdlib's conversion leaves out the silence at the ends of the notes, and the notes
// that stay silent. A removal that leaves 15 channels or fewer with notes then shows the other tracks' notes as long as
// they sound, and their silent notes. The tool counts such a removal apart. Each note of another track has to play at
// its start and key, for as long as before or longer. The track can gain notes (LongerNotes).
//
// With --filler, the songs of a ROM that fills the GBA's 32 MiB go into its filler runs where they outgrow their
// places.
//
// usage: hokuto_check_tracks [--model] [--filler] <archive or GBA ROM> [sound]

#include "check_tracks.h"

namespace hokuto::tools::tracks
{

song::Sequence ComparableRead(const Archive& archive, const File& file, uint32_t number, const song::Sequence& sound)
{
    song::Sequence kept = sound;
    if (!archive.archive.Konami())
    {
        return kept;
    }

    const ntdlib::commands::Sequence& commands = *file.sequences[archive.places[number]->sequence];
    const std::vector<uint32_t> offsets = edit::Offsets(commands, edit::ConsoleOf(file));
    const bool dungeon = archive.archive.Konami()->info.revision == ntdlib::konami::Revision::kDungeonDiceMonsters;
    for (song::Track& track : kept.tracks)
    {
        const auto elsewhere = [dungeon, &track](const song::Note& note)
        {
            return dungeon && note.command_track >= 0 && note.command_track != track.number;
        };
        std::erase_if(track.notes, elsewhere);
        for (song::Note& note : track.notes)
        {
            const std::optional<uint32_t> item = edit::ItemAtOffset(offsets, note.command);
            if (item && commands.items[*item].kind == ntdlib::commands::Kind::kNote && !commands.items[*item].sample &&
                commands.items[*item].key == 255)
            {
                note.key = 0;
            }
        }
    }

    return kept;
}

std::map<uint32_t, song::Sequence> ReadAll(const Archive& archive, const File& version, const std::string& round,
                                           Totals& totals)
{
    std::map<uint32_t, song::Sequence> songs;
    CheckedArchive built;
    try
    {
        hokuto::tools::CheckSaved(version);
        built = hokuto::tools::BuildChecked(version, archive.name);
    }
    catch (const std::exception& error)
    {
        std::printf("%s: the file after the %s can't be exported: %s\n", archive.name.c_str(), round.c_str(),
                    error.what());
        ++totals.problems;
        return songs;
    }

    for (const auto& [sound, before] : archive.songs)
    {
        try
        {
            songs.emplace(sound, built.Read(sound));
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s can't be read after the %s: %s\n", archive.name.c_str(), before.name.c_str(),
                        round.c_str(), error.what());
            ++totals.problems;
        }
    }

    return songs;
}

} // namespace hokuto::tools::tracks

namespace
{

namespace edit = hokuto::edit;
namespace sar = ntdlib::sar;
namespace song = hokuto::song;
using hokuto::format::File;
using hokuto::tools::CheckedArchive;
using hokuto::tools::LoopText;
using hokuto::tools::SpreadIndices;
using hokuto::tools::tracks::AddTracks;
using hokuto::tools::tracks::Archive;
using hokuto::tools::tracks::CopyTracks;
using hokuto::tools::tracks::g_alone;
using hokuto::tools::tracks::g_brownie;
using hokuto::tools::tracks::g_ds;
using hokuto::tools::tracks::g_dse;
using hokuto::tools::tracks::g_rd2;
using hokuto::tools::tracks::g_voice_alone;
using hokuto::tools::tracks::kMostSounds;
using hokuto::tools::tracks::PasteTracks;
using hokuto::tools::tracks::RemoveTracks;
using hokuto::tools::tracks::Totals;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::Source;

// Up to four sounds of each sequence of `by_sequence`, spread over the sounds that play it. Counts the sequences in
// `totals`.
std::vector<uint32_t> SpreadSounds(const Archive& archive, const std::map<uint32_t, std::vector<uint32_t>>& by_sequence,
                                   Totals& totals)
{
    std::vector<uint32_t> chosen;
    for (const auto& [sequence, sounds] : by_sequence)
    {
        ++totals.sequences;
        const bool one = archive.archive.gba || archive.archive.driver || archive.archive.ds;
        for (const std::size_t index : SpreadIndices(sounds.size(), one ? std::size_t{1} : kMostSounds))
        {
            chosen.push_back(sounds[index]);
        }
    }

    return chosen;
}

// Prints the refusals, the saves and the totals of the rounds.
void PrintSummary(const Archive& archive, const Totals& totals)
{
    for (const auto& [message, count] : totals.refusals)
    {
        std::printf("%s: refused %zu times: %s\n", archive.name.c_str(), count, message.c_str());
    }
    hokuto::tools::PrintSaves(archive.name);
    if (archive.archive.gba)
    {
        std::printf(
            "%s: on the GBA, %zu reads of songs that share an edited song's header, %zu edits that stayed out: their "
            "songs outgrew their places, and the ROM has no room for them. %zu removals brought a track past the "
            "music player's count into play. %zu notes that never started stood alone in one read of a comparison. "
            "%zu pastes played otherwise only in quiet notes of the PSG channels.\n",
            archive.name.c_str(), totals.shared, totals.outgrown, totals.revealed, g_alone, totals.quiet_pastes);
    }
    if (archive.archive.Konami())
    {
        std::printf(
            "%s: on Konami's driver, %zu reads of a song's other numbers and live versions. After %zu copies, "
            "the performer found no loop. %zu pastes compared up to a song's first pass.\n",
            archive.name.c_str(), totals.shared, totals.copied_without_loop, totals.pasted_first_pass);
    }
    if (archive.archive.Krawall())
    {
        std::printf(
            "%s: on Krawall, %zu songs took the removal of a track of another song of their module. %zu removals "
            "left 15 tracks or fewer with notes, and the other tracks' notes then kept their silent ends.\n",
            archive.name.c_str(), totals.module_songs, totals.silent_ends);
    }
    if (archive.archive.Rd2())
    {
        std::printf(
            "%s: on Nintendo R&D2's driver, %zu new notes, copies, pastes and removals played otherwise only "
            "where the driver's voices decide: in the lengths of notes, and in %zu notes that stood alone in "
            "one read of a comparison. %zu new notes found their PSG channel or sample voices held by notes of "
            "other tracks.\n",
            archive.name.c_str(), totals.voice_notes, g_voice_alone, totals.held_channels);
    }
    if (g_brownie)
    {
        std::printf(
            "%s: on Brownie Brown's driver, %zu copies of a track that sweeps went to a channel without a "
            "sweep, and played otherwise. After %zu new notes, only notes of a tick played otherwise.\n",
            archive.name.c_str(), totals.copied_without_sweep, totals.beat_notes);
    }
    if (totals.added_to_empty > 0)
    {
        std::printf(
            "%s: %zu new tracks went into sounds that end in their first frame. The sounds' other tracks then "
            "showed, without notes.\n",
            archive.name.c_str(), totals.added_to_empty);
    }
    if (totals.loop_end_notes > 0)
    {
        std::printf("%s: %zu new notes lay at the end of a loop that moved a whole number of passes sooner.\n",
                    archive.name.c_str(), totals.loop_end_notes);
    }
    if (totals.unstarted_notes > 0)
    {
        std::printf("%s: the driver never started %zu new notes.\n", archive.name.c_str(), totals.unstarted_notes);
    }
    if (totals.taken_notes > 0)
    {
        std::printf("%s: a later note took the PSG channel of %zu new notes before they sounded.\n",
                    archive.name.c_str(), totals.taken_notes);
    }
    if (totals.sooner_notes > 0)
    {
        std::printf("%s: %zu new notes played first a whole number of loop passes sooner.\n", archive.name.c_str(),
                    totals.sooner_notes);
    }
    std::printf(
        "%s: %zu sequences, %zu sounds read, %zu unreadable, %zu taking part. %zu new tracks, %zu of them in sounds "
        "that hold, %zu of those playing otherwise, and %zu new notes in them. %zu copies, %zu of them counted "
        "apart, %zu of those playing otherwise. %zu removals, %zu of them counted apart, %zu of those playing "
        "otherwise, and %zu sounds ending or looping otherwise. %zu pastes, %zu of them counted apart, %zu of those "
        "playing otherwise, %zu starting after the sound's start, %zu with banks in other slots, and %zu sounds ending "
        "or looping otherwise. %zu reads of other sounds. %zu loops moved a whole number of passes. %zu copies and "
        "pastes played otherwise only in notes of a tick. %zu problems.\n",
        archive.name.c_str(), totals.sequences, totals.sounds, totals.unreadable, totals.taking_part, totals.added,
        totals.added_holding, totals.added_differing, totals.notes, totals.copied, totals.copied_apart,
        totals.copied_differing, totals.removed, totals.removed_apart, totals.removed_differing, totals.removed_ending,
        totals.pasted, totals.pasted_apart, totals.pasted_differing, totals.pasted_late, totals.pasted_slots,
        totals.pasted_ending, totals.others, totals.moved_loops, totals.tick_notes, totals.problems);
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> arguments = xsflib::console::Arguments(argc, argv);
    hokuto::tools::TakeModelOption(arguments);
    hokuto::tools::TakeFillerOption(arguments);
    if (arguments.size() < 2 || arguments.size() > 3)
    {
        std::fprintf(stderr, "usage: hokuto_check_tracks [--model] [--filler] <archive or GBA ROM> [sound]\n");
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
        g_ds = archive.archive.ds != nullptr;
        g_dse = archive.archive.dse != nullptr;
        g_rd2 = archive.archive.Rd2() != nullptr;
        g_brownie = archive.archive.Brownie() != nullptr;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", archive.name.c_str(), error.what());
        return 2;
    }

    // The sequence sounds with a place, by their sequence.
    Totals totals;
    hokuto::tools::SequenceSounds sequence_sounds =
        hokuto::tools::ReadSequenceSounds(archive.archive, archive.places, only, archive.songs);
    const std::map<uint32_t, std::vector<uint32_t>> by_sequence = std::move(sequence_sounds.by_sequence);
    totals.sounds = sequence_sounds.read;
    totals.unreadable = sequence_sounds.unreadable;

    // Up to four sounds of each sequence, spread over the sounds that play it, or the named sound. GBA songs that share
    // a sequence share its header, and one of them takes part. On the DS, the sounds of a sequence file start at the
    // same command, and one of them takes part too.
    const std::vector<uint32_t> chosen = SpreadSounds(archive, by_sequence, totals);
    totals.taking_part = chosen.size();

    // A round that meets a file that it can't handle stops the tool with a message.
    try
    {
        AddTracks(archive, chosen, totals);
        CopyTracks(archive, chosen, totals);
        RemoveTracks(archive, chosen, totals);
        PasteTracks(archive, chosen, totals);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", archive.name.c_str(), error.what());
        return 2;
    }

    PrintSummary(archive, totals);

    return totals.problems == 0 ? 0 : 1;
}
