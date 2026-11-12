// SPDX-License-Identifier: MIT

#include "edit/dse_files.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/dse/commands.h"
#include "ntdlib/dse/formats.h"
#include "ntdlib/dse/layout.h"
#include "ntdlib/dse/player.h"
#include "xsflib/binary.h"

#include "edit/converter.h"
#include "edit/dse_files_parts.h"
#include "edit/midi_imports.h"
#include "edit/midi_songs.h"
#include "edit/native_runs.h"
#include "format/archive.h"
#include "format/kinds.h"
#include "song/archive.h"

namespace hokuto::edit
{
namespace
{

namespace dse = ntdlib::dse;
using dse_files_parts::NumberList;
using dse_files_parts::ProgramsOf;
using dse_files_parts::RequireSet;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Sequence;

// DSE's commands that select a bank, and the one that marks a track's loop point.
constexpr uint16_t kBank = ntdlib::dse::kBank;
constexpr uint16_t kBankHigh = ntdlib::dse::kBankHigh;
constexpr uint16_t kBankLow = ntdlib::dse::kBankLow;
constexpr uint16_t kLoopPoint = ntdlib::dse::kLoopPoint;
constexpr uint16_t kKeyBend = ntdlib::dse::kKeyBend;

// The bank that a converted track selects at its opening (AsDse). The select stands in for the bank of the sound:
// WithBanks replaces it with a select of the first bank of the sound where the sound plays no bank 0.
constexpr uint16_t kStandInBank = 0;

// A track's start: the chunk's header and the preamble (ntdlib/dse/commands.h). The second byte of the preamble sets
// the channel of the track.
constexpr std::size_t kTrackStart = 20;
constexpr std::size_t kTrackChannel = 17;

// The places of a sequence's settings (ntdlib::dse::SequenceInfo): the ticks of a quarter note, and the count of
// tracks. The count of channels follows it.
constexpr std::size_t kTicksPerQuarter = 2;
constexpr std::size_t kTrackCount = 6;

// The driver's channels.
constexpr std::size_t kDseChannels = 16;

bool IsTrackStart(const Command& command)
{
    return command.kind == Kind::kData && command.bytes.size() == kTrackStart &&
           std::memcmp(command.bytes.data(), "trk ", 4) == 0;
}

bool SelectsBank(const Command& command)
{
    const uint16_t opcode = command.encoding.opcode;
    return command.kind == Kind::kOther && (opcode == kBank || opcode == kBankHigh || opcode == kBankLow);
}

// The bank id after `command`, a bank select, where the channel's id was `id`.
uint16_t IdAfter(uint16_t id, const Command& command)
{
    const uint16_t opcode = command.encoding.opcode;
    if (opcode == kBank && command.bytes.size() == 2)
    {
        return static_cast<uint16_t>(command.bytes[0] << 8 | command.bytes[1]);
    }
    if (opcode == kBankHigh && command.bytes.size() == 1)
    {
        return static_cast<uint16_t>(command.bytes[0] << 8 | (id & 0xFF));
    }
    if (opcode == kBankLow && command.bytes.size() == 1)
    {
        return static_cast<uint16_t>((id & 0xFF00) | command.bytes[0]);
    }

    return id;
}

Command BankSelect(uint16_t id)
{
    Command select;
    select.kind = Kind::kOther;
    select.encoding.opcode = kBank;
    select.bytes = {static_cast<uint8_t>(id >> 8), static_cast<uint8_t>(id)};

    return select;
}

// The IDs of `banks`.
std::vector<uint16_t> IdsOf(const std::vector<std::shared_ptr<const dse::WaveBank>>& banks)
{
    std::vector<uint16_t> ids;
    for (const std::shared_ptr<const dse::WaveBank>& bank : banks)
    {
        ids.push_back(song::DseBankId(*bank));
    }

    return ids;
}

// A run of bank selects of `tracks` with nothing between them, and the id that the run leaves the track's channel.
struct SelectRun
{
    std::size_t first = 0;
    std::size_t count = 0;
    uint16_t id = 0;
};

std::vector<SelectRun> SelectRuns(const Sequence& tracks)
{
    std::vector<SelectRun> runs;
    uint16_t id = 0;
    for (std::size_t item = 0; item < tracks.items.size(); ++item)
    {
        const Command& command = tracks.items[item];
        if (IsTrackStart(command))
        {
            id = 0;
            continue;
        }

        if (!SelectsBank(command))
        {
            continue;
        }

        id = IdAfter(id, command);
        if (!runs.empty() && runs.back().first + runs.back().count == item)
        {
            runs.back().count++;
            runs.back().id = id;
        }
        else
        {
            runs.push_back({item, 1, id});
        }
    }

    return runs;
}

// Whether `tracks` were written for other banks than those of `ids`: they select banks, and none of `ids`.
bool SelectsOtherBanks(const Sequence& tracks, std::span<const uint16_t> ids)
{
    const std::vector<SelectRun> runs = SelectRuns(tracks);
    return !ids.empty() && !runs.empty() &&
           std::ranges::none_of(runs,
                                [&](const SelectRun& run) { return std::ranges::find(ids, run.id) != ids.end(); });
}

// `tracks` with each run of bank selects in the place of a select of `ids`'s first, where the tracks select none of
// `ids` (SelectsOtherBanks). Tracks that select one of `ids` keep their selects. A game's tracks can select a bank
// that the game doesn't load with the sound, and a sound's file then comes back as it was.
Sequence WithBanks(const Sequence& tracks, std::span<const uint16_t> ids)
{
    if (!SelectsOtherBanks(tracks, ids))
    {
        return tracks;
    }

    std::map<std::size_t, SelectRun> moved;
    for (const SelectRun& run : SelectRuns(tracks))
    {
        moved.emplace(run.first, run);
    }

    Sequence result;
    result.labels = tracks.labels;
    for (std::size_t item = 0; item < tracks.items.size(); ++item)
    {
        const auto run = moved.find(item);
        if (run == moved.end())
        {
            result.items.push_back(tracks.items[item]);
            continue;
        }

        result.items.push_back(BankSelect(ids.front()));
        item += run->second.count - 1;
    }

    return result;
}

// `tracks` with the programs of `programs` in the place of those that their program changes select.
Sequence WithDsePrograms(Sequence tracks, const ProgramMap& programs)
{
    for (Command& command : tracks.items)
    {
        if (command.kind != Kind::kProgram || command.value.source != ntdlib::commands::Source::kValue)
        {
            continue;
        }

        const auto found = programs.find(Program{0, static_cast<int>(command.value.value / kOne)});
        if (found != programs.end())
        {
            command.value.value = int64_t{found->second} * kOne;
        }
    }

    return tracks;
}

// The tracks of `tracks`, and the most channels that they play.
struct TrackCount
{
    std::size_t tracks = 0;
    std::size_t channels = 0;
};

TrackCount TracksOf(const Sequence& tracks)
{
    TrackCount count;
    for (const Command& command : tracks.items)
    {
        if (IsTrackStart(command))
        {
            count.tracks++;
            count.channels = std::max<std::size_t>(count.channels, std::size_t{command.bytes[kTrackChannel]} + 1);
        }
    }

    return count;
}

// The bytes of the files of `set` by their paths, and where each starts in `set`.
struct SetFile
{
    std::span<const uint8_t> bytes;
    uint64_t offset = 0;
};

std::map<std::string, SetFile> FilesOf(std::span<const uint8_t> set)
{
    std::map<std::string, SetFile> files;
    for (const dse::NamedFile& named : dse::ReadSet(set))
    {
        files.emplace(named.path, SetFile{named.bytes, static_cast<uint64_t>(named.bytes.data() - set.data())});
    }

    return files;
}

const ntdlib::sequence_export::dse::SequenceInput& SoundAt(const song::DseSet& set, uint32_t sound)
{
    if (sound >= set.sounds.size())
    {
        throw std::invalid_argument("The image has no DSE sound " + std::to_string(sound) + ".");
    }

    return set.sounds[sound];
}

// The tracks' starts of `song`, by their numbers. Throws std::invalid_argument with a message for the user for more
// tracks than DSE's channels.
std::map<uint8_t, uint32_t> TrackStarts(const Sequence& song)
{
    // The tracks' starts: track 0 at the song's start, and the others where it opens them.
    std::map<uint8_t, uint32_t> starts = {{0, 0}};
    for (const Command& command : song.items)
    {
        if (command.kind == Kind::kOpenTrack)
        {
            starts.emplace(command.index, command.target);
        }

        if (command.kind == Kind::kEnd || command.kind == Kind::kJump)
        {
            break;
        }
    }
    if (starts.size() > kDseChannels)
    {
        throw std::invalid_argument("The song has more than 16 tracks.");
    }

    return starts;
}

// The items of a track of a song, up to its end or its jump back (ConvertDseTrack): the items, the item where the
// track's loop starts, and whether the track plays notes.
struct WalkedTrack
{
    std::vector<uint32_t> walked;
    std::optional<uint32_t> loop;
    bool notes = false;
};

// The items of the track of `song` that starts at item `start`. Throws std::invalid_argument with a message for the
// user for a track that calls subroutines.
WalkedTrack WalkDseTrack(const Sequence& song, uint32_t start)
{
    WalkedTrack track;
    for (uint32_t item = start; item < song.items.size(); ++item)
    {
        const Command& command = song.items[item];
        if (command.kind == Kind::kEnd)
        {
            break;
        }

        if (command.kind == Kind::kJump)
        {
            track.loop = !command.target_outside && command.target >= start && command.target <= item
                             ? std::optional<uint32_t>(command.target)
                             : std::nullopt;
            break;
        }

        if (command.kind == Kind::kCall || command.kind == Kind::kReturn)
        {
            throw std::invalid_argument("The song's tracks call subroutines. DSE has none.");
        }

        track.walked.push_back(item);
        track.notes = track.notes || command.kind == Kind::kNote;
    }

    return track;
}

// Adds `command` to `out` as DSE plays it (ConvertDseTrack). Counts in `left_out` the commands that DSE has no
// command for, and in `times` the times that stay out.
void AddDseCommand(const Command& command, Sequence& out, std::map<std::string, std::size_t>& left_out,
                   std::size_t& times)
{
    switch (command.kind)
    {
    case Kind::kOpenTrack:
    case Kind::kAllocateTracks:
    case Kind::kTimebase:
        break;

    case Kind::kNote:
    case Kind::kWait:
    case Kind::kProgram:
    case Kind::kVolume:
    case Kind::kVolume2:
    case Kind::kPan:
    case Kind::kTempo:
    case Kind::kBendRange:
        {
            Command kept = command;
            kept.encoding = {};
            if (kept.time)
            {
                kept.time.reset();
                ++times;
            }

            out.items.push_back(std::move(kept));
            break;
        }

    case Kind::kPitchBend:
        {
            const int64_t bend = std::clamp<int64_t>(
                std::llround(static_cast<double>(command.value.value) * 8192.0 / kOne), -8192, 8191);
            Command key_bend;
            key_bend.kind = Kind::kOther;
            key_bend.encoding.opcode = kKeyBend;
            const auto bits = static_cast<uint16_t>(static_cast<int16_t>(bend));
            key_bend.bytes = {static_cast<uint8_t>(bits >> 8), static_cast<uint8_t>(bits)};
            out.items.push_back(std::move(key_bend));
            break;
        }

    default:
        ++left_out[song::SettingName(command)];
        break;
    }
}

// Adds track `number` of `song`, which starts at item `start`, to `out` as DSE plays it (AsDse).
void ConvertDseTrack(const Sequence& song, uint8_t number, uint32_t start, Sequence& out,
                     std::map<std::string, std::size_t>& left_out, std::size_t& times)
{
    const WalkedTrack track = WalkDseTrack(song, start);
    out.items.push_back(dse::TrackOpening(number, number));
    if (track.notes)
    {
        out.items.push_back(BankSelect(kStandInBank));
    }

    for (const uint32_t item : track.walked)
    {
        if (track.loop && item == *track.loop)
        {
            Command point;
            point.kind = Kind::kOther;
            point.encoding.opcode = kLoopPoint;
            out.items.push_back(std::move(point));
        }

        AddDseCommand(song.items[item], out, left_out, times);
    }

    out.items.push_back(Command{.kind = Kind::kEnd});
}

// The tracks of a song of NintendoWare's MIDI converter as DSE plays them. The song is laid out with every bar in its
// track. Each track of the song becomes a track on the channel of its number. A track's jump back marks its loop point,
// and its end ends it. A track with notes selects a bank at its opening: ReplaceDseSound makes it select the bank of
// the sound. The kinds that DSE's bytes hold keep their values. A pitch bend becomes the driver's key bend, a fraction
// of the bend range in 8192ths (0x02073C90). `left_out` counts the commands that stay out, by what they set.
Sequence AsDse(const Sequence& song, std::map<std::string, std::size_t>& left_out, std::size_t& times)
{
    const std::map<uint8_t, uint32_t> starts = TrackStarts(song);

    Sequence out;
    for (const auto& [number, start] : starts)
    {
        ConvertDseTrack(song, number, start, out, left_out, times);
    }

    return out;
}

} // namespace

namespace dse_files_parts
{

void RequireSet(const format::File& file)
{
    if (!file.native || file.native->engine != format::Engine::kDse)
    {
        throw std::invalid_argument("The file contains no DSE files.");
    }
}

std::set<uint32_t> ProgramsOf(const Sequence& tracks)
{
    std::set<uint32_t> programs;
    for (const Command& command : tracks.items)
    {
        if (command.kind == Kind::kProgram && command.value.source == ntdlib::commands::Source::kValue)
        {
            programs.insert(static_cast<uint32_t>(command.value.value / kOne));
        }
    }

    return programs;
}

std::string NumberList(const std::set<uint32_t>& numbers)
{
    std::string list;
    for (const uint32_t number : numbers)
    {
        list += (list.empty() ? "" : ", ") + std::to_string(number);
    }

    return list;
}

const song::DseBank& BankAt(const song::DseSet& set, uint32_t bank)
{
    if (bank >= set.banks.size())
    {
        throw std::invalid_argument("The image has no bank " + std::to_string(bank) + ".");
    }

    return set.banks[bank];
}

} // namespace dse_files_parts

DseIncomingSong ReadIncomingDseSong(std::span<const uint8_t> bytes)
{
    dse::Song song;
    try
    {
        song = dse::Song::Parse(bytes);
    }
    catch (const xsflib::FormatError& error)
    {
        throw std::invalid_argument(std::string("Can't read the file as a DSE song (smdl): ") + error.what() + ".");
    }

    DseIncomingSong incoming;
    incoming.tracks = dse::DecodeTracks(song.tracks);
    incoming.ticks_per_quarter = song.info.TicksPerQuarter();
    incoming.track_count = song.info.TrackCount();
    incoming.channels = song.info.ChannelCount();
    if (song.tracks.empty())
    {
        throw std::invalid_argument("The song has no tracks.");
    }

    return incoming;
}

DseIncomingSong DseSongOfMidi(std::span<const uint8_t> midi, const std::string& file_name, const ProgramMap& programs,
                              const format::MidiDrumNotes& drum_notes)
{
    // The song at 48 ticks a quarter note, every bar in its track.
    const MidiSong song = ImportedSong(midi, StemOf(file_name), 48, drum_notes, "DSE");
    const Sequence laid_out = WithPrograms(LayOutConverted(song.timeline, song.hints, false), 0, programs);

    DseIncomingSong incoming;
    std::map<std::string, std::size_t> left_out;
    std::size_t times = 0;
    incoming.tracks = AsDse(laid_out, left_out, times);
    incoming.ticks_per_quarter = 48;
    incoming.channels = static_cast<uint8_t>(std::max<std::size_t>(TracksOf(incoming.tracks).channels, 1));
    try
    {
        dse::EncodeTracks(incoming.tracks);
    }
    catch (const std::invalid_argument& error)
    {
        throw std::invalid_argument(std::string("Can't write the song's commands for DSE. ") +
                                    format::AsSentence(error.what()));
    }

    // What the song leaves out, and what DSE changes.
    incoming.warnings = song.warnings;
    if (!left_out.empty())
    {
        std::string names;
        for (const auto& [name, count] : left_out)
        {
            names += (names.empty() ? "" : ", ") + name;
        }

        incoming.warnings.push_back("DSE has no commands for these settings, and they're left out: " + names + ".");
    }

    if (times > 0)
    {
        incoming.warnings.push_back("DSE sets values at once. " + std::to_string(times) +
                                    (times == 1 ? " setting loses its time." : " settings lose their times."));
    }

    return incoming;
}

std::vector<uint32_t> DseSongPrograms(const DseIncomingSong& song)
{
    const std::set<uint32_t> programs = ProgramsOf(song.tracks);
    return {programs.begin(), programs.end()};
}

std::size_t DseSongTracks(const DseIncomingSong& song)
{
    return TracksOf(song.tracks).tracks;
}

format::File ReplaceDseSound(const format::File& file, const song::DseSet& set, uint32_t sound,
                             const DseIncomingSong& song, const ProgramMap& programs)
{
    RequireSet(file);
    const auto& input = SoundAt(set, sound);
    const std::string name = song::DseSoundName(set, sound);

    // The sound's sequence, and where its settings are in the set.
    const std::vector<uint8_t> bytes = format::ExportArchive(file);
    const std::vector<ntdlib::dse::SequenceRef> refs = song::DseSoundRefs(set);
    const std::optional<uint32_t> sequence = format::DseSoundSequences(file, bytes, refs).at(sound);
    const std::map<std::string, SetFile> files = FilesOf(bytes);
    const auto held = files.find(refs.at(sound).path);
    if (!sequence || held == files.end() || set.tracks_offsets.at(sound) < dse::SequenceInfo::kSize)
    {
        throw std::invalid_argument("Hokuto can't edit the tracks of " + name + ".");
    }

    // The new tracks, with the sound's banks and the chosen programs. A sound effect plays at most 4 tracks, and Hokuto
    // plays at most 32.
    Sequence tracks = WithDsePrograms(WithBanks(song.tracks, IdsOf(input.banks)), programs);
    const TrackCount count = TracksOf(tracks);
    const std::size_t most = input.effects ? kDseEffectTracks : static_cast<std::size_t>(dse::Player::kHostTracks);
    if (count.tracks == 0 || count.tracks > most)
    {
        throw std::invalid_argument(
            name + (input.effects ? " is a sound effect, and plays at most " : " plays at most ") +
            std::to_string(most) + " tracks. The file has " + std::to_string(count.tracks) + ".");
    }
    if (count.channels > kDseChannels)
    {
        throw std::invalid_argument("The file's tracks play channels past the driver's 16.");
    }

    try
    {
        dse::EncodeTracks(tracks);
    }
    catch (const std::invalid_argument& error)
    {
        throw std::invalid_argument(std::string("Can't write the tracks for DSE. ") + format::AsSentence(error.what()));
    }

    // The sequence keeps its first label. The label names its file.
    const Sequence& old = *file.sequences.at(*sequence);
    tracks.labels.clear();
    if (!old.labels.empty())
    {
        tracks.labels.push_back({old.labels.front().name, 0});
    }

    // The settings take the new ticks of a quarter note and the counts of tracks and channels. The counts of a song
    // file can exceed the tracks that it contains, and they stay.
    const uint64_t info = held->second.offset + set.tracks_offsets[sound] - dse::SequenceInfo::kSize;
    const uint8_t channels = static_cast<uint8_t>(std::max<std::size_t>(song.channels, count.channels));
    const uint8_t track_count = static_cast<uint8_t>(std::max<std::size_t>(song.track_count, count.tracks));
    const std::vector<ArchiveBytes> changes = {
        {info + kTicksPerQuarter,
         {static_cast<uint8_t>(song.ticks_per_quarter), static_cast<uint8_t>(song.ticks_per_quarter >> 8)}},
        {info + kTrackCount, {track_count, channels}},
    };
    format::File result = WithArchiveBytes(file, format::NativePieceStarts(file), changes, name);
    result.sequences[*sequence] = std::move(tracks);

    return result;
}

std::vector<std::string> DseSoundWarnings(const song::DseSet& set, uint32_t sound, const DseIncomingSong& song,
                                          const ProgramMap& programs)
{
    const auto& input = SoundAt(set, sound);
    std::vector<std::string> warnings = song.warnings;
    const std::vector<uint16_t> ids = IdsOf(input.banks);
    if (input.banks.empty())
    {
        warnings.push_back(song::DseSoundName(set, sound) + " plays no bank. Its notes are silent.");
        return warnings;
    }

    // The bank selects that select another bank, and the programs that are missing from the sound's first bank.
    const std::string first = song::DseBankName(*input.banks.front());
    if (SelectsOtherBanks(song.tracks, ids))
    {
        warnings.push_back("The new tracks select banks that this sound doesn't play. They play " + first +
                           " instead.");
    }
    else if (std::ranges::any_of(SelectRuns(song.tracks),
                                 [&](const SelectRun& run) { return std::ranges::find(ids, run.id) == ids.end(); }))
    {
        warnings.push_back(
            "Some tracks select banks that this sound doesn't load. Their notes play only if the game "
            "has loaded those banks.");
    }

    const std::vector<uint32_t> held = song::DseBankPrograms(*input.banks.front());
    std::set<uint32_t> missing;
    for (const uint32_t program : ProgramsOf(WithDsePrograms(song.tracks, programs)))
    {
        if (std::ranges::find(held, program) == held.end())
        {
            missing.insert(program);
        }
    }
    if (!missing.empty())
    {
        warnings.push_back("Programs missing from " + first + ": " + NumberList(missing) + ". Their notes are silent.");
    }

    return warnings;
}

} // namespace hokuto::edit
