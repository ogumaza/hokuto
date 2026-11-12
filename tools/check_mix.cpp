// SPDX-License-Identifier: MIT

// Checks the edits of a track's mix (src/edit/mix.h) on a sound archive of the Wii, the 3DS, the Wii U, the Switch or
// the DS, on a GBA ROM whose songs MusicPlayer2000 plays, or on the DSE files of a DS image. The tool imports the
// archive into a Hokuto file the way the program does. From a DS cartridge image, the tool uses the largest sound
// archive, or the DSE files where the image contains no sound archive (tools/checked_archive.h). The tool refuses a ROM
// with Konami's driver: hokuto_check_konami_mix checks its mix. The tool reads each sequence sound with ntdlib's
// performer. It then makes five rounds of edits, one for each setting: the volume, the pan, and the sends to the aux
// buses A to C. In each round, one track of each sequence file takes an edit, through the first sound that plays the
// file. The tracks that play notes take turns from round to round. The edit starts from the value that the mixer shows
// at the track's first note. A level halves, or goes from 0 to 64. A pan moves 16 steps to the right, or to the left
// from 48 up.
//
// The tool works out each edit's map from old values to new ones. Where the track's commands give the setting one
// value, the map takes every value to the new one. Otherwise it scales a level and shifts a pan. The default value
// counts where a note plays before the track's first command. A new command then opens the track with the default
// mapped, unless the map leaves the default as it is. Where the command before the track's start can run on into it, a
// jump goes before the new command. A return can run on: a sound that reaches it outside every call has no call to
// return to, and the player runs on past it. The tool also works out which edits the sequence refuses. The 3DS's tracks
// have no send to bus C, and the tool works out a refusal for each edit of that round there. A GBA track's volume runs
// from 0 to 127 and starts at 0, and the GBA's tracks have no sends. Brownie Brown's driver refuses every pan, and the
// volume of a PSG channel. Its volume round uses the tracks of the sample channels where any of them play notes.
//
// On DSE, the tracks of one channel of the driver share its volume and pan, and an edit changes the settings of the
// stretches of the channel (song::MixStretches). A channel's volume starts at 100. The targets of the driver's slides
// (0xE2 and 0xEA) map like values, a volume change's amount (0xE1) scales, and a pan change's (0xE9) stays. A new
// command at the track opening is inserted after the selects of a bank there. The performer shows the starting volume
// at tick 0, and the tool compares a track's volumes against it (AgainstDseStart). DSE's tracks have no sends, and each
// edit of a send has to be refused. A track that has no end runs past its chunk, into bytes that the file stores as
// data, and counts apart.
//
// After each round, the tool exports the edited file and checks four things:
//
// - The sequence refuses the edits that the tool worked out as refused, and no others.
// - The archive, imported again, gives back the edited sequences. Each edited command holds its mapped value, and a new
//   command and a jump stand where they were worked out. A GBA song can hold zeros after its items
//   (tools::HoldsSequence).
// - Each sound plays the notes that it played before. ntdlib's performer can find a sound's loop a whole number of
//   passes away after an edit. The sound then compares up to the earlier loop's end, and counts apart. A sound without
//   a loop ends once its last release falls silent, and an edit of its levels can move its end. Every note of both
//   reads then has to start before the earlier end. The notes' lengths and the settings compare up to that end, and
//   the sound counts apart with a line. Any other change of the sound's loop or length fails.
// - Each track's settings change as the edit says. A track whose commands of the setting all took the edit shows each
//   of its old values mapped. A command that set the default shows no change before the edit, and a change to the
//   mapped default after it. A track that the new command opens starts with the default mapped: a track with the
//   edited track's number, opened where the edited track opened. A sound that runs on into the edited track's start
//   doesn't run the new command. Every other setting of every track stays as it was.
//
// Some tracks are counted apart: a track that runs both edited commands and other commands of the setting, and a track
// whose setting moves over time or takes random values. The values that such a track shows depend on more than the map.
// On the GBA and on Nintendo R&D2's driver, a sound counts apart where its notes differ only on PSG channels of the
// tracks whose volume or pan changed (PsgNotesApart and Rd2PsgNotesApart). On the DS, the driver assigns a note the
// quietest of the channels of the lowest priority. A volume edit can therefore give a note another channel. After a
// volume edit, a sound counts apart where each note that is missing from one read is a note that the other read lists
// among the notes that the driver never started, or plays with another length where either read cuts it short. A note
// whose length runs out where a later note claims its channel can also end up to a frame sooner. The notes of a chord
// can also take each other's channels, and with them each other's lengths (ChannelsDecide). A ROM without free space
// has no room for a song that its edits make longer. The edits of those songs then stay out, and the tool counts the
// songs apart. With --filler, the songs move into the filler runs of the ROM instead
// (format::NativeArchive::gba_filler), and every edit is made. On the GBA's other drivers, a song's edit stays out
// where the export then has no room for it, such as in the room of a list of songs on Quintet's driver
// (tools::ExportOutgrows). A volume step of Quintet's driver (F1), a volume up (5F) and a volume down (6E) add to the
// volume, and the edit refuses a new volume on a track that runs one. A new volume on another of the GBA's drivers
// stops at the most that the driver allows the track (DriverRules::MostVolume). Krawall sets a channel's volume and pan
// with each note and each row, and its channels have no sends. The edit refuses every setting there, and the tool works
// out each refusal.
//
// The tool also checks what the mixer lists before an edit (src/edit/reach.h). Every other track of the edited sound
// whose setting changed has to be among the tracks that the edited track's reach lists. Every other sound with such a
// track, or with its loop on another pass, has to be among the reach's sounds. The tool counts the listed tracks and
// sounds, and those that changed. It also gives the time of the slowest reach.
//
// With --timelines, the tool opens the file's timelines as the program does (edit/timelines.h). Each edit of a timeline
// then takes the converter's layout as the program settles it (edit::SettleTimelines). The tool counts apart the edits
// that the layout can't hold. The layout places the commands as the converter does. The tool therefore doesn't check
// where a timeline's commands stand. The file that the saved archive imports into, with the timelines' hints, has to
// give back the settled sequences. The sounds play from the archive that the program plays, with every bar in its
// track. The archive that a saved file holds, with the converter's subroutines, has to play the same notes and
// settings.
//
// The tool prints a line for each track that differs, and a summary. It returns 1 when an edit or a track differs from
// what the tool worked out, or when an export or a read fails. With a sound's name, it checks that sound alone.
//
// With --model, the file goes into the model form before the edits (format::InModelForm), and the archive that the
// model builds has to be the game's. Each file that the edits make then has to save in both forms and open again as it
// was (tools/checked_archive.h).
//
// usage: hokuto_check_mix [--model] [--filler] [--timelines] <archive> [sound]

