// SPDX-License-Identifier: MIT

#include "edit/new_items.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/nwsnd/commands.h"
#include "ntdlib/rsar/archive_model.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/sequence_file.h"
#include "ntdlib/rsar/stream_file.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/info_block.h"
#include "ntdlib/sar/layout.h"
#include "ntdlib/sar/sequence_file.h"
#include "ntdlib/sar/stream_file.h"
#include "ntdlib/sar/string_block.h"
#include "ntdlib/sar/tables.h"
#include "ntdlib/sar/wave_file.h"
#include "ntdlib/sound_build.h"
#include "seqlib/file_name.h"
#include "xsflib/ascii.h"
#include "xsflib/binary.h"

#include "edit/files.h"
#include "format/archive.h"
#include "format/kinds.h"
#include "format/model.h"
#include "song/archive.h"
#include "song/items.h"

namespace hokuto::edit
{
namespace
{

namespace sar = ntdlib::sar;

// A bank entry of the INFO block: its file at 0, then a reference to its list of wave archives. The reference's type
// sits at 4 and its offset from the entry's start at 8. The list holds a count, then the wave archives' item IDs.
constexpr std::size_t kListReference = 4;
constexpr uint16_t kListType = 0x0100;

// The most tracks and channels that the 3DS's player of stream sounds plays (Pokémon X's code.bin, 0x180898).
constexpr std::size_t kCtrMostTracks = 4;
constexpr std::size_t kCtrMostChannels = 8;

// Whether ntdlib lays out the stream sounds of a 3DS archive of `version`: every version from CSAR 2.0.0.0 on but
// 2.1.0.0 and 2.3.1.0.
bool CtrStreamSoundsLaidOut(uint32_t version)
{
    return version == 0x02000000 || version == 0x02020000 || version == 0x02030000 || version >= 0x02030200;
}

// The tables of `file`'s archive. Throws std::invalid_argument with a message for the user for a file without them.
// `item` names the new item in the message, such as "sound".
const format::NativeTables& TablesOf(const format::File& file, const std::string& item)
{
    if (!file.native || !file.native->tables)
    {
        throw std::invalid_argument("Hokuto can't read this archive's tables. A new " + item + " requires them.");
    }

    return *file.native->tables;
}

// Checks the name of a new item, `item` in the messages, for an archive of `flavor` whose STRG block is `strings`.
// Throws std::invalid_argument with a message for the user for a name that the archive holds already, and for a name in
// an archive without names. With `required`, the item requires a name: an empty one and an archive without names throw
// too.
void CheckName(const std::optional<std::span<const uint8_t>>& strings, sar::Flavor flavor, const std::string& name,
               const std::string& item, bool required)
{
    if ((required && name.empty()) || name.find('\0') != std::string::npos)
    {
        throw std::invalid_argument("Give the new " + item + " a name.");
    }
    if (!strings && (required || !name.empty()))
    {
        throw std::invalid_argument(NoNamesReason(item));
    }
    if (!name.empty() && sar::FindItem(*strings, flavor, name))
    {
        throw std::invalid_argument("The archive holds the name " + name + " already.");
    }
}

// Whether a bank entry of `info`, an INFO block of `flavor`, lists wave archives.
bool ListsWaveArchives(std::span<const uint8_t> info, sar::Flavor flavor)
{
    for (const std::vector<uint8_t>& entry : sar::InfoBlock::Read(info, flavor).banks)
    {
        const xsflib::Reader r(entry, flavor == sar::Flavor::kCafe);
        if (entry.size() >= kListReference + 8 && r.U16(kListReference) == kListType)
        {
            const std::size_t list = r.U32(kListReference + 4);
            if (list + 4 <= entry.size() && r.U32(list) > 0)
            {
                return true;
            }
        }
    }

    return false;
}

// The version of the sequence file of sound `sound` of `archive`, an archive of `flavor` that `file` exports. Only the
// group files beside the archive can hold the sequence file. Throws std::invalid_argument with a message for the user
// for a file that ntdlib can't read.
uint32_t SequenceVersion(const format::File& file, const std::vector<uint8_t>& archive, sar::Flavor flavor,
                         uint32_t sound)
{
    sar::Options options;
    options.flavor = flavor;
    options.rules = sar::Rules::kStopAtFirstProblem;
    try
    {
        sar::SoundArchive read = sar::SoundArchive::Load(archive, options);
        const uint32_t id = read.Sounds().at(sound).file_id;
        if (read.FileData(id).empty())
        {
            for (std::vector<uint8_t>& group_file : format::ExportGroupFiles(file))
            {
                read.AddGroupFile(std::move(group_file));
            }
        }

        return sar::SequenceFile::Read(read.FileData(id)).version;
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("The sequence file of the sound to copy can't be read. ") +
                                    format::AsSentence(error.what()));
    }
}

// The bytes of a sequence file of `flavor` with version `version` that holds `sequence`, and the offset of each of its
// items in the bytecode. Throws std::invalid_argument with a message for the user for commands that the console's
// bytecode can't hold.
std::pair<std::vector<uint8_t>, std::vector<uint32_t>> FileOf(const ntdlib::commands::Sequence& sequence,
                                                              sar::Flavor flavor, uint32_t version)
{
    ntdlib::nwsnd::Encoded encoded;
    try
    {
        encoded = ntdlib::nwsnd::EncodeSequence(sequence, song::GrammarOf(flavor));
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("The new commands can't be written for the archive's console. ") +
                                    format::AsSentence(error.what()));
    }

    sar::SequenceFile file;
    file.flavor = flavor;
    file.version = version;
    file.data = std::move(encoded.bytes);
    for (const ntdlib::commands::Label& label : sequence.labels)
    {
        file.labels.push_back({label.name, encoded.offsets.at(label.item)});
    }

    return {file.Write(), std::move(encoded.offsets)};
}

// Options that read a file of `flavor` by the strict rules.
sar::Options StrictOptions(sar::Flavor flavor)
{
    sar::Options options;
    options.flavor = flavor;
    options.rules = sar::Rules::kStopAtFirstProblem;

    return options;
}

