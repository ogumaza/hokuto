// SPDX-License-Identifier: MIT

// The model form of the Wii's archives (format/model.h).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/nwsnd/commands.h"
#include "ntdlib/nwsnd/console.h"
#include "ntdlib/nwsnd/units.h"
#include "ntdlib/rsar/archive_model.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/sequence_file.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/archive_model.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sound_build.h"
#include "xsflib/binary.h"

#include "format/archive.h"
#include "format/binary.h"
#include "format/codes.h"
#include "format/command_values.h"
#include "format/kinds.h"
#include "format/model.h"
#include "format/model_parts.h"

namespace hokuto::format
{
namespace
{

namespace rsar = ntdlib::rsar;
namespace sar = ntdlib::sar;
using binary::Damaged;
using binary::In;
using binary::Newer;
using binary::Out;
using model_parts::CutOrder;
using model_parts::kAlignedTrees;
using model_parts::KnownOf;
using model_parts::kOctober2006;
using model_parts::kReleasePriorityFix;
using model_parts::kSequenceFile;
using model_parts::kStartItem;
using model_parts::kUnterminatedPaths;
using model_parts::kWiiArchive;
using model_parts::kWiiBuild;
using model_parts::kWiiFileFlags;
using model_parts::kWiiHeldFile;
using model_parts::kWiiItemFlags;
using model_parts::kWiiPathFile;
using model_parts::kWiiSoundFlags;
using model_parts::ReadBytes;
using model_parts::ReadList;
using model_parts::Renamed;
using model_parts::RequireWhole;
using model_parts::SequenceVersions;
using model_parts::SetSequenceStarts;
using model_parts::TakeSequenceStarts;
using model_parts::WriteBytes;
using model_parts::WriteList;
using ntdlib::commands::Fixed;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Sequence;

// The code of a kind of the Wii's sounds in the SNDS chunk: the code of the later consoles' kind.
uint8_t WiiSoundCode(rsar::SoundType type)
{
    switch (type)
    {
    case rsar::SoundType::kSequence:
        return CodeOf(sar::SoundType::kSequence);
    case rsar::SoundType::kWaveSound:
        return CodeOf(sar::SoundType::kWaveSound);
    case rsar::SoundType::kStream:
        return CodeOf(sar::SoundType::kStream);
    case rsar::SoundType::kUnknown:
        break;
    }

    return 0;
}

std::optional<rsar::SoundType> WiiSoundTypeOfCode(uint8_t code)
{
    for (const rsar::SoundType type :
         {rsar::SoundType::kSequence, rsar::SoundType::kWaveSound, rsar::SoundType::kStream})
    {
        if (WiiSoundCode(type) == code)
        {
            return type;
        }
    }

    return std::nullopt;
}

// The bytes of a sequence file of the Wii of `version` that holds `sequence`, encoded by the rules of NW4R SND's build
// `build`, and the offset of each of its items in the bytecode, then the end. ntdlib's join writes a sequence file of
// the Wii the same way.
std::pair<std::vector<uint8_t>, std::vector<uint32_t>> WiiSequenceFileOf(const Sequence& sequence,
                                                                         const ntdlib::SndBuild& build,
                                                                         uint32_t version)
{
    ntdlib::nwsnd::Encoded encoded =
        ntdlib::nwsnd::EncodeSequence(sequence, ntdlib::nwsnd::GrammarOf<ntdlib::nwsnd::RevolutionPolicy>(build));
    rsar::SequenceFile file;
    file.version = static_cast<uint16_t>(version);
    file.data = std::move(encoded.bytes);
    for (const ntdlib::commands::Label& label : sequence.labels)
    {
        file.labels.push_back({label.name, encoded.offsets.at(label.item)});
    }

    return {file.Write(), std::move(encoded.offsets)};
}

// Writes file `id` of the Wii's model to the ITMS chunk.
void WriteWiiFileModel(Out& out, const Model& model, uint32_t id)
{
    const rsar::FileModel& held = model.wii->files[id];
    const auto sequence = model.sequences.find(id);
    const auto beside = model.streams.find(id);
    const bool has_sequence = sequence != model.sequences.end();
    const bool has_bytes = held.path && beside != model.streams.end();
    out.U8((has_sequence ? kSequenceFile : 0) | (held.path ? kWiiPathFile : 0) | (has_bytes ? kWiiHeldFile : 0));
    out.Zeros(3);
    if (has_sequence)
    {
        out.U32(sequence->second.sequence);
        out.U32(sequence->second.version);
    }

    if (held.path)
    {
        out.Name(*held.path, "bytes in a file path");
        out.U32(held.path_size);
        if (has_bytes)
        {
            WriteBytes(out, beside->second);
        }

        return;
    }

    if (!has_sequence)
    {
        WriteBytes(out, held.bytes);
    }

    WriteBytes(out, held.wave_data);
}

} // namespace

namespace model_parts
{

// The model of `file`'s archive of the Wii (ModelOf).
Model WiiModelOf(const File& file)
{
    // The archive that the sequences without the encoder's choices build, and its items.
    File plain = file;
    plain.sequences = WithoutChoices(file.sequences);
    const std::vector<uint8_t> bytes = ExportArchive(plain);
    RequireWhole(bytes, 8, true);
    Model model;
    model.wii = rsar::ReadArchiveModel(bytes);
    model.wii_build = file.native->wii_build;
    rsar::ArchiveModel& archive = *model.wii;

    // The sequence that each file holds. Each copy of a sequence file takes its bytes from the sequence.
    const rsar::SoundArchive read = rsar::SoundArchive::Load(bytes, ntdlib::rvl::snd::kRsarOptions);
    const std::vector<std::optional<uint32_t>> sequences = SequencesOfFiles(plain, bytes, read);
    const std::map<uint32_t, uint32_t> versions = SequenceVersions(*file.native);
    for (uint32_t id = 0; id < sequences.size() && id < archive.files.size(); ++id)
    {
        if (sequences[id])
        {
            model.sequences[id] = {*sequences[id], versions.at(*sequences[id])};
            archive.files[id].bytes.clear();
        }
    }

    // Each sequence sound whose file holds a sequence starts at an item of the sequence.
    TakeSequenceStarts(archive, model, plain,
                       ntdlib::nwsnd::GrammarOf<ntdlib::nwsnd::RevolutionPolicy>(BuildOfWiiArchive(*file.native)),
                       rsar::SoundType::kSequence);

    for (const auto& [id, stream] : file.native->streams)
    {
        model.streams[id] = *stream.bytes;
    }

    return model;
}

// `file` with the archive of `model`, a model of the Wii's archive (WithModel).
File WithWiiModel(File file, const Model& model)
{
    // Each sequence file's bytes, and each sequence sound's start.
    rsar::ArchiveModel archive = *model.wii;
    const ntdlib::SndBuild build = model.wii_build.value_or(BuildOfWiiVersion(archive.version));
    std::map<uint32_t, std::vector<uint32_t>> offsets;
    for (const auto& [id, held] : model.sequences)
    {
        if (id >= archive.files.size() || held.sequence >= file.sequences.size() || archive.files[id].path)
        {
            Damaged("file " + std::to_string(id) + " of the model names no file or no sequence");
        }

        std::pair<std::vector<uint8_t>, std::vector<uint32_t>> written;
        try
        {
            written = WiiSequenceFileOf(*file.sequences[held.sequence], build, held.version);
        }
        catch (const std::exception& error)
        {
            throw xsflib::FormatError("Can't write sequence " + std::to_string(held.sequence) + " for the Wii. " +
                                      AsSentence(error.what()));
        }

        archive.files[id].bytes = std::move(written.first);
        offsets[held.sequence] = std::move(written.second);
    }

    SetSequenceStarts(archive, model, offsets, rsar::SoundType::kSequence);

    // The archive that the model lays out, cut as an import cuts a game's archive.
    std::vector<uint8_t> bytes;
    try
    {
        bytes = rsar::BuildArchive(archive);
    }
    catch (const std::invalid_argument& error)
    {
        throw xsflib::FormatError(AsSentence(std::string("Can't lay out the archive from the model: ") + error.what()));
    }

    File built = ImportWiiArchive(bytes, model.wii_build, model.streams, KnownOf(file, model.sequences));

    // The cut's sequences are the file's sequences, in another order.
    const rsar::SoundArchive read = rsar::SoundArchive::Load(bytes, ntdlib::rvl::snd::kRsarOptions);
    const std::vector<std::optional<uint32_t>> cut = SequencesOfFiles(built, bytes, read);
    const std::map<uint32_t, uint32_t> map = CutOrder(file, built, model, cut);

    // The native archive names the file's sequences. Its streams are Hokuto's, and it keeps no game's sizes.
    NativeArchive native = *built.native;
    native.pieces = Renamed(*native.pieces, map);
    for (auto& [id, stream] : native.streams)
    {
        stream.from_game = false;
    }

    native.game_sizes.reset();
    file.native = std::move(native);
    file.form = ArchiveForm::kModel;

    return file;
}

// The Wii's form of the SNDS chunk (SoundsChunk).
std::vector<uint8_t> WiiSoundsChunk(const Model& model)
{
    const rsar::ArchiveModel& archive = *model.wii;
    Out out;
    out.Count(archive.sounds.size(), "sounds");
    for (uint32_t s = 0; s < archive.sounds.size(); ++s)
    {
        const rsar::SoundModel& sound = archive.sounds[s];
        const auto start = model.starts.find(s);
        uint8_t flags = 0;
        flags |= sound.release_priority_fix ? kReleasePriorityFix : 0;
        flags |= start != model.starts.end() ? kStartItem : 0;

        out.Name(sound.name, "bytes in a sound name");
        out.U8(WiiSoundCode(sound.type));
        out.U8(flags);
        out.Zeros(2);
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
        out.U32(sound.user_params[0]);
        out.U32(sound.user_params[1]);
        out.U32(sound.settings_3d.flags);
        out.U8(sound.settings_3d.decay_curve);
        out.U8(sound.settings_3d.decay_ratio);
        out.U8(sound.settings_3d.doppler_factor);
        out.U8(0);
        switch (sound.type)
        {
        case rsar::SoundType::kSequence:
            out.U32(start != model.starts.end() ? start->second : sound.start_offset);
            out.U32(sound.bank);
            out.U32(sound.allocate_track_flags);
            break;

        case rsar::SoundType::kWaveSound:
            out.U32(static_cast<uint32_t>(sound.wave_sound_index));
            out.U32(sound.allocate_track_count);
            break;

        case rsar::SoundType::kStream:
            out.U32(sound.stream_start);
            out.U16(sound.allocate_channel_count);
            out.U16(sound.stream_track_flags);
            break;

        case rsar::SoundType::kUnknown:
            throw std::invalid_argument("sound " + std::to_string(s) + " is of an unknown kind");
        }
    }

    return out.Take();
}

// The Wii's form of the ITMS chunk (ItemsChunk).
std::vector<uint8_t> WiiItemsChunk(const Model& model)
{
    const rsar::ArchiveModel& archive = *model.wii;
    Out out;
    out.U8(kWiiArchive);
    uint8_t flags = 0;
    flags |= model.wii_build ? kWiiBuild : 0;
    flags |= archive.aligned_trees ? kAlignedTrees : 0;
    flags |= archive.terminated_paths ? 0 : kUnterminatedPaths;
    out.U8(flags);
    out.Zeros(2);
    out.U32(archive.version);
    out.U32(model.wii_build ? model.wii_build->date : 0);
    out.U8(model.wii_build && model.wii_build->october_2006 ? kOctober2006 : 0);
    out.Zeros(3);
    const rsar::PlayerSettings& settings = archive.player_settings;
    for (const uint16_t count :
         {settings.sequence_sounds, settings.sequence_tracks, settings.stream_sounds, settings.stream_tracks,
          settings.stream_channels, settings.wave_sounds, settings.wave_tracks})
    {
        out.U16(count);
    }

    out.Zeros(2);

    out.Count(archive.banks.size(), "banks");
    for (const rsar::BankModel& bank : archive.banks)
    {
        out.Name(bank.name, "bytes in a bank name");
        out.U32(bank.file_id);
    }

    out.Count(archive.players.size(), "players");
    for (const rsar::PlayerModel& player : archive.players)
    {
        out.Name(player.name, "bytes in a player name");
        out.U32(player.playable_sound_count);
        out.U32(player.heap_size);
    }

    out.Count(archive.groups.size(), "groups");
    for (const rsar::GroupModel& group : archive.groups)
    {
        out.Name(group.name, "bytes in a group name");
        WriteList(out, group.file_ids, "files of a group");
    }

    // The files: each one's flags, then its sequence, its bytes and its wave data, or its path and the bytes of a
    // stream there.
    out.Count(archive.files.size(), "files");
    for (uint32_t id = 0; id < archive.files.size(); ++id)
    {
        WriteWiiFileModel(out, model, id);
    }

    return out.Take();
}

} // namespace model_parts

namespace
{

// Reads file `id` of the ITMS chunk of a Wii archive into `model`.
void ReadWiiFileModel(In& in, uint32_t id, Model& model)
{
    rsar::ArchiveModel& archive = *model.wii;
    rsar::FileModel& held = archive.files.emplace_back();
    const uint8_t file_flags = in.U8();
    if ((file_flags & ~kWiiFileFlags) != 0)
    {
        Newer("a file flag");
    }

    in.Take(3);
    if ((file_flags & kSequenceFile) != 0)
    {
        ModelSequence& sequence = model.sequences[id];
        sequence.sequence = in.U32();
        sequence.version = in.U32();
    }

    if ((file_flags & kWiiPathFile) != 0)
    {
        if ((file_flags & kSequenceFile) != 0)
        {
            Damaged("file " + std::to_string(id) + " is next to the archive and contains a sequence");
        }

        held.path = in.Name();
        held.path_size = in.U32();
        if ((file_flags & kWiiHeldFile) != 0)
        {
            model.streams[id] = ReadBytes(in);
        }

        return;
    }

    if ((file_flags & kWiiHeldFile) != 0)
    {
        Damaged("file " + std::to_string(id) + " contains the bytes of a file next to the archive");
    }

    if ((file_flags & kSequenceFile) == 0)
    {
        held.bytes = ReadBytes(in);
    }

    held.wave_data = ReadBytes(in);
}

// Reads sound `s` of the SNDS chunk of a Wii archive into `model`.
void ReadWiiSoundModel(In& sound_in, uint32_t s, Model& model)
{
    rsar::ArchiveModel& archive = *model.wii;
    rsar::SoundModel& sound = archive.sounds.emplace_back();
    sound.name = sound_in.Name();
    const uint8_t kind = sound_in.U8();
    const std::optional<rsar::SoundType> type = WiiSoundTypeOfCode(kind);
    if (!type)
    {
        Newer("a sound type (" + std::to_string(kind) + ")");
    }

    sound.type = *type;
    const uint8_t sound_flags = sound_in.U8();
    if ((sound_flags & ~kWiiSoundFlags) != 0)
    {
        Newer("a sound flag");
    }

    sound_in.Take(2);
    sound.release_priority_fix = (sound_flags & kReleasePriorityFix) != 0;
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
    sound.user_params[0] = sound_in.U32();
    sound.user_params[1] = sound_in.U32();
    sound.settings_3d.flags = sound_in.U32();
    sound.settings_3d.decay_curve = sound_in.U8();
    sound.settings_3d.decay_ratio = sound_in.U8();
    sound.settings_3d.doppler_factor = sound_in.U8();
    sound_in.U8();

    const bool start_item = (sound_flags & kStartItem) != 0;
    if (start_item != (sound.type == rsar::SoundType::kSequence && model.sequences.contains(sound.file_id)))
    {
        Damaged("sound " + std::to_string(s) + " starts at an item, but its file contains no sequence");
    }

    switch (sound.type)
    {
    case rsar::SoundType::kSequence:
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

            sound.bank = sound_in.U32();
            sound.allocate_track_flags = sound_in.U32();
            break;
        }

    case rsar::SoundType::kWaveSound:
        sound.wave_sound_index = static_cast<int32_t>(sound_in.U32());
        sound.allocate_track_count = sound_in.U32();
        break;

    default:
        sound.stream_start = sound_in.U32();
        sound.allocate_channel_count = sound_in.U16();
        sound.stream_track_flags = sound_in.U16();
        break;
    }
}

// Reads the head of the Wii's ITMS chunk after its kind into `model`: the flags, the archive's version, the build of
// the sound library and the player settings.
void ReadWiiArchiveHead(In& in, Model& model)
{
    rsar::ArchiveModel& archive = model.wii.emplace();
    const uint8_t flags = in.U8();
    if ((flags & ~kWiiItemFlags) != 0)
    {
        Newer("a Wii archive flag");
    }

    in.Take(2);

    const uint32_t version = in.U32();
    if (version > 0xFFFF)
    {
        Damaged("the Wii archive has a version above 0xFFFF");
    }

    archive.version = static_cast<uint16_t>(version);

    const uint32_t date = in.U32();
    const uint8_t build_flags = in.U8();
    if ((build_flags & ~kOctober2006) != 0)
    {
        Newer("a build of the Wii sound library");
    }

    in.Take(3);
    if ((flags & kWiiBuild) != 0)
    {
        model.wii_build = ntdlib::SndBuild{.date = date, .october_2006 = (build_flags & kOctober2006) != 0};
    }

    archive.aligned_trees = (flags & kAlignedTrees) != 0;
    archive.terminated_paths = (flags & kUnterminatedPaths) == 0;

    rsar::PlayerSettings& settings = archive.player_settings;
    for (uint16_t* count :
         {&settings.sequence_sounds, &settings.sequence_tracks, &settings.stream_sounds, &settings.stream_tracks,
          &settings.stream_channels, &settings.wave_sounds, &settings.wave_tracks})
    {
        *count = in.U16();
    }

    in.Take(2);
}

// Reads the banks, players and groups of the Wii's ITMS chunk into `archive`.
void ReadWiiItemLists(In& in, rsar::ArchiveModel& archive)
{
    const uint32_t banks = in.U32();
    for (uint32_t i = 0; i < banks; ++i)
    {
        rsar::BankModel& bank = archive.banks.emplace_back();
        bank.name = in.Name();
        bank.file_id = in.U32();
    }

    const uint32_t players = in.U32();
    for (uint32_t i = 0; i < players; ++i)
    {
        rsar::PlayerModel& player = archive.players.emplace_back();
        player.name = in.Name();
        const uint32_t limit = in.U32();
        if (limit > 0xFF)
        {
            Damaged("player " + std::to_string(i) + " plays more than 255 sounds at once");
        }

        player.playable_sound_count = static_cast<uint8_t>(limit);
        player.heap_size = in.U32();
    }

    const uint32_t groups = in.U32();
    for (uint32_t i = 0; i < groups; ++i)
    {
        rsar::GroupModel& group = archive.groups.emplace_back();
        group.name = in.Name();
        group.file_ids = ReadList(in);
    }
}

} // namespace

namespace model_parts
{

// The model that the Wii's SNDS chunk `sounds` and ITMS chunk hold. `in` reads the ITMS chunk after its kind
// (ReadModel).
Model ReadWiiModel(std::span<const uint8_t> sounds, In& in)
{
    Model model;
    ReadWiiArchiveHead(in, model);
    ReadWiiItemLists(in, *model.wii);

    const uint32_t files = in.U32();
    for (uint32_t id = 0; id < files; ++id)
    {
        ReadWiiFileModel(in, id, model);
    }

    // The sounds.
    In sound_in(sounds, "the SNDS chunk");
    const uint32_t count = sound_in.U32();
    for (uint32_t s = 0; s < count; ++s)
    {
        ReadWiiSoundModel(sound_in, s, model);
    }

    return model;
}

} // namespace model_parts

} // namespace hokuto::format
