// SPDX-License-Identifier: MIT

// Checks the edits of the mixer and the inspector on a GBA ROM with Konami's driver. A track of that driver sets its
// volume in each note's command as well as in its commands of volume, some revisions hold no pan of a track, and the
// tracks have no sends. hokuto_check_mix's model applies to the other consoles' commands of a setting. This tool
// therefore models the driver's commands. Each song that first plays a sequence takes part, on one track with notes in
// each round.
//
// - Volume. The track's fader falls to half its value. Each note and each command of volume that the track runs takes
//   the new volume: the new value where the track contains one, and each value by the fader's factor where it contains
//   several. No other item changes, but for what the track before it sets for it. The export has to give the edited
//   sequence back. ntdlib's performer then reads the song. The track's notes have to play at their ticks, with their
//   keys and the new volumes. Every other track has to play as before. ArchivePlayer then plays the song for a few
//   seconds from both ROMs, and the track's channel has to peak lower, or stay silent where it was silent. The wave
//   channel's images hold whole digits, and two volumes can share a peak: such a channel counts apart. A note of a PSG
//   track off centre that gives way to a note of the track before it counts apart. A volume command can start a PSG
//   note again, at the command's volume. Where the new volume there is 0, the note stays silent, and counts with the
//   silent notes.
// - Pan. On the revisions that hold a pan of a track, the track's pan moves by a quarter of the scale. The track has to
//   be in the read, and the performer has to show the new pan at the track's first note, within a step of the
//   revision's scale. On the other revisions, the edit has to be refused. In the revisions whose records hold a pan, a
//   PSG track off centre plays on the next track's channel (agblib/konami/archive_player.h). The notes of the PSG
//   tracks then count apart. An edited PSG track's shown pan counts apart there too.
// - Sends. Each edit of a send has to be refused.
// - Instrument. The notes of the track that play its first note's sample take the next sample that the ROM's songs
//   play. A PSG track's notes take the next duty or wave of its channel. The track has to be in the read, the notes
//   have to keep their ticks, and every other track has to play as before. In the revisions whose records hold a pan,
//   the notes of the other PSG tracks count apart. The noise channel selects nothing.
//
// The tool also checks what the mixer and the inspector list before an edit (src/edit/reach.h). After each round, each
// sequence sound is read again. Every other track of an edited sound whose setting changed has to be among the tracks
// that the edited track's reach lists: its volume or its pan, or the instruments of its notes. Every other sound that
// changed, or whose loop moved to another pass, has to play an edited sequence and be among the sounds that the edit's
// reach lists. The tool counts the listed tracks and sounds, and those that changed. It also gives the time of the
// slowest reach.
//
// The performer finds a loop where the tracks' states repeat, and a new command can move it to another pass. The reads
// then compare up to the earlier loop's end. The tool prints a line for each problem and a summary, and returns 1 when
// it found a problem. With a song's name, only that song takes part. With --filler, a song that outgrows its place in a
// ROM that fills the GBA's 32 MiB goes into the ROM's filler runs.
//
// The check expects these refusals and counts them apart: a pan on a revision without a pan, every send
// (ExpectedPanRefusal, edit::RefusedSend), a volume edit of a track at -∞ dB at its first note
// (edit::RefusedSilentTrim), and a volume or instrument edit of a track whose notes other tracks' commands start
// (edit::kKonamiVolumeWithoutNotes, edit::kNoNotesInstrumentRefusal). Any other refusal is a problem, and the summary
// lists it with its count.
//
// usage: hokuto_check_konami_mix [--filler] <rom.gba> [song]

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "agblib/konami/archive_player.h"
#include "agblib/konami/seqformat.h"
#include "ntdlib/commands/sequence.h"
#include "xsflib/console.h"
#include "xsflib/files.h"

#include "check_support.h"
#include "checked_archive.h"
#include "edit/bytecode.h"
#include "edit/drivers.h"
#include "edit/instruments.h"
#include "edit/konami.h"
#include "edit/mix.h"
#include "edit/reach.h"
#include "edit/replace.h"
#include "format/archive.h"
#include "notes_apart.h"
#include "song/archive.h"
#include "song/konami.h"
#include "song/sequence.h"

