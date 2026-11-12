// SPDX-License-Identifier: MIT

// The track edits of a GBA song (edit/engine_tracks.h).
//
// A song of a GBA ROM follows MusicPlayer2000's rules. Its header lists its tracks, and a field of the native layout
// stores the address of each track there (ntdlib/mp2k/layout.h). A new track, a copy and a pasted track take the
// header's next place, and the next number. Their commands go before the zeros at the end of the song, and take
// MusicPlayer2000's bytes (Writable). A header that gains a track moves with its song into the ROM's free space. The
// music player of each song that shares the header plays its tracks up to its count, and a header has 16 tracks at
// most. A song past those counts accepts no new track: it wouldn't play. MusicPlayer2000 starts each track at volume 0.
// A new track therefore sets full volume first. It then selects its instrument, or voice 0. On every console, a copy
// and a paste leave out the commands that set the tempo. Every track starts at tick 0. A song plays one voice group,
// and a pasted track comes from a song of the same voice group. A removal deletes the place of any track from the
// header, and the tracks after it move up a number. Its commands stay in the song. A song keeps one track at least. A
// header can list more tracks than the song's music player plays. The player leaves the last ones out, and a removal
// then brings the first of them into play.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/sar/layout.h"
#include "xsflib/bytes.h"

#include "edit/bytecode.h"
#include "edit/engine_tracks.h"
#include "edit/gba_tables.h"
#include "edit/mp2k.h"
#include "edit/track_common.h"
#include "edit/tracks.h"
#include "format/drivers.h"
#include "song/gba.h"

