// SPDX-License-Identifier: MIT

// The track edits of a song of Konami's GBA driver (edit/konami.h).
//
// A song of Konami's driver has a field in the song table for each of its tracks, and each track plays one channel:
// tracks 0 and 1 the PSG's square waves, track 2 its wave channel, track 3 its noise, and the others one voice of the
// samples each. A song therefore takes no new track. A copy and a pasted track go to the first track of the same kind
// of channel that runs no note, in place of its commands. Their commands go before the bytes at the song's end that no
// track reads, with the commands that their calls play. They leave out a loop or a stop of the whole song. A pasted
// track comes from a song of the same build of the driver, and plays the same ROM's samples. A removal points the field
// of the track to a new track that ends at once, and its commands stay in the song. A track that loops or stops the
// whole song stays.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/konami/seqformat.h"
#include "ntdlib/sar/layout.h"

#include "edit/bytecode.h"
#include "edit/konami.h"
#include "edit/mp2k.h"
#include "edit/track_common.h"
#include "edit/tracks.h"
#include "format/drivers.h"

namespace hokuto::edit
{

namespace
{

using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::Sequence;
using track_detail::InsertedBeforeData;
using track_detail::SoundStart;
using track_detail::StartOf;
using track_detail::TrailingDataStart;

// In the song table, each song of Konami's driver has a field for each track.
const std::vector<uint32_t>& KonamiFieldsOf(const format::File& file, std::span<const std::optional<SoundPlace>> places,
                                            uint32_t sound)
{
    StartOf(file, places, sound);
    const std::vector<uint32_t>& fields = places[sound]->track_fields;
    if (fields.empty())
    {
        throw std::invalid_argument("Can't find the song table entry that starts the tracks of this song.");
    }

    return fields;
}

// The item where track `track` of a song of Konami's driver starts.
uint32_t KonamiEntryOf(const format::File& file, std::span<const uint32_t> fields, int track)
{
    if (track < 0 || static_cast<std::size_t>(track) >= fields.size())
    {
        throw std::invalid_argument("The song has no such track.");
    }

    return file.native->fields.at(fields[static_cast<std::size_t>(track)]).to.offset;
}

// The item past the last of the track that starts at `entry`, without its calls: past its first end, loop end or data,
// or the end of `sequence`.
uint32_t KonamiTrackEnd(const Sequence& sequence, uint32_t entry)
{
    uint32_t end = entry;
    while (end < sequence.items.size() && sequence.items[end].kind != Kind::kEnd &&
           sequence.items[end].kind != Kind::kLoopEnd && sequence.items[end].kind != Kind::kData)
    {
        end++;
    }

    return std::min(end + 1, static_cast<uint32_t>(sequence.items.size()));
}

// The items of the track that starts at `entry`, up to its end, with every command that its calls play: a first item
// and the item past the last. Items of other tracks can be among them.
std::pair<uint32_t, uint32_t> KonamiTrackSpan(const Sequence& sequence, uint32_t entry)
{
    uint32_t end = KonamiTrackEnd(sequence, entry);

    const std::vector<KonamiCallRange> ranges = KonamiCallRanges(sequence);
    for (bool grew = true; grew;)
    {
        grew = false;
        for (const KonamiCallRange& range : ranges)
        {
            if (range.call >= entry && range.call < end && range.end > end)
            {
                end = range.end;
                grew = true;
            }
        }
    }

    return {entry, end};
}

// Whether a command that the track at `entry` runs meets `test`: a command up to the track's end, or one that its calls
// play wherever those are, through further calls too.
template <typename Test>
bool KonamiTrackRuns(const Sequence& sequence, uint32_t entry, Test test)
{
    uint32_t end = KonamiTrackEnd(sequence, entry);

    const std::vector<KonamiCallRange> ranges = KonamiCallRanges(sequence);
    std::vector<std::pair<uint32_t, uint32_t>> spans = {{entry, end}};
    for (std::size_t span = 0; span < spans.size(); span++)
    {
        const auto [first, last] = spans[span];
        if (std::any_of(sequence.items.begin() + first, sequence.items.begin() + last, test))
        {
            return true;
        }

        for (const KonamiCallRange& range : ranges)
        {
            const std::pair<uint32_t, uint32_t> called{range.first, range.end};
            if (range.call >= first && range.call < last &&
                std::find(spans.begin(), spans.end(), called) == spans.end())
            {
                spans.push_back(called);
            }
        }
    }

    return false;
}

// Whether the track that starts at `entry` runs a note.
bool KonamiTrackPlays(const Sequence& sequence, uint32_t entry)
{
    return KonamiTrackRuns(sequence, entry, [](const Command& command) { return command.kind == Kind::kNote; });
}

// Whether a track's commands can go to track `to`. Each track plays one channel: tracks 0 and 1 the square waves, track
// 2 the wave channel, track 3 the noise, and the others the samples.
bool KonamiChannelTakes(int from, int to)
{
    if (ntdlib::konami::IsPsgTrack(from) != ntdlib::konami::IsPsgTrack(to))
    {
        return false;
    }

    return !ntdlib::konami::IsPsgTrack(from) || from == to || (from < 2 && to < 2);
}

// The name of track `track`'s channel as the zones show it.
std::string KonamiChannelName(int track)
{
    constexpr std::array<const char*, ntdlib::konami::kPsgTracks> kPsgNames = {"Square 1", "Square 2", "Wave", "Noise"};
    return ntdlib::konami::IsPsgTrack(track) ? kPsgNames[static_cast<std::size_t>(track)]
                                             : "Voice " + std::to_string(track - ntdlib::konami::kPsgTracks);
}

// Whether track `track` of song `fields` runs no note, and the track before it plays no note on its channel. A note can
// say that the next track plays it. In the revisions with pan bytes in their records, a PSG track off centre plays its
// left side on the next channel, in place of the next track.
bool KonamiTrackFree(const format::File& file, const Sequence& sequence, std::span<const uint32_t> fields, int track)
{
    if (KonamiTrackPlays(sequence, KonamiEntryOf(file, fields, track)))
    {
        return false;
    }
    if (track == 0)
    {
        return true;
    }

    const bool off_centre =
        ntdlib::konami::IsPsgTrack(track - 1) && ntdlib::konami::RecordHasPan(*KonamiOf(ConsoleOf(file)));
    const auto reaches_track = [off_centre](const Command& command)
    {
        return (command.kind == Kind::kNote && command.sample && command.index == 1) ||
               (off_centre && command.kind == Kind::kPan && command.value.value != 0);
    };
    return !KonamiTrackRuns(sequence, KonamiEntryOf(file, fields, track - 1), reaches_track);
}

// The first track of song `fields` that's free (KonamiTrackFree) and takes the commands of track `from`. A copy within
// a song leaves out track `from` (`other_than_from`). Throws std::invalid_argument with a message for the user where
// none is.
int FreeKonamiTrack(const format::File& file, const Sequence& sequence, std::span<const uint32_t> fields, int from,
                    bool other_than_from = true)
{
    for (int track = 0; track < static_cast<int>(fields.size()); track++)
    {
        if ((!other_than_from || track != from) && KonamiChannelTakes(from, track) &&
            KonamiTrackFree(file, sequence, fields, track))
        {
            return track;
        }
    }

    throw std::invalid_argument(
        ntdlib::konami::IsPsgTrack(from)
            ? "In Konami's driver, each track plays one channel. No other track of this song that can play this "
              "channel is free."
            : "In Konami's driver, each track plays one channel. Every sample track of this song already has notes. "
              "Remove one first.");
}

// The commands of the track that starts at `entry` for track `track` of a song, with targets that count from the first
// command. A copy never ends or loops the whole song. Throws std::invalid_argument with a message for the user for a
// call to commands before the track.
std::vector<Command> KonamiTrackCopy(const Sequence& sequence, uint32_t entry, int track)
{
    const auto [first, end] = KonamiTrackSpan(sequence, entry);
    std::vector<Command> copy(sequence.items.begin() + first, sequence.items.begin() + end);
    for (Command& command : copy)
    {
        if (ntdlib::commands::HasTarget(command.kind) && !command.target_outside)
        {
            if (command.target < first)
            {
                throw std::invalid_argument(
                    "A call in this track runs commands before the start of the track. Can't copy it.");
            }

            command.target -= first;
        }

        if (command.kind == Kind::kCall)
        {
            command.index = static_cast<uint8_t>(track);
        }
        if (command.kind == Kind::kLoopEnd || (command.kind == Kind::kEnd && command.index == 2))
        {
            command = Command{.kind = Kind::kEnd};
        }
    }

    return copy;
}

// `file` once track `track` of a song of Konami's driver, sequence `sequence`, runs `commands`. The commands go before
// the bytes at the song's end that no track reads. A target in `commands` counts from its first item.
TrackEdit WithKonamiTrack(const format::File& file, uint32_t sequence, std::span<const uint32_t> fields, int track,
                          std::span<const Command> commands)
{
    const Sequence& before = *file.sequences[sequence];
    const uint32_t at = TrailingDataStart(before);
    const auto size = static_cast<uint32_t>(commands.size());
    const auto moved = [at, size](uint32_t item)
    {
        return item < at ? item : item + size;
    };

    TrackEdit edit{file, track};
    edit.file.sequences[sequence] = InsertedBeforeData(before, commands, at, at, moved);
    MoveItemPlaces(edit.file, sequence, moved);
    const auto start = [&fields, track, at](format::NativeArchive& native)
    {
        native.fields[fields[static_cast<std::size_t>(track)]].to.offset = at;
    };
    edit.file.native.Edit(start);
    edit.file = Writable(file, Edited{std::move(edit.file), {}, {}}, sequence).file;

    return edit;
}

} // namespace

// AddTrack on Konami's driver. The entry of a song contains a field for each track of the revision.
TrackEdit AddKonamiTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound)
{
    const SoundStart start = StartOf(file, places, sound);
    const std::vector<uint32_t>& fields = KonamiFieldsOf(file, places, sound);
    const Sequence& sequence = SequenceToEdit(file, start.sequence);
    for (int track = 0; track < static_cast<int>(fields.size()); track++)
    {
        if (KonamiTrackFree(file, sequence, fields, track))
        {
            throw std::invalid_argument("Songs in Konami's driver always have " + std::to_string(fields.size()) +
                                        " tracks, one per channel. " + KonamiChannelName(track) +
                                        " has no notes yet. Paste or duplicate a track of the same kind of channel "
                                        "to fill it.");
        }
    }

    throw std::invalid_argument("Songs in Konami's driver have " + std::to_string(fields.size()) +
                                " tracks, one per channel. All of them already have notes.");
}