namespace
{

using hokuto::edit::MixEdit;
using hokuto::edit::Reach;
using hokuto::edit::SoundPlace;
using hokuto::format::File;
using hokuto::song::Mix;
using hokuto::song::Sequence;
using hokuto::song::Track;
using hokuto::tools::CheckedArchive;
using hokuto::tools::TrackNumbered;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;

// The seconds of a song that the levels compare, and the driver's frames in them.
constexpr int kLevelFrames = 4 * 60;

struct Totals
{
    std::size_t songs = 0;
    std::size_t unread = 0;
    std::size_t volumes = 0;
    std::size_t volume_notes = 0;
    std::size_t silent_notes = 0; // notes that the new volume or the old one silences
    std::size_t lower = 0;        // tracks whose channels peak lower after a volume edit
    std::size_t silent = 0;       // tracks whose channels stay silent in the seconds that compare
    std::size_t same_peak = 0;    // wave tracks whose new volume's image peaks at the old one's digit
    std::size_t pans = 0;
    std::size_t new_pans = 0;     // pans that a new command gives a track without a pan
    std::size_t pan_refusals = 0; // on the revisions without a pan
    std::size_t psg_apart = 0;    // songs whose PSG notes differ after an edit of a pan
    std::size_t psg_pans = 0;     // edited PSG tracks that show another pan, where the records hold a pan
    std::size_t sends = 0;
    std::size_t joined = 0;     // PSG notes that a new volume silences or joins to the note before them
    std::size_t off_centre = 0; // notes of a PSG track off centre that gave way on the next channel
    std::size_t instruments = 0;
    std::size_t instrument_notes = 0;
    std::size_t psg_instruments = 0; // songs whose other PSG tracks play otherwise after an edit of an instrument
    std::size_t other_passes = 0;
    std::size_t outgrown = 0;
    std::size_t refused_on_purpose = 0;          // volume and instrument edits of a track that can't take them
    std::size_t listed_tracks = 0;               // the tracks that the reaches of the edits list
    std::size_t listed_sounds = 0;               // the sounds that the reaches of the edits list
    std::size_t reached_tracks = 0;              // listed tracks that changed
    std::size_t reached_sounds = 0;              // listed sounds that changed
    std::size_t unlisted = 0;                    // tracks and sounds that changed outside the reach
    double slowest_reach = 0.0;                  // seconds
    std::map<std::string, std::size_t> refusals; // the unexpected ones, by round and message
    std::size_t problems = 0;
};

// The expected refusals. The message that the mixer gives for a pan on a revision without a pan.
std::string ExpectedPanRefusal(const hokuto::song::Console& console)
{
    return console.driver != nullptr ? hokuto::edit::EditsOf(*console.driver).RefusedPan(*console.driver)
                                     : std::string();
}

// Whether another track's commands start every note of `track` that plays program `program`, or every note of the
// track when `program` is nothing. Such a track's commands start none of those notes.
bool OthersStart(const Track& track, std::optional<uint32_t> program)
{
    return std::ranges::all_of(track.notes, [&](const hokuto::song::Note& note)
                               { return (program && note.program != *program) || note.command_track >= 0; });
}

// Counts `error`, a refusal in `round` that the check doesn't expect: a problem, listed in the summary.
void CountUnexpected(const char* round, const std::exception& error, Totals& totals)
{
    ++totals.refusals[std::string(round) + ": " + error.what()];
    ++totals.problems;
}

// A note as the tool compares it: its start, key, length and velocity.
using Played = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>;

std::vector<Played> NotesOf(const Sequence& song, int number, uint32_t end)
{
    std::vector<Played> notes;
    for (const Track& track : song.tracks)
    {
        for (const hokuto::song::Note& note : track.notes)
        {
            if (track.number == number && note.start < end)
            {
                notes.emplace_back(note.start, note.key, note.length, note.velocity);
            }
        }
    }

    return notes;
}

// The tick up to which `before` and `after` compare: the earlier loop's end where the loops differ, and else every
// tick.
uint32_t CompareEnd(const Sequence& before, const Sequence& after)
{
    if (before.loop == after.loop)
    {
        return UINT32_MAX;
    }

    const uint32_t first = before.loop ? before.loop->end : before.length;
    const uint32_t second = after.loop ? after.loop->end : after.length;
    return std::min(first, second);
}

// The same, and a read that compares up to an earlier loop counts in `totals`.
uint32_t EndOf(const Sequence& before, const Sequence& after, Totals& totals)
{
    const uint32_t end = CompareEnd(before, after);
    totals.other_passes += end != UINT32_MAX ? 1 : 0;
    return end;
}

// The values of `settings` before tick `end`.
std::vector<hokuto::song::Setting> SettingsBefore(const std::vector<hokuto::song::Setting>& settings, uint32_t end)
{
    std::vector<hokuto::song::Setting> kept;
    std::ranges::copy_if(settings, std::back_inserter(kept),
                         [end](const hokuto::song::Setting& setting) { return setting.tick < end; });
    return kept;
}

// The programs of the notes of `track` that start before tick `end`, with their ticks.
std::vector<std::pair<uint32_t, uint32_t>> ProgramsBefore(const Track& track, uint32_t end)
{
    std::vector<std::pair<uint32_t, uint32_t>> programs;
    for (const hokuto::song::Note& note : track.notes)
    {
        if (note.start < end)
        {
            programs.emplace_back(note.start, note.program);
        }
    }

    return programs;
}

// The tracks of `before` whose setting differs in `after` up to tick `end`: the volume or the pan where `setting` is
// one, and else the instruments of the notes. With `notes`, a track whose notes differ counts too. A track that
// `after` doesn't have counts.
std::set<int> ChangedTracks(const Sequence& before, const Sequence& after, uint32_t end, std::optional<Mix> setting,
                            bool notes)
{
    std::set<int> changed;
    for (const Track& old : before.tracks)
    {
        const Track* now = TrackNumbered(after, old.number);
        const bool differs = now == nullptr ||
                             (setting ? SettingsBefore(hokuto::song::MixSettings(old, *setting), end) !=
                                            SettingsBefore(hokuto::song::MixSettings(*now, *setting), end)
                                      : ProgramsBefore(old, end) != ProgramsBefore(*now, end)) ||
                             (notes && NotesOf(before, old.number, end) != NotesOf(after, old.number, end));
        if (differs)
        {
            changed.insert(old.number);
        }
    }

    return changed;
}

// What the mixer or the inspector lists before the edit of track `number` of sound `sound`, read as `read`: the reach
// of `setting`, or of the instrument where `setting` is nothing (edit::ReachOfTracks). `places` gives where each sound
// of `file` plays. Prints a line under the ROM's `name` and counts a problem where the reach can't be found.
std::optional<Reach> ListedReach(const std::string& name, const CheckedArchive& archive, const File& file,
                                 std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                                 const Sequence& read, int number, std::optional<Mix> setting, Totals& totals)
{
    const auto start = std::chrono::steady_clock::now();
    std::optional<Reach> reach;
    try
    {
        const std::vector<hokuto::edit::TrackReach> reaches = hokuto::edit::ReachOfTracks(file, places, sound, read);
        const auto track = std::ranges::find(read.tracks, number, &Track::number);
        const auto index = static_cast<std::size_t>(track - read.tracks.begin());
        if (index >= reaches.size())
        {
            throw std::logic_error("the reach has no track " + std::to_string(number));
        }

        reach = setting ? reaches[index].mix[static_cast<std::size_t>(*setting)] : reaches[index].instrument;
        totals.listed_tracks += reach->tracks.size();
        totals.listed_sounds += reach->sounds.size();
    }
    catch (const std::exception& error)
    {
        std::printf("%s: %s: the reach can't be found: %s\n", name.c_str(), archive.NameOf(sound).c_str(),
                    error.what());
        ++totals.problems;
    }
    const std::chrono::duration<double> taken = std::chrono::steady_clock::now() - start;
    totals.slowest_reach = std::max(totals.slowest_reach, taken.count());

    return reach;
}

// The edit of a round in a sequence: the edited sound and track, and what the edit's reach lists.
struct ReachedEdit
{
    uint32_t sound = 0;
    int track = 0;
    Reach reach;
};

// Checks what the edits of `round`, in `edits` by sequence, changed besides their tracks, against their reaches. Each
// sound of `reads` is read again from `after`. `sequences` gives the sequence of each sound, and `setting` the
// setting of the round: the volume, the pan, or the instrument where it's nothing.
void CheckReaches(const std::string& name, const CheckedArchive& archive, const CheckedArchive& after,
                  const std::vector<std::optional<uint32_t>>& sequences, const std::map<uint32_t, Sequence>& reads,
                  const std::map<uint32_t, ReachedEdit>& edits, std::optional<Mix> setting, const char* round,
                  Totals& totals)
{
    const auto listed = [](const auto& list, const auto& value)
    {
        return std::ranges::find(list, value) != list.end();
    };
    for (const auto& [sound, before] : reads)
    {
        Sequence read;
        try
        {
            read = after.Read(sound);
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s can't be read after the %s edits: %s\n", name.c_str(), archive.NameOf(sound).c_str(),
                        round, error.what());
            ++totals.problems;
            continue;
        }

        const auto edit = edits.find(*sequences[sound]);
        const uint32_t end = CompareEnd(before, read);
        if (edit != edits.end() && edit->second.sound == sound)
        {
            for (const int number : ChangedTracks(before, read, end, setting, false))
            {
                if (number == edit->second.track)
                {
                    continue;
                }
                if (listed(edit->second.reach.tracks, number))
                {
                    ++totals.reached_tracks;
                    continue;
                }

                std::printf("%s: %s: track %d changed with the %s edit of track %d, but its reach leaves it out\n",
                            name.c_str(), archive.NameOf(sound).c_str(), number, round, edit->second.track);
                ++totals.unlisted;
                ++totals.problems;
            }
            continue;
        }

        if (ChangedTracks(before, read, end, setting, true).empty() && before.loop == read.loop &&
            before.length == read.length)
        {
            continue;
        }
        if (edit != edits.end() && listed(edit->second.reach.sounds, sound))
        {
            ++totals.reached_sounds;
            continue;
        }

        std::printf("%s: %s changed with the %s edits, but no edit's reach lists it\n", name.c_str(),
                    archive.NameOf(sound).c_str(), round);
        ++totals.unlisted;
        ++totals.problems;
    }
}