namespace hokuto::edit
{

namespace
{

using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::Sequence;
using track_detail::CommandOf;
using track_detail::CopyOf;
using track_detail::InsertedBeforeData;
using track_detail::InTimebase;
using track_detail::kTracks;
using track_detail::kWholeSound;
using track_detail::SoundStart;
using track_detail::StartOf;
using track_detail::TrailingDataStart;

// The header of a GBA song: the fields that start its tracks, in the order of the header, the run that contains it, and
// the piece of the song (ntdlib/mp2k/layout.h).
struct GbaHeader
{
    std::vector<uint32_t> fields;
    uint32_t run = 0;
    uint32_t piece = 0;
};

// The header of song `sound` of `file`, a file of a GBA ROM. Throws std::invalid_argument with a message for the user
// for a song that plays no sequence of the file.
GbaHeader GbaHeaderOf(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound)
{
    StartOf(file, places, sound);
    const std::vector<uint32_t>& fields = places[sound]->track_fields;
    if (fields.empty())
    {
        throw std::invalid_argument("Can't find the song header that starts the tracks.");
    }

    const ntdlib::sar::Field& first = file.native->fields.at(fields.front());
    return {fields, first.at.piece, first.to.piece};
}

// The address of the voice group that song `header` plays: the second word of its header.
uint32_t GbaVoicesOf(const format::File& file, const GbaHeader& header)
{
    const auto* run = std::get_if<std::vector<uint8_t>>(&file.native->pieces->at(header.run));
    if (run == nullptr || run->size() < kGbaHeaderSize)
    {
        throw std::invalid_argument("Can't find the song header that starts the tracks.");
    }

    return xsflib::LoadLe32(run->data() + kGbaHeaderVoices);
}

// Throws std::invalid_argument with a message for the user when song `sound` of a GBA ROM, with `count` tracks, can't
// take another. The music players of the songs that share its header play their tracks up to their counts, and a header
// has 16 tracks at most.
void CheckGbaRoom(std::span<const std::optional<SoundPlace>> places, uint32_t sound, std::size_t count)
{
    uint32_t most = kTracks;
    for (const std::optional<SoundPlace>& place : places)
    {
        if (place && place->sequence == places[sound]->sequence && place->player_tracks > 0)
        {
            most = std::min(most, place->player_tracks);
        }
    }

    if (count >= kTracks)
    {
        throw std::invalid_argument("The song already has 16 tracks, the maximum for a GBA song.");
    }
    if (count >= most)
    {
        throw std::invalid_argument("The song's music player plays only " + std::to_string(most) +
                                    (most == 1 ? " track" : " tracks") + ", and the song already has " +
                                    std::to_string(count) + ". A new track wouldn't play.");
    }
}

// `file` once sequence `sequence`, a GBA song with the header `header`, contains `track` before the data at its end,
// and the next place of the header starts the new track at its item `entry`. A target in `track` counts from its first
// item. The song's commands then take MusicPlayer2000's bytes (Writable). Throws std::invalid_argument for commands
// that the bytes can't hold.
TrackEdit WithGbaTrack(const format::File& file, uint32_t sequence, const GbaHeader& header,
                       std::span<const Command> track, uint32_t entry)
{
    // The data at the end of a song contains the zeros before its header.
    const Sequence& before = *file.sequences[sequence];
    const uint32_t at = TrailingDataStart(before);
    const auto size = static_cast<uint32_t>(track.size());
    const auto moved = [at, size](uint32_t item)
    {
        return item < at ? item : item + size;
    };

    TrackEdit edit{file, static_cast<int>(header.fields.size())};
    edit.file.sequences[sequence] = InsertedBeforeData(before, track, at, at, moved);
    MoveItemPlaces(edit.file, sequence, moved);
    const auto start = [&header, at, entry](format::NativeArchive& native)
    {
        const auto place = kGbaHeaderSize + 4 * static_cast<uint32_t>(header.fields.size());
        native.fields.push_back({{header.run, place}, {0, 0}, {header.piece, at + entry, true}});
    };
    edit.file.native.Edit(start);
    edit.file = Writable(file, Edited{std::move(edit.file), {}, {}}, sequence).file;

    return edit;
}

} // namespace

namespace track_detail
{

TrackEdit AddGbaTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                      const song::Sequence& song, std::optional<Program> instrument)
{
    const SoundStart start = StartOf(file, places, sound);
    const GbaHeader header = GbaHeaderOf(file, places, sound);
    CheckGbaRoom(places, sound, header.fields.size());

    std::vector<Command> track = {CommandOf(Kind::kVolume, 127),
                                  CommandOf(Kind::kProgram, instrument ? instrument->number : 0)};
    if (song.loop && song.loop->end > song.loop->start)
    {
        if (song.loop->start > 0)
        {
            track.push_back(CommandOf(Kind::kWait, song.loop->start));
        }

        const auto loop = static_cast<uint32_t>(track.size());
        track.push_back(CommandOf(Kind::kWait, song.loop->end - song.loop->start));
        Command jump = CommandOf(Kind::kJump, 0);
        jump.target = loop;
        track.push_back(jump);
    }
    else
    {
        const uint32_t end = song.notes_end > 0 ? song.notes_end : song.length;
        track.push_back(CommandOf(Kind::kWait, end > 0 ? end : 4 * static_cast<uint32_t>(song.timebase)));
        track.push_back(CommandOf(Kind::kEnd, 0));
    }

    return WithGbaTrack(file, start.sequence, header, track, 0);
}

} // namespace track_detail

namespace
{

// The item where track `track` of a GBA song with the header `header` starts. Throws std::invalid_argument with a
// message for the user for a track that is missing from the header.
uint32_t GbaEntryOf(const format::File& file, const GbaHeader& header, int track)
{
    if (track < 0 || static_cast<std::size_t>(track) >= header.fields.size())
    {
        throw std::invalid_argument("The song has no such track.");
    }

    return file.native->fields.at(header.fields[static_cast<std::size_t>(track)]).to.offset;
}

} // namespace

