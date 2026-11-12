// SPDX-License-Identifier: MIT

// The track edits of a sound of DSE (edit/engine_tracks.h).
//
// A sound of DSE lists its tracks in its file, in chunks that follow its settings, and every track starts at tick 0.
// The settings count the tracks and the channels. A new track, a copy and a pasted track are added after the last
// track, with the next number, on the lowest channel that no track of the sound uses. Tracks of a channel share its
// volume, pan and program. A song has up to 32 tracks and 16 channels, the limits of Hokuto, and a sound effect up to 4
// of each, the limits of the driver. A sound whose channels are all in use accepts no new track. The end of a new track
// jumps back to a loop point at the loop of the sound. A copy and a paste leave out the commands that set the tempo. A
// pasted track comes from a sound of DSE whose banks the sound has, by their ids, and that counts the same ticks to a
// quarter note. A removal deletes the commands of the track from the file, and the tracks after it move up a number.
// The channels keep their count. The track that sets the tempo stays, and so does a sound's last track.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/dse/commands.h"
#include "ntdlib/dse/driver.h"
#include "ntdlib/dse/formats.h"
#include "ntdlib/dse/player.h"
#include "ntdlib/sar/layout.h"

#include "edit/bytecode.h"
#include "edit/engine_tracks.h"
#include "edit/instruments.h"
#include "edit/track_common.h"
#include "edit/tracks.h"
#include "format/drivers.h"