#include "check_mix.h"

namespace
{

using hokuto::edit::TrackReach;
using hokuto::format::File;
using hokuto::song::Mix;
using hokuto::song::Sequence;
using hokuto::song::Track;
using hokuto::tools::CheckedArchive;
using hokuto::tools::CheckedSound;
using hokuto::tools::mix::AgainstDseStart;
using hokuto::tools::mix::CheckSoundAgain;
using hokuto::tools::mix::HoldsEdit;
using hokuto::tools::mix::kSettings;
using hokuto::tools::mix::NameOf;
using hokuto::tools::mix::Plan;
using hokuto::tools::mix::PlanFor;
using hokuto::tools::mix::Playing;
using hokuto::tools::mix::PsgNotesApart;
using hokuto::tools::mix::RunsIntoData;
using hokuto::tools::mix::SequenceOf;
using hokuto::tools::mix::Totals;

// Exports `edited`, `file` with the edits of `plans`, from a GBA ROM. Where the ROM has no room for the songs that the
// edits make longer, their plans leave `plans`, and `edited` is made again with the other edits. Throws on any other
// failure. `name` is the name of the ROM, and `setting` the setting of the round.
void DropOutgrownEdits(const File& file, File& edited, std::map<uint32_t, Plan>& plans, const std::string& name,
                       Mix setting, Totals& totals)
{
    try
    {
        hokuto::format::ExportArchive(edited);
    }
    catch (const std::invalid_argument& error)
    {
        const std::vector<uint32_t> moved = hokuto::format::MovedSongs(edited);
        if (moved.empty() || std::string(error.what()).find("32 MiB") == std::string::npos)
        {
            throw;
        }

        std::printf(
            "%s: %zu songs outgrow their places after the %s edits, and the ROM has no room for "
            "them. Their edits stay out.\n",
            name.c_str(), moved.size(), NameOf(setting));
        totals.outgrown += moved.size();
        for (const uint32_t sequence : moved)
        {
            const auto found = plans.find(sequence);
            if (found != plans.end())
            {
                totals.new_commands -= found->second.opening ? 1 : 0;
                totals.jumps -= found->second.jump ? 1 : 0;
                totals.listed_tracks -= found->second.reach.tracks.size();
                totals.listed_sounds -= found->second.reach.sounds.size();
                plans.erase(found);
            }
        }
        edited = file;
        for (const auto& [sequence, plan] : plans)
        {
            edited = hokuto::edit::EditMix(edited, sequence, plan.track, plan.stretches, plan.edit).file;
        }
    }
}

// Reads each sequence sound of `archive` as `playing` plays it into `before`, or only the sound named `only`. Returns
// the first sound that plays each sequence file and plays notes.
std::map<uint32_t, uint32_t> ReadSoundsBefore(const CheckedArchive& archive, const CheckedArchive& playing,
                                              const std::vector<std::optional<uint32_t>>& sequences,
                                              const std::string& only, const std::string& name,
                                              std::vector<std::optional<Sequence>>& before, Totals& totals)
{
    std::map<uint32_t, uint32_t> first_sounds;
    for (uint32_t sound = 0; sound < archive.SoundCount(); ++sound)
    {
        const CheckedSound info = archive.Info(sound);
        if (!info.sequence || (!only.empty() && info.name != only))
        {
            continue;
        }

        ++totals.sounds;
        try
        {
            before[sound] = playing.Read(sound);
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s can't be read before the edits: %s\n", name.c_str(), info.name.c_str(), error.what());
            ++totals.unread;
            continue;
        }

        const std::optional<uint32_t> sequence = SequenceOf(sequences, info);
        if (sequence && !Playing(*before[sound]).empty())
        {
            first_sounds.try_emplace(*sequence, sound);
        }
    }

    return first_sounds;
}

// The offsets of the commands of each sequence of `first_sounds` in `file`. A sequence whose offsets can't be found
// leaves `first_sounds`, and sets `failed`.
std::map<uint32_t, std::vector<uint32_t>> FindOffsets(const File& file, const CheckedArchive& archive,
                                                      const std::string& name,
                                                      std::map<uint32_t, uint32_t>& first_sounds, bool& failed)
{
    std::map<uint32_t, std::vector<uint32_t>> offsets;
    for (auto entry = first_sounds.begin(); entry != first_sounds.end();)
    {
        const auto [sequence, sound] = *entry;
        try
        {
            offsets[sequence] = hokuto::edit::Offsets(*file.sequences[sequence], hokuto::edit::ConsoleOf(file));
            ++entry;
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s: the offsets of the sequence's commands can't be found: %s\n", name.c_str(),
                        archive.NameOf(sound).c_str(), error.what());
            failed = true;
            entry = first_sounds.erase(entry);
        }
    }