// The wave archive of this archive whose tables are `tables` for each one that `named` lists, by their item IDs, from
// `map`, and the chosen wave archives in the order of their first use. `item` names the file in the message, such as
// "bank". Throws std::invalid_argument with a message for the user for a wave archive that the map leaves out, and for
// a place past the archive's list.
std::pair<std::map<uint32_t, uint32_t>, std::vector<uint32_t>> WaveArchiveIds(
    const std::vector<NamedWaveArchive>& named, const std::map<uint32_t, uint32_t>& map,
    const format::NativeTables& tables, const std::string& item)
{
    std::map<uint32_t, uint32_t> ids;
    std::vector<uint32_t> chosen;
    for (const NamedWaveArchive& wave_archive : named)
    {
        const auto found = map.find(wave_archive.id);
        if (found == map.end())
        {
            throw std::invalid_argument("Choose a wave archive for each one that the " + item + " names.");
        }
        if (found->second >= tables.wave_archives.size())
        {
            throw std::invalid_argument("The archive has no wave archive " + std::to_string(found->second) + ".");
        }
        ids[wave_archive.id] = sar::ItemId(sar::kWaveArchiveItem, found->second);
        if (std::ranges::find(chosen, ids[wave_archive.id]) == chosen.end())
        {
            chosen.push_back(ids[wave_archive.id]);
        }
    }

    return {ids, chosen};
}

// Whether the file table of `archive` names `path` outside the archive, in any case, with either kind of slash.
bool PathTaken(const sar::SoundArchive& archive, const std::string& path)
{
    const auto plain = [](std::string text)
    {
        std::ranges::replace(text, '\\', '/');
        return xsflib::AsciiLower(text);
    };
    const std::string wanted = plain(path);
    for (uint32_t file = 0; file < archive.FileCount(); ++file)
    {
        if (!archive.ExternalPath(file).empty() && plain(archive.ExternalPath(file)) == wanted)
        {
            return true;
        }
    }

    return false;
}

// The file that `archive` makes with the group files of `file`, with the new files' sequences of `added`.
format::File RebuiltArchive(const format::File& file, std::span<const uint8_t> archive,
                            const sar::KnownSequences& added = {})
{
    return Rebuilt(file, archive, format::ExportGroupFiles(file), added);
}

// The bank slots of a new sound without a model: one slot, with the archive's first bank or none.
std::vector<uint32_t> DefaultBanks(const format::NativeTables& tables)
{
    return {tables.banks.empty() ? sar::kNoItem : sar::ItemId(sar::kBankItem, 0)};
}

// `file`, a file in the model form, after its archive gains sequence sound `name` with Hokuto's settings. The sound
// plays `sequence` from item `start`, with bank slots `banks`. It allocates the tracks that the commands open. The new
// sequence comes after the file's other sequences, in a new sequence file at the end of the file table, with the
// version that Export… writes for the archive's console. The sound goes at the end of the sound table. It plays on the
// archive's first player, with the settings of an entry that sets nothing: a volume of 127, the centre, priorities of
// 64 and the 3D settings that NintendoWare's tools give. Throws std::invalid_argument with a message for the user for
// an archive without a player, and for an archive that the model can't lay out with the new sound.
format::File WithDefaultSound(const format::File& file, const std::string& name,
                              const ntdlib::commands::Sequence& sequence, uint32_t start,
                              const std::vector<uint32_t>& banks)
{
    format::Model model = format::ModelOf(file);
    sar::ArchiveModel& archive = model.archive;
    if (archive.players.empty())
    {
        throw std::invalid_argument("The archive holds no player for the new sound.");
    }

    format::File base = file;
    base.sequences = format::WithoutChoices(file.sequences);
    base.sequences.push_back(format::WithoutChoices({format::Shared<ntdlib::commands::Sequence>(sequence)}).front());
    const auto id = static_cast<uint32_t>(archive.files.size());
    archive.files.emplace_back();
    model.sequences[id] = {static_cast<uint32_t>(base.sequences.size() - 1),
                           song::ExportVersions(song::ItemKind::kSequence, archive.flavor).file};

    sar::SoundModel sound;
    sound.name = name;
    sound.type = sar::SoundType::kSequence;
    sound.file_id = id;
    sound.player_id = sar::ItemId(sar::kPlayerItem, 0);
    sound.banks = banks;
    sound.allocate_track_flags = TracksOpened(sequence, start);
    model.starts[static_cast<uint32_t>(archive.sounds.size())] = start;
    archive.sounds.push_back(std::move(sound));
    try
    {
        return format::WithModel(std::move(base), model);
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Hokuto can't add a sound to this archive. ") +
                                    format::AsSentence(error.what()));
    }
}

// Whether a group file of `native` holds a sequence file whose sequence no piece of the archive holds.
bool HoldsGroupSequences(const format::NativeArchive& native)
{
    std::set<uint32_t> in_archive;
    for (const sar::Piece& piece : *native.pieces)
    {
        if (const auto* sequence = std::get_if<sar::SequencePiece>(&piece))
        {
            in_archive.insert(sequence->sequence);
        }
    }
    for (const format::NativeGroupFile& group_file : native.group_files)
    {
        for (const sar::Piece& piece : *group_file.pieces)
        {
            const auto* sequence = std::get_if<sar::SequencePiece>(&piece);
            if (sequence && !in_archive.contains(sequence->sequence))
            {
                return true;
            }
        }
    }

    return false;
}

// The sequence of each sequence file of `bytes`, the archive that `file` builds, by the file's ID.
std::map<uint32_t, uint32_t> SequencesByFile(const format::File& file, std::span<const uint8_t> bytes)
{
    sar::Options options;
    options.flavor = file.native->flavor;
    const sar::SoundArchive read = sar::SoundArchive::Load(std::vector<uint8_t>(bytes.begin(), bytes.end()), options);
    const std::vector<std::optional<uint32_t>> sequences = format::SequencesOfFiles(file, bytes, read);
    std::map<uint32_t, uint32_t> by_file;
    for (uint32_t id = 0; id < sequences.size(); ++id)
    {
        if (sequences[id])
        {
            by_file[id] = *sequences[id];
        }
    }

    return by_file;
}