// DuplicateTrack on Konami's driver. The copy is placed on a track of the same kind of channel that runs no note.
TrackEdit DuplicateKonamiTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places,
                               uint32_t sound, int track)
{
    const SoundStart start = StartOf(file, places, sound);
    const std::vector<uint32_t>& fields = KonamiFieldsOf(file, places, sound);
    const Sequence& sequence = SequenceToEdit(file, start.sequence);
    const uint32_t entry = KonamiEntryOf(file, fields, track);
    const int free_track = FreeKonamiTrack(file, sequence, fields, track);
    return WithKonamiTrack(file, start.sequence, fields, free_track, KonamiTrackCopy(sequence, entry, free_track));
}

// CopyTrack on Konami's driver. `banks` contains the revision: a track's samples and calls are those of its ROM.
CopiedTrack CopyKonamiTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                            const song::Sequence& song, int track)
{
    const SoundStart start = StartOf(file, places, sound);
    const std::vector<uint32_t>& fields = KonamiFieldsOf(file, places, sound);
    const Sequence& sequence = SequenceToEdit(file, start.sequence);

    CopiedTrack copied;
    copied.commands = KonamiTrackCopy(sequence, KonamiEntryOf(file, fields, track), track);
    copied.timebase = song.timebase;
    copied.banks = {static_cast<uint32_t>(*KonamiOf(ConsoleOf(file)))};
    copied.sound = sound;
    copied.track = track;
    copied.driver = format::Engine::kKonami;

    return copied;
}