    return offsets;
}

// The tracks and sounds that the mixer lists for an edit of each track of each sound of `first_sounds`, by sound. A
// sound whose reach can't be found sets `failed`.
std::map<uint32_t, std::vector<TrackReach>> MixerListings(
    const File& file, std::span<const std::optional<hokuto::edit::SoundPlace>> places, const CheckedArchive& archive,
    const std::string& name, const std::map<uint32_t, uint32_t>& first_sounds,
    const std::vector<std::optional<Sequence>>& before, bool& failed, Totals& totals)
{
    std::map<uint32_t, std::vector<TrackReach>> reaches;
    for (const auto& [sequence, sound] : first_sounds)
    {
        const auto start = std::chrono::steady_clock::now();
        try
        {
            reaches[sound] = hokuto::edit::ReachOfTracks(file, places, sound, *before[sound]);
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s: the reach can't be found: %s\n", name.c_str(), archive.NameOf(sound).c_str(),
                        error.what());
            failed = true;
        }
        const std::chrono::duration<double> taken = std::chrono::steady_clock::now() - start;
        totals.slowest_reach = std::max(totals.slowest_reach, taken.count());
    }

    return reaches;
}

// The track of `song`, sequence `sequence` of `file`, that round `round` edits: one that plays notes, in turn.
const Track& EditedTrack(const File& file, const Sequence& song, uint32_t sequence, std::size_t round, Mix setting)
{
    std::vector<const Track*> tracks = Playing(song);
    // On Brownie Brown's driver, only a sample channel's volume is a level. The volume round uses those tracks where
    // any of them play notes.
    const hokuto::song::Console console = hokuto::edit::ConsoleOf(file);
    if (setting == Mix::kVolume && console.driver && console.driver->DriverEngine() == hokuto::format::Engine::kBrownie)
    {
        std::vector<const Track*> samples;
        std::ranges::copy_if(tracks, std::back_inserter(samples),
                             [&console](const Track* playing) { return !console.driver->PsgTrack(playing->number); });
        if (!samples.empty())
        {
            tracks = std::move(samples);
        }
    }

    return *tracks[(sequence + round) % tracks.size()];
}

