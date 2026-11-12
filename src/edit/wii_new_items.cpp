// SPDX-License-Identifier: MIT

// New items in the archives of the Wii (edit/new_items.h): sequence sounds, banks, wave sounds and stream sounds,
// through the model of the archive's items.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/rounding.h"
#include "ntdlib/rsar/archive_model.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/sequence_file.h"
#include "ntdlib/rsar/stream_file.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/layout.h"
#include "ntdlib/sound_build.h"
#include "seqlib/file_name.h"
#include "xsflib/ascii.h"
#include "xsflib/binary.h"

#include "edit/files.h"
#include "edit/files_internal.h"
#include "edit/groups.h"
#include "edit/new_items.h"
#include "format/archive.h"
#include "format/kinds.h"
#include "format/model.h"
#include "song/archive.h"

namespace hokuto::edit
{

namespace
{

namespace rsar = ntdlib::rsar;
namespace sar = ntdlib::sar;

// The sequence of each sequence file of `bytes`, the archive of the Wii that `file` builds, by the file's ID.
std::map<uint32_t, uint32_t> WiiSequencesByFile(const format::File& file, std::span<const uint8_t> bytes)
{
    const rsar::SoundArchive read =
        rsar::SoundArchive::Load(std::vector<uint8_t>(bytes.begin(), bytes.end()), ntdlib::rvl::snd::kRsarOptions);

    return SequencesByFileId(format::SequencesOfFiles(file, bytes, read));
}

// Throws std::invalid_argument with a message for the user for `file` when it's a file of another console.
void RequireWii(const format::File& file)
{
    if (!file.native || file.native->engine != format::Engine::kWii)
    {
        throw std::invalid_argument("The file contains no Wii archive.");
    }
}

// The model of `bytes`, an archive of the Wii. Throws std::invalid_argument with a message for the user for an archive
// that is cut short, and an archive that NintendoWare for Revolution's tools didn't lay out.
rsar::ArchiveModel ModelOfBytes(std::span<const uint8_t> bytes)
{
    try
    {
        return rsar::ReadArchiveModel(bytes);
    }
    catch (const xsflib::FormatError& error)
    {
        // The message of a reader for a read past the end states only an offset. The tools' tables name only data
        // inside the archive. Such a read therefore means that the archive is cut short.
        if (std::string_view(error.what()).starts_with("read out of bounds"))
        {
            throw std::invalid_argument(
                "Can't add items to this archive. It's cut short: its tables refer to data past its end.");
        }

        throw std::invalid_argument(std::string("NintendoWare for Revolution's tools didn't lay out this archive. "
                                                "Hokuto can add items only to archives laid out by those tools. ") +
                                    format::AsSentence(error.what()));
    }
}

// The model of the archive of `file`, a file of the Wii, and the archive's bytes. Throws std::invalid_argument with a
// message for the user for a file of another console, an archive that is cut short, and an archive that NintendoWare
// for Revolution's tools didn't lay out.
std::pair<rsar::ArchiveModel, std::vector<uint8_t>> WiiModelOf(const format::File& file)
{
    RequireWii(file);
    std::vector<uint8_t> bytes = format::ExportArchive(file);
    rsar::ArchiveModel model = ModelOfBytes(bytes);

    return {std::move(model), std::move(bytes)};
}

// The groups of `archive` for the windows (WiiGroupsOf).
std::vector<Group> GroupsOfModel(const rsar::ArchiveModel& archive)
{
    std::vector<Group> groups;
    for (uint32_t g = 0; g < archive.groups.size(); ++g)
    {
        groups.push_back({.group = g, .name = archive.groups[g].name, .files = archive.groups[g].file_ids});
    }

    return groups;
}

// `file`, a file of the Wii, after its archive takes the place of `bytes` with the archive of `model`, its model with
// the new items. The file's sequences keep their numbers, and the new ones follow in the order of the cut. The
// sequences that didn't change stay shared. The file's other parts stay. A file in the model form stays in it. The cut
// takes the new files' sequences of `added` by their IDs too. Throws std::invalid_argument with a message for the user
// for a model that ntdlib can't lay out.
format::File WithWiiModel(const format::File& file, std::span<const uint8_t> bytes, const rsar::ArchiveModel& model,
                          const sar::KnownSequences& added = {})
{
    std::vector<uint8_t> built;
    try
    {
        built = rsar::BuildArchive(model);
    }
    catch (const std::invalid_argument& error)
    {
        throw std::invalid_argument(std::string("Can't lay out the archive with the new items: ") + error.what() + ".");
    }

    // The cut takes the file's sequences by their files' IDs. The new items leave the other files' IDs as they were.
    const std::map<uint32_t, uint32_t> before = WiiSequencesByFile(file, bytes);
    sar::KnownSequences known;
    for (const auto& [id, sequence] : before)
    {
        known[id] = *file.sequences[sequence];
    }

    known.insert(added.begin(), added.end());
    format::File imported = format::ImportWiiArchive(built, file.native->wii_build, {}, known);

    // The cut numbers the sequences in the order of their first files in the archive. A new file in a group with a name
    // can come before other files. Each sequence of `file` takes its number again, by its file's ID.
    const std::map<uint32_t, uint32_t> after = WiiSequencesByFile(imported, built);
    const SequenceNumbering numbering = SequenceNumbers(before, after, static_cast<uint32_t>(file.sequences.size()),
                                                        static_cast<uint32_t>(imported.sequences.size()));
    const std::map<uint32_t, uint32_t>& numbers = numbering.numbers;
    format::File result = file;
    result.sequences.assign(imported.sequences.size(), format::Shared<ntdlib::commands::Sequence>());
    for (const auto& [cut, number] : numbers)
    {
        const bool same = number < file.sequences.size() && *imported.sequences[cut] == *file.sequences[number];
        result.sequences.at(number) = same ? file.sequences[number] : imported.sequences[cut];
    }

    // The native archive names the file's sequences by their numbers. It keeps the game's sizes and the streams.
    format::NativeArchive native = *imported.native;
    const auto renumber = [&numbers](std::vector<ntdlib::sar::Piece>& pieces)
    {
        for (ntdlib::sar::Piece& piece : pieces)
        {
            if (auto* sequence = std::get_if<ntdlib::sar::SequencePiece>(&piece))
            {
                sequence->sequence = numbers.at(sequence->sequence);
            }
        }
    };
    native.pieces.Edit(renumber);
    native.game_sizes = file.native->game_sizes;
    native.streams = file.native->streams;
    result.native = std::move(native);
    if (file.form == format::ArchiveForm::kModel)
    {
        result.form = format::ArchiveForm::kNative;
        return format::InModelForm(std::move(result));
    }

    return result;
}

// Checks the name of a new item, `item` in the messages, for the Wii's `archive`. No two items of the archive share a
// name. Throws std::invalid_argument with a message for the user for an empty name, a name with a zero byte and a name
// that the archive holds already.
void CheckWiiName(const rsar::ArchiveModel& archive, const std::string& name, const std::string& item)
{
    if (name.empty())
    {
        throw std::invalid_argument("Enter a name for the new " + item + ".");
    }
    if (name.find('\0') != std::string::npos)
    {
        throw std::invalid_argument("A name can't contain a zero byte.");
    }

    const auto holds = [&name](const auto& items)
    {
        return std::ranges::any_of(items, [&name](const auto& each) { return each.name == name; });
    };
    if (holds(archive.sounds) || holds(archive.banks) || holds(archive.players) || holds(archive.groups))
    {
        throw std::invalid_argument("The archive already has an item named " + name + ".");
    }
}

// `bytes` with zeros up to a multiple of 32 bytes: the size of a file and of its wave data in the Wii's groups.
std::vector<uint8_t> Padded(std::span<const uint8_t> bytes)
{
    std::vector<uint8_t> padded(bytes.begin(), bytes.end());
    padded.resize(ntdlib::AlignUp(padded.size(), 32), 0);

    return padded;
}

// The files that sound `sound` of the Wii's `archive` requires, by their IDs: its file, and a sequence sound's bank's
// file.
std::vector<uint32_t> WiiFilesOfSound(const rsar::ArchiveModel& archive, uint32_t sound)
{
    const rsar::SoundModel& entry = archive.sounds.at(sound);
    std::vector<uint32_t> files{entry.file_id};
    if (entry.type == rsar::SoundType::kSequence && entry.bank < archive.banks.size() &&
        archive.banks[entry.bank].file_id != entry.file_id)
    {
        files.push_back(archive.banks[entry.bank].file_id);
    }

    return files;
}

// `archive` after each group of `groups`, by its index among the groups with names, takes the files of `files` that it
// lacks, after its other files. A group that holds the first of `model_files` loads the model: it takes only the files
// that `model_files` lacks, and the game loads the others from elsewhere. Throws std::invalid_argument with a message
// for the user for a group past the groups with names.
void JoinWiiGroups(rsar::ArchiveModel& archive, std::span<const uint32_t> groups, std::span<const uint32_t> files,
                   std::span<const uint32_t> model_files)
{
    for (const uint32_t g : groups)
    {
        if (g >= archive.groups.size())
        {
            throw std::invalid_argument("The archive has no named group " + std::to_string(g) + ".");
        }

        std::vector<uint32_t>& held = archive.groups[g].file_ids;
        const std::vector<uint32_t> taken = Taken(files, held, model_files);
        held.insert(held.end(), taken.begin(), taken.end());
    }
}

// The version of the first sequence file of the Wii's `archive`, or 1.0.
uint16_t FirstSequenceVersion(const rsar::ArchiveModel& archive)
{
    for (const rsar::FileModel& each : archive.files)
    {
        if (each.bytes.size() >= 8 && std::ranges::equal(std::span(each.bytes).first(4), std::string_view("RSEQ")))
        {
            return xsflib::LoadBe16(&each.bytes[kWiiVersionField]);
        }
    }

    return 0x0100;
}

// A new sound with the values of `copied` that a new wave or stream sound inherits from its model: the player, the
// volume, the player priority, the remote filter, the user parameters, the pan mode and curve, the actor player and the
// 3D settings (AddWiiWaveSounds and AddWiiStreamSound).
rsar::SoundModel CopiedSoundValues(const rsar::SoundModel& copied)
{
    rsar::SoundModel sound;
    sound.player_id = copied.player_id;
    sound.volume = copied.volume;
    sound.player_priority = copied.player_priority;
    sound.remote_filter = copied.remote_filter;
    sound.user_params = copied.user_params;
    sound.pan_mode = copied.pan_mode;
    sound.pan_curve = copied.pan_curve;
    sound.actor_player_id = copied.actor_player_id;
    sound.settings_3d = copied.settings_3d;

    return sound;
}

} // namespace

format::File AddWiiSequenceSound(const format::File& file, std::optional<uint32_t> model, const std::string& name,
                                 const ntdlib::commands::Sequence& incoming, uint32_t start, const ProgramMap& programs,
                                 const std::optional<std::vector<uint32_t>>& banks, std::span<const uint32_t> groups)
{
    auto [archive, bytes] = WiiModelOf(file);
    if (model && (*model >= archive.sounds.size() || archive.sounds[*model].type != rsar::SoundType::kSequence))
    {
        throw std::invalid_argument("The sound to copy isn't a sequence sound.");
    }
    if (start >= incoming.items.size())
    {
        throw std::invalid_argument("The start is past the end of the new sequence.");
    }
    if (banks && banks->size() != 1)
    {
        throw std::invalid_argument("A Wii sound uses only one bank, in slot 0.");
    }
    if (!model && archive.players.empty())
    {
        throw std::invalid_argument("The archive has no sound player for the new sound.");
    }

    CheckWiiName(archive, name, "sound");

    // The bank: the chosen one, the model's, or the archive's first.
    uint32_t bank = model ? archive.sounds[*model].bank : 0;
    if (banks)
    {
        const uint32_t slot = banks->front();
        if (slot == ntdlib::sar::kNoItem || ntdlib::sar::ItemType(slot) != ntdlib::sar::kBankItem)
        {
            throw std::invalid_argument("A Wii sequence sound requires a bank. Choose one for its slot.");
        }

        bank = ntdlib::sar::ItemNumber(slot);
    }
    if (bank >= archive.banks.size())
    {
        throw std::invalid_argument(archive.banks.empty()
                                        ? "The archive has no banks. A Wii sequence sound requires one. Add a "
                                          "bank first."
                                        : "The archive has no bank " + std::to_string(bank) + ".");
    }

    // The new commands with the programs of the map, in a sequence file of the model's version.
    const ntdlib::commands::Sequence mapped = WithPrograms(incoming, start, programs);
    uint16_t version = FirstSequenceVersion(archive);
    if (model)
    {
        const std::vector<uint8_t>& present = archive.files.at(archive.sounds[*model].file_id).bytes;
        version = present.size() >= 8 ? xsflib::LoadBe16(&present[kWiiVersionField]) : version;
    }

    const WiiSequenceFile sequence_file = WiiSequenceFileOf(mapped, *file.native, version);

    // The new file, and the sound that plays it.
    const auto id = static_cast<uint32_t>(archive.files.size());
    archive.files.push_back({.bytes = Padded(sequence_file.file.Write())});
    rsar::SoundModel sound;
    if (model)
    {
        sound = archive.sounds[*model];
    }
    else
    {
        sound.player_id = 0;
    }

    sound.name = name;
    sound.type = rsar::SoundType::kSequence;
    sound.file_id = id;
    sound.start_offset = sequence_file.offsets.at(start);
    sound.bank = bank;
    sound.allocate_track_flags = TracksOpened(mapped, start);
    const std::vector<uint32_t> model_files = model ? WiiFilesOfSound(archive, *model) : std::vector<uint32_t>();
    archive.sounds.push_back(std::move(sound));
    JoinWiiGroups(archive, groups, WiiFilesOfSound(archive, static_cast<uint32_t>(archive.sounds.size() - 1)),
                  model_files);

    return WithWiiModel(file, bytes, archive, {{id, mapped}});
}

format::File AddWiiBank(const format::File& file, const std::string& name, std::span<const uint8_t> bytes,
                        std::span<const uint8_t> wave_data, std::span<const uint32_t> groups)
{
    auto [archive, exported] = WiiModelOf(file);
    CheckWiiName(archive, name, "bank");
    if (const std::string problem = WiiWaveDataProblem(bytes, FileKind::kBank, wave_data); !problem.empty())
    {
        throw std::invalid_argument(problem);
    }

    const auto id = static_cast<uint32_t>(archive.files.size());
    archive.files.push_back({.bytes = Padded(bytes), .wave_data = Padded(wave_data)});
    archive.banks.push_back({.name = name, .file_id = id});
    JoinWiiGroups(archive, groups, std::span(&id, 1), {});

    return WithWiiModel(file, exported, archive);
}

format::File AddWiiWaveSounds(const format::File& file, uint32_t model, const std::string& name,
                              std::span<const uint8_t> bytes, std::span<const uint8_t> wave_data,
                              std::span<const uint32_t> groups)
{
    auto [archive, exported] = WiiModelOf(file);
    if (model >= archive.sounds.size() || (archive.sounds[model].type != rsar::SoundType::kWaveSound &&
                                           archive.sounds[model].type != rsar::SoundType::kSequence))
    {
        throw std::invalid_argument("The sound to copy isn't a wave sound or a sequence sound.");
    }

    std::size_t count = 0;
    try
    {
        count = rsar::WaveSoundFile::Parse(bytes, ntdlib::rvl::snd::kRsarOptions).Sounds().size();
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Can't read the wave sound file. ") + format::AsSentence(error.what()));
    }

    if (const std::string problem = WiiWaveDataProblem(bytes, FileKind::kWaveSoundFile, wave_data); !problem.empty())
    {
        throw std::invalid_argument(problem);
    }
    if (count == 0)
    {
        throw std::invalid_argument("The wave sound file contains no wave sounds.");
    }

    const std::vector<std::string> names = WaveSoundNames(name, count);
    for (const std::string& each : names)
    {
        CheckWiiName(archive, each, "sound");
    }

    // The new file, and a sound for each of its wave sounds. A copy of a sequence sound leaves its details out.
    const auto id = static_cast<uint32_t>(archive.files.size());
    archive.files.push_back({.bytes = Padded(bytes), .wave_data = Padded(wave_data)});
    const std::vector<uint32_t> model_files = WiiFilesOfSound(archive, model);
    const rsar::SoundModel& copied = archive.sounds[model];
    rsar::SoundModel sound = CopiedSoundValues(copied);
    sound.channel_priority = copied.channel_priority;
    sound.release_priority_fix = copied.release_priority_fix;
    sound.allocate_track_count = copied.type == rsar::SoundType::kWaveSound ? copied.allocate_track_count : 1;
    sound.type = rsar::SoundType::kWaveSound;
    sound.file_id = id;
    for (std::size_t i = 0; i < count; ++i)
    {
        sound.name = names[i];
        sound.wave_sound_index = static_cast<int32_t>(i);
        archive.sounds.push_back(sound);
    }

    JoinWiiGroups(archive, groups, std::span(&id, 1), model_files);

    return WithWiiModel(file, exported, archive);
}

namespace
{

// The tracks and the channels that a stream sound of the Wii plays at most (ntdlib::rvl::snd::StrmPlayer).
constexpr std::size_t kMostWiiStreamTracks = 8;
constexpr std::size_t kMostWiiStreamChannels = 16;

// `path` in lower case, with forward slashes.
std::string PlainPath(std::string path)
{
    std::ranges::replace(path, '\\', '/');

    return xsflib::AsciiLower(path);
}

// Whether one of `paths`, the paths of a Wii archive's files beside it, is `path` in any case, with either kind of
// slash.
bool WiiPathTaken(const std::vector<std::optional<std::string>>& paths, const std::string& path)
{
    const std::string wanted = PlainPath(path);
    return std::ranges::any_of(
        paths, [&wanted](const std::optional<std::string>& each) { return each && PlainPath(*each) == wanted; });
}

// The channels and the tracks that the entry of a stream sound of `stream` allocates in a Wii archive of `version`:
// every channel and track of the stream. The tools write them that way. From version 1.4 on, the entry counts the
// channels. Before, it holds a bit for each. A bit for each track gives the tracks.
std::pair<uint16_t, uint16_t> WiiStreamAllocations(uint16_t version, const rsar::StreamFile& stream)
{
    const std::size_t channels = stream.channels.size();
    const auto counted = version >= kWiiChannelCountVersion ? static_cast<uint16_t>(channels)
                                                            : static_cast<uint16_t>((1U << channels) - 1);
    return {counted, static_cast<uint16_t>((1U << stream.tracks.size()) - 1)};
}

// Whether `stream` holds a track for each pair of its channels, the last one alone, at a volume of 127 and the centre:
// the tracks that a stream without tracks takes (ntdlib::rsar::WiiStreamOf and ntdlib::rsar::MakeStream).
bool HoldsPairTracks(const rsar::StreamFile& stream)
{
    const std::size_t channels = stream.channels.size();
    if (stream.tracks.size() != (channels + 1) / 2)
    {
        return false;
    }

    for (std::size_t t = 0; t < stream.tracks.size(); ++t)
    {
        std::vector<uint8_t> pair = {static_cast<uint8_t>(2 * t)};
        if (2 * t + 1 < channels)
        {
            pair.push_back(static_cast<uint8_t>(2 * t + 1));
        }

        const rsar::StreamFile::Track& track = stream.tracks[t];
        if (track.volume != 127 || track.pan != 64 || track.channels != pair)
        {
            return false;
        }
    }

    return true;
}

// The stream of `bytes` for a stream sound of the Wii. Throws std::invalid_argument with a message for the user for a
// stream that ntdlib can't read, a stream without channels, and more tracks or channels than a stream sound plays.
rsar::StreamFile WiiStreamForSound(std::span<const uint8_t> bytes)
{
    rsar::StreamFile stream;
    try
    {
        stream = rsar::StreamFile::Read(bytes);
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Can't read the stream. ") + format::AsSentence(error.what()));
    }

    if (stream.channels.empty() || stream.tracks.empty())
    {
        throw std::invalid_argument("The stream has no channels or no tracks.");
    }
    if (stream.tracks.size() > kMostWiiStreamTracks || stream.channels.size() > kMostWiiStreamChannels)
    {
        throw std::invalid_argument("A Wii stream sound can have at most " + std::to_string(kMostWiiStreamTracks) +
                                    " tracks and " + std::to_string(kMostWiiStreamChannels) +
                                    " channels. The stream has " + std::to_string(stream.channels.size()) +
                                    " channels in " + std::to_string(stream.tracks.size()) + " tracks.");
    }

    return stream;
}

} // namespace

NewStream MakeNewWiiStream(const rsar::SoundArchive& archive, uint32_t model, const std::string& name,
                           const IncomingStream& incoming)
{
    if (model >= archive.Sounds().size())
    {
        throw std::invalid_argument("The sound to copy doesn't exist in the archive.");
    }

    const rsar::StreamFile read = WiiStreamForSound(incoming.bytes);

    // The path: a number after the name while the file table names it.
    std::vector<std::optional<std::string>> paths;
    for (uint32_t file = 0; file < archive.FileCount(); ++file)
    {
        paths.push_back(archive.ExternalPath(file));
    }

    NewStream stream;
    stream.bytes = incoming.bytes;
    const std::string stem = seqlib::SafeFileName(name.empty() ? std::string("STREAM") : name);
    stream.path = "stream/" + stem + ".brstm";
    for (int number = 2; WiiPathTaken(paths, stream.path); ++number)
    {
        stream.path = "stream/" + stem + "_" + std::to_string(number) + ".brstm";
    }

    stream.file_tracks = incoming.wav_rate == 0 && (!incoming.converted || !HoldsPairTracks(read));
    stream.tracks_in_file = true;

    // The new sound's entry. AddWiiStreamSound makes the same entry.
    rsar::SoundInfo& sound = stream.wii_sound.emplace(archive.Sounds()[model]);
    sound.name = name;
    sound.type = rsar::SoundType::kStream;
    sound.file_id = archive.FileCount();
    std::tie(sound.allocate_channels, sound.allocate_track_flags) = WiiStreamAllocations(archive.Version(), read);

    return stream;
}

format::File AddWiiStreamSound(const format::File& file, uint32_t model, const std::string& name,
                               const NewStream& stream)
{
    auto [archive, exported] = WiiModelOf(file);
    if (model >= archive.sounds.size())
    {
        throw std::invalid_argument("The sound to copy doesn't exist in the archive.");
    }

    CheckWiiName(archive, name, "sound");
    std::vector<std::optional<std::string>> paths;
    for (const rsar::FileModel& each : archive.files)
    {
        paths.push_back(each.path);
    }
    if (stream.path.empty() || WiiPathTaken(paths, stream.path))
    {
        throw std::invalid_argument("The archive's file table already contains " + stream.path + ".");
    }

    const rsar::StreamFile read = WiiStreamForSound(stream.bytes);

    // The new file beside the archive, and the sound. Its entry names every channel and track of the stream.
    const auto id = static_cast<uint32_t>(archive.files.size());
    archive.files.push_back({.path = stream.path, .path_size = static_cast<uint32_t>(stream.bytes.size())});
    const rsar::SoundModel& copied = archive.sounds[model];
    rsar::SoundModel sound = CopiedSoundValues(copied);
    sound.name = name;
    sound.type = rsar::SoundType::kStream;
    sound.file_id = id;
    std::tie(sound.allocate_channel_count, sound.stream_track_flags) = WiiStreamAllocations(archive.version, read);
    archive.sounds.push_back(std::move(sound));

    // The file keeps the stream as Hokuto's. The archive's other streams stay.
    const auto keep_stream = [id, &stream](format::NativeArchive& native)
    {
        native.streams[id] = format::NativeStream{stream.bytes, false};
    };
    format::File with_stream = file;
    with_stream.native.Edit(keep_stream);

    return WithWiiModel(with_stream, exported, archive);
}

std::optional<std::string> WiiAdditionsRefused(const format::File& file)
{
    return WiiAdditionsOf(file).refused;
}

std::vector<Group> WiiGroupsOf(const format::File& file)
{
    return WiiAdditionsOf(file).groups;
}

WiiAdditions WiiAdditionsOf(const format::File& file)
{
    std::vector<uint8_t> bytes;
    try
    {
        RequireWii(file);
        bytes = format::ExportArchive(file);
    }
    catch (const std::invalid_argument& error)
    {
        return {.groups = {}, .refused = error.what()};
    }

    return WiiAdditionsOf(file, bytes);
}

WiiAdditions WiiAdditionsOf(const format::File& file, std::span<const uint8_t> archive)
{
    WiiAdditions additions;
    try
    {
        RequireWii(file);
        additions.groups = GroupsOfModel(ModelOfBytes(archive));
    }
    catch (const std::invalid_argument& error)
    {
        additions.refused = error.what();
    }

    return additions;
}

} // namespace hokuto::edit