// The largest peak that track `number` shows in the first seconds of song `song` of `rom`.
int32_t PeakOf(const hokuto::song::KonamiRom& rom, int song, int number)
{
    agblib::konami::ArchivePlayer player(rom.rom, rom.info);
    player.Start(song, {});
    player.MeterTracks(true);
    std::vector<int16_t> samples;
    int32_t peak = 0;
    for (int frame = 0; frame < kLevelFrames && player.Busy(); ++frame)
    {
        samples.clear();
        player.RunFrame(samples);
        const std::array<int32_t, 2> peaks = player.TrackPeaks(number);
        peak = std::max({peak, std::abs(peaks[0]), std::abs(peaks[1])});
    }

    return peak;
}

// What a round reads again: the archive that the edited file exports, or nothing when the ROM has no room for it. Each
// edited sequence has to come back from the import of that archive.
std::optional<CheckedArchive> Exported(const std::string& name, const File& edited, const CheckedArchive& archive,
                                       const std::map<uint32_t, uint32_t>& sounds, const char* round, Totals& totals)
{
    std::vector<uint8_t> bytes;
    try
    {
        bytes = hokuto::format::ExportArchive(edited);
    }
    catch (const std::invalid_argument& error)
    {
        if (std::string(error.what()).find("32 MiB") == std::string::npos)
        {
            throw;
        }

        std::printf("%s: the songs outgrow the ROM after the %s edits, and it has no room for them.\n", name.c_str(),
                    round);
        ++totals.outgrown;
        return std::nullopt;
    }

    CheckedArchive after = hokuto::tools::DriverChecked(hokuto::format::Engine::kKonami, bytes);
    const File again = hokuto::tools::ImportChecked(bytes, archive);
    for (const auto& [sequence, sound] : sounds)
    {
        const std::optional<uint32_t> back = hokuto::format::KonamiSongSequence(
            again, archive.Konami()->info.song_table, hokuto::song::KonamiSongOf(*archive.Konami(), sound));
        if (!back || !hokuto::tools::HoldsSequence(after, *edited.sequences[sequence], *again.sequences[*back]))
        {
            std::printf("%s: sequence %u doesn't come back after the %s edit\n", name.c_str(), sequence, round);
            ++totals.problems;
        }
    }

    return after;
}

// The track that takes part in a round: one with notes, chosen by `turn`. Nothing for a song without notes.
const Track* ChosenTrack(const Sequence& song, std::size_t turn, bool (*takes)(const Track&))
{
    std::vector<const Track*> tracks;
    for (const Track& track : song.tracks)
    {
        if (!track.notes.empty() && takes(track))
        {
            tracks.push_back(&track);
        }
    }

    return tracks.empty() ? nullptr : tracks[turn % tracks.size()];
}

bool AnyTrack(const Track&)
{
    return true;
}

bool SampleOrSquareOrWave(const Track& track)
{
    return track.number != 3;
}

// The stretches of the track before `number`, for the edit's `previous`.
std::vector<hokuto::song::Stretch> PreviousOf(const Sequence& song, int number)
{
    const Track* before = TrackNumbered(song, number - 1);
    return before != nullptr ? before->stretches : std::vector<hokuto::song::Stretch>();
}

// The commands of the track before a track, in the stretches `previous`, that set that track's volume in their
// `velocity`: a note that plays on the next track, and the pair of volumes of the revisions that have it.
std::set<uint32_t> NextVolumesOf(const ntdlib::commands::Sequence& commands, const std::vector<uint32_t>& offsets,
                                 std::span<const hokuto::song::Stretch> previous)
{
    std::set<uint32_t> items;
    for (const hokuto::song::Stretch& stretch : previous)
    {
        const std::optional<uint32_t> first = hokuto::edit::ItemAtOffset(offsets, stretch.first);
        const std::optional<uint32_t> last = hokuto::edit::ItemAtOffset(offsets, stretch.last);
        for (uint32_t item = first.value_or(1); first && last && item <= *last; ++item)
        {
            const Command& command = commands.items[item];
            if (hokuto::edit::KonamiPlaysOnNextTrack(command) || (command.kind == Kind::kVolume && command.index == 2))
            {
                items.insert(item);
            }
        }
    }

    return items;
}