// `imported`, the file of the archive `bytes`, with its sequences numbered as `file` numbers them, by their files' IDs.
// The new sequences follow in the order of the cut. The cut numbers the sequences in the order of their first copies:
// the archive's, then those that only group files hold. A new sequence file in the archive comes before the latter.
// Throws std::invalid_argument with a message for the user when the new archive lacks a sequence of `file`.
format::File InFileOrder(const format::File& file, format::File imported, std::span<const uint8_t> bytes)
{
    const std::map<uint32_t, uint32_t> before = SequencesByFile(file, format::ExportArchive(file));
    const std::map<uint32_t, uint32_t> after = SequencesByFile(imported, bytes);
    std::map<uint32_t, uint32_t> numbers;
    for (const auto& [id, sequence] : before)
    {
        const auto found = after.find(id);
        if (found == after.end() || numbers.emplace(found->second, sequence).first->second != sequence)
        {
            throw std::invalid_argument("Hokuto can't find the file's sequences in the archive with the new items.");
        }
    }
    auto next = static_cast<uint32_t>(file.sequences.size());
    for (uint32_t c = 0; c < imported.sequences.size(); ++c)
    {
        if (numbers.try_emplace(c, next).second)
        {
            ++next;
        }
    }
    if (next != imported.sequences.size())
    {
        throw std::invalid_argument("Hokuto can't find the file's sequences in the archive with the new items.");
    }

    std::vector<format::Shared<ntdlib::commands::Sequence>> sequences(imported.sequences.size());
    for (const auto& [cut, number] : numbers)
    {
        sequences.at(number) = imported.sequences[cut];
    }
    imported.sequences = std::move(sequences);

    // The pieces and the fields name the sequences by their numbers.
    const auto rename_pieces = [&numbers](std::vector<sar::Piece>& pieces)
    {
        for (sar::Piece& piece : pieces)
        {
            if (auto* sequence = std::get_if<sar::SequencePiece>(&piece))
            {
                sequence->sequence = numbers.at(sequence->sequence);
            }
        }
    };

    const auto rename_fields = [&numbers](std::vector<sar::Field>& fields)
    {
        for (sar::Field& field : fields)
        {
            for (sar::Place* place : {&field.from, &field.to})
            {
                place->piece = place->sequence ? numbers.at(place->piece) : place->piece;
            }
        }
    };
    const auto rename = [&](format::NativeArchive& native)
    {
        native.pieces.Edit(rename_pieces);
        rename_fields(native.fields);
        for (format::NativeGroupFile& group_file : native.group_files)
        {
            group_file.pieces.Edit(rename_pieces);
            rename_fields(group_file.fields);
        }
    };
    imported.native.Edit(rename);

    return imported;
}

} // namespace

bool HoldsNames(std::span<const uint8_t> archive, sar::Flavor flavor)
{
    try
    {
        return sar::TablesOf(archive, flavor).strings.has_value();
    }
    catch (const xsflib::FormatError&)
    {
        return true;
    }
}

std::string NoNamesReason(const std::string& item)
{
    return "The archive holds no names. Hokuto can't name a new " + item + " in it.";
}

format::File Rebuilt(const format::File& file, std::span<const uint8_t> archive,
                     std::vector<std::vector<uint8_t>> group_files, const sar::KnownSequences& added)
{
    // The new items leave the other files' IDs as they were. Without the file's sequences by their files' IDs, such as
    // in an archive that ntdlib can't read, the cut decodes every sequence file.
    sar::KnownSequences known;
    try
    {
        for (const auto& [id, sequence] : SequencesByFile(file, format::ExportArchive(file)))
        {
            known[id] = *file.sequences[sequence];
        }
    }
    catch (const std::exception&)
    {
        known.clear();
    }
    known.insert(added.begin(), added.end());
    format::File imported = format::ImportArchive(archive, file.native->flavor, std::move(group_files), {}, {}, known);
    if (HoldsGroupSequences(*file.native))
    {
        imported = InFileOrder(file, std::move(imported), archive);
    }
    format::File result = file;
    result.sequences = std::move(imported.sequences);
    result.native = std::move(imported.native);

    // Without tables, the game's sizes hold the archive's size alone. A new group has a size of 0 among them: the game
    // has no such group. The streams keep their file IDs: a new file goes after the others. Each group file stays the
    // game's, by its size and its SHA-1, and keeps its group.
    const auto keep_parts = [&file](format::NativeArchive& native)
    {
        native.game_sizes = file.native->game_sizes;
        if (native.game_sizes && !native.tables)
        {
            native.game_sizes->groups.clear();
        }
        else if (native.game_sizes && native.game_sizes->groups.size() < native.tables->groups.size())
        {
            native.game_sizes->groups.resize(native.tables->groups.size(), 0);
        }
        native.streams = file.native->streams;
        for (std::size_t g = 0; g < native.group_files.size() && g < file.native->group_files.size(); ++g)
        {
            const format::NativeGroupFile& old = file.native->group_files[g];
            format::NativeGroupFile& group_file = native.group_files[g];
            group_file.game_size = old.game_size;
            group_file.game_hash = old.game_hash;
            group_file.group = old.group ? old.group : group_file.group;
        }
    };
    result.native.Edit(keep_parts);

    for (std::size_t i = 0; i < std::min(file.sequences.size(), result.sequences.size()); ++i)
    {
        if (result.sequences[i] == file.sequences[i])
        {
            result.sequences[i] = file.sequences[i];
        }
    }

    return result;
}