// The file `edited` with the edit of `plan` to sequence `sequence`, as the program makes it, or nothing where the edit
// isn't made: the sequence refuses it (as the plan says, or otherwise), the timeline refuses it, or the ROM has no room
// for the song. Sets `failed` where the edit fails otherwise. `sound` plays the sequence.
std::optional<File> MixedFile(const File& edited, uint32_t sequence, const Plan& plan, const CheckedArchive& archive,
                              uint32_t sound, Mix setting, const std::string& name, bool& failed, Totals& totals)
{
    std::optional<std::string> refused;
    std::optional<File> mixed;
    try
    {
        mixed = hokuto::edit::EditMix(edited, sequence, plan.track, plan.stretches, plan.edit).file;
    }
    catch (const std::invalid_argument& error)
    {
        refused = error.what();
    }
    catch (const std::exception& error)
    {
        std::printf("%s: sequence %u can't take its edit: %s\n", name.c_str(), sequence, error.what());
        failed = true;
        return std::nullopt;
    }

    if (refused.has_value() != plan.refusal.has_value())
    {
        std::printf("%s: %s: track %d's %s from %d to %d: %s, where the tool worked out %s\n", name.c_str(),
                    archive.NameOf(sound).c_str(), plan.track, NameOf(setting), plan.edit.from, plan.edit.to,
                    refused ? ("refused: " + *refused).c_str() : "taken",
                    plan.refusal ? ("a refusal for " + *plan.refusal).c_str() : "no refusal");
        ++totals.wrong_edits;
        return std::nullopt;
    }

    if (refused)
    {
        ++totals.refused;
        std::printf("%s: %s: track %d's %s from %d to %d is refused for %s\n", name.c_str(),
                    archive.NameOf(sound).c_str(), plan.track, NameOf(setting), plan.edit.from, plan.edit.to,
                    plan.refusal->c_str());
        return std::nullopt;
    }

    // Like the program, the tool settles an edit of a timeline in the converter's layout.
    if (hokuto::edit::IsTimeline(edited, sequence))
    {
        try
        {
            mixed = hokuto::edit::SettleTimelines(edited, std::move(*mixed)).file;
        }
        catch (const std::invalid_argument& error)
        {
            std::printf("%s: %s: track %d's %s from %d to %d is refused by the timeline: %s\n", name.c_str(),
                        archive.NameOf(sound).c_str(), plan.track, NameOf(setting), plan.edit.from, plan.edit.to,
                        error.what());
            ++totals.timeline_refused;
            return std::nullopt;
        }
        ++totals.timeline_edits;
    }

    // A ROM of another of the GBA's drivers can have no room for the song that the edit makes longer. The
    // export further on reports any other refusal.
    bool roomless = false;
    try
    {
        roomless = archive.driver && hokuto::tools::ExportOutgrows(*mixed);
    }
    catch (const std::exception&)
    {
        roomless = false;
    }
    if (roomless)
    {
        std::printf("%s: %s: track %d's %s from %d to %d stays out: the ROM has no room for the song\n", name.c_str(),
                    archive.NameOf(sound).c_str(), plan.track, NameOf(setting), plan.edit.from, plan.edit.to);
        ++totals.roomless;
        return std::nullopt;
    }

    return mixed;
}