// Whether the edit of track `edited`'s volume reaches `note`, a note on track `number`. A track off centre plays its
// notes on the next channel too, and they hold the volume of its commands. A note of the revision without a mixer can
// play on the next track, and that track's volume holds it.
bool VolumeReaches(const hokuto::song::Note& note, int number, int edited, agblib::konami::Revision revision)
{
    return revision == agblib::konami::Revision::kDungeonDiceMonsters
               ? number == edited
               : (note.command_track >= 0 ? note.command_track : number) == edited;
}

// The volume edit of a sequence: its track, whether the volume falls, and the new velocity of each old one that the
// track's notes hold.
struct VolumePlan
{
    int track = 0;
    bool lower = false;
    std::map<uint32_t, uint32_t> velocities;
};

// The edited file and the plan of the volume edit of a chosen track of sequence `sequence`, played by `song`, in
// `edited`, the file after the edits so far. `file` is the file before the edits, and `revision` and `console` its
// driver's revision and console. Returns nothing, and counts the refusal, when the edit is refused. Returns nothing
// without a count when the sequence has no track to choose.
std::optional<std::pair<hokuto::edit::Edited, VolumePlan>> PlanVolume(const std::string& name, const File& file,
                                                                      const File& edited, uint32_t sequence,
                                                                      const Sequence& song,
                                                                      agblib::konami::Revision revision,
                                                                      hokuto::song::Console console, Totals& totals)
{
    const Track* track = ChosenTrack(song, sequence, AnyTrack);
    if (track == nullptr)
    {
        return std::nullopt;
    }

    const int from = hokuto::song::MixAt(*track, Mix::kVolume, track->notes.front().start);
    const int most = hokuto::song::KonamiMostVolume(revision, track->number);
    const int to = from > 1 ? from / 2 : std::min(from + 1, most);
    const ntdlib::commands::Sequence& commands = *file.sequences[sequence];
    const std::vector<uint32_t> offsets = hokuto::edit::Offsets(commands, console);
    const std::vector<hokuto::song::Stretch> stretches = hokuto::song::MixStretches(song, *track);
    const std::vector<hokuto::song::Stretch> previous = PreviousOf(song, track->number);
    const std::set<uint32_t> setters =
        hokuto::edit::MixSetters(commands, offsets, stretches, Mix::kVolume, false, true).items;
    const std::set<uint32_t> next_volumes = NextVolumesOf(commands, offsets, previous);

    // The values that the track contains, and what each becomes.
    std::set<int64_t> values;
    for (const uint32_t item : setters)
    {
        const Command& command = commands.items[item];
        values.insert((command.kind == Kind::kNote ? command.velocity : command.value.value) / kOne);
    }
    for (const uint32_t item : next_volumes)
    {
        values.insert(commands.items[item].velocity / kOne);
    }
    const bool trim = values.size() > 1;
    const auto mapped = [&](int64_t value) -> int64_t
    {
        const int64_t result = trim ? std::llround(static_cast<double>(value) * to / from) : to;
        return std::clamp<int64_t>(result, 0, most);
    };

    hokuto::edit::Edited result;
    try
    {
        result = hokuto::edit::EditMix(edited, sequence, track->number, stretches, MixEdit{Mix::kVolume, from, to},
                                       previous);
    }
    catch (const std::invalid_argument& error)
    {
        const std::string_view message = error.what();
        if ((from <= 0 && message == hokuto::edit::RefusedSilentTrim(Mix::kVolume)) ||
            (OthersStart(*track, std::nullopt) && message == hokuto::edit::kKonamiVolumeWithoutNotes))
        {
            ++totals.refused_on_purpose;
        }
        else
        {
            CountUnexpected("volume", error, totals);
        }
        return std::nullopt;
    }

    // Each command contains its new value. Only the track before it can change besides. A delay can take other bytes.
    const std::vector<Command>& changed = result.file.sequences[sequence]->items;
    bool same_items = changed.size() == commands.items.size();
    for (uint32_t item = 0; same_items && item < commands.items.size(); ++item)
    {
        Command expected = commands.items[item];
        if (expected.kind == Kind::kNote && setters.contains(item))
        {
            expected.velocity = mapped(expected.velocity / kOne) * kOne;
        }
        else if (setters.contains(item))
        {
            expected.value.value = mapped(expected.value.value / kOne) * kOne;
        }
        if (next_volumes.contains(item))
        {
            expected.velocity = mapped(expected.velocity / kOne) * kOne;
        }
        Command actual = changed[item];
        actual.encoding = expected.encoding;
        actual.bytes = expected.bytes;
        same_items = actual == expected;
    }
    if (!same_items)
    {
        std::printf("%s: sequence %u: the volume edit of track %d isn't in the commands\n", name.c_str(), sequence,
                    track->number);
        ++totals.problems;
    }

    const bool sample_note = !agblib::konami::IsPsgTrack(track->number);
    VolumePlan plan{.track = track->number, .lower = to < from, .velocities = {}};
    for (const Track& other : song.tracks)
    {
        for (const hokuto::song::Note& note : other.notes)
        {
            if (VolumeReaches(note, other.number, track->number, revision))
            {
                const int volume = hokuto::song::KonamiVolumeOf(note.velocity, revision, sample_note);
                plan.velocities[note.velocity] = static_cast<uint32_t>(
                    hokuto::song::KonamiVelocityOf(static_cast<int>(mapped(volume)), revision, sample_note));
            }
        }
    }

    return std::pair(std::move(result), std::move(plan));
}

// The key of the last note of `track` that starts before tick `before`. Nothing when no note does.
std::optional<uint32_t> KeyHeld(const Track& track, uint32_t before)
{
    std::optional<uint32_t> key;
    for (const hokuto::song::Note& note : track.notes)
    {
        if (note.start < before)
        {
            key = note.key;
        }
    }

    return key;
}

