// SPDX-License-Identifier: MIT

// The SNDS and ITMS chunks of the model form of the Switch, the Wii U and the 3DS (format/model.h). model_wii.cpp
// writes and reads the chunks of the Wii.

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
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/nwsnd/commands.h"
#include "ntdlib/nwsnd/console.h"
#include "ntdlib/nwsnd/units.h"
#include "ntdlib/sar/archive_model.h"
#include "ntdlib/sar/formats.h"

#include "format/binary.h"
#include "format/codes.h"
#include "format/model.h"
#include "format/model_parts.h"

namespace hokuto::format
{
namespace
{

namespace sar = ntdlib::sar;
using binary::Damaged;
using binary::In;
using binary::Newer;
using binary::Out;
using model_parts::kBytesPlace;
using model_parts::kCopyPlace;
using model_parts::kExtension;
using model_parts::kFileFlags;
using model_parts::kGroupsOnlyPlace;
using model_parts::kHeldFile;
using model_parts::kOption17;
using model_parts::kPathPlace;
using model_parts::kReleasePriorityFix;
using model_parts::kSequenceFile;
using model_parts::kSinglePlay;
using model_parts::kSoundFlags;
using model_parts::kStartItem;
using model_parts::kUserFlags;
using model_parts::kWiiArchive;
using model_parts::ReadBytes;
using model_parts::ReadList;
using model_parts::ReadWiiModel;
using model_parts::WiiItemsChunk;
using model_parts::WiiSoundsChunk;
using model_parts::WriteBytes;
using model_parts::WriteList;
using ntdlib::commands::Fixed;
using ntdlib::commands::Kind;

// The fields after a stream sound's sends, by their numbers in the SNDS chunk.
constexpr uint8_t kNoFields = 0;
constexpr uint8_t kExtensionField = 1;
constexpr uint8_t kPrefetchFields = 2;

// The options of a 3DS stream sound of 2.3.0.0: the values of the extension that its entry holds, a bit each.
constexpr uint8_t kStreamOptions = (1 << 3) - 1;

// The group of a group file in the ITMS chunk where Hokuto couldn't find it.
constexpr uint32_t kNoGroup = 0xFFFFFFFF;

// The flags of a sound group and of a wave archive.
constexpr uint8_t kWaveArchives = 1 << 0;
constexpr uint8_t kLoadIndividually = 1 << 0;
constexpr uint8_t kWaveCount = 1 << 1;

// The scales of the values that a stream sound's details share with the commands: a pan, a surround pan, a send and a
// low-pass filter. The Wii U and the Switch use the same scales.
const ntdlib::nwsnd::Grammar kGrammar = ntdlib::nwsnd::GrammarOf<ntdlib::nwsnd::NxPolicy>();
const ntdlib::nwsnd::Scale kPanScale = ntdlib::nwsnd::ScaleOf(Kind::kPan, kGrammar);
const ntdlib::nwsnd::Scale kSpanScale = ntdlib::nwsnd::ScaleOf(Kind::kSurroundPan, kGrammar);
const ntdlib::nwsnd::Scale kSendScale = ntdlib::nwsnd::ScaleOf(Kind::kSend, kGrammar);
const ntdlib::nwsnd::Scale kLowPassScale = ntdlib::nwsnd::ScaleOf(Kind::kLowPass, kGrammar);

// A byte of the console in its unit, and the byte nearest a value in its unit.
Fixed ToUnit(uint8_t value, const ntdlib::nwsnd::Scale& scale)
{
    return ntdlib::nwsnd::ToUnit(value, scale);
}

uint8_t ByteOf(Fixed unit, const ntdlib::nwsnd::Scale& scale)
{
    return static_cast<uint8_t>(std::clamp<int64_t>(ntdlib::nwsnd::FromUnit(unit, scale), 0, 255));
}

// A main send and the sends to the three aux buses, in Hokuto's units.
void WriteSends(Out& out, uint8_t main, const std::array<uint8_t, 3>& aux)
{
    out.I64(ToUnit(main, kSendScale));
    for (const uint8_t send : aux)
    {
        out.I64(ToUnit(send, kSendScale));
    }
}

void ReadSends(In& in, uint8_t& main, std::array<uint8_t, 3>& aux)
{
    main = ByteOf(in.I64(), kSendScale);
    for (uint8_t& send : aux)
    {
        send = ByteOf(in.I64(), kSendScale);
    }
}

void WriteStream(Out& out, const sar::SoundModel& sound)
{
    const sar::StreamSoundInfo& stream = sound.stream;
    out.U16(stream.valid_tracks);
    out.U16(stream.channel_count);
    out.F32(stream.pitch);
    WriteSends(out, stream.main_send, stream.aux_sends);
    switch (sound.stream_fields)
    {
    case sar::StreamFields::kNone:
        out.U8(kNoFields);
        break;
    case sar::StreamFields::kExtension:
        out.U8(kExtensionField);
        break;
    case sar::StreamFields::kPrefetch:
        out.U8(kPrefetchFields);
        break;
    }

    out.U8(sound.stream_options);
    out.Zeros(2);
    out.U32(sound.prefetch_file_id);
    if (const std::optional<sar::StreamExtension>& extension = sound.stream_extension)
    {
        out.U32(extension->stream_type);
        out.U32(extension->loop_start);
        out.U32(extension->loop_end);
    }

    out.Count(stream.tracks.size(), "tracks of a stream sound");
    for (const sar::StreamTrackInfo& track : stream.tracks)
    {
        out.Count(track.channels.size(), "channels of a stream track");
        out.Bytes(track.channels);
        out.I64(LevelOfVolume(track.volume));
        out.I64(ToUnit(track.pan, kPanScale));
        out.I64(ToUnit(track.span, kSpanScale));
        out.U8(track.flags);
        out.U8(track.biquad_type);
        out.U8(track.biquad_value);
        out.U8(0);
        WriteSends(out, track.main_send, track.aux_sends);
        out.I64(ToUnit(track.low_pass, kLowPassScale));
    }
}

void ReadStream(In& in, sar::SoundModel& sound, bool extension)
{
    sar::StreamSoundInfo& stream = sound.stream;
    stream.valid_tracks = in.U16();
    stream.channel_count = in.U16();
    stream.pitch = in.F32();
    ReadSends(in, stream.main_send, stream.aux_sends);
    const uint8_t fields = in.U8();
    if (fields > kPrefetchFields)
    {
        Newer("a type of stream sound details (" + std::to_string(fields) + ")");
    }

    sound.stream_fields = fields == kNoFields         ? sar::StreamFields::kNone
                          : fields == kExtensionField ? sar::StreamFields::kExtension
                                                      : sar::StreamFields::kPrefetch;
    sound.stream_options = in.U8();
    if ((sound.stream_options & ~kStreamOptions) != 0)
    {
        Newer("a stream sound option");
    }

    in.Take(2);
    sound.prefetch_file_id = in.U32();
    if (extension)
    {
        sar::StreamExtension& read = sound.stream_extension.emplace();
        read.stream_type = in.U32();
        read.loop_start = in.U32();
        read.loop_end = in.U32();
    }

    const uint32_t tracks = in.U32();
    for (uint32_t t = 0; t < tracks; ++t)
    {
        sar::StreamTrackInfo& track = stream.tracks.emplace_back();
        const std::span<const uint8_t> channels = in.Take(in.U32());
        track.channels.assign(channels.begin(), channels.end());
        track.volume = VolumeOfLevel(in.I64());
        track.pan = ByteOf(in.I64(), kPanScale);
        track.span = ByteOf(in.I64(), kSpanScale);
        track.flags = in.U8();
        track.biquad_type = in.U8();
        track.biquad_value = in.U8();
        in.U8();
        ReadSends(in, track.main_send, track.aux_sends);
        track.low_pass = ByteOf(in.I64(), kLowPassScale);
    }
}

// Writes sound `s` of the model to the SNDS chunk.
void WriteSoundModel(Out& out, const Model& model, uint32_t s)
{
    const sar::SoundModel& sound = model.archive.sounds[s];
    const auto start = model.starts.find(s);
    uint8_t flags = 0;
    flags |= sound.release_priority_fix ? kReleasePriorityFix : 0;
    flags |= sound.single_play ? kSinglePlay : 0;
    flags |= sound.option_17 ? kOption17 : 0;
    flags |= start != model.starts.end() ? kStartItem : 0;
    flags |= sound.stream_extension ? kExtension : 0;
    uint8_t users = 0;
    for (std::size_t i = 0; i < sound.user_options.size(); ++i)
    {
        users |= sound.user_options[i] ? 1 << i : 0;
    }

    out.Name(sound.name, "bytes in a sound name");
    out.U8(CodeOf(sound.type));
    out.U8(flags);
    out.U8(users);
    out.U8(0);
    out.U32(sound.file_id);
    out.U32(sound.player_id);
    out.I64(LevelOfVolume(sound.volume));
    out.U8(sound.remote_filter);
    out.U8(sound.pan_mode);
    out.U8(sound.pan_curve);
    out.U8(sound.player_priority);
    out.U8(sound.actor_player_id);
    out.U8(sound.channel_priority);
    out.Zeros(2);
    if (sound.single_play)
    {
        out.U32(*sound.single_play);
    }

    if (sound.option_17)
    {
        out.U32(*sound.option_17);
    }

    for (const std::optional<uint32_t>& option : sound.user_options)
    {
        if (option)
        {
            out.U32(*option);
        }
    }

    out.U32(sound.settings_3d.flags);
    out.F32(sound.settings_3d.decay_ratio);
    out.U8(sound.settings_3d.decay_curve);
    out.U8(sound.settings_3d.doppler_factor);
    out.Zeros(2);
    out.U32(sound.after_3d);

    // The details of the sound's kind.
    switch (sound.type)
    {
    case sar::SoundType::kSequence:
        out.U32(start != model.starts.end() ? start->second : sound.start_offset);
        out.U32(sound.allocate_track_flags);
        WriteList(out, sound.banks, "banks of a sound");
        break;

    case sar::SoundType::kWaveSound:
        out.U32(sound.wave_sound_index);
        break;

    case sar::SoundType::kStream:
        WriteStream(out, sound);
        break;

    case sar::SoundType::kUnknown:
        throw std::invalid_argument("sound " + std::to_string(s) + " is of an unknown kind");
    }
}

// Writes file `id` of the model to the ITMS chunk.
void WriteFileModel(Out& out, const Model& model, uint32_t id)
{
    const sar::FileModel& held = model.archive.files[id];
    const auto sequence = model.sequences.find(id);
    const auto beside = model.streams.find(id);
    const bool has_sequence = sequence != model.sequences.end();
    const bool has_bytes = held.place == sar::FilePlace::kPath && beside != model.streams.end();
    uint8_t place = kBytesPlace;
    switch (held.place)
    {
    case sar::FilePlace::kBytes:
        place = kBytesPlace;
        break;
    case sar::FilePlace::kCopy:
        place = kCopyPlace;
        break;
    case sar::FilePlace::kGroupsOnly:
        place = kGroupsOnlyPlace;
        break;
    case sar::FilePlace::kPath:
        place = kPathPlace;
        break;
    }

    out.U8(place);
    out.U8((has_sequence ? kSequenceFile : 0) | (has_bytes ? kHeldFile : 0));
    out.Zeros(2);
    WriteList(out, held.groups, "groups of a file");
    if (has_sequence)
    {
        out.U32(sequence->second.sequence);
        out.U32(sequence->second.version);
    }

    switch (held.place)
    {
    case sar::FilePlace::kBytes:
        if (!has_sequence)
        {
            WriteBytes(out, held.bytes);
        }
        break;

    case sar::FilePlace::kCopy:
        out.U32(held.copy_group);
        break;

    case sar::FilePlace::kGroupsOnly:
        break;

    case sar::FilePlace::kPath:
        out.Name(held.path, "bytes in a file path");
        if (has_bytes)
        {
            WriteBytes(out, beside->second);
        }
        break;
    }
}

// Writes the sound groups, banks, wave archives, groups and players of `archive` to the ITMS chunk.
void WriteItemLists(Out& out, const sar::ArchiveModel& archive)
{
    out.Count(archive.sound_groups.size(), "sound groups");
    for (const sar::SoundGroupModel& group : archive.sound_groups)
    {
        out.Name(group.name, "bytes in a sound group name");
        out.U32(group.first_sound);
        out.U32(group.last_sound);
        WriteList(out, group.file_ids, "files of a sound group");
        out.U8(group.wave_archives ? kWaveArchives : 0);
        out.Zeros(3);
        if (group.wave_archives)
        {
            WriteList(out, *group.wave_archives, "wave archives of a sound group");
        }
    }

    out.Count(archive.banks.size(), "banks");
    for (const sar::BankModel& bank : archive.banks)
    {
        out.Name(bank.name, "bytes in a bank name");
        out.U32(bank.file_id);
        WriteList(out, bank.wave_archives, "wave archives of a bank");
    }

    out.Count(archive.wave_archives.size(), "wave archives");
    for (const sar::WaveArchiveModel& wave_archive : archive.wave_archives)
    {
        out.Name(wave_archive.name, "bytes in a wave archive name");
        out.U32(wave_archive.file_id);
        out.U8((wave_archive.load_individually ? kLoadIndividually : 0) | (wave_archive.wave_count ? kWaveCount : 0));
        out.Zeros(3);
        out.U32(wave_archive.wave_count.value_or(0));
    }

    out.Count(archive.groups.size(), "groups");
    for (const sar::GroupModel& group : archive.groups)
    {
        out.Name(group.name, "bytes in a group name");
        out.U32(group.file_id);
    }

    out.Count(archive.players.size(), "players");
    for (const sar::PlayerModel& player : archive.players)
    {
        out.Name(player.name, "bytes in a player name");
        out.U32(player.playable_sound_max);
        out.U32(player.heap_size);
    }
}

} // namespace

std::vector<uint8_t> SoundsChunk(const Model& model)
{
    if (model.wii)
    {
        return WiiSoundsChunk(model);
    }

    Out out;
    out.Count(model.archive.sounds.size(), "sounds");
    for (uint32_t s = 0; s < model.archive.sounds.size(); ++s)
    {
        WriteSoundModel(out, model, s);
    }

    return out.Take();
}

std::vector<uint8_t> ItemsChunk(const Model& model)
{
    if (model.wii)
    {
        return WiiItemsChunk(model);
    }

    const sar::ArchiveModel& archive = model.archive;
    Out out;
    out.U8(CodeOf(archive.flavor));
    out.Zeros(3);
    out.U32(archive.version);
    const sar::PlayerSettings& settings = archive.player_settings;
    for (const uint16_t count :
         {settings.sequence_sounds, settings.sequence_tracks, settings.stream_sounds, settings.stream_tracks,
          settings.stream_channels, settings.wave_sounds, settings.wave_tracks})
    {
        out.U16(count);
    }

    out.U8(settings.stream_buffer_times);
    out.U8(0);

    WriteItemLists(out, archive);

    // The files: each one's place and flags, its groups, then its sequence, its bytes, its group or its path.
    out.Count(archive.files.size(), "files");
    for (uint32_t id = 0; id < archive.files.size(); ++id)
    {
        WriteFileModel(out, model, id);
    }

    out.Count(model.group_files.size(), "group files");
    for (const std::vector<uint8_t>& group_file : model.group_files)
    {
        WriteBytes(out, group_file);
    }

    for (std::size_t g = 0; g < model.group_files.size(); ++g)
    {
        const bool known = g < model.group_file_groups.size() && model.group_file_groups[g];
        out.U32(known ? *model.group_file_groups[g] : kNoGroup);
    }

    return out.Take();
}

namespace
{

// Reads the sound groups, banks, wave archives, groups and players of the ITMS chunk into `archive`.
void ReadItemLists(In& in, sar::ArchiveModel& archive)
{
    const uint32_t sound_groups = in.U32();
    for (uint32_t i = 0; i < sound_groups; ++i)
    {
        sar::SoundGroupModel& group = archive.sound_groups.emplace_back();
        group.name = in.Name();
        group.first_sound = in.U32();
        group.last_sound = in.U32();
        group.file_ids = ReadList(in);
        const uint8_t flags = in.U8();
        if ((flags & ~kWaveArchives) != 0)
        {
            Newer("a sound group flag");
        }

        in.Take(3);
        if ((flags & kWaveArchives) != 0)
        {
            group.wave_archives = ReadList(in);
        }
    }

    const uint32_t banks = in.U32();
    for (uint32_t i = 0; i < banks; ++i)
    {
        sar::BankModel& bank = archive.banks.emplace_back();
        bank.name = in.Name();
        bank.file_id = in.U32();
        bank.wave_archives = ReadList(in);
    }

    const uint32_t wave_archives = in.U32();
    for (uint32_t i = 0; i < wave_archives; ++i)
    {
        sar::WaveArchiveModel& wave_archive = archive.wave_archives.emplace_back();
        wave_archive.name = in.Name();
        wave_archive.file_id = in.U32();
        const uint8_t flags = in.U8();
        if ((flags & ~(kLoadIndividually | kWaveCount)) != 0)
        {
            Newer("a wave archive flag");
        }

        in.Take(3);
        wave_archive.load_individually = (flags & kLoadIndividually) != 0;
        const uint32_t count = in.U32();
        if ((flags & kWaveCount) != 0)
        {
            wave_archive.wave_count = count;
        }
    }

    const uint32_t groups = in.U32();
    for (uint32_t i = 0; i < groups; ++i)
    {
        sar::GroupModel& group = archive.groups.emplace_back();
        group.name = in.Name();
        group.file_id = in.U32();
    }

    const uint32_t players = in.U32();
    for (uint32_t i = 0; i < players; ++i)
    {
        sar::PlayerModel& player = archive.players.emplace_back();
        player.name = in.Name();
        player.playable_sound_max = in.U32();
        player.heap_size = in.U32();
    }
}

// Reads file `id` of the ITMS chunk into `model`.
void ReadFileModel(In& in, uint32_t id, Model& model)
{
    sar::ArchiveModel& archive = model.archive;
    sar::FileModel& held = archive.files.emplace_back();
    const uint8_t place = in.U8();
    const uint8_t flags = in.U8();
    if (place > kPathPlace)
    {
        Newer("a file location (" + std::to_string(place) + ")");
    }

    if ((flags & ~kFileFlags) != 0)
    {
        Newer("a file flag");
    }

    in.Take(2);
    held.groups = ReadList(in);
    if ((flags & kSequenceFile) != 0)
    {
        ModelSequence& sequence = model.sequences[id];
        sequence.sequence = in.U32();
        sequence.version = in.U32();
    }

    switch (place)
    {
    case kBytesPlace:
        held.place = sar::FilePlace::kBytes;
        if ((flags & kSequenceFile) == 0)
        {
            held.bytes = ReadBytes(in);
        }
        break;

    case kCopyPlace:
        held.place = sar::FilePlace::kCopy;
        held.copy_group = in.U32();
        break;

    case kGroupsOnlyPlace:
        held.place = sar::FilePlace::kGroupsOnly;
        break;

    default:
        held.place = sar::FilePlace::kPath;
        held.path = in.Name();
        if ((flags & kHeldFile) != 0)
        {
            model.streams[id] = ReadBytes(in);
        }
        break;
    }

    if ((flags & kHeldFile) != 0 && held.place != sar::FilePlace::kPath)
    {
        Damaged("file " + std::to_string(id) + " contains the bytes of a file next to the archive");
    }
}

// Reads the fields of a sound in the SNDS chunk that every type of sound has, after its flags `flags` and the flags of
// its user parameters `users`, into `sound`.
void ReadSoundFields(In& sound_in, uint8_t flags, uint8_t users, sar::SoundModel& sound)
{
    sound.file_id = sound_in.U32();
    sound.player_id = sound_in.U32();
    sound.volume = VolumeOfLevel(sound_in.I64());
    sound.remote_filter = sound_in.U8();
    sound.pan_mode = sound_in.U8();
    sound.pan_curve = sound_in.U8();
    sound.player_priority = sound_in.U8();
    sound.actor_player_id = sound_in.U8();
    sound.channel_priority = sound_in.U8();
    sound_in.Take(2);

    if ((flags & kSinglePlay) != 0)
    {
        sound.single_play = sound_in.U32();
    }

    if ((flags & kOption17) != 0)
    {
        sound.option_17 = sound_in.U32();
    }

    for (std::size_t i = 0; i < sound.user_options.size(); ++i)
    {
        if ((users >> i & 1) != 0)
        {
            sound.user_options[i] = sound_in.U32();
        }
    }

    sound.settings_3d.flags = sound_in.U32();
    sound.settings_3d.decay_ratio = sound_in.F32();
    sound.settings_3d.decay_curve = sound_in.U8();
    sound.settings_3d.doppler_factor = sound_in.U8();
    sound_in.Take(2);
    sound.after_3d = sound_in.U32();
}

// Reads sound `s` of the SNDS chunk into `model`.
void ReadSoundModel(In& sound_in, uint32_t s, Model& model)
{
    sar::ArchiveModel& archive = model.archive;
    sar::SoundModel& sound = archive.sounds.emplace_back();
    sound.name = sound_in.Name();
    const uint8_t kind = sound_in.U8();
    const std::optional<sar::SoundType> type = SoundTypeOfCode(kind);
    if (!type || *type == sar::SoundType::kUnknown)
    {
        Newer("a sound type (" + std::to_string(kind) + ")");
    }

    sound.type = *type;
    const uint8_t flags = sound_in.U8();
    const uint8_t users = sound_in.U8();
    if ((flags & ~kSoundFlags) != 0 || (users & ~kUserFlags) != 0)
    {
        Newer("a sound flag");
    }

    sound_in.U8();
    sound.release_priority_fix = (flags & kReleasePriorityFix) != 0;
    ReadSoundFields(sound_in, flags, users, sound);

    const bool start_item = (flags & kStartItem) != 0;
    if (start_item != (sound.type == sar::SoundType::kSequence && model.sequences.contains(sound.file_id)))
    {
        Damaged("sound " + std::to_string(s) + " starts at an item, but its file contains no sequence");
    }

    if ((flags & kExtension) != 0 && sound.type != sar::SoundType::kStream)
    {
        Damaged("sound " + std::to_string(s) + " has a stream sound extension");
    }

    switch (sound.type)
    {
    case sar::SoundType::kSequence:
        {
            const uint32_t start = sound_in.U32();
            if (start_item)
            {
                model.starts[s] = start;
            }
            else
            {
                sound.start_offset = start;
            }

            sound.allocate_track_flags = sound_in.U32();
            sound.banks = ReadList(sound_in);
            break;
        }

    case sar::SoundType::kWaveSound:
        sound.wave_sound_index = sound_in.U32();
        break;

    case sar::SoundType::kStream:
    case sar::SoundType::kUnknown: // refused above
        ReadStream(sound_in, sound, (flags & kExtension) != 0);
        break;
    }
}

} // namespace

Model ReadModel(std::span<const uint8_t> sounds, std::span<const uint8_t> items)
{
    Model model;
    sar::ArchiveModel& archive = model.archive;
    In in(items, "the ITMS chunk");
    const uint8_t code = in.U8();
    if (code == kWiiArchive)
    {
        return ReadWiiModel(sounds, in);
    }

    const std::optional<sar::Flavor> flavor = FlavorOfCode(code);
    if (!flavor)
    {
        Newer("an archive type (" + std::to_string(code) + ")");
    }

    archive.flavor = *flavor;
    in.Take(3);
    archive.version = in.U32();
    sar::PlayerSettings& settings = archive.player_settings;
    for (uint16_t* count :
         {&settings.sequence_sounds, &settings.sequence_tracks, &settings.stream_sounds, &settings.stream_tracks,
          &settings.stream_channels, &settings.wave_sounds, &settings.wave_tracks})
    {
        *count = in.U16();
    }

    settings.stream_buffer_times = in.U8();
    in.U8();

    ReadItemLists(in, archive);

    const uint32_t files = in.U32();
    for (uint32_t id = 0; id < files; ++id)
    {
        ReadFileModel(in, id, model);
    }

    const uint32_t group_files = in.U32();
    for (uint32_t i = 0; i < group_files; ++i)
    {
        model.group_files.push_back(ReadBytes(in));
    }

    // Each group file's group. A chunk that ends after the group files gives none: the group files then take the groups
    // that their copies give.
    if (in.Left() > 0)
    {
        for (uint32_t i = 0; i < group_files; ++i)
        {
            const uint32_t group = in.U32();
            model.group_file_groups.push_back(group == kNoGroup ? std::nullopt : std::optional<uint32_t>(group));
        }
    }

    // The sounds.
    In sound_in(sounds, "the SNDS chunk");
    const uint32_t count = sound_in.U32();
    for (uint32_t s = 0; s < count; ++s)
    {
        ReadSoundModel(sound_in, s, model);
    }

    return model;
}

} // namespace hokuto::format