// Counts and prints each edit of `plans` that `edited` or `again` is missing. `edited` is `file` after the edits.
// `after` is the archive that the program plays, and `again` is the file that it imports into again.
void CheckEditsComeBack(const File& file, const File& edited, const CheckedArchive& after, const File& again,
                        const std::map<uint32_t, Plan>& plans, const std::string& name, Mix setting, Totals& totals)
{
    for (const auto& [sequence, plan] : plans)
    {
        const std::optional<uint64_t> field = after.driver ? after.driver->FieldsOf(plan.sound).sequence : std::nullopt;
        const std::optional<uint32_t> back_sequence =
            after.gba ? hokuto::format::GbaSongSequence(again, after.gba->info.song_table, static_cast<int>(plan.sound))
            : after.driver ? (field ? hokuto::format::SequenceNamedAt(again, *field) : std::nullopt)
                           : std::optional<uint32_t>(sequence);
        const bool back =
            again.sequences.size() == edited.sequences.size() && back_sequence &&
            hokuto::tools::HoldsSequence(after, *edited.sequences[sequence], *again.sequences[*back_sequence]);
        const bool timeline = hokuto::edit::IsTimeline(edited, sequence);
        if (!back || (!timeline && !HoldsEdit(*file.sequences[sequence], *edited.sequences[sequence], plan)))
        {
            std::printf("%s: sequence %u: the %s edit of track %d %s\n", name.c_str(), sequence, NameOf(setting),
                        plan.track, back ? "isn't in the commands" : "doesn't come back from the archive");
            ++totals.wrong_edits;
        }
    }
}

// Counts the timelines of `file`, then prints the totals of the check of the archive `name`. With `timelines`, the
// totals include those of the timelines.
void PrintTotals(const std::string& name, const CheckedArchive& archive, const File& file, bool timelines,
                 Totals& totals)
{
    for (uint32_t sequence = 0; sequence < file.sequences.size(); ++sequence)
    {
        totals.timelines += hokuto::edit::IsTimeline(file, sequence) ? 1 : 0;
    }
    const std::string timeline_line =
        timelines ? " " + std::to_string(totals.timelines) + " sequences are timelines. They took " +
                        std::to_string(totals.timeline_edits) + " edits, and refused " +
                        std::to_string(totals.timeline_refused) + ". " + std::to_string(totals.saved) +
                        " sounds play otherwise from the saved archive."
                  : "";
    std::printf(
        "%s: %zu sequence sounds. %zu edits in 5 rounds, %zu of them refused, and %zu with a new command, %zu of "
        "those after a jump. %zu edits differ from what the tool worked out. %zu tracks checked: %zu settings mapped, "
        "%zu of them with a command that set the default, %zu opened by a new command, %zu that run on past one, and "
        "%zu that differ. %zu sounds whose notes differ, %zu whose loop or length moves otherwise, and %zu that can't "
        "be read. Apart: %zu tracks that run edited and other commands, %zu that move or take random values and "
        "differ, %zu sounds with the loop a whole number of passes away, and %zu without a loop that end elsewhere. "
        "The reaches list %zu tracks and %zu sounds. %zu of the tracks and %zu of the sounds changed. %zu tracks and "
        "sounds changed outside the reach. The slowest reach took %.1f ms.%s\n",
        name.c_str(), totals.sounds, totals.edits, totals.refused, totals.new_commands, totals.jumps,
        totals.wrong_edits, totals.tracks, totals.mapped, totals.again, totals.opened, totals.runs_past, totals.differ,
        totals.note_sounds, totals.moved_loops, totals.unread, totals.mixed, totals.moving, totals.other_passes,
        totals.other_ends, totals.listed_tracks, totals.listed_sounds, totals.reached_tracks, totals.reached_sounds,
        totals.unlisted, totals.slowest_reach * 1000.0, timeline_line.c_str());
    if (archive.gba)
    {
        std::printf(
            "%s: Apart on the GBA: %zu sounds whose notes of a PSG channel end elsewhere after a volume or a "
            "pan edit, and %zu songs that outgrow their places in a ROM without room for them.\n",
            name.c_str(), totals.psg_sounds, totals.outgrown);
    }
    if (archive.driver)
    {
        std::printf(
            "%s: Apart on the GBA's other drivers: %zu edits that stay out: the ROM has no room for the song.\n",
            name.c_str(), totals.roomless);
    }
    if (archive.ds)
    {
        std::printf("%s: Apart on the DS: %zu sounds whose notes take other channels after a volume edit.\n",
                    name.c_str(), totals.channel_sounds);
    }
    if (archive.dse)
    {
        std::printf(
            "%s: Apart on DSE: %zu tracks that run past their chunks, into bytes that the file holds as data.\n",
            name.c_str(), totals.past_end);
    }
    hokuto::tools::PrintSaves(name);
}

