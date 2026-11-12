// SPDX-License-Identifier: MIT

// Checks the edit of a track's instrument (src/edit/instruments.h) on a sound archive of the Wii, the 3DS, the Wii U,
// the Switch or the DS, on a GBA ROM whose songs MusicPlayer2000 plays, or on the DSE files of a DS image. From a DS
// cartridge image, the tool uses the largest sound archive, or the DSE files where the image contains no sound archive
// (tools/checked_archive.h). The tool refuses a ROM with Konami's driver: hokuto_check_konami_mix checks its
// instruments. The inspector's Bank and Program menus make this edit. The tool imports the archive into a Hokuto file
// the way the program does, and ntdlib's performer reads each sequence sound. Up to four sounds of each sequence file
// take part, spread over the sounds that play the file. The track with the most notes of each such sound takes two
// edits, each in a separate version of the file:
//
// - The notes that play the instrument of the track's first note take the next program of the same bank.
// - Those notes take the bank in the next slot of the sound that holds one. They keep their program where that bank has
//   it, and take its first program otherwise.
//
// A sound of the Wii plays one bank, and takes the first edit alone. So does a GBA song: it plays one voice group. The
// programs of a voice group are those that the ROM's songs select in it (song::GbaVoiceGroups). A song of another of
// the GBA's drivers uses the programs that the driver assigns each slot of the song's list of banks
// (song::DriverRom::BankSlots). On the GBA, a voice decides the channel of its notes: one of the four PSG channels, or
// a DirectSound channel for a sample. A note takes a channel from a note of another track, and a PSG channel holds one
// note at a time. A track can therefore gain or lose notes on the channels that the edited notes play on, before or
// after the edit. The tool counts such a track apart when it gains or loses notes only on those channels, and plays its
// other notes as before. On the DS, an instrument's kind decides the channels of its notes: a PSG instrument plays on
// channels 8 to 13, noise on 14 and 15, and a sample on any. The driver assigns a note the quietest of the channels of
// the lowest priority, and a note without a channel doesn't start. A track can therefore gain or lose notes. The tool
// counts it apart when each note that is missing from one read is a note that the other read lists among the notes that
// the driver never started (DsChannelsDecide).
//
// A GBA ROM that fills its 32 MiB without free space has no room for a song that grows (tools::Roomless). On such a
// ROM, the tool leaves out the edits whose songs outgrow their places, and counts them apart.
//
// The edit has to change nothing but the programs of program changes and the slots of bank selects. It can put in
// nothing but program changes, bank selects and a jump past them. ntdlib's performer then reads every sound of the
// sequence file again from the exported archive. The edited track has to play its notes as before: at the same ticks,
// with the same keys and velocities. Its notes that played the old instrument have to play the new one, and its other
// notes their instruments as before. An instrument can lack a region for the key or the velocity of a note. A key group
// can cut a note in the tick that it starts. The performer then leaves the note out. The tool counts apart the edits
// after which the track leaves out notes of the new instrument, and those after which it plays more of them. An edit
// that does both counts among the first.
//
// The engine sets where some notes end, such as a note that a one-shot wave ends, or a note that lasts to the end of
// the performance. The new instrument can end them elsewhere. The tool counts such notes apart. From the first end that
// moves, a sound's tracks can play otherwise. A note of length 0 on a track whose notes wait for their length holds the
// track until its sound ends. The random numbers follow the time, and the sound can end elsewhere. The tool counts
// apart the tracks that play as before up to that end, and otherwise after it. Every other track of the file's sounds
// has to play as before, unless it runs a command that the edit changed, or the command that opened the edited track.
// Such a track shares commands with the edited track, and is counted apart. On Nintendo R&D2's driver, a track that the
// edited track or such a track starts gets the instrument of the starting track (edit::DriverEdits). It counts apart
// too.
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

#include "check_instruments.h"