// Checks that the notes of `track`, a track of sound `sound` that the volume edit of `plan` reaches, play their new
// velocities in `now`, the notes that the track plays after the edit up to tick `end`. `after` is the track in the read
// after the edit, or nothing when the read has no such track. `revision` is the revision of the driver.
void CheckReachedTrack(const std::string& name, const CheckedArchive& archive, uint32_t sound, const Track& track,
                       const Track* after, const std::vector<Played>& now, uint32_t end, const VolumePlan& plan,
                       agblib::konami::Revision revision, Totals& totals)
{
    const auto reached = [&](const hokuto::song::Note& note)
    {
        return VolumeReaches(note, track.number, plan.track, revision);
    };

    // A note that falls to volume 0 stays silent, and the performer can leave it out. A PSG channel starts at the
    // volume of the command that starts it. A volume command can start a note again. That note shows the velocity of
    // its note command, not the volume that it plays at (song::TakeKonamiCommands). The track's volume at the note's
    // tick is that volume, for a note that the track's commands start.
    const bool psg = agblib::konami::IsPsgTrack(track.number);
    const auto silent = [&](const hokuto::song::Note& note, uint32_t velocity)
    {
        if (psg && after != nullptr && note.command_track < 0)
        {
            return hokuto::song::MixAt(*after, Mix::kVolume, note.start) == 0;
        }

        return velocity == 0 || note.velocity == 0;
    };

    std::size_t later = 0;
    bool same = true;
    for (const hokuto::song::Note& note : track.notes)
    {
        if (note.start >= end)
        {
            continue;
        }

        const uint32_t velocity = reached(note) ? plan.velocities.at(note.velocity) : note.velocity;
        if (silent(note, velocity))
        {
            ++totals.silent_notes;
            continue;
        }

        while (later < now.size() && std::get<0>(now[later]) < note.start)
        {
            ++later;
        }

        const bool plays = later < now.size() && std::get<0>(now[later]) == note.start &&
                           std::get<1>(now[later]) == note.key && std::get<3>(now[later]) == velocity;

        // A PSG note's envelope starts at a sixteenth of its volume, and takes a volume of 1 to nothing. The
        // Eternal Duelist and Captain Tsubasa revisions start a PSG note again only for a new volume. Two
        // volumes that come out the same therefore join their notes.
        // In those revisions, a PSG note whose volume is the track's volume only moves the channel's pitch, and a
        // volume command starts the channel again at the pitch that it holds (ntdlib src/konami/sequencer_wct2004.cpp,
        // Op::kPsgNote and Op::kVolume). The read starts a note only where the channel starts again, at the key that
        // the channel then has: that of the track's last note up to that tick. The read therefore shows a joined note
        // in one of two ways. One is an earlier note that lasts past the note's start, at the key of the track's last
        // note up to the read note's tick. The other is a note at its tick with the new velocity, from a volume command
        // at that tick, at the key of the track's last note before the tick.
        const auto covers = [&track, &note, velocity](const Played& other)
        {
            const uint32_t start = std::get<0>(other);
            const std::optional<uint32_t> held = KeyHeld(track, start < note.start ? start + 1 : note.start);
            return held && std::get<1>(other) == *held &&
                   ((start < note.start && note.start < start + std::get<2>(other)) ||
                    (start == note.start && std::get<3>(other) == velocity));
        };
        if (!plays && psg &&
            (velocity <= static_cast<uint32_t>(hokuto::song::KonamiVelocityOf(1, revision, false)) ||
             (agblib::konami::EternalDuelistLayout(revision) && std::ranges::any_of(now, covers))))
        {
            ++totals.joined;
            continue;
        }

        // A PSG track off centre plays on the next channel, in place of the next track. A new volume on either
        // track can decide which of their notes sounds there.
        if (!plays && psg && note.command_track >= 0 && !reached(note))
        {
            ++totals.off_centre;
            continue;
        }
        if (!plays && same)
        {
            std::printf("%s: %s: track %d, tick %u, key %u: the new velocity is %u, and the read gives %s\n",
                        name.c_str(), archive.NameOf(sound).c_str(), track.number, note.start, note.key, velocity,
                        later < now.size() ? ("tick " + std::to_string(std::get<0>(now[later])) + ", key " +
                                              std::to_string(std::get<1>(now[later])) + ", velocity " +
                                              std::to_string(std::get<3>(now[later])))
                                                 .c_str()
                                           : "no note");
        }
        same = same && plays;
        ++totals.volume_notes;
    }
    if (!same)
    {
        std::printf("%s: %s: track %d doesn't play its new volumes\n", name.c_str(), archive.NameOf(sound).c_str(),
                    track.number);
        ++totals.problems;
    }
}

// Checks sound `sound` after the volume edit of `plan`: the tracks that the edit doesn't reach play as before, the
// notes that it reaches play their new velocities, and the edited channel plays lower or louder. `before` is the sound
// before the edits, `after` the archive after them, and `revision` the revision of the driver.
void CheckVolumeSound(const std::string& name, const CheckedArchive& archive, const CheckedArchive& after,
                      uint32_t sound, const VolumePlan& plan, const Sequence& before, agblib::konami::Revision revision,
                      Totals& totals)
{
    const Sequence read = after.Read(sound);
    const uint32_t end = EndOf(before, read, totals);
    for (const Track& track : before.tracks)
    {
        const std::vector<Played> now = NotesOf(read, track.number, end);
        const auto reached = [&](const hokuto::song::Note& note)
        {
            return VolumeReaches(note, track.number, plan.track, revision);
        };
        if (std::ranges::none_of(track.notes, reached))
        {
            if (NotesOf(before, track.number, end) != now)
            {
                std::printf("%s: %s: track %d plays otherwise after the volume edit of track %d\n", name.c_str(),
                            archive.NameOf(sound).c_str(), track.number, plan.track);
                ++totals.problems;
            }
            continue;
        }

        CheckReachedTrack(name, archive, sound, track, TrackNumbered(read, track.number), now, end, plan, revision,
                          totals);
    }

    // The channel plays lower. A channel can stay silent in the seconds that compare.
    const int song = hokuto::song::KonamiSongOf(*archive.Konami(), sound);
    const int32_t was = PeakOf(*archive.Konami(), song, plan.track);
    const int32_t now = PeakOf(*after.Konami(), song, plan.track);
    if (was == 0 && now == 0)
    {
        ++totals.silent;
    }
    else if (plan.lower ? now < was : now >= was)
    {
        ++totals.lower;
    }
    else if (plan.lower && plan.track == 2 && now == was)
    {
        ++totals.same_peak;
    }
    else
    {
        std::printf("%s: %s: track %d peaks at %d before the volume edit and at %d after\n", name.c_str(),
                    archive.NameOf(sound).c_str(), plan.track, was, now);
        ++totals.problems;
    }
}