namespace track_detail
{

TrackEdit DuplicateGbaTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                            int track)
{
    const SoundStart start = StartOf(file, places, sound);
    const Sequence& sequence = SequenceToEdit(file, start.sequence);
    const GbaHeader header = GbaHeaderOf(file, places, sound);
    const uint32_t entry = GbaEntryOf(file, header, track);
    CheckGbaRoom(places, sound, header.fields.size());

    std::map<uint32_t, uint32_t> moved;
    const std::vector<Command> copy = CopyOf(sequence, std::array{entry}, moved, kWholeSound);
    return WithGbaTrack(file, start.sequence, header, copy, moved.at(entry));
}

CopiedTrack CopyGbaTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                         const song::Sequence& song, int track)
{
    const SoundStart start = StartOf(file, places, sound);
    const Sequence& sequence = SequenceToEdit(file, start.sequence);
    const GbaHeader header = GbaHeaderOf(file, places, sound);
    const uint32_t entry = GbaEntryOf(file, header, track);

    CopiedTrack copied;
    std::map<uint32_t, uint32_t> moved;
    copied.commands = CopyOf(sequence, std::array{entry}, moved, kWholeSound);
    copied.entry = moved.at(entry);
    copied.timebase = song.timebase;
    copied.banks = {GbaVoicesOf(file, header)};
    copied.sound = sound;
    copied.track = track;
    copied.gba = true;

    return copied;
}

TrackEdit PasteGbaTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                        const song::Sequence& song, const CopiedTrack& copied)
{
    const SoundStart start = StartOf(file, places, sound);
    const GbaHeader header = GbaHeaderOf(file, places, sound);
    if (!copied.gba)
    {
        throw std::invalid_argument("Only a track from a GBA song can be pasted into a GBA song.");
    }

    CheckGbaRoom(places, sound, header.fields.size());
    const uint32_t voices = GbaVoicesOf(file, header);
    if (copied.banks.size() != 1 || copied.banks.front() != voices)
    {
        const std::string from =
            copied.banks.empty() ? "another voice group" : song::GbaVoiceGroupName(copied.banks.front());
        throw std::invalid_argument("The track uses " + from + ", but the song uses " +
                                    song::GbaVoiceGroupName(voices) + ". A GBA song can use only one voice group.");
    }

    // MusicPlayer2000 counts 24 ticks to a quarter note.
    std::optional<std::vector<Command>> commands = copied.commands;
    if (song.timebase != copied.timebase)
    {
        commands = InTimebase(copied.commands, copied.timebase, song.timebase);
        if (!commands)
        {
            throw std::invalid_argument("The track uses " + std::to_string(copied.timebase) +
                                        " ticks per quarter note, but the song uses " + std::to_string(song.timebase) +
                                        ". Some of its events would fall between the song's ticks.");
        }
    }

    // The track jumps to the command where the copied track started, when commands that the track reaches come before
    // that one.
    std::vector<Command> track;
    if (copied.entry != 0)
    {
        Command jump = CommandOf(Kind::kJump, 0);
        jump.target = 1 + copied.entry;
        track.push_back(jump);
    }

    const auto lead = static_cast<uint32_t>(track.size());
    for (Command command : *commands)
    {
        if (ntdlib::commands::HasTarget(command.kind) && !command.target_outside)
        {
            command.target += lead;
        }

        track.push_back(std::move(command));
    }

    return WithGbaTrack(file, start.sequence, header, track, 0);
}

format::File RemoveGbaTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                            int track)
{
    const GbaHeader header = GbaHeaderOf(file, places, sound);
    GbaEntryOf(file, header, track);
    if (header.fields.size() == 1)
    {
        throw std::invalid_argument("A song must keep at least one track.");
    }

    format::File removed = file;
    const auto take_out = [&header, track](format::NativeArchive& native)
    {
        for (auto t = static_cast<std::size_t>(track); t + 1 < header.fields.size(); t++)
        {
            native.fields[header.fields[t]].to = native.fields[header.fields[t + 1]].to;
        }

        native.fields.erase(native.fields.begin() + static_cast<std::ptrdiff_t>(header.fields.back()));
    };
    removed.native.Edit(take_out);

    return removed;
}

} // namespace track_detail

} // namespace hokuto::edit