// PasteTrack on Konami's driver. The track is placed on a track of the same kind of channel that runs no note.
TrackEdit PasteKonamiTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                           const CopiedTrack& copied)
{
    if (copied.driver != format::Engine::kKonami ||
        copied.banks != std::vector<uint32_t>{static_cast<uint32_t>(*KonamiOf(ConsoleOf(file)))})
    {
        throw std::invalid_argument(
            "Only a track from the same build of Konami's driver can be pasted into this song.");
    }

    const SoundStart start = StartOf(file, places, sound);
    const std::vector<uint32_t>& fields = KonamiFieldsOf(file, places, sound);
    const Sequence& sequence = SequenceToEdit(file, start.sequence);
    const int free_track = FreeKonamiTrack(file, sequence, fields, copied.track, false);
    std::vector<Command> commands = copied.commands;
    for (Command& command : commands)
    {
        if (command.kind == Kind::kCall)
        {
            command.index = static_cast<uint8_t>(free_track);
        }
    }

    return WithKonamiTrack(file, start.sequence, fields, free_track, commands);
}

// RemoveTrack on Konami's driver. The field of the track points to a new track that ends at once. The old commands stay
// in the song. A track that loops or stops the whole song stays.
format::File RemoveKonamiTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places,
                               uint32_t sound, int track)
{
    const SoundStart start = StartOf(file, places, sound);
    const std::vector<uint32_t>& fields = KonamiFieldsOf(file, places, sound);
    const Sequence& sequence = SequenceToEdit(file, start.sequence);
    const auto [first, end] = KonamiTrackSpan(sequence, KonamiEntryOf(file, fields, track));
    const bool leads =
        std::any_of(sequence.items.begin() + first, sequence.items.begin() + end, [](const Command& command)
                    { return command.kind == Kind::kLoopEnd || (command.kind == Kind::kEnd && command.index == 2); });
    if (leads)
    {
        throw std::invalid_argument("This track loops or stops the whole song. Can't remove it.");
    }

    const std::array<Command, 2> silent = {Command{.kind = Kind::kWait}, Command{.kind = Kind::kEnd}};
    return WithKonamiTrack(file, start.sequence, fields, track, silent).file;
}

} // namespace hokuto::edit
