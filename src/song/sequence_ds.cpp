// SPDX-License-Identifier: MIT

// The songs of the DS: the sequences of its sound archives and the songs of DSE.

#include <exception>

#include "ntdlib/dse/player.h"
#include "ntdlib/ntr/snd/commands.h"
#include "ntdlib/sequence_export/dse/performance.h"
#include "ntdlib/sequence_export/instruments.h"
#include "ntdlib/sequence_export/ntr/performance.h"
#include "xsflib/binary.h"

#include "song/archive.h"
#include "song/ds.h"
#include "song/dse.h"
#include "song/sequence_parts.h"

namespace hokuto::song
{
namespace
{

namespace export_ = ntdlib::sequence_export;

using sequence_parts::AddTrack;
using sequence_parts::AddUnstarted;
using sequence_parts::ClipToLoop;
using sequence_parts::EventKind;
using sequence_parts::FileCommands;
using sequence_parts::GameChanges;
using sequence_parts::ReadReason;
using sequence_parts::ReadTrack;
using sequence_parts::SequenceHead;
using sequence_parts::SetCallsAndVariables;
using sequence_parts::TrackEvent;

// The commands of the DS's bytecode `data`, decoded from `starts`, the starts of the sequences that play it. The
// encoder returns the offset of each item. The bytes after the encoding have to be zeros: the padding of the sequence's
// file. Returns nothing for bytecode that the decoder keeps as data, or that the codec can't give back.
std::optional<FileCommands> DecodeDsCommands(std::span<const uint8_t> data, const std::vector<uint32_t>& starts)
{
    try
    {
        ntdlib::ntr::snd::Decoded decoded = ntdlib::ntr::snd::DecodeSequence(data, {}, starts);
        if (decoded.opaque)
        {
            return std::nullopt;
        }

        ntdlib::ntr::snd::Encoded encoded = ntdlib::ntr::snd::EncodeSequence(decoded.sequence);
        if (encoded.bytes.size() <= data.size() &&
            std::ranges::equal(encoded.bytes, data.first(encoded.bytes.size())) &&
            std::ranges::all_of(data.subspan(encoded.bytes.size()), [](uint8_t byte) { return byte == 0; }))
        {
            return FileCommands{std::move(decoded.sequence), std::move(encoded.offsets), std::move(decoded.entries)};
        }
    }
    catch (const std::invalid_argument&)
    {
    }
    catch (const std::out_of_range&)
    {
    }

    return std::nullopt;
}

// The bytecode of the file of `sequence`, and the offsets in it where the file's sequences start: the SSEQ's start, or
// the start of each sequence of the SSAR. Throws xsflib::FormatError for a file that can't be read.
std::pair<std::span<const uint8_t>, std::vector<uint32_t>> DsFileStarts(const ntdlib::sdat::Archive& archive,
                                                                        const ntdlib::ntr::SequenceRef& sequence)
{
    if (!sequence.archived)
    {
        const ntdlib::sdat::SequenceInfo& info = *archive.sequences.at(sequence.index);
        return {ntdlib::sdat::Sequence::Parse(archive.File(info.file_id)).bytecode, {0}};
    }

    const ntdlib::sdat::SequenceArchiveInfo& info = *archive.sequence_archives.at(sequence.index);
    const ntdlib::sdat::SequenceArchive parsed = ntdlib::sdat::SequenceArchive::Parse(archive.File(info.file_id));
    std::set<uint32_t> starts;
    for (const ntdlib::sdat::ArchivedSequence& archived : parsed.sequences)
    {
        if (archived.offset != kNoSsarSequence)
        {
            starts.insert(archived.offset);
        }
    }

    return {parsed.bytecode, std::vector<uint32_t>(starts.begin(), starts.end())};
}

// Bank `bank` of `archive`, or nothing for a bank that is missing from the archive or can't be read.
std::optional<ntdlib::sdat::Bank> ReadDsBank(const DsArchive& archive, uint32_t bank)
{
    if (bank >= archive.archive.banks.size() || !archive.archive.banks[bank])
    {
        return std::nullopt;
    }

    try
    {
        return ntdlib::sdat::Bank::Parse(archive.archive.File(archive.archive.banks[bank]->file_id));
    }
    catch (const xsflib::FormatError&)
    {
        return std::nullopt;
    }
}

// The PSG channel of `note` (Note::psg): the driver's channel less 7, where the note's region plays a square wave on
// channels 8 to 13 or noise on channels 14 and 15. A wave on those channels has 0.
uint8_t DsPsgChannel(const ntdlib::sdat::Bank& bank, const Note& note)
{
    using enum ntdlib::sdat::InstrumentType;
    constexpr int kFirstSquare = 8;
    constexpr int kFirstNoise = 14;
    constexpr int kChannels = 16;
    if (note.channel < kFirstSquare || note.channel >= kChannels || note.program >= bank.instruments.size())
    {
        return 0;
    }

    const ntdlib::sdat::Instrument& instrument = bank.instruments[note.program];
    ntdlib::sdat::InstrumentType kind = instrument.type;
    if (kind == kDrumSet || kind == kKeySplit)
    {
        const ntdlib::sdat::Region* region = DsRegionAt(instrument, note.key);
        kind = region != nullptr ? region->type : kNone;
    }

    const bool plays = note.channel < kFirstNoise ? kind == kPsg : kind == kNoise;
    return plays ? static_cast<uint8_t>(note.channel - kFirstSquare + 1) : 0;
}

// The offset of `sequence`'s start in the bytecode of its file.
uint32_t DsStart(const ntdlib::sdat::Archive& archive, const ntdlib::ntr::SequenceRef& sequence)
{
    if (!sequence.archived)
    {
        return 0;
    }

    const ntdlib::sdat::SequenceArchiveInfo& info = *archive.sequence_archives.at(sequence.index);
    return ntdlib::sdat::SequenceArchive::Parse(archive.File(info.file_id)).sequences.at(*sequence.archived).offset;
}

// Adds to `sequence` the calls and the variables of the DS sequence `ref`.
void AddDsCalls(Sequence& sequence, const ntdlib::sdat::Archive& archive, const ntdlib::ntr::SequenceRef& ref)
{
    // The calls and the variables come from the commands of the sequence's file. Every sequence of an SSAR starts
    // a path through its bytecode, and the calls of each count. The DS's global variables start at -1. A file that
    // the decoder can't read plays all the same.
    try
    {
        const auto [data, starts] = DsFileStarts(archive, ref);
        if (const std::optional<FileCommands> commands = DecodeDsCommands(data, starts))
        {
            SetCallsAndVariables(sequence, *commands, starts, DsStart(archive, ref), -1);
        }
    }
    catch (const xsflib::FormatError&)
    {
    }
}

// Adds to `sequence` the tracks of `performance`, a performance of a sequence of `archive`.
void AddDsTracks(Sequence& sequence, const DsArchive& archive, const export_::ntr::Performance& performance)
{
    // The tracks and the length follow the same rules as the other consoles' (sequence.cpp, ReadSequence).
    const std::string bank_name = DsBankName(archive, performance.bank);
    const std::optional<ntdlib::sdat::Bank> bank = ReadDsBank(archive, performance.bank);
    const auto preset_of = [&performance](uint32_t preset) -> std::optional<export_::PresetKey>
    {
        if (preset >= performance.presets.size())
        {
            return std::nullopt;
        }

        export_::PresetKey key;
        key.program = performance.presets[preset].program;
        return key;
    };

    for (std::size_t i = 0; i < performance.tracks.size(); i++)
    {
        Track track = ReadTrack(static_cast<int>(i), performance.tracks[i], performance, preset_of,
                                [&bank_name](uint32_t) { return bank_name; });
        if (track.notes.empty() && track.stretches.empty())
        {
            continue;
        }

        if (bank)
        {
            for (Note& note : track.notes)
            {
                note.psg = DsPsgChannel(*bank, note);
            }
        }

        AddUnstarted(track, performance.unstarted, i);
        ClipToLoop(sequence, track);
        AddTrack(sequence, std::move(track));
    }
}

} // namespace

Sequence ReadSequence(const DsArchive& archive, uint32_t sound, const StartValues& values, const Changes& changes)
{
    const std::string name = DsSoundName(archive, sound);
    if (sound >= archive.sounds.size() || !archive.sounds[sound])
    {
        throw std::runtime_error("The archive has no sequence " + std::to_string(sound) + ".");
    }

    // The performer plays one pass of the loop, with the sound's profile, and sets the variables before the sequence
    // starts. A game sets them the same way. Every note plays the sequence's bank.
    const ntdlib::ntr::SequenceRef& ref = *archive.sounds[sound];
    export_::ntr::PerformOptions options = DsPerformOptions(archive, sound, values);
    options.loops = 1;
    StartValues kept;
    for (const auto& [number, value] : values)
    {
        if (number >= 0 && number < kGameVariables)
        {
            kept[number] = value;
        }
    }

    // The performer counts the driver's frames, two to a frame of the model.
    Changes kept_changes = GameChanges(changes);
    for (const VariableChange& change : kept_changes)
    {
        options.changes.push_back(
            {2 * ChangeFrame(change.time, kDsFrameMs), static_cast<uint8_t>(change.variable), change.value});
    }

    try
    {
        const export_::ntr::Performance performance = export_::ntr::Perform(archive.archive, ref, options);

        Sequence sequence = SequenceHead(performance, name, sound);
        sequence.start_values = std::move(kept);
        sequence.changes = std::move(kept_changes);
        sequence.holds = performance.holds;
        sequence.ds = true;
        AddDsTracks(sequence, archive, performance);
        AddDsCalls(sequence, archive.archive, ref);

        return sequence;
    }
    catch (const std::runtime_error& error)
    {
        // The error keeps its cause nested. A caller can find a full sound heap (ntdlib::sdat::HeapFullError) there.
        std::throw_with_nested(std::runtime_error("Can't read " + name + ". " + ReadReason(error.what())));
    }
    catch (const std::invalid_argument& error)
    {
        throw std::runtime_error("Can't read " + name + ". " + ReadReason(error.what()));
    }
}

namespace
{

// A DSE performance as ReadTrack reads it: by the sequence's tracks, where the performance's MIDI tracks are the
// sequence's channels.
struct DseTracks
{
    std::vector<std::string> track_names;
    std::vector<export_::NoteCommand> note_commands;
    std::vector<export_::dse::NoteVoices> note_channels;
    decltype(export_::dse::Performance::loop) loop;
    uint32_t end_tick = 0;
    std::vector<std::vector<export_::Stretch>> stretches;
    std::vector<std::vector<export_::MixChange>> mix;
};

// The channel of each track of the song or the sound effect of `input`, by the tracks' slots.
std::vector<uint8_t> DseTrackChannels(const export_::dse::SequenceInput& input)
{
    const std::vector<ntdlib::dse::Track>* tracks = nullptr;
    if (input.song)
    {
        tracks = &input.song->tracks;
    }
    else if (input.effects && input.effect < input.effects->sequences.size() && input.effects->sequences[input.effect])
    {
        tracks = &input.effects->sequences[input.effect]->tracks;
    }

    std::vector<uint8_t> channels;
    if (tracks != nullptr)
    {
        for (const ntdlib::dse::Track& track : *tracks)
        {
            channels.push_back(track.channel);
        }
    }

    return channels;
}

// Moves the offsets of `performance` from the file's start to `tracks_offset`, the start of the sequence's tracks.
void ShiftOffsets(export_::dse::Performance& performance, uint32_t tracks_offset)
{
    for (export_::NoteCommand& command : performance.note_commands)
    {
        command.offset -= tracks_offset;
    }

    for (export_::dse::UnstartedNote& unstarted : performance.unstarted)
    {
        unstarted.offset -= tracks_offset;
    }

    for (std::vector<export_::Stretch>& stretches : performance.stretches)
    {
        for (export_::Stretch& stretch : stretches)
        {
            stretch.first -= tracks_offset;
            stretch.last -= tracks_offset;
        }
    }
}

// The IDs and the names of a sound's banks, by the banks' slots.
struct DseBanks
{
    std::vector<std::string> names;
    std::vector<uint16_t> ids;
};

DseBanks DseBanksOf(const export_::dse::SequenceInput& input)
{
    DseBanks banks;
    for (const auto& bank : input.banks)
    {
        banks.ids.push_back(DseBankId(*bank));
        banks.names.push_back(DseBankName(*bank));
    }

    return banks;
}

// Adds `event` to the events of every track whose channel in `channels` is `channel`.
void AddToChannelTracks(std::vector<std::vector<TrackEvent>>& events, const TrackEvent& event,
                        const std::vector<uint8_t>& channels, std::size_t channel)
{
    for (std::size_t t = 0; t < channels.size(); t++)
    {
        if (channels[t] == channel)
        {
            events[t].push_back(event);
        }
    }
}

// The events of each of the `track_count` tracks of `performance`, whose MIDI tracks are the sequence's `channels`. A
// note is added to the track that played it, and a preset to every track of its channel.
std::vector<std::vector<TrackEvent>> DseTrackEvents(const export_::dse::Performance& performance,
                                                    const std::vector<uint8_t>& channels, std::size_t track_count)
{
    std::vector<std::vector<TrackEvent>> events(track_count);
    for (std::size_t c = 0; c < performance.tracks.size(); c++)
    {
        for (const TrackEvent& event : performance.tracks[c])
        {
            if (event.kind == EventKind::kNoteOn || event.kind == EventKind::kNoteOff)
            {
                if (event.note >= 1 && event.note <= performance.note_tracks.size() &&
                    performance.note_tracks[event.note - 1] < track_count)
                {
                    events[performance.note_tracks[event.note - 1]].push_back(event);
                }
            }
            else if (event.kind == EventKind::kPreset)
            {
                AddToChannelTracks(events, event, channels, c);
            }
        }
    }

    return events;
}

// Adds to `sequence` the `track_count` tracks of `performance`, the performance of the sound of `input`, whose MIDI
// tracks are the sequence's `channels`.
void AddDseTracks(Sequence& sequence, const export_::dse::SequenceInput& input,
                  const export_::dse::Performance& performance, const std::vector<uint8_t>& channels,
                  std::size_t track_count)
{
    const DseBanks banks = DseBanksOf(input);

    const auto preset_of = [&](uint32_t preset) -> std::optional<export_::PresetKey>
    {
        if (preset >= performance.presets.size())
        {
            return std::nullopt;
        }

        export_::PresetKey key;
        const auto slot = std::find(banks.ids.begin(), banks.ids.end(), performance.presets[preset].bank);
        key.bank_slot = slot == banks.ids.end() ? 0 : static_cast<uint8_t>(slot - banks.ids.begin());
        key.program = performance.presets[preset].program;

        return key;
    };

    const auto bank_name = [&](uint32_t slot)
    {
        return slot < banks.names.size() ? banks.names[slot] : "BANK_" + std::to_string(slot);
    };

    // Each note is added to the track that played it, and each preset to every track of its channel.
    DseTracks tracks{std::vector<std::string>(track_count),
                     performance.note_commands,
                     performance.note_channels,
                     performance.loop,
                     performance.end_tick,
                     performance.stretches,
                     performance.mix};
    tracks.stretches.resize(track_count);
    tracks.mix.resize(track_count);
    std::vector<std::vector<TrackEvent>> events = DseTrackEvents(performance, channels, track_count);

    // The length follows the same rules as the other consoles' (sequence.cpp, ReadSequence).
    for (std::size_t t = 0; t < track_count; t++)
    {
        Track track = ReadTrack(static_cast<int>(t), std::move(events[t]), tracks, preset_of, bank_name);
        if (track.notes.empty() && track.stretches.empty())
        {
            continue;
        }

        track.channel = t < channels.size() ? channels[t] : -1;
        AddUnstarted(track, performance.unstarted, t);
        ClipToLoop(sequence, track);
        AddTrack(sequence, std::move(track));
    }
}

} // namespace

Sequence ReadSequence(const DseSet& set, uint32_t sound)
{
    const std::string name = DseSoundName(set, sound);
    if (sound >= set.sounds.size())
    {
        throw std::runtime_error("The image has no DSE sound " + std::to_string(sound) + ".");
    }

    // The performer plays one pass of the loop. A note's bank slot is the place in the sound's banks of the bank that
    // its channel selects.
    const export_::dse::SequenceInput& input = set.sounds[sound];
    export_::dse::Performance performance;
    try
    {
        performance = export_::dse::Perform(input.Files(), {.loops = 1, .build = set.build});
    }
    catch (const std::runtime_error& error)
    {
        throw std::runtime_error("Can't read " + name + ". " + ReadReason(error.what()));
    }
    catch (const std::invalid_argument& error)
    {
        throw std::runtime_error("Can't read " + name + ". " + ReadReason(error.what()));
    }

    // A sound effect that the driver refuses runs no events. The refusal is the last warning: the warnings about the
    // banks come first (ntdlib's Performer::Run).
    if (!performance.warnings.empty() && performance.stretches.empty())
    {
        throw std::runtime_error("Can't read " + name + ". " + ReadReason(performance.warnings.back()));
    }

    // The offsets of the performance count from the start of the file, and the bytes of the sequence from the start of
    // its tracks.
    const uint32_t tracks_offset = set.tracks_offsets[sound];
    ShiftOffsets(performance, tracks_offset);

    // A model mutes and meters each track by a bit of a 32-bit mask (ntdlib's Player::kHostTracks).
    const std::vector<uint8_t> channels = DseTrackChannels(input);
    const std::size_t track_count = std::max(performance.stretches.size(), channels.size());
    if (track_count > static_cast<std::size_t>(ntdlib::dse::Player::kHostTracks))
    {
        throw std::runtime_error("Can't read " + name + ". It has " + std::to_string(track_count) +
                                 " tracks, and Hokuto plays at most " +
                                 std::to_string(ntdlib::dse::Player::kHostTracks) + ".");
    }

    Sequence sequence = SequenceHead(performance, name, sound);
    sequence.holds = performance.holds;
    AddDseTracks(sequence, input, performance, channels, track_count);

    return sequence;
}

} // namespace hokuto::song