format::File AddSequenceSound(const format::File& file, std::optional<uint32_t> model, const std::string& name,
                              const ntdlib::commands::Sequence& incoming, uint32_t start, const ProgramMap& programs,
                              const std::optional<std::vector<uint32_t>>& banks)
{
    const format::NativeTables& tables = TablesOf(file, "sound");
    if (!model && file.form != format::ArchiveForm::kModel)
    {
        throw std::invalid_argument("The archive holds no sequence sound for the new one to copy.");
    }
    if (model && (*model >= tables.sounds.size() || tables.sounds[*model].kind != sar::SoundType::kSequence))
    {
        throw std::invalid_argument("The sound to copy isn't a sequence sound.");
    }
    if (start >= incoming.items.size())
    {
        throw std::invalid_argument("The start lies past the end of the new sequence.");
    }
    const std::vector<uint32_t> slots = banks ? *banks : model ? tables.sounds[*model].banks : DefaultBanks(tables);
    CheckBankSlots(tables, slots);

    // The archive that the file exports. Its names mustn't hold the new name.
    const sar::Flavor flavor = file.native->flavor;
    const std::vector<uint8_t> archive = format::ExportArchive(file);
    CheckName(sar::TablesOf(archive, flavor).strings, flavor, name, "sound", true);

    // The new commands with the programs of the map. Without a model, the model of the file's archive takes the new
    // sound.
    const ntdlib::commands::Sequence mapped = WithPrograms(incoming, start, programs);
    if (!model)
    {
        return WithDefaultSound(file, name, mapped, start, slots);
    }

    // The new commands in a file of the archive's console with the model's version, and the archive with the new file
    // and the new sound, and the file that it makes.
    const auto [bytes, offsets] = FileOf(mapped, flavor, SequenceVersion(file, archive, flavor, *model));
    format::File result;
    try
    {
        const sar::ArchiveWithFile with = sar::WithNewFile(archive, flavor, bytes);
        const sar::NewSequenceSound sound{
            .model = *model, .name = name, .file = with.file, .start = offsets.at(start), .banks = slots};
        result = RebuiltArchive(file, sar::WithNewSequenceSound(with.archive, flavor, sound), {{with.file, mapped}});
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Hokuto can't add a sound to this archive. ") +
                                    format::AsSentence(error.what()));
    }
    if (!result.native->tables || result.native->tables->sounds.size() != tables.sounds.size() + 1)
    {
        throw std::invalid_argument("Hokuto can't read the tables of the archive with the new sound.");
    }

    // The new sound allocates the tracks that its commands open.
    const uint32_t tracks = TracksOpened(mapped, start);
    result.native.Edit([tracks](format::NativeArchive& native) { native.tables->sounds.back().tracks = tracks; });

    return result;
}

std::size_t NewSequenceFileSize(const format::File& file, const ntdlib::commands::Sequence& sequence)
{
    if (!file.native)
    {
        throw std::invalid_argument("The file holds no archive of a game.");
    }

    if (!file.native->wii)
    {
        const sar::Flavor flavor = file.native->flavor;
        return FileOf(sequence, flavor, song::ExportVersions(song::ItemKind::kSequence, flavor).file).first.size();
    }

    ntdlib::nwsnd::Encoded encoded;
    try
    {
        const ntdlib::SndBuild build = format::BuildOfWiiArchive(*file.native);
        encoded = ntdlib::nwsnd::EncodeSequence(sequence, song::GrammarOf(song::Console{.wii = build}));
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("The new commands can't be written for the Wii. ") +
                                    format::AsSentence(error.what()));
    }
    ntdlib::rsar::SequenceFile sequence_file;
    sequence_file.version = 0x0100;
    sequence_file.data = std::move(encoded.bytes);
    for (const ntdlib::commands::Label& label : sequence.labels)
    {
        sequence_file.labels.push_back({label.name, encoded.offsets.at(label.item)});
    }

    return (sequence_file.Write().size() + 31) / 32 * 32;
}

std::vector<NamedWaveArchive> WaveArchivesNamed(std::span<const uint8_t> file, sar::Flavor flavor)
{
    const bool wave_sounds = file.size() >= 4 && std::ranges::equal(file.subspan(1, 3), std::string_view("WSD"));
    std::vector<sar::WaveId> waves;
    try
    {
        waves = wave_sounds ? sar::WaveSoundFile::Parse(file, StrictOptions(flavor)).waves
                            : sar::Bank::Parse(file, StrictOptions(flavor)).waves;
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(
            std::string(wave_sounds ? "Hokuto can't read the wave sound file. " : "Hokuto can't read the bank. ") +
            format::AsSentence(error.what()));
    }

    // The wave archives in the order of their first use, then the waves of each in order.
    std::vector<NamedWaveArchive> named;
    for (const sar::WaveId& wave : waves)
    {
        auto found = std::ranges::find(named, wave.wave_archive_item, &NamedWaveArchive::id);
        if (found == named.end())
        {
            found = named.insert(named.end(), {wave.wave_archive_item, {}});
        }
        found->waves.push_back(wave.index);
    }
    for (NamedWaveArchive& entry : named)
    {
        std::ranges::sort(entry.waves);
        entry.waves.erase(std::ranges::unique(entry.waves).begin(), entry.waves.end());
    }

    return named;
}

std::vector<uint8_t> WithWaveArchiveMap(const format::File& file, std::span<const uint8_t> bytes,
                                        const std::map<uint32_t, uint32_t>& map)
{
    const bool wave_sounds = bytes.size() >= 4 && std::ranges::equal(bytes.subspan(1, 3), std::string_view("WSD"));
    const std::string item = wave_sounds ? "wave sound file" : "bank";
    if (!file.native || !file.native->tables)
    {
        throw std::invalid_argument("Hokuto can't read this archive's tables. The map of the " + item +
                                    "'s wave archives requires them.");
    }

    const sar::Flavor flavor = file.native->flavor;
    const std::map<uint32_t, uint32_t> ids =
        WaveArchiveIds(WaveArchivesNamed(bytes, flavor), map, *file.native->tables, item).first;
    try
    {
        return sar::WithWaveArchives(bytes, flavor, ids);
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument("Hokuto can't give the " + item + " other wave archives. " +
                                    format::AsSentence(error.what()));
    }
}