// What the rounds share: the ROM's name, its archive and its file, where each sound plays and the sequence of each
// sound, the first sound of each sequence, and the read of each sound.
struct Rounds
{
    std::string name;
    const CheckedArchive& archive;
    const File& file;
    std::vector<std::optional<SoundPlace>> places;
    std::vector<std::optional<uint32_t>> sequences;
    std::map<uint32_t, uint32_t> songs;
    std::map<uint32_t, Sequence> reads;
};

// The round of the track's volume.
void VolumeRound(const Rounds& rounds, Totals& totals)
{
    const auto& [name, archive, file, places, sequences, songs, reads] = rounds;
    const agblib::konami::Revision revision = archive.Konami()->info.revision;
    const hokuto::song::Console console = hokuto::edit::ConsoleOf(file);
    File edited = file;
    std::map<uint32_t, VolumePlan> plans;
    std::map<uint32_t, uint32_t> sounds;
    std::map<uint32_t, ReachedEdit> reached;
    for (const auto& [sequence, sound] : songs)
    {
        const Sequence& song = reads.at(sound);
        std::optional<std::pair<hokuto::edit::Edited, VolumePlan>> planned =
            PlanVolume(name, file, edited, sequence, song, revision, console, totals);
        if (!planned)
        {
            continue;
        }

        auto& [result, plan] = *planned;
        if (std::optional<Reach> reach =
                ListedReach(name, archive, file, places, sound, song, plan.track, Mix::kVolume, totals))
        {
            reached.emplace(sequence, ReachedEdit{sound, plan.track, std::move(*reach)});
        }
        edited = std::move(result.file);
        plans.emplace(sequence, std::move(plan));
        sounds.emplace(sequence, sound);
        ++totals.volumes;
    }

    const std::optional<CheckedArchive> after = Exported(name, edited, archive, sounds, "volume", totals);
    if (!after)
    {
        return;
    }
    for (const auto& [sequence, sound] : sounds)
    {
        CheckVolumeSound(name, archive, *after, sound, plans.at(sequence), reads.at(sound), revision, totals);
    }
    CheckReaches(name, archive, *after, sequences, reads, reached, Mix::kVolume, "volume", totals);
}

// The round of the track's pan.
void PanRound(const Rounds& rounds, Totals& totals)
{
    const auto& [name, archive, file, places, sequences, songs, reads] = rounds;
    const agblib::konami::Revision revision = archive.Konami()->info.revision;
    const hokuto::song::Console console = hokuto::edit::ConsoleOf(file);
    const bool holds = hokuto::song::Runs(console, Kind::kPan, 0);
    const std::string expected_refusal = ExpectedPanRefusal(console);
    const int step = agblib::konami::UltimateMasters(revision) ? 1 : 5;
    File edited = file;
    std::map<uint32_t, std::pair<int, int>> plans; // each sequence's track and new pan
    std::map<uint32_t, uint32_t> sounds;
    std::map<uint32_t, ReachedEdit> reached;
    for (const auto& [sequence, sound] : songs)
    {
        const Sequence& song = reads.at(sound);
        const Track* track = ChosenTrack(song, sequence + 1, AnyTrack);
        if (track == nullptr)
        {
            continue;
        }

        const int from = hokuto::song::MixAt(*track, Mix::kPan, track->notes.front().start);
        const int to = from <= 15 ? from + 32 : from - 32;
        const std::size_t items = edited.sequences[sequence]->items.size();
        try
        {
            hokuto::edit::Edited result =
                hokuto::edit::EditMix(edited, sequence, track->number, hokuto::song::MixStretches(song, *track),
                                      MixEdit{Mix::kPan, from, to}, PreviousOf(song, track->number));
            if (!holds)
            {
                std::printf("%s: %s took a pan, on a revision that holds none\n", name.c_str(),
                            archive.NameOf(sound).c_str());
                ++totals.problems;
                continue;
            }
            totals.new_pans += result.file.sequences[sequence]->items.size() != items ? 1 : 0;
            if (std::optional<Reach> reach =
                    ListedReach(name, archive, file, places, sound, song, track->number, Mix::kPan, totals))
            {
                reached.emplace(sequence, ReachedEdit{sound, track->number, std::move(*reach)});
            }
            edited = std::move(result.file);
            plans.emplace(sequence, std::pair(track->number, to));
            sounds.emplace(sequence, sound);
            ++totals.pans;
        }
        catch (const std::invalid_argument& error)
        {
            if (!holds && error.what() == expected_refusal)
            {
                ++totals.pan_refusals;
            }
            else
            {
                CountUnexpected("pan", error, totals);
            }
        }
    }

    const std::optional<CheckedArchive> after = Exported(name, edited, archive, sounds, "pan", totals);
    if (!after)
    {
        return;
    }
    for (const auto& [sequence, sound] : sounds)
    {
        const auto& [number, to] = plans.at(sequence);
        const Sequence& before = reads.at(sound);
        const Sequence read = after->Read(sound);
        const uint32_t end = EndOf(before, read, totals);
        const Track* track = TrackNumbered(read, number);
        const Track* old = TrackNumbered(before, number);
        if (track == nullptr || old == nullptr)
        {
            std::printf("%s: %s: track %d is missing after the pan edit\n", name.c_str(), archive.NameOf(sound).c_str(),
                        number);
            ++totals.problems;
            continue;
        }

        // In the revisions whose records hold a pan, an edited PSG track's shown pan counts apart.
        const int shown = hokuto::song::MixAt(*track, Mix::kPan, old->notes.front().start);
        const bool psg_decides = agblib::konami::RecordHasPan(revision);
        const bool off = std::abs(shown - to) > step;
        if (off && psg_decides && agblib::konami::IsPsgTrack(number))
        {
            ++totals.psg_pans;
        }
        else if (off)
        {
            std::printf("%s: %s: track %d shows a pan of %d after the edit to %d\n", name.c_str(),
                        archive.NameOf(sound).c_str(), number, shown, to);
            ++totals.problems;
        }

        bool psg = false;
        for (const Track& other : before.tracks)
        {
            if (NotesOf(before, other.number, end) == NotesOf(read, other.number, end))
            {
                continue;
            }
            if (psg_decides && agblib::konami::IsPsgTrack(other.number))
            {
                psg = true;
                continue;
            }

            std::printf("%s: %s: track %d plays otherwise after the pan edit of track %d\n", name.c_str(),
                        archive.NameOf(sound).c_str(), other.number, number);
            ++totals.problems;
        }
        totals.psg_apart += psg ? 1 : 0;
    }
    CheckReaches(name, archive, *after, sequences, reads, reached, Mix::kPan, "pan", totals);
}

