// SPDX-License-Identifier: MIT

// New items in the archives of the 3DS, the Wii U and the Switch (edit/new_items.h). The Wii's are in
// wii_new_items.cpp.

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
#include <utility>
#include <variant>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/nwsnd/commands.h"
#include "ntdlib/rounding.h"
#include "ntdlib/rsar/sequence_file.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/info_block.h"
#include "ntdlib/sar/layout.h"
#include "ntdlib/sar/sequence_file.h"
#include "ntdlib/sar/string_block.h"
#include "ntdlib/sar/tables.h"
#include "ntdlib/sar/wave_file.h"
#include "ntdlib/sound_build.h"
#include "xsflib/binary.h"

#include "edit/files.h"
#include "edit/files_internal.h"
#include "edit/new_items_parts.h"
#include "format/archive.h"
#include "format/kinds.h"
#include "format/model.h"
#include "song/archive.h"
#include "song/items.h"

namespace hokuto::edit
{

namespace new_items_parts
{

const format::NativeTables& TablesOf(const format::File& file, const std::string& item)
{
    if (!file.native || !file.native->tables)
    {
        throw std::invalid_argument("Can't read the archive tables. Adding a " + item + " requires them.");
    }

    return *file.native->tables;
}

void CheckName(const std::optional<std::span<const uint8_t>>& strings, sar::Flavor flavor, const std::string& name,
               const std::string& item, bool required)
{
    if ((required && name.empty()) || name.find('\0') != std::string::npos)
    {
        throw std::invalid_argument("Enter a name for the new " + item + ".");
    }
    if (!strings && (required || !name.empty()))
    {
        throw std::invalid_argument(NoNamesReason(item));
    }
    if (!name.empty() && sar::FindItem(*strings, flavor, name))
    {
        throw std::invalid_argument("The archive already has an item named " + name + ".");
    }
}

format::File RebuiltArchive(const format::File& file, std::span<const uint8_t> archive,
                            const sar::KnownSequences& added)
{
    return Rebuilt(file, archive, format::ExportGroupFiles(file), added);
}

} // namespace new_items_parts

namespace
{

namespace sar = ntdlib::sar;
using new_items_parts::CheckName;
using new_items_parts::RebuiltArchive;
using new_items_parts::TablesOf;

// Whether a bank entry of `info`, an INFO block of `flavor`, lists wave archives.
bool ListsWaveArchives(std::span<const uint8_t> info, sar::Flavor flavor)
{
    for (const std::vector<uint8_t>& entry : sar::InfoBlock::Read(info, flavor).banks)
    {
        const std::optional<std::size_t> list = WaveArchiveListOf(entry, flavor);
        if (list && xsflib::Reader(entry, flavor == sar::Flavor::kCafe).U32(*list) > 0)
        {
            return true;
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
    try
    {
        sar::SoundArchive read = sar::SoundArchive::Load(archive, StrictOptions(flavor));
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
        throw std::invalid_argument(std::string("Can't read the sequence file of the sound to copy. ") +
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
        throw std::invalid_argument(std::string("Can't write the new commands for this console. ") +
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
            throw std::invalid_argument("Choose a wave archive for each wave archive that the " + item + " refers to.");
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
        throw std::invalid_argument("The archive has no sound player for the new sound.");
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
        throw std::invalid_argument(std::string("Can't add a sound to this archive. ") +
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
    options.flavor = FlavorOf(*file.native);
    const sar::SoundArchive read = sar::SoundArchive::Load(std::vector<uint8_t>(bytes.begin(), bytes.end()), options);

    return SequencesByFileId(format::SequencesOfFiles(file, bytes, read));
}

// `imported`, the file of the archive `bytes`, with its sequences numbered as `file` numbers them, by their files' IDs.
// The new sequences follow in the order of the cut. The cut numbers the sequences in the order of their first copies:
// the archive's, then those that only group files hold. A new sequence file in the archive comes before the latter.
// Throws std::invalid_argument with a message for the user when the new archive lacks a sequence of `file`.
format::File InFileOrder(const format::File& file, format::File imported, std::span<const uint8_t> bytes)
{
    const std::map<uint32_t, uint32_t> before = SequencesByFile(file, format::ExportArchive(file));
    const std::map<uint32_t, uint32_t> after = SequencesByFile(imported, bytes);
    const SequenceNumbering numbering = SequenceNumbers(before, after, static_cast<uint32_t>(file.sequences.size()),
                                                        static_cast<uint32_t>(imported.sequences.size()));
    const std::map<uint32_t, uint32_t>& numbers = numbering.numbers;
    if (numbering.next != imported.sequences.size())
    {
        throw std::invalid_argument("After adding the items, can't find the existing sequences in the archive.");
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
    return "The archive has no item names. Can't name a new " + item + " in it.";
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
    format::File imported =
        format::ImportArchive(archive, FlavorOf(*file.native), std::move(group_files), {}, {}, known);
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
        throw std::invalid_argument("The archive has no sequence sound for the new sound to copy.");
    }
    if (model && (*model >= tables.sounds.size() || tables.sounds[*model].kind != sar::SoundType::kSequence))
    {
        throw std::invalid_argument("The sound to copy isn't a sequence sound.");
    }
    if (start >= incoming.items.size())
    {
        throw std::invalid_argument("The start is past the end of the new sequence.");
    }

    const std::vector<uint32_t> slots = banks ? *banks : model ? tables.sounds[*model].banks : DefaultBanks(tables);
    CheckBankSlots(tables, slots);

    // The archive that the file exports. Its names mustn't hold the new name.
    const sar::Flavor flavor = FlavorOf(*file.native);
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
        throw std::invalid_argument(std::string("Can't add a sound to this archive. ") +
                                    format::AsSentence(error.what()));
    }

    if (!result.native->tables || result.native->tables->sounds.size() != tables.sounds.size() + 1)
    {
        throw std::invalid_argument("Can't read the archive tables after adding the sound.");
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
        throw std::invalid_argument("The file contains no game archive.");
    }

    if (file.native->engine != format::Engine::kWii)
    {
        const sar::Flavor flavor = FlavorOf(*file.native);
        return FileOf(sequence, flavor, song::ExportVersions(song::ItemKind::kSequence, flavor).file).first.size();
    }

    return ntdlib::AlignUp(WiiSequenceFileOf(sequence, *file.native, 0x0100).file.Write().size(), 32);
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
            std::string(wave_sounds ? "Can't read the wave sound file. " : "Can't read the bank. ") +
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
        throw std::invalid_argument("Can't read the archive tables. Mapping the wave archives of the " + item +
                                    " requires them.");
    }

    const sar::Flavor flavor = FlavorOf(*file.native);
    const std::map<uint32_t, uint32_t> ids =
        WaveArchiveIds(WaveArchivesNamed(bytes, flavor), map, *file.native->tables, item).first;
    try
    {
        return sar::WithWaveArchives(bytes, flavor, ids);
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument("Can't assign other wave archives to the " + item + ". " +
                                    format::AsSentence(error.what()));
    }
}

format::File AddWaveArchive(const format::File& file, const std::string& name, std::span<const uint8_t> bytes)
{
    const format::NativeTables& tables = TablesOf(file, "wave archive");
    const sar::Flavor flavor = FlavorOf(*file.native);
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
        throw std::invalid_argument(std::string("Can't add a wave archive to this archive. ") +
                                    format::AsSentence(error.what()));
    }

    if (!result.native->tables || result.native->tables->wave_archives.size() != tables.wave_archives.size() + 1)
    {
        throw std::invalid_argument("Can't read the archive tables after adding the wave archive.");
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
    const sar::Flavor flavor = FlavorOf(*file.native);
    std::vector<uint8_t> archive = format::ExportArchive(file);
    const std::optional<std::span<const uint8_t>> strings = sar::TablesOf(archive, flavor).strings;
    for (std::size_t k = 0; k < names.size(); k++)
    {
        CheckName(strings, flavor, names[k], "group", true);
        if (std::ranges::find(names.first(k), names[k]) != names.first(k).end())
        {
            throw std::invalid_argument("Two of the new groups have the same name: " + names[k] + ".");
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
        throw std::invalid_argument(std::string("Can't add a group to this archive. ") +
                                    format::AsSentence(error.what()));
    }

    if (!result.native->tables || result.native->tables->groups.size() != tables.groups.size() + names.size())
    {
        throw std::invalid_argument("Can't read the archive tables after adding the groups.");
    }

    return result;
}

format::File AddBank(const format::File& file, const std::string& name, std::span<const uint8_t> bytes,
                     const std::map<uint32_t, uint32_t>& map)
{
    const format::NativeTables& tables = TablesOf(file, "bank");
    const sar::Flavor flavor = FlavorOf(*file.native);
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
        throw std::invalid_argument(std::string("Can't add a bank to this archive. ") +
                                    format::AsSentence(error.what()));
    }

    if (!result.native->tables || result.native->tables->banks.size() != tables.banks.size() + 1)
    {
        throw std::invalid_argument("Can't read the archive tables after adding the bank.");
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
        throw std::invalid_argument(std::string("Can't read the wave sound file. ") + format::AsSentence(error.what()));
    }
}

format::File AddWaveSounds(const format::File& file, uint32_t model, const std::string& name,
                           std::span<const uint8_t> bytes, const std::map<uint32_t, uint32_t>& map)
{
    const format::NativeTables& tables = TablesOf(file, "sound");
    const sar::Flavor flavor = FlavorOf(*file.native);
    if (model >= tables.sounds.size() || (tables.sounds[model].kind != sar::SoundType::kWaveSound &&
                                          tables.sounds[model].kind != sar::SoundType::kSequence))
    {
        throw std::invalid_argument("The sound to copy isn't a wave sound or a sequence sound.");
    }

    const IncomingFile incoming = ReadIncomingFile(bytes, FileKind::kWaveSoundFile, flavor);
    const std::size_t count = WaveSoundCount(incoming.bytes, flavor);
    if (count == 0)
    {
        throw std::invalid_argument("The wave sound file contains no wave sounds.");
    }

    // The wave archive of this archive for each one that the file names, by their item IDs.
    const std::map<uint32_t, uint32_t> ids =
        WaveArchiveIds(WaveArchivesNamed(incoming.bytes, flavor), map, tables, "wave sound file").first;

    // The archive that the file exports. Its names mustn't hold the new names.
    const std::vector<uint8_t> archive = format::ExportArchive(file);
    const sar::Tables blocks = sar::TablesOf(archive, flavor);
    if (name.empty())
    {
        throw std::invalid_argument(count == 1 ? "Enter a name for the new sound."
                                               : "Enter a name for the new sounds.");
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
        throw std::invalid_argument(std::string("Can't add wave sounds to this archive. ") +
                                    format::AsSentence(error.what()));
    }

    if (!result.native->tables || result.native->tables->sounds.size() != tables.sounds.size() + count)
    {
        throw std::invalid_argument("Can't read the archive tables after adding the sounds.");
    }

    return result;
}

} // namespace hokuto::edit