format::File AddWaveArchive(const format::File& file, const std::string& name, std::span<const uint8_t> bytes)
{
    const format::NativeTables& tables = TablesOf(file, "wave archive");
    const sar::Flavor flavor = file.native->flavor;
    const IncomingFile incoming = ReadIncomingFile(bytes, FileKind::kWaveArchive, flavor);
    const std::vector<uint8_t> archive = format::ExportArchive(file);
    CheckName(sar::TablesOf(archive, flavor).strings, flavor, name, "wave archive", false);

    // The archive with the new file and the new wave archive, and the file that it makes.
    format::File result;
    try
    {
        const sar::ArchiveWithFile with = sar::WithNewFile(archive, flavor, incoming.bytes);
        result = RebuiltArchive(file, sar::WithNewWaveArchive(with.archive, flavor, {.name = name, .file = with.file}));
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Hokuto can't add a wave archive to this archive. ") +
                                    format::AsSentence(error.what()));
    }
    if (!result.native->tables || result.native->tables->wave_archives.size() != tables.wave_archives.size() + 1)
    {
        throw std::invalid_argument("Hokuto can't read the tables of the archive with the new wave archive.");
    }

    return result;
}

format::File AddGroups(const format::File& file, std::span<const std::string> names)
{
    const format::NativeTables& tables = TablesOf(file, "group");
    if (names.empty())
    {
        return file;
    }

    // The names first. Each has to be new in the archive and among the new groups.
    const sar::Flavor flavor = file.native->flavor;
    std::vector<uint8_t> archive = format::ExportArchive(file);
    const std::optional<std::span<const uint8_t>> strings = sar::TablesOf(archive, flavor).strings;
    for (std::size_t k = 0; k < names.size(); k++)
    {
        CheckName(strings, flavor, names[k], "group", true);
        if (std::ranges::find(names.first(k), names[k]) != names.first(k).end())
        {
            throw std::invalid_argument("Two new groups have the name " + names[k] + ".");
        }
    }

    // The archive with the new groups, and the file that it makes.
    format::File result;
    try
    {
        for (const std::string& name : names)
        {
            archive = sar::WithNewGroup(archive, flavor, name);
        }
        result = RebuiltArchive(file, archive);
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Hokuto can't add a group to this archive. ") +
                                    format::AsSentence(error.what()));
    }
    if (!result.native->tables || result.native->tables->groups.size() != tables.groups.size() + names.size())
    {
        throw std::invalid_argument("Hokuto can't read the tables of the archive with the new groups.");
    }

    return result;
}

format::File AddBank(const format::File& file, const std::string& name, std::span<const uint8_t> bytes,
                     const std::map<uint32_t, uint32_t>& map)
{
    const format::NativeTables& tables = TablesOf(file, "bank");
    const sar::Flavor flavor = file.native->flavor;
    const IncomingFile incoming = ReadIncomingFile(bytes, FileKind::kBank, flavor);

    // The wave archive of this archive for each one that the bank names, by their item IDs, and the list in their
    // order.
    const auto [ids, chosen] = WaveArchiveIds(WaveArchivesNamed(incoming.bytes, flavor), map, tables, "bank");
    std::vector<uint32_t> listed = chosen;

    // The archive that the file exports. Its names mustn't hold the new name.
    const std::vector<uint8_t> archive = format::ExportArchive(file);
    const sar::Tables blocks = sar::TablesOf(archive, flavor);
    CheckName(blocks.strings, flavor, name, "bank", true);

    // The archive with the new file and the new bank, and the file that it makes. The entry lists the wave archives
    // where another bank's entry lists wave archives.
    format::File result;
    try
    {
        if (!ListsWaveArchives(blocks.info, flavor))
        {
            listed.clear();
        }
        const sar::ArchiveWithFile with =
            sar::WithNewFile(archive, flavor, sar::WithWaveArchives(incoming.bytes, flavor, ids));
        const sar::NewBank bank{.name = name, .file = with.file, .wave_archives = listed};
        result = RebuiltArchive(file, sar::WithNewBank(with.archive, flavor, bank));
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Hokuto can't add a bank to this archive. ") +
                                    format::AsSentence(error.what()));
    }
    if (!result.native->tables || result.native->tables->banks.size() != tables.banks.size() + 1)
    {
        throw std::invalid_argument("Hokuto can't read the tables of the archive with the new bank.");
    }

    return result;
}

std::vector<std::string> WaveSoundNames(const std::string& name, std::size_t count)
{
    if (count == 1)
    {
        return {name};
    }

    const std::size_t digits = std::to_string(count).size();
    std::vector<std::string> names;
    for (std::size_t k = 0; k < count; ++k)
    {
        const std::string index = std::to_string(k);
        names.push_back(name + "_" + std::string(digits - index.size(), '0') + index);
    }

    return names;
}

std::size_t WaveSoundCount(std::span<const uint8_t> bytes, sar::Flavor flavor)
{
    try
    {
        return sar::WaveSoundFile::Parse(bytes, StrictOptions(flavor)).sounds.size();
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Hokuto can't read the wave sound file. ") +
                                    format::AsSentence(error.what()));
    }
}

format::File AddWaveSounds(const format::File& file, uint32_t model, const std::string& name,
                           std::span<const uint8_t> bytes, const std::map<uint32_t, uint32_t>& map)
{
    const format::NativeTables& tables = TablesOf(file, "sound");
    const sar::Flavor flavor = file.native->flavor;
    if (model >= tables.sounds.size() || (tables.sounds[model].kind != sar::SoundType::kWaveSound &&
                                          tables.sounds[model].kind != sar::SoundType::kSequence))
    {
        throw std::invalid_argument("The sound to copy isn't a wave sound or a sequence sound.");
    }
    const IncomingFile incoming = ReadIncomingFile(bytes, FileKind::kWaveSoundFile, flavor);
    const std::size_t count = WaveSoundCount(incoming.bytes, flavor);
    if (count == 0)
    {
        throw std::invalid_argument("The wave sound file holds no wave sound.");
    }

    // The wave archive of this archive for each one that the file names, by their item IDs.
    const std::map<uint32_t, uint32_t> ids =
        WaveArchiveIds(WaveArchivesNamed(incoming.bytes, flavor), map, tables, "wave sound file").first;

    // The archive that the file exports. Its names mustn't hold the new names.
    const std::vector<uint8_t> archive = format::ExportArchive(file);
    const sar::Tables blocks = sar::TablesOf(archive, flavor);
    if (name.empty())
    {
        throw std::invalid_argument(count == 1 ? "Give the new sound a name." : "Give the new sounds a name.");
    }
    const std::vector<std::string> names = WaveSoundNames(name, count);
    for (const std::string& each : names)
    {
        CheckName(blocks.strings, flavor, each, "sound", true);
    }

    // The archive with the new file and the new sounds, and the file that it makes.
    format::File result;
    try
    {
        const sar::ArchiveWithFile with =
            sar::WithNewFile(archive, flavor, sar::WithWaveArchives(incoming.bytes, flavor, ids));
        std::vector<sar::NewWaveSound> sounds;
        for (std::size_t k = 0; k < count; ++k)
        {
            sounds.push_back({.model = model, .name = names[k], .file = with.file, .index = static_cast<uint32_t>(k)});
        }
        result = RebuiltArchive(file, sar::WithNewWaveSounds(with.archive, flavor, sounds));
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Hokuto can't add wave sounds to this archive. ") +
                                    format::AsSentence(error.what()));
    }
    if (!result.native->tables || result.native->tables->sounds.size() != tables.sounds.size() + count)
    {
        throw std::invalid_argument("Hokuto can't read the tables of the archive with the new sounds.");
    }

    return result;
}