// The round of the sends: each has to be refused.
void SendsRound(const Rounds& rounds, Totals& totals)
{
    const auto& [name, archive, file, places, sequences, songs, reads] = rounds;
    const std::string expected_refusal = hokuto::edit::RefusedSend(hokuto::edit::ConsoleOf(file));
    for (const auto& [sequence, sound] : songs)
    {
        const Sequence& song = reads.at(sound);
        const Track* track = ChosenTrack(song, sequence, AnyTrack);
        for (const Mix send : {Mix::kSendA, Mix::kSendB, Mix::kSendC})
        {
            if (track == nullptr)
            {
                continue;
            }

            try
            {
                hokuto::edit::EditMix(file, sequence, track->number, track->stretches, MixEdit{send, 0, 64});
                std::printf("%s: %s took a send\n", name.c_str(), archive.NameOf(sound).c_str());
                ++totals.problems;
            }
            catch (const std::invalid_argument& error)
            {
                if (error.what() == expected_refusal)
                {
                    ++totals.sends;
                }
                else
                {
                    CountUnexpected("send", error, totals);
                }
            }
        }
    }
}

// The round of the instruments.
void InstrumentRound(const Rounds& rounds, Totals& totals)
{
    const auto& [name, archive, file, places, sequences, songs, reads] = rounds;
    const std::vector<int> samples = hokuto::song::KonamiSamples(*archive.Konami());
    File edited = file;
    std::map<uint32_t, std::tuple<int, uint32_t, uint32_t>> plans; // each sequence's track, old and new program
    std::map<uint32_t, uint32_t> sounds;
    std::map<uint32_t, ReachedEdit> reached;
    for (const auto& [sequence, sound] : songs)
    {
        const Sequence& song = reads.at(sound);
        const Track* track = ChosenTrack(song, sequence + 2, SampleOrSquareOrWave);
        if (track == nullptr)
        {
            continue;
        }

        const auto from = static_cast<int>(track->notes.front().program);
        std::vector<int> choices;
        if (agblib::konami::IsPsgTrack(track->number))
        {
            for (const uint32_t program : hokuto::song::KonamiTrackPrograms(*archive.Konami(), track->number))
            {
                choices.push_back(static_cast<int>(program));
            }
        }
        else
        {
            choices = samples;
        }
        const auto next = std::ranges::upper_bound(choices, from);
        if (choices.empty() || (choices.size() == 1 && choices[0] == from))
        {
            continue;
        }
        const int to = next != choices.end() ? *next : choices.front();

        try
        {
            hokuto::edit::Edited result = hokuto::edit::EditInstrument(
                edited, sequence, track->number, track->stretches, {0, from}, {0, to}, PreviousOf(song, track->number));
            if (std::optional<Reach> reach =
                    ListedReach(name, archive, file, places, sound, song, track->number, std::nullopt, totals))
            {
                reached.emplace(sequence, ReachedEdit{sound, track->number, std::move(*reach)});
            }
            edited = std::move(result.file);
            plans.emplace(sequence, std::tuple(track->number, static_cast<uint32_t>(from), static_cast<uint32_t>(to)));
            sounds.emplace(sequence, sound);
            ++totals.instruments;
        }
        catch (const std::invalid_argument& error)
        {
            if (OthersStart(*track, static_cast<uint32_t>(from)) &&
                std::string_view(error.what()) == hokuto::edit::kNoNotesInstrumentRefusal)
            {
                ++totals.refused_on_purpose;
            }
            else
            {
                CountUnexpected("instrument", error, totals);
            }
        }
    }

    const std::optional<CheckedArchive> after = Exported(name, edited, archive, sounds, "instrument", totals);
    if (!after)
    {
        return;
    }
    for (const auto& [sequence, sound] : sounds)
    {
        const auto& [number, from, to] = plans.at(sequence);
        const Sequence& before = reads.at(sound);
        const Sequence read = after->Read(sound);
        const uint32_t end = EndOf(before, read, totals);
        const bool psg_decides = agblib::konami::RecordHasPan(archive.Konami()->info.revision);
        bool psg = false;
        for (const Track& track : before.tracks)
        {
            if (track.number == number || NotesOf(before, track.number, end) == NotesOf(read, track.number, end))
            {
                continue;
            }

            if (psg_decides && agblib::konami::IsPsgTrack(track.number))
            {
                psg = true;
                continue;
            }

            std::printf("%s: %s: track %d plays otherwise after the instrument edit of track %d\n", name.c_str(),
                        archive.NameOf(sound).c_str(), track.number, number);
            ++totals.problems;
        }
        totals.psg_instruments += psg ? 1 : 0;

        // Each note takes the new program at its tick, or keeps the program that it played.
        const Track* now = TrackNumbered(read, number);
        const Track* was = TrackNumbered(before, number);
        if (now == nullptr || was == nullptr)
        {
            std::printf("%s: %s: track %d is missing after the instrument edit\n", name.c_str(),
                        archive.NameOf(sound).c_str(), number);
            ++totals.problems;
            continue;
        }

        std::map<uint32_t, uint32_t> programs;
        for (const hokuto::song::Note& note : now->notes)
        {
            programs.emplace(note.start, note.program);
        }
        std::size_t wrong = 0;
        for (const hokuto::song::Note& note : was->notes)
        {
            if (note.start >= end)
            {
                continue;
            }

            const auto found = programs.find(note.start);
            const uint32_t expected = note.program == from ? to : note.program;
            wrong += found == programs.end() || found->second != expected ? 1 : 0;
            ++totals.instrument_notes;
        }
        if (wrong != 0)
        {
            std::printf("%s: %s: %zu notes of track %d don't play their new instrument, of %u for %u\n", name.c_str(),
                        archive.NameOf(sound).c_str(), wrong, number, to, from);
            ++totals.problems;
        }
    }
    CheckReaches(name, archive, *after, sequences, reads, reached, std::nullopt, "instrument", totals);
}