// One run of the check on the archive `name`: the file before the edits, each sequence sound before them, and what
// each round's edits take.
struct MixRun
{
    std::string name;
    bool timelines = false;

    // The archive, the archive that the program plays, the Hokuto file, the sequence of each of the archive's files,
    // and where each sound plays in the file.
    CheckedArchive archive;
    CheckedArchive playing;
    File file;
    std::vector<std::optional<uint32_t>> sequences;
    std::vector<std::optional<hokuto::edit::SoundPlace>> places;

    // Each sequence sound before the edits, the first sound that plays each sequence file, the offsets of each
    // sequence's commands, and the tracks and sounds that the mixer lists for an edit of each track of each sound.
    std::vector<std::optional<Sequence>> before;
    std::map<uint32_t, uint32_t> first_sounds;
    std::map<uint32_t, std::vector<uint32_t>> offsets;
    std::map<uint32_t, std::vector<TrackReach>> reaches;
};

// Opens the archive at `path` into `run`: the archive, the Hokuto file that it imports into, and the archive that the
// program plays. Throws when one of them fails, or for Konami's driver.
void OpenMixFile(const std::filesystem::path& path, MixRun& run)
{
    std::vector<uint8_t> bytes;
    xsflib::ReadFile(path, bytes);
    run.archive = hokuto::tools::OpenChecked(path, bytes);
    if (run.archive.Konami())
    {
        throw std::invalid_argument("the tool doesn't check Konami's driver: hokuto_check_konami_mix does");
    }
    run.file = hokuto::tools::CheckedForm(hokuto::tools::ImportChecked(bytes, run.archive), bytes);
    run.sequences = hokuto::tools::SequencesOfChecked(run.file, bytes, run.archive);
    run.places = hokuto::tools::PlacesOfChecked(run.file, bytes, run.archive);
    run.playing = run.archive;
    if (run.timelines)
    {
        run.file = hokuto::edit::OpenTimelines(std::move(run.file));
        run.playing = hokuto::tools::BuildChecked(run.file, run.name);
    }
}

// Plans one edit of `setting` for each sequence file of `run`, on a track that plays notes, and makes it in `edited`.
// Returns the plans by sequence. Sets `failed` where a plan or an edit fails.
std::map<uint32_t, Plan> PlanRoundEdits(const MixRun& run, std::size_t round, Mix setting, File& edited, bool& failed,
                                        Totals& totals)
{
    std::map<uint32_t, Plan> plans;
    for (const auto& [sequence, sound] : run.first_sounds)
    {
        const Track& track = EditedTrack(run.file, *run.before[sound], sequence, round, setting);
        // A DSE track's settings are those of its channel.
        Track channel_track = track;
        channel_track.stretches = hokuto::song::MixStretches(*run.before[sound], track);
        std::optional<Plan> plan = PlanFor(run.file, sequence, run.offsets.at(sequence), sound, channel_track, setting);
        if (!plan && run.archive.dse &&
            RunsIntoData(*run.file.sequences[sequence], run.offsets.at(sequence), channel_track))
        {
            ++totals.past_end;
            continue;
        }
        if (!plan)
        {
            std::printf("%s: %s: track %d's stretches aren't made of items\n", run.name.c_str(),
                        run.archive.NameOf(sound).c_str(), track.number);
            failed = true;
            continue;
        }

        ++totals.edits;
        std::optional<File> mixed =
            MixedFile(edited, sequence, *plan, run.archive, sound, setting, run.name, failed, totals);
        if (!mixed)
        {
            continue;
        }
        edited = std::move(*mixed);

        totals.new_commands += plan->opening ? 1 : 0;
        totals.jumps += plan->jump ? 1 : 0;
        const auto reach = run.reaches.find(sound);
        const auto index = static_cast<std::size_t>(&track - run.before[sound]->tracks.data());
        if (reach != run.reaches.end() && index < reach->second.size())
        {
            plan->reach = reach->second[index].mix[static_cast<std::size_t>(setting)];
            totals.listed_tracks += plan->reach.tracks.size();
            totals.listed_sounds += plan->reach.sounds.size();
        }
        plans.emplace(sequence, std::move(*plan));
    }

    return plans;
}

// The archives of a round's edited file: the archive that a saved file holds, the archive that the program plays, and
// the file that the first imports into again.
struct RoundArchives
{
    CheckedArchive saved;
    CheckedArchive after;
    File again;
};