NewStream MakeNewStream(const sar::SoundArchive& archive, uint32_t model, const std::string& name,
                        const IncomingStream& incoming)
{
    if (model >= archive.Sounds().size())
    {
        throw std::invalid_argument("The sound to copy lies past the archive's sounds.");
    }

    const bool ctr = archive.ReadOptions().flavor == sar::Flavor::kCtr;
    if (ctr && !CtrStreamSoundsLaidOut(archive.Version()))
    {
        throw std::invalid_argument("Hokuto can't add a stream sound to a 3DS archive of version " +
                                    song::VersionText(sar::Flavor::kCtr, archive.Version()) +
                                    ". The layout of such an archive's stream sounds is unknown.");
    }
    sar::StreamFile file;
    try
    {
        file = sar::StreamFile::Read(incoming.bytes);
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Hokuto can't read the stream. ") + format::AsSentence(error.what()));
    }
    if (file.channels.empty())
    {
        throw std::invalid_argument("The stream holds no channel.");
    }

    // The path: a number after the name while the file table names it.
    NewStream stream;
    stream.bytes = incoming.bytes;
    const std::string stem = seqlib::SafeFileName(name.empty() ? std::string("STREAM") : name);
    const std::string extension =
        std::string(file.encoding == sar::WaveEncoding::kDspAdpcm ? ".dspadpcm" : "") + (ctr ? ".bcstm" : ".bfstm");
    stream.path = "stream/" + stem + extension;
    for (int number = 2; PathTaken(archive, stream.path); ++number)
    {
        stream.path = "stream/" + stem + "_" + std::to_string(number) + extension;
    }

    // The tracks: the file's, or a track for each pair of channels.
    std::vector<sar::StreamFile::Track> tracks;
    if (file.tracks && !file.tracks->empty())
    {
        tracks = *file.tracks;
        stream.file_tracks = true;
    }
    else
    {
        for (std::size_t c = 0; c < file.channels.size(); c += 2)
        {
            sar::StreamFile::Track& track = tracks.emplace_back();
            track.channels.push_back(static_cast<uint8_t>(c));
            if (c + 1 < file.channels.size())
            {
                track.channels.push_back(static_cast<uint8_t>(c + 1));
            }
        }
    }
    const std::string held =
        std::to_string(file.channels.size()) + " channels" +
        (file.tracks ? " in " + std::to_string(tracks.size()) + (tracks.size() == 1 ? " track." : " tracks.") : ".");
    if (ctr && tracks.size() > kCtrMostTracks)
    {
        throw std::invalid_argument("A stream sound of the 3DS plays 4 tracks at most. The stream holds " + held);
    }
    if (tracks.size() > 8 || file.channels.size() > 255)
    {
        throw std::invalid_argument("A stream sound plays 8 tracks at most. The stream holds " + held);
    }
    for (std::size_t t = 0; ctr && t < tracks.size(); ++t)
    {
        if (tracks[t].channels.size() > 2)
        {
            throw std::invalid_argument("A track of a stream sound of the 3DS plays 2 channels at most. Track " +
                                        std::to_string(t) + " of the stream plays " +
                                        std::to_string(tracks[t].channels.size()) + ".");
        }
    }
    if (ctr && file.channels.size() > kCtrMostChannels)
    {
        throw std::invalid_argument("A stream sound of the 3DS plays 8 channels at most. The stream holds " + held);
    }

    // Where the sound plays the tracks of its stream file, the details give only the tracks that play and the count of
    // channels. A stream file without tracks takes those of the pairs of its channels.
    sar::StreamSoundInfo& details = stream.details;
    details.channel_count = static_cast<uint16_t>(file.channels.size());
    details.valid_tracks = static_cast<uint16_t>((1u << tracks.size()) - 1);
    if (TracksInFiles(archive))
    {
        stream.tracks_in_file = true;
        if (!stream.file_tracks)
        {
            file.tracks = tracks;
            try
            {
                stream.bytes = file.Write();
            }
            catch (const std::exception& error)
            {
                throw std::invalid_argument(std::string("Hokuto can't give the stream its tracks. ") +
                                            format::AsSentence(error.what()));
            }
        }

        return stream;
    }

    // Each track's settings: the model's track in its place, and the file's volume, pan, span and flags.
    const sar::SoundInfo& copied = archive.Sounds()[model];
    const bool stream_model = copied.type == sar::SoundType::kStream;
    if (stream_model)
    {
        details.pitch = copied.stream.pitch;
        details.main_send = copied.stream.main_send;
        details.aux_sends = copied.stream.aux_sends;
    }
    for (std::size_t t = 0; t < tracks.size(); ++t)
    {
        sar::StreamTrackInfo track;
        if (stream_model && t < copied.stream.tracks.size())
        {
            track = copied.stream.tracks[t];
        }
        if (stream.file_tracks)
        {
            track.volume = tracks[t].volume;
            track.pan = tracks[t].pan;
            track.span = tracks[t].span;
            track.flags = tracks[t].flags;
        }
        track.channels = tracks[t].channels;
        details.tracks.push_back(std::move(track));
    }

    return stream;
}