namespace hokuto::tools::instruments
{

namespace
{

// The sounds of a sequence file that take part.
constexpr std::size_t kMostSounds = 4;

// The programs of the voice group of GBA song `song` that the ROM's songs select. Only slot 0 has them.
const std::vector<uint32_t>& GbaProgramsIn(Archive& archive, uint32_t song, int slot)
{
    static const std::vector<uint32_t> kNone;
    const song::GbaRom& rom = *archive.archive.gba;
    ntdlib::mp2k::SongHeader header;
    if (slot != 0 || !ntdlib::mp2k::ReadSong(rom.rom, rom.info, static_cast<int>(song), header))
    {
        return kNone;
    }

    if (!archive.gba_groups_read)
    {
        for (const song::GbaVoiceGroup& group : song::GbaVoiceGroups(rom))
        {
            archive.programs[group.address].assign(group.programs.begin(), group.programs.end());
        }
        archive.gba_groups_read = true;
    }

    const auto found = archive.programs.find(header.voices);
    return found != archive.programs.end() ? found->second : kNone;
}

// The programs of the bank in slot `slot` of sound `sound`. A slot without a bank, and a bank that can't be read, have
// none. On another of the GBA's drivers, the driver defines the slots of the sound's list of banks
// (song::DriverRom::BankSlots).
const std::vector<uint32_t>& ProgramsIn(Archive& archive, uint32_t sound, int slot)
{
    static const std::vector<uint32_t> kNone;
    if (archive.archive.gba)
    {
        return GbaProgramsIn(archive, sound, slot);
    }
    if (archive.archive.driver)
    {
        const auto key = std::make_pair(sound, slot);
        const auto read = archive.slot_programs.find(key);
        if (read != archive.slot_programs.end())
        {
            return read->second;
        }

        const std::vector<song::DriverBankSlot> slots = archive.archive.driver->BankSlots(sound);
        std::vector<uint32_t> programs;
        if (slot >= 0 && static_cast<std::size_t>(slot) < slots.size())
        {
            programs = slots[static_cast<std::size_t>(slot)].programs;
        }
        return archive.slot_programs.emplace(key, std::move(programs)).first->second;
    }

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

// The instrument edit that EditSound makes, as CheckOtherSound reads it: the edited sound and track, the old and the
// new instrument, the kind of edit and its text, the commands before the edit, the items that the edit changed and the
// track ran, the GBA's channels and voices of the edited notes (GbaChannelsOf), and the track's reach.
struct InstrumentEdit
{
    uint32_t sound;
    const song::Track* track;
    edit::Program from;
    edit::Program to;
    std::size_t kind;
    std::string what;
    const ntdlib::commands::Sequence* old_commands;
    song::Console console;
    std::set<uint32_t> changed;
    uint32_t voices;
    std::set<int> channels;
    const edit::Reach* reach;
};

// The comparison of each track of `was`, sound `other` before `change`, with its track in `after`, from which no note
// starting at `cut` or later counts. Sets `moved` to the first end that moves.
std::vector<Comparison> CompareTracks(const Archive& archive, const InstrumentEdit& change, uint32_t other,
                                      const song::Sequence& was, const song::Sequence& after,
                                      const std::optional<uint32_t>& cut, std::optional<uint32_t>& moved)
{
    std::vector<Comparison> comparisons;
    for (const song::Track& old_track : was.tracks)
    {
        Comparison& comparison = comparisons.emplace_back();
        comparison.before = &old_track;
        comparison.after = TrackNumbered(after, old_track.number);
        comparison.edited = other == change.sound && old_track.number == change.track->number;
        comparison.expected = NotesOf(old_track);
        for (PlayedNote& note : comparison.expected)
        {
            if (comparison.edited && std::get<3>(note) == change.from.slot && std::get<4>(note) == change.from.number)
            {
                std::get<3>(note) = change.to.slot;
                std::get<4>(note) = change.to.number;
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
        comparison.same = FirstDifferingIndex(comparison.expected, comparison.played);
        comparison.channels = comparison.played != comparison.expected &&
                              ChannelsDecide(archive, change.voices, change.channels, comparison);
        comparison.ds_channels =
            comparison.played != comparison.expected && archive.archive.ds && DsChannelsDecide(comparison);
        const std::optional<uint32_t> end = MovedEnd(comparison);
        moved = end && (!moved || *end < *moved) ? end : moved;
    }

    return comparisons;
}

// Counts and prints the tracks of `was`, sound `other` after `change`, that play otherwise in `comparisons`, and the
// sound itself, that the edited track's reach leaves out. From `moved`, the first end that moves, a track of the
// edited sound can play otherwise outside the reach. So can every track where the instrument decides which notes play
// (`follows`).
void CheckChangesReached(const Archive& archive, const InstrumentEdit& change, uint32_t other,
                         const song::Sequence& was, const std::vector<Comparison>& comparisons,
                         const std::optional<uint32_t>& moved, bool follows, Totals& totals)
{
    bool other_changed = false;
    for (const Comparison& comparison : comparisons)
    {
        if (comparison.edited || comparison.channels || comparison.ds_channels ||
            (comparison.after != nullptr && comparison.played == comparison.expected))
        {
            continue;
        }

        const int number = comparison.before->number;
        if (other != change.sound)
        {
            other_changed = true;
        }
        else if (Listed(change.reach->tracks, number))
        {
            ++totals.reached_tracks;
        }
        else if (!follows && (!moved || DifferenceTick(comparison) < *moved))
        {
            std::printf("%s: %s's %s plays otherwise after %s, but the reach leaves the track out\n",
                        archive.name.c_str(), was.name.c_str(), comparison.before->name.c_str(), change.what.c_str());
            ++totals.unlisted;
        }
    }
    if (other_changed && Listed(change.reach->sounds, other))
    {
        ++totals.reached_sounds;
    }
    else if (other_changed)
    {
        std::printf("%s: %s plays otherwise after %s, but the reach leaves the sound out\n", archive.name.c_str(),
                    was.name.c_str(), change.what.c_str());
        ++totals.unlisted;
    }
}

// Counts each track of `comparisons` that plays otherwise after `change` by the reason that the check allows, and
// prints the others. `was` is the sound before the edit, `taking` lists the tracks that take the new instrument from
// the track that starts them, `moved` is the first end that moves, and `follows` says whether the instrument decides
// which notes play.
void CountTrackChanges(const Archive& archive, const InstrumentEdit& change, const song::Sequence& was,
                       const std::vector<Comparison>& comparisons, const std::set<int>& taking,
                       const std::optional<uint32_t>& moved, bool follows, Totals& totals)
{
    for (const Comparison& comparison : comparisons)
    {
        const song::Track& old_track = *comparison.before;
        if (comparison.after != nullptr && comparison.played == comparison.expected)
        {
            totals.notes[change.kind] += comparison.mapped;
            totals.lengths[change.kind] += LengthsChanged(old_track, *comparison.after);
            continue;
        }

        // The edited track can leave out notes of the new instrument, or play notes that the old instrument left
        // out.
        const std::vector<PlayedNote>& played = comparison.played;
        const std::vector<PlayedNote>& expected = comparison.expected;
        if (comparison.edited && comparison.after != nullptr && WithoutNotesOf(played, expected, change.to))
        {
            ++totals.fewer[change.kind];
            continue;
        }
        if (comparison.edited && comparison.after != nullptr && WithoutNotesOf(expected, played, change.to))
        {
            ++totals.more[change.kind];
            continue;
        }

        // The new instrument plays some keys that the old one left out, and leaves out others.
        if (comparison.edited && comparison.after != nullptr &&
            DifferOnlyInNotesOf(comparison.expected, comparison.played, change.to))
        {
            ++totals.fewer[change.kind];
            continue;
        }

        // A track that runs the changed commands of the edited track shares them.
        const std::set<uint32_t> ran = ItemsRun(*change.old_commands, change.console, old_track.stretches);
        const auto shares = [&change](uint32_t item)
        {
            return change.changed.contains(item);
        };
        if (!comparison.edited && (std::any_of(ran.begin(), ran.end(), shares) || taking.contains(old_track.number)))
        {
            ++totals.sharing;
            continue;
        }

        // From the first end that moves, a track can play otherwise.
        if (moved && DifferenceTick(comparison) >= *moved)
        {
            ++totals.later[change.kind];
            continue;
        }

        // In a sound with legatos, or with notes that wait until the track's notes have ended, the instrument
        // determines which notes play and when.
        if (follows)
        {
            ++totals.follow[change.kind];
            continue;
        }

        // On the GBA, the edited notes can take channels from other notes, or leave them channels.
        if (comparison.channels)
        {
            ++totals.channels;
            continue;
        }

        // On the DS, the new instrument can play on other channels. A note can then take a channel from another
        // note, or leave it one.
        if (comparison.ds_channels)
        {
            ++totals.ds_channels;
            continue;
        }

        std::printf("%s: %s's %s plays otherwise after %s: %s\n", archive.name.c_str(), was.name.c_str(),
                    old_track.name.c_str(), change.what.c_str(), Difference(expected, played).c_str());
        ++totals.problems;
    }
}

// Checks sound `other` of the edited sequence after `change`, as `built` plays it: each track plays as before but for
// the changes that the edit allows, and the tracks and sounds that play otherwise are in the edited track's reach.
void CheckOtherSound(const Archive& archive, const CheckedArchive& built, uint32_t other, const InstrumentEdit& change,
                     Totals& totals)
{
    const song::Sequence& was = archive.songs.at(other);
    song::Sequence after;
    try
    {
        after = built.Read(other);
    }
    catch (const std::exception& error)
    {
        std::printf("%s: %s can't be read after %s: %s\n", archive.name.c_str(), was.name.c_str(), change.what.c_str(),
                    error.what());
        ++totals.problems;
        return;
    }
    ++totals.reads;
    const bool follows = FollowsInstrument(*change.old_commands, change.console, was);
    const std::optional<uint32_t> cut = EarlierEnd(was, after);
    totals.loops_apart += cut ? 1 : 0;

    // On a driver whose started tracks take the starting track's settings, the tracks that the edited track or a
    // track that shares the changed commands starts, and those that they start, take the new instrument.
    const std::set<int> taking =
        TakingTracks(*change.old_commands, change.console, was, change.changed,
                     other == change.sound ? std::optional<int>(change.track->number) : std::nullopt);

    // Each track plays as before. The edited track's notes of the old instrument play the new one. The notes that
    // match at the start show the first end that moves.
    std::optional<uint32_t> moved;
    const std::vector<Comparison> comparisons = CompareTracks(archive, change, other, was, after, cut, moved);

    // The tracks that play otherwise, and the other sounds with such tracks, are in the edited track's reach. From
    // the first end that moves, a track of the edited sound can play otherwise outside it.
    CheckChangesReached(archive, change, other, was, comparisons, moved, follows, totals);

    CountTrackChanges(archive, change, was, comparisons, taking, moved, follows, totals);
}

// Makes edit `kind` of `track`, from instrument `from` to `to`, in the sequence file `sequence` of `archive`, named
// `what`, and returns the edited file. Returns nothing, and counts why, when the edit is refused or fails, or when the
// edited file outgrows the archive.
std::optional<edit::Edited> ApplyInstrumentEdit(const Archive& archive, uint32_t sequence, const song::Track& track,
                                                const edit::Program& from, const edit::Program& to, std::size_t kind,
                                                const std::string& what, Totals& totals)
{
    edit::Edited edited;
    try
    {
        edited = edit::EditInstrument(archive.file, sequence, track.number, track.stretches, from, to);
    }
    catch (const std::invalid_argument& error)
    {
        ++totals.refusals[error.what()];
        return std::nullopt;
    }
    catch (const std::exception& error)
    {
        std::printf("%s: %s fails: %s\n", archive.name.c_str(), what.c_str(), error.what());
        ++totals.problems;
        return std::nullopt;
    }

    try
    {
        if (hokuto::tools::Outgrows(archive.archive, edited.file, sequence))
        {
            ++totals.outgrown;
            return std::nullopt;
        }
    }
    catch (const std::runtime_error& error)
    {
        std::printf("%s: %s fails: %s\n", archive.name.c_str(), what.c_str(), error.what());
        ++totals.problems;
        return std::nullopt;
    }
    ++totals.edits[kind];

    return edited;
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
    const std::optional<edit::Edited> made =
        ApplyInstrumentEdit(archive, sequence, *track, from, *to, kind, what, totals);
    if (!made)
    {
        return;
    }
    const edit::Edited& edited = *made;

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
    uint32_t voices = 0;
    const std::set<int> channels = GbaChannelsOf(archive, sound, *track, from, *to, voices);
    const std::optional<edit::Reach> reach =
        ReachOf(archive, sound, static_cast<std::size_t>(track - before.tracks.data()), totals);
    if (!reach)
    {
        std::printf("%s: the reach of %s can't be found\n", archive.name.c_str(), what.c_str());
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

    const InstrumentEdit change{.sound = sound,
                                .track = track,
                                .from = from,
                                .to = *to,
                                .kind = kind,
                                .what = what,
                                .old_commands = &old_commands,
                                .console = console,
                                .changed = std::move(changed),
                                .voices = voices,
                                .channels = channels,
                                .reach = &*reach};
    for (const uint32_t other : archive.by_sequence.at(sequence))
    {
        CheckOtherSound(archive, built, other, change, totals);
    }
}

// Opens the archive at `path` into `archive`, and makes its Hokuto file with the group files beside it. Throws where
// the archive can't be read, or where the tool doesn't check its driver.
void OpenInstrumentsArchive(const std::filesystem::path& path, Archive& archive)
{
    // The archive and the group files beside it make a Hokuto file.
    std::vector<uint8_t> bytes;
    xsflib::ReadFile(path, bytes);
    archive.archive = OpenChecked(path, bytes);
    if (archive.archive.Konami())
    {
        throw std::invalid_argument("the tool doesn't check Konami's driver: hokuto_check_konami_mix does");
    }
    archive.file = CheckedForm(ImportChecked(bytes, archive.archive), bytes);
    archive.places = PlacesOfChecked(archive.file, bytes, archive.archive);
}

// The sounds that take part: up to four sounds of each sequence of `archive`, spread over the sounds that play it, or
// the sound named `only`. Counts the sequences in `totals`.
std::vector<uint32_t> ChooseSounds(const Archive& archive, const std::string& only, Totals& totals)
{
    std::vector<uint32_t> chosen;
    for (const auto& [sequence, sounds] : archive.by_sequence)
    {
        ++totals.sequences;
        std::vector<uint32_t> named;
        for (const uint32_t sound : sounds)
        {
            if (only.empty() || archive.songs.at(sound).name == only)
            {
                named.push_back(sound);
            }
        }
        for (const std::size_t index : SpreadIndices(named.size(), kMostSounds))
        {
            chosen.push_back(named[index]);
        }
    }

    return chosen;
}

// Prints the refusals and the summary of `totals`, under the name of `archive`.
void PrintTotals(const Archive& archive, const Totals& totals)
{
    for (const auto& [message, count] : totals.refusals)
    {
        std::printf("%s: refused %zu times: %s\n", archive.name.c_str(), count, message.c_str());
    }
    PrintSaves(archive.name);
    if (archive.archive.gba)
    {
        std::printf(
            "%s: apart on the GBA: %zu edits stayed out: the song outgrew its place, and the ROM has no room "
            "for it. %zu tracks gained or lost notes on the channels of the edited notes.\n",
            archive.name.c_str(), totals.outgrown, totals.channels);
    }
    if (archive.archive.ds)
    {
        std::printf("%s: apart on the DS: %zu tracks gained or lost notes that the driver never started.\n",
                    archive.name.c_str(), totals.ds_channels);
    }
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
}

} // namespace

} // namespace hokuto::tools::instruments

using hokuto::tools::instruments::Archive;
using hokuto::tools::instruments::ChooseSounds;
using hokuto::tools::instruments::EditSound;
using hokuto::tools::instruments::kEdits;
using hokuto::tools::instruments::OpenInstrumentsArchive;
using hokuto::tools::instruments::PrintTotals;
using hokuto::tools::instruments::Totals;

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

    const std::filesystem::path path = xsflib::PathFromUtf8(arguments[1]);
    Archive archive;
    archive.name = xsflib::Utf8(path.filename());
    try
    {
        OpenInstrumentsArchive(path, archive);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", archive.name.c_str(), error.what());
        return 2;
    }

    // The sequence sounds with a place, by their sequence.
    Totals totals;
    hokuto::tools::SequenceSounds sequence_sounds =
        hokuto::tools::ReadSequenceSounds(archive.archive, archive.places, {}, archive.songs);
    archive.by_sequence = std::move(sequence_sounds.by_sequence);
    totals.sounds = sequence_sounds.read;
    totals.unreadable = sequence_sounds.unreadable;

    // Up to four sounds of each sequence, spread over the sounds that play it, or the named sound.
    const std::vector<uint32_t> chosen = ChooseSounds(archive, only, totals);
    totals.taking_part = chosen.size();
    for (std::size_t kind = 0; kind < kEdits; kind++)
    {
        for (const uint32_t sound : chosen)
        {
            // A throw fails the edit with a line, and the run goes on.
            try
            {
                EditSound(archive, sound, kind, totals);
            }
            catch (const std::exception& error)
            {
                std::printf("%s: %s: an edit fails: %s\n", archive.name.c_str(), archive.songs.at(sound).name.c_str(),
                            error.what());
                ++totals.problems;
            }
        }
    }
    PrintTotals(archive, totals);

    return totals.problems == 0 && totals.unlisted == 0 ? 0 : 1;
}