namespace hokuto::edit
{

namespace
{

using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::Label;
using ntdlib::commands::Sequence;
using track_detail::CommandOf;
using track_detail::kWholeSound;
using track_detail::SoundStart;
using track_detail::StartOf;

constexpr uint16_t kDseLoopPoint = ntdlib::dse::kLoopPoint;
constexpr int kDseSongChannels = 16;

// The items where the tracks of DSE sequence `sequence` start, in their order.
std::vector<uint32_t> DseTrackItems(const Sequence& sequence)
{
    std::vector<uint32_t> items;
    for (uint32_t item = 0; item < sequence.items.size(); item++)
    {
        if (ntdlib::dse::ReadTrackOpening(sequence.items[item]))
        {
            items.push_back(item);
        }
    }

    return items;
}

// The place in `pieces` of the run that ends with the settings of DSE sequence `sequence`: the run before its piece
// (ntdlib/dse/layout.h). Throws std::invalid_argument with a message for the user when the layout has no such run.
std::size_t DseSettingsRun(std::span<const ntdlib::sar::Piece> pieces, uint32_t sequence)
{
    for (std::size_t p = 1; p < pieces.size(); p++)
    {
        const auto* piece = std::get_if<ntdlib::sar::SequencePiece>(&pieces[p]);
        const auto* run = std::get_if<std::vector<uint8_t>>(&pieces[p - 1]);
        if (piece != nullptr && piece->sequence == sequence && run != nullptr &&
            run->size() >= ntdlib::dse::SequenceInfo::kSize)
        {
            return p - 1;
        }
    }

    throw std::invalid_argument("Can't find the track settings of this sound in its file.");
}

// Whether DSE sequence `sequence` of `file` is a song: the run of its settings starts with the song file. The run of a
// sound effect starts inside a bank of sound effects.
bool IsDseSong(const format::File& file, uint32_t sequence)
{
    const std::vector<ntdlib::sar::Piece>& pieces = *file.native->pieces;
    const auto& run = std::get<std::vector<uint8_t>>(pieces[DseSettingsRun(pieces, sequence)]);
    return run.size() >= 4 && std::equal(run.begin(), run.begin() + 4, "smdl");
}

// `file` once the settings of DSE sequence `sequence` give `tracks` tracks, and at least `channels` channels.
void SetDseCounts(format::File& file, uint32_t sequence, std::size_t tracks, int channels)
{
    const auto set = [&](format::NativeArchive& native)
    {
        std::vector<ntdlib::sar::Piece> pieces = *native.pieces;
        auto& run = std::get<std::vector<uint8_t>>(pieces[DseSettingsRun(pieces, sequence)]);
        const auto settings = run.end() - static_cast<std::ptrdiff_t>(ntdlib::dse::SequenceInfo::kSize);
        ntdlib::dse::SequenceInfo info;
        std::copy(settings, run.end(), info.bytes.begin());
        info.SetTrackCount(static_cast<uint8_t>(tracks));
        info.SetChannelCount(std::max(info.ChannelCount(), static_cast<uint8_t>(channels)));
        std::ranges::copy(info.bytes, settings);
        native.pieces = std::move(pieces);
    };
    file.native.Edit(set);
}

// The file after DSE sequence `sequence` of `file` gains a track of `commands`, on the lowest channel that no track of
// the sequence uses. Tracks of a channel share its volume, pan and program. `commands` hold no track start. `start` is
// the bytes of the track start to copy, or else a new one's. Throws std::invalid_argument with a message for the user
// for a song past the tracks that Hokuto reads, a sound effect past the driver's tracks and channels, a sequence whose
// channels are all taken, and commands that the bytes can't hold.
TrackEdit WithDseTrack(const format::File& file, uint32_t sequence, std::span<const Command> commands,
                       const Command* start)
{
    const Sequence& before = SequenceToEdit(file, sequence);
    const std::vector<uint32_t> items = DseTrackItems(before);
    const bool is_song = IsDseSong(file, sequence);
    const std::size_t most_tracks =
        is_song ? std::size_t{ntdlib::dse::Player::kHostTracks} : std::size_t{ntdlib::dse::Driver::kEffectTracks};
    if (items.size() >= most_tracks)
    {
        throw std::invalid_argument(
            is_song ? "Hokuto plays at most " + std::to_string(most_tracks) + " tracks in a DSE song."
                    : "DSE plays at most " + std::to_string(most_tracks) + " tracks in a sound effect.");
    }

    const int most_channels = is_song ? kDseSongChannels : ntdlib::dse::Driver::kEffectTracks;
    std::set<int> taken;
    for (const uint32_t item : items)
    {
        taken.insert(ntdlib::dse::ReadTrackOpening(before.items[item])->channel);
    }

    int channel = 0;
    while (channel < most_channels && taken.contains(channel))
    {
        channel++;
    }
    if (channel == most_channels)
    {
        throw std::invalid_argument("All " + std::to_string(most_channels) +
                                    " channels of the sound already have a track. Tracks on the same channel share "
                                    "its volume, pan and program.");
    }

    Sequence after = before;
    Command opening = start != nullptr ? *start : ntdlib::dse::TrackOpening(0, 0);
    ntdlib::dse::SetTrackOpening(opening, {static_cast<uint8_t>(items.size()), static_cast<uint8_t>(channel)});
    after.items.push_back(std::move(opening));
    after.items.insert(after.items.end(), commands.begin(), commands.end());
    try
    {
        Offsets(after, ConsoleOf(file));
    }
    catch (const std::exception&)
    {
        throw std::invalid_argument("This console's sequence format can't store the commands of the new track.");
    }

    TrackEdit edit{file, static_cast<int>(items.size())};
    edit.file.sequences[sequence] = std::move(after);
    SetDseCounts(edit.file, sequence, items.size() + 1, channel + 1);

    return edit;
}

} // namespace

namespace track_detail
{

// AddTrack on DSE. A channel's volume and pan start where the driver starts them. A channel starts without one of the
// sound's banks, and a track with an instrument selects its bank first. The end of a track jumps back to its loop
// point. The loop point comes at the sound's loop.
TrackEdit AddDseTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                      const song::Sequence& song, std::optional<Program> instrument)
{
    const SoundStart start = StartOf(file, places, sound);
    std::vector<Command> track;
    if (instrument)
    {
        const std::vector<uint16_t> banks = DseBankIds(file, start.sequence);
        const auto slot = static_cast<std::size_t>(instrument->slot);
        if (slot < banks.size())
        {
            track.push_back(DseBankSelect(banks[slot]));
        }

        track.push_back(CommandOf(Kind::kProgram, instrument->number));
    }

    if (song.loop && song.loop->end > song.loop->start)
    {
        if (song.loop->start > 0)
        {
            track.push_back(CommandOf(Kind::kWait, song.loop->start));
        }

        Command loop;
        loop.kind = Kind::kOther;
        loop.encoding.opcode = kDseLoopPoint;
        track.push_back(std::move(loop));
        track.push_back(CommandOf(Kind::kWait, song.loop->end - song.loop->start));
    }
    else
    {
        const uint32_t end = song.notes_end > 0 ? song.notes_end : song.length;
        track.push_back(CommandOf(Kind::kWait, end > 0 ? end : 4 * static_cast<uint32_t>(song.timebase)));
    }

    track.push_back(CommandOf(Kind::kEnd, 0));

    return WithDseTrack(file, start.sequence, track, nullptr);
}

} // namespace track_detail

namespace
{

// The commands that a copy of track `track` of `sequence` contains: those between its start and the start of the next
// track, without the commands of the whole sound. `items` contains the starts of the tracks. Throws
// std::invalid_argument with a message for the user for a track that is missing from the sequence.
std::vector<Command> DseTrackCopy(const Sequence& sequence, std::span<const uint32_t> items, int track)
{
    if (track < 0 || static_cast<std::size_t>(track) >= items.size())
    {
        throw std::invalid_argument("The sound has no such track.");
    }

    const auto number = static_cast<std::size_t>(track);
    const uint32_t end = number + 1 < items.size() ? items[number + 1] : static_cast<uint32_t>(sequence.items.size());
    std::vector<Command> copy;
    for (uint32_t item = items[number] + 1; item < end; item++)
    {
        const Command& command = sequence.items[item];
        if (command.kind != Kind::kData && std::ranges::find(kWholeSound, command.kind) == kWholeSound.end())
        {
            copy.push_back(command);
        }
    }

    return copy;
}

} // namespace