format::File AddStreamSound(const format::File& file, uint32_t model, const std::string& name, const NewStream& stream)
{
    const format::NativeTables& tables = TablesOf(file, "sound");
    const sar::Flavor flavor = file.native->flavor;
    if (model >= tables.sounds.size())
    {
        throw std::invalid_argument("The sound to copy lies past the archive's sounds.");
    }

    // The archive that the file exports. Its names mustn't hold the new name, and its file table mustn't name the path.
    const std::vector<uint8_t> archive = format::ExportArchive(file);
    const sar::Tables blocks = sar::TablesOf(archive, flavor);
    CheckName(blocks.strings, flavor, name, "sound", true);
    if (PathTaken(sar::SoundArchive::Load(archive, StrictOptions(flavor)), stream.path))
    {
        throw std::invalid_argument("The archive's file table names " + stream.path + " already.");
    }

    // The archive with the new file and the new sound, and the file that it makes. The file keeps the stream.
    format::File result;
    try
    {
        const sar::ArchiveWithFile with = sar::WithNewExternalFile(archive, flavor, stream.path);
        const sar::NewStreamSound sound{.model = model, .name = name, .file = with.file, .stream = stream.details};
        result = RebuiltArchive(file, sar::WithNewStreamSound(with.archive, flavor, sound));
        const auto keep_stream = [&stream, id = with.file](format::NativeArchive& native)
        {
            native.streams[id] = format::NativeStream{stream.bytes, false};
        };
        result.native.Edit(keep_stream);
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Hokuto can't add a stream sound to this archive. ") +
                                    format::AsSentence(error.what()));
    }
    if (!result.native->tables || result.native->tables->sounds.size() != tables.sounds.size() + 1)
    {
        throw std::invalid_argument("Hokuto can't read the tables of the archive with the new sound.");
    }

    return result;
}

namespace
{

namespace rsar = ntdlib::rsar;

// The sequence of each sequence file of `bytes`, the archive of the Wii that `file` builds, by the file's ID.
std::map<uint32_t, uint32_t> WiiSequencesByFile(const format::File& file, std::span<const uint8_t> bytes)
{
    const rsar::SoundArchive read =
        rsar::SoundArchive::Load(std::vector<uint8_t>(bytes.begin(), bytes.end()), ntdlib::rvl::snd::kRsarOptions);
    const std::vector<std::optional<uint32_t>> sequences = format::SequencesOfFiles(file, bytes, read);
    std::map<uint32_t, uint32_t> by_file;
    for (uint32_t id = 0; id < sequences.size(); ++id)
    {
        if (sequences[id])
        {
            by_file[id] = *sequences[id];
        }
    }

    return by_file;
}

// The model of the archive of `file`, a file of the Wii, and the archive's bytes. Throws std::invalid_argument with a
// message for the user for a file of another console, and for an archive that NintendoWare for Revolution's tools
// didn't lay out.
std::pair<rsar::ArchiveModel, std::vector<uint8_t>> WiiModelOf(const format::File& file)
{
    if (!file.native || !file.native->wii)
    {
        throw std::invalid_argument("The file holds no archive of the Wii.");
    }

    std::vector<uint8_t> bytes = format::ExportArchive(file);
    try
    {
        return {rsar::ReadArchiveModel(bytes), std::move(bytes)};
    }
    catch (const xsflib::FormatError& error)
    {
        throw std::invalid_argument(std::string("NintendoWare for Revolution's tools didn't lay this archive out as "
                                                "Hokuto reads it, and Hokuto adds items only to such an archive. ") +
                                    format::AsSentence(error.what()));
    }
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
        throw std::invalid_argument(std::string("Hokuto can't lay the archive out with the new items: ") +
                                    error.what() + ".");
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
    std::map<uint32_t, uint32_t> numbers;
    for (const auto& [id, sequence] : before)
    {
        const auto found = after.find(id);
        if (found == after.end() || numbers.emplace(found->second, sequence).first->second != sequence)
        {
            throw std::invalid_argument("Hokuto can't find the file's sequences in the archive with the new items.");
        }
    }
    auto next = static_cast<uint32_t>(file.sequences.size());
    for (uint32_t c = 0; c < imported.sequences.size(); ++c)
    {
        if (numbers.try_emplace(c, next).second)
        {
            ++next;
        }
    }
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
        throw std::invalid_argument("Give the new " + item + " a name.");
    }
    if (name.find('\0') != std::string::npos)
    {
        throw std::invalid_argument("A name can't hold a zero byte.");
    }

    const auto holds = [&name](const auto& items)
    {
        return std::ranges::any_of(items, [&name](const auto& each) { return each.name == name; });
    };
    if (holds(archive.sounds) || holds(archive.banks) || holds(archive.players) || holds(archive.groups))
    {
        throw std::invalid_argument("The archive holds the name " + name + " already.");
    }
}

// `bytes` with zeros up to a multiple of 32 bytes: the size of a file and of its wave data in the Wii's groups.
std::vector<uint8_t> Padded(std::span<const uint8_t> bytes)
{
    std::vector<uint8_t> padded(bytes.begin(), bytes.end());
    padded.resize((padded.size() + 31) / 32 * 32, 0);

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
            throw std::invalid_argument("The archive holds no group " + std::to_string(g) + " with a name.");
        }

        std::vector<uint32_t>& held = archive.groups[g].file_ids;
        const bool loads_model = !model_files.empty() && std::ranges::find(held, model_files.front()) != held.end();
        for (const uint32_t id : files)
        {
            const bool required = !loads_model || std::ranges::find(model_files, id) == model_files.end();
            if (required && std::ranges::find(held, id) == held.end())
            {
                held.push_back(id);
            }
        }
    }
}