// Makes round `round` of the edits of `run`: the edits of `kSettings[round]`, the export, and the checks of each
// sound again. Returns false where the edited file can't be exported.
bool RunMixRound(MixRun& run, std::size_t round, bool& failed, Totals& totals)
{
    // One edit for each sequence file, on a track that plays notes.
    const Mix setting = kSettings[round];
    File edited = run.file;
    std::map<uint32_t, Plan> plans = PlanRoundEdits(run, round, setting, edited, failed, totals);

    // The edited archive that a saved file holds, the archive that the program plays, and the file that the first
    // imports into again. That file takes the timelines' hints that a saved file keeps.
    RoundArchives built;
    try
    {
        // A GBA ROM without free space has no room for the songs that their edits make longer. Their edits then
        // stay out, and the others go in again.
        if (run.archive.gba)
        {
            DropOutgrownEdits(run.file, edited, plans, run.name, setting, totals);
        }

        const File folded = hokuto::edit::FoldTimelines(edited);
        hokuto::tools::CheckSaved(folded);
        const std::vector<uint8_t> exported = hokuto::format::ExportArchive(folded);
        const std::vector<std::vector<uint8_t>> group_files = hokuto::format::ExportGroupFiles(folded);
        built.saved = hokuto::tools::BuildChecked(folded, run.name);
        built.after = run.timelines ? hokuto::tools::BuildChecked(edited, run.name) : built.saved;
        built.again = hokuto::tools::ImportChecked(exported, run.archive, group_files);
        if (run.timelines)
        {
            built.again.timelines = edited.timelines;
            built.again = hokuto::edit::OpenTimelines(std::move(built.again));
        }
    }
    catch (const std::exception& error)
    {
        std::printf("%s: the file after the %s edits can't be exported: %s\n", run.name.c_str(), NameOf(setting),
                    error.what());
        return false;
    }

    // The import of a GBA ROM numbers the songs' sequences in the order of their places in the ROM. A song that grew
    // moves to the free space, and the song table points to its sequence. So does the table of another of the GBA's
    // drivers.
    CheckEditsComeBack(run.file, edited, built.after, built.again, plans, run.name, setting, totals);

    // Each sound that plays an edited sequence file, again.
    for (uint32_t sound = 0; sound < run.archive.SoundCount(); ++sound)
    {
        CheckSoundAgain(sound, run.archive, run.file, run.sequences, run.before, plans, built.saved, built.after,
                        run.timelines, run.offsets, setting, run.name, totals);
    }

    return true;
}

} // namespace

int main(int argc, char** argv)
{
    const std::optional<hokuto::tools::EditOptions> options = hokuto::tools::ParseEditOptions(
        xsflib::console::Arguments(argc, argv),
        "usage: hokuto_check_mix [--model] [--filler] [--timelines] <archive> [sound]\n");
    if (!options)
    {
        return 2;
    }

    // The archive and the group files beside it make a Hokuto file. `playing` is the archive that the program plays.
    MixRun run;
    run.name = xsflib::Utf8(options->path.filename());
    run.timelines = options->timelines;
    try
    {
        OpenMixFile(options->path, run);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", run.name.c_str(), error.what());
        return 2;
    }

    // Each sequence sound before the edits, and the first sound that plays each sequence file.
    Totals totals;
    run.before.resize(run.archive.SoundCount());
    run.first_sounds =
        ReadSoundsBefore(run.archive, run.playing, run.sequences, options->only, run.name, run.before, totals);

    // A sequence whose command offsets can't be found receives no edits, and the tool fails.
    bool failed = false;
    run.offsets = FindOffsets(run.file, run.archive, run.name, run.first_sounds, failed);

    // The tracks and sounds that the mixer lists for an edit of each track of each sound that takes the edits.
    run.reaches =
        MixerListings(run.file, run.places, run.archive, run.name, run.first_sounds, run.before, failed, totals);

    for (std::size_t round = 0; round < kSettings.size(); ++round)
    {
        if (!RunMixRound(run, round, failed, totals))
        {
            return 1;
        }
    }

    PrintTotals(run.name, run.archive, run.file, run.timelines, totals);

    const bool differ = totals.wrong_edits != 0 || totals.differ != 0 || totals.note_sounds != 0 || totals.saved != 0 ||
                        totals.moved_loops != 0;
    return !differ && totals.unread == 0 && totals.unlisted == 0 && !failed ? 0 : 1;
}