namespace track_detail
{

TrackEdit DuplicateDseTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                            int track)
{
    const SoundStart start = StartOf(file, places, sound);
    const Sequence& sequence = SequenceToEdit(file, start.sequence);
    const std::vector<uint32_t> items = DseTrackItems(sequence);
    const std::vector<Command> copy = DseTrackCopy(sequence, items, track);
    return WithDseTrack(file, start.sequence, copy, &sequence.items[items[static_cast<std::size_t>(track)]]);
}

// CopyTrack on DSE. Every track starts at tick 0, and a sequence has no labels of its tracks. `banks` contains the ids
// of the banks of the sound.
CopiedTrack CopyDseTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                         const song::Sequence& song, int track)
{
    const SoundStart start = StartOf(file, places, sound);
    const Sequence& sequence = SequenceToEdit(file, start.sequence);

    CopiedTrack copied;
    copied.commands = DseTrackCopy(sequence, DseTrackItems(sequence), track);
    copied.timebase = song.timebase;
    for (const uint16_t id : DseBankIds(file, start.sequence))
    {
        copied.banks.push_back(id);
    }

    copied.sound = sound;
    copied.track = track;
    copied.dse = true;

    return copied;
}

// PasteTrack on DSE. A track's bank selects name banks by their ids. The sound has to hold each bank of the other
// sound. A slide's time has no unit of the common form, and a track of other ticks to a quarter note can't go in.
TrackEdit PasteDseTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                        const song::Sequence& song, const CopiedTrack& copied)
{
    if (!copied.dse)
    {
        throw std::invalid_argument("Only a track from a DSE sound can be pasted into a DSE sound.");
    }

    const SoundStart start = StartOf(file, places, sound);
    const std::vector<uint16_t> banks = DseBankIds(file, start.sequence);
    for (const uint32_t bank : copied.banks)
    {
        if (std::ranges::find(banks, static_cast<uint16_t>(bank)) == banks.end())
        {
            throw std::invalid_argument(
                "The track uses a bank that this sound doesn't load. A DSE sound can use only the banks of "
                "its files.");
        }
    }

    if (song.timebase != copied.timebase)
    {
        throw std::invalid_argument("The track uses " + std::to_string(copied.timebase) +
                                    " ticks per quarter note, but the sound uses " + std::to_string(song.timebase) +
                                    ".");
    }

    return WithDseTrack(file, start.sequence, copied.commands, nullptr);
}

// RemoveTrack on DSE. A removal deletes the commands of the track, and the tracks after it move up a number. The
// channels keep their count. Other tracks of the track's channel lose the settings that it made. The track that sets
// the tempo stays: every other track would play at another speed.
format::File RemoveDseTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                            int track)
{
    const SoundStart start = StartOf(file, places, sound);
    const Sequence& sequence = SequenceToEdit(file, start.sequence);
    const std::vector<uint32_t> items = DseTrackItems(sequence);
    if (track < 0 || static_cast<std::size_t>(track) >= items.size())
    {
        throw std::invalid_argument("The sound has no such track.");
    }
    if (items.size() == 1)
    {
        throw std::invalid_argument("A sound must keep at least one track.");
    }

    const auto number = static_cast<std::size_t>(track);
    const uint32_t first = items[number];
    const uint32_t end = number + 1 < items.size() ? items[number + 1] : static_cast<uint32_t>(sequence.items.size());
    for (uint32_t item = first; item < end; item++)
    {
        if (sequence.items[item].kind == Kind::kTempo)
        {
            throw std::invalid_argument("Track " + std::to_string(track) + " sets the tempo. Can't remove it.");
        }
    }

    Sequence after;
    for (uint32_t item = 0; item < sequence.items.size(); item++)
    {
        if (item < first || item >= end)
        {
            after.items.push_back(sequence.items[item]);
        }
    }

    const uint32_t size = end - first;
    for (const Label& label : sequence.labels)
    {
        after.labels.push_back({label.name, label.item < end ? std::min(label.item, first) : label.item - size});
    }

    for (std::size_t later = number + 1; later < items.size(); later++)
    {
        Command& opening = after.items[items[later] - size];
        const ntdlib::dse::TrackStart read = *ntdlib::dse::ReadTrackOpening(opening);
        if (read.number == later)
        {
            ntdlib::dse::SetTrackOpening(opening, {static_cast<uint8_t>(later - 1), read.channel});
        }
    }

    format::File removed = file;
    removed.sequences[start.sequence] = std::move(after);
    SetDseCounts(removed, start.sequence, items.size() - 1, 0);

    return removed;
}

} // namespace track_detail

} // namespace hokuto::edit