// Reads each sequence sound of `archive` with a sequence in `sequences` into `reads`, or only the sound `only` where
// it is set, and puts the first sound of each sequence in `songs`. Prints a line under the ROM's `name` for a sound
// that can't be read. Counts the songs and the sounds that can't be read in `totals`.
void ReadSongs(const std::string& name, const CheckedArchive& archive,
               const std::vector<std::optional<uint32_t>>& sequences, const std::string& only,
               std::map<uint32_t, uint32_t>& songs, std::map<uint32_t, Sequence>& reads, Totals& totals)
{
    for (uint32_t sound = 0; sound < archive.SoundCount(); ++sound)
    {
        if (!archive.IsSequence(sound) || !sequences[sound] || (!only.empty() && archive.NameOf(sound) != only))
        {
            continue;
        }

        ++totals.songs;
        try
        {
            reads.emplace(sound, archive.Read(sound));
            songs.emplace(*sequences[sound], sound);
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s can't be read: %s\n", name.c_str(), archive.NameOf(sound).c_str(), error.what());
            ++totals.unread;
        }
    }
}

// Prints the refusals and the summary of `totals`, under the ROM's `name`.
void PrintTotals(const std::string& name, const Totals& totals)
{
    for (const auto& [message, count] : totals.refusals)
    {
        std::printf("%s: refused %zu times, unexpectedly: %s\n", name.c_str(), count, message.c_str());
    }
    std::printf(
        "%s: %zu songs, %zu that can't be read. %zu volume edits gave %zu notes new volumes, and %zu notes were or "
        "fell silent. %zu PSG notes joined the note before them or fell silent at volume 1. %zu notes of a PSG track "
        "off centre gave way on the next channel. %zu channels "
        "peaked lower or rose as the fader did, %zu wave channels kept a peak's digit, and %zu stayed silent. %zu pan "
        "edits, %zu of them with a new command, and %zu refused for a revision without a pan. %zu songs' PSG notes "
        "differ after a pan, apart, and %zu edited PSG tracks show another pan, apart. %zu sends were refused. %zu "
        "instrument edits gave %zu notes new instruments. %zu songs' PSG notes differ after an instrument edit, apart. "
        "%zu volume and instrument edits were refused for a track that can't take them. %zu reads compared up to an "
        "earlier loop. %zu rounds outgrew the ROM. The reaches list %zu tracks and %zu sounds. %zu of the tracks and "
        "%zu of the sounds changed. %zu tracks and sounds changed outside the reach. The slowest reach took %.1f ms. "
        "%zu problems.\n",
        name.c_str(), totals.songs, totals.unread, totals.volumes, totals.volume_notes, totals.silent_notes,
        totals.joined, totals.off_centre, totals.lower, totals.same_peak, totals.silent, totals.pans, totals.new_pans,
        totals.pan_refusals, totals.psg_apart, totals.psg_pans, totals.sends, totals.instruments,
        totals.instrument_notes, totals.psg_instruments, totals.refused_on_purpose, totals.other_passes,
        totals.outgrown, totals.listed_tracks, totals.listed_sounds, totals.reached_tracks, totals.reached_sounds,
        totals.unlisted, totals.slowest_reach * 1000.0, totals.problems);
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> arguments = xsflib::console::Arguments(argc, argv);
    hokuto::tools::TakeFillerOption(arguments);
    if (arguments.size() < 2 || arguments.size() > 3)
    {
        std::fprintf(stderr, "usage: hokuto_check_konami_mix [--filler] <rom.gba> [song]\n");
        return 2;
    }
    const std::string only = arguments.size() > 2 ? arguments[2] : std::string();

    const std::filesystem::path path = xsflib::PathFromUtf8(arguments[1]);
    const std::string name = xsflib::Utf8(path.filename());
    Totals totals;
    try
    {
        std::vector<uint8_t> bytes;
        xsflib::ReadFile(path, bytes);
        const CheckedArchive archive = hokuto::tools::OpenChecked(path, bytes);
        if (!archive.Konami())
        {
            std::fprintf(stderr, "%s: the ROM doesn't hold Konami's driver\n", name.c_str());
            return 2;
        }

        const File file = hokuto::tools::ImportChecked(bytes, archive);
        Rounds rounds{.name = name,
                      .archive = archive,
                      .file = file,
                      .places = hokuto::tools::PlacesOfChecked(file, bytes, archive),
                      .sequences = hokuto::tools::SequencesOfChecked(file, bytes, archive),
                      .songs = {},
                      .reads = {}};

        // Each sound has to read. The first sound of each sequence takes part in the rounds.
        ReadSongs(name, archive, rounds.sequences, only, rounds.songs, rounds.reads, totals);

        VolumeRound(rounds, totals);
        PanRound(rounds, totals);
        SendsRound(rounds, totals);
        InstrumentRound(rounds, totals);
    }
    catch (const std::exception& error)
    {
        std::printf("%s: %s\n", name.c_str(), error.what());
        ++totals.problems;
    }
    PrintTotals(name, totals);

    return totals.problems == 0 && totals.unread == 0 ? 0 : 1;
}