// The version of the first sequence file of the Wii's `archive`, or 1.0.
uint16_t FirstSequenceVersion(const rsar::ArchiveModel& archive)
{
    for (const rsar::FileModel& each : archive.files)
    {
        if (each.bytes.size() >= 8 && std::ranges::equal(std::span(each.bytes).first(4), std::string_view("RSEQ")))
        {
            return xsflib::LoadBe16(&each.bytes[6]);
        }
    }

    return 0x0100;
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
        throw std::invalid_argument("The start lies past the end of the new sequence.");
    }
    if (banks && banks->size() != 1)
    {
        throw std::invalid_argument("A sound of the Wii plays one bank, in slot 0.");
    }
    if (!model && archive.players.empty())
    {
        throw std::invalid_argument("The archive holds no player for the new sound.");
    }
    CheckWiiName(archive, name, "sound");

    // The bank: the chosen one, the model's, or the archive's first.
    uint32_t bank = model ? archive.sounds[*model].bank : 0;
    if (banks)
    {
        const uint32_t slot = banks->front();
        if (slot == ntdlib::sar::kNoItem || ntdlib::sar::ItemType(slot) != ntdlib::sar::kBankItem)
        {
            throw std::invalid_argument("A sequence sound of the Wii plays a bank. Choose one for its slot.");
        }
        bank = ntdlib::sar::ItemNumber(slot);
    }
    if (bank >= archive.banks.size())
    {
        throw std::invalid_argument(archive.banks.empty()
                                        ? "The archive holds no bank. A sequence sound of the Wii plays one: add a "
                                          "bank first."
                                        : "The archive holds no bank " + std::to_string(bank) + ".");
    }

    // The new commands with the programs of the map, in a sequence file of the model's version.
    const ntdlib::commands::Sequence mapped = WithPrograms(incoming, start, programs);
    const ntdlib::SndBuild build = format::BuildOfWiiArchive(*file.native);
    ntdlib::nwsnd::Encoded encoded;
    try
    {
        encoded = ntdlib::nwsnd::EncodeSequence(mapped, song::GrammarOf(song::Console{.wii = build}));
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("The new commands can't be written for the Wii. ") +
                                    format::AsSentence(error.what()));
    }
    rsar::SequenceFile sequence_file;
    sequence_file.version = FirstSequenceVersion(archive);
    if (model)
    {
        const std::vector<uint8_t>& present = archive.files.at(archive.sounds[*model].file_id).bytes;
        sequence_file.version = present.size() >= 8 ? xsflib::LoadBe16(&present[6]) : sequence_file.version;
    }
    sequence_file.data = std::move(encoded.bytes);
    for (const ntdlib::commands::Label& label : mapped.labels)
    {
        sequence_file.labels.push_back({label.name, encoded.offsets.at(label.item)});
    }

    // The new file, and the sound that plays it.
    const auto id = static_cast<uint32_t>(archive.files.size());
    archive.files.push_back({.bytes = Padded(sequence_file.Write())});
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
    sound.start_offset = encoded.offsets.at(start);
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
        throw std::invalid_argument(std::string("Hokuto can't read the wave sound file. ") +
                                    format::AsSentence(error.what()));
    }
    if (const std::string problem = WiiWaveDataProblem(bytes, FileKind::kWaveSoundFile, wave_data); !problem.empty())
    {
        throw std::invalid_argument(problem);
    }
    if (count == 0)
    {
        throw std::invalid_argument("The wave sound file holds no wave sound.");
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

// The version of a Wii archive from which a stream sound's entry counts its channels. Before, it holds a bit for each.
constexpr uint16_t kWiiChannelCountVersion = 0x0104;

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
        throw std::invalid_argument(std::string("Hokuto can't read the stream. ") + format::AsSentence(error.what()));
    }
    if (stream.channels.empty() || stream.tracks.empty())
    {
        throw std::invalid_argument("The stream holds no channel or no track.");
    }
    if (stream.tracks.size() > kMostWiiStreamTracks || stream.channels.size() > kMostWiiStreamChannels)
    {
        throw std::invalid_argument("A stream sound of the Wii plays " + std::to_string(kMostWiiStreamTracks) +
                                    " tracks and " + std::to_string(kMostWiiStreamChannels) +
                                    " channels at most. The stream holds " + std::to_string(stream.channels.size()) +
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
        throw std::invalid_argument("The sound to copy lies past the archive's sounds.");
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
        throw std::invalid_argument("The sound to copy lies past the archive's sounds.");
    }
    CheckWiiName(archive, name, "sound");
    std::vector<std::optional<std::string>> paths;
    for (const rsar::FileModel& each : archive.files)
    {
        paths.push_back(each.path);
    }
    if (stream.path.empty() || WiiPathTaken(paths, stream.path))
    {
        throw std::invalid_argument("The archive's file table names " + stream.path + " already.");
    }
    const rsar::StreamFile read = WiiStreamForSound(stream.bytes);

    // The new file beside the archive, and the sound. Its entry names every channel and track of the stream.
    const auto id = static_cast<uint32_t>(archive.files.size());
    archive.files.push_back({.path = stream.path, .path_size = static_cast<uint32_t>(stream.bytes.size())});
    const rsar::SoundModel& copied = archive.sounds[model];
    rsar::SoundModel sound;
    sound.name = name;
    sound.type = rsar::SoundType::kStream;
    sound.file_id = id;
    sound.player_id = copied.player_id;
    sound.volume = copied.volume;
    sound.player_priority = copied.player_priority;
    sound.remote_filter = copied.remote_filter;
    sound.user_params = copied.user_params;
    sound.pan_mode = copied.pan_mode;
    sound.pan_curve = copied.pan_curve;
    sound.actor_player_id = copied.actor_player_id;
    sound.settings_3d = copied.settings_3d;
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
    try
    {
        WiiModelOf(file);
    }
    catch (const std::invalid_argument& error)
    {
        return error.what();
    }

    return std::nullopt;
}

std::vector<Group> WiiGroupsOf(const format::File& file)
{
    std::vector<Group> groups;
    try
    {
        const rsar::ArchiveModel archive = WiiModelOf(file).first;
        for (uint32_t g = 0; g < archive.groups.size(); ++g)
        {
            groups.push_back({.group = g, .name = archive.groups[g].name, .files = archive.groups[g].file_ids});
        }
    }
    catch (const std::invalid_argument&)
    {
        groups.clear();
    }

    return groups;
}

} // namespace hokuto::edit
