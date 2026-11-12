// SPDX-License-Identifier: MIT

#include "format/model.h"

#include <algorithm>
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
#include "ntdlib/sar/archive_model.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/groups.h"
#include "ntdlib/sar/layout.h"
#include "ntdlib/sar/sequence_file.h"
#include "ntdlib/sdat/files.h"
#include "ntdlib/sdat/formats.h"
#include "ntdlib/sdat/layout.h"
#include "ntdlib/sound_build.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"

#include "format/archive.h"
#include "format/binary.h"
#include "format/command_values.h"
#include "format/kinds.h"
#include "format/model_parts.h"

namespace hokuto::format
{
namespace
{

namespace sar = ntdlib::sar;
using binary::Damaged;
using binary::In;
using binary::Out;
using model_parts::CutOrder;
using model_parts::KnownOf;
using model_parts::Renamed;
using model_parts::RequireWhole;
using model_parts::SequenceVersions;
using model_parts::SetSequenceStarts;
using model_parts::TakeSequenceStarts;
using model_parts::WiiModelOf;
using model_parts::WithWiiModel;
using ntdlib::commands::Fixed;
using ntdlib::commands::kOne;
using ntdlib::commands::Sequence;

// The oldest versions of the archives that Hokuto lays out: FSAR 2.2.0 on the Switch and 2.0.0 on the Wii U, and CSAR
// 2.0.0.0 on the 3DS (sar::kCtrFirstVersion).
constexpr uint32_t kOldestSwitchArchive = 0x00020200;
constexpr uint32_t kOldestWiiUArchive = 0x00020000;

// The newest version of the Wii's archives that Hokuto models: RSAR 1.4.
constexpr uint16_t kNewestWiiArchive = 0x0104;

// `sequence` without the encoder's choices. A kOther command keeps its opcode: the opcode is the command.
Sequence Plain(Sequence sequence)
{
    for (ntdlib::commands::Command& item : sequence.items)
    {
        item.encoding = {.opcode = item.encoding.opcode};
    }

    return sequence;
}

// The bytes of a sequence file of `flavor` and `version` that holds `sequence`, and the offset of each of its items in
// the bytecode, then the end. ntdlib's join writes a sequence file the same way.
std::pair<std::vector<uint8_t>, std::vector<uint32_t>> SequenceFileOf(const Sequence& sequence, sar::Flavor flavor,
                                                                      uint32_t version)
{
    ntdlib::nwsnd::Encoded encoded = ntdlib::nwsnd::EncodeSequence(sequence, GrammarOf(flavor));
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

// `group_file`, a group file of `flavor`, with each copy of a file of `sequence_files` holding the file's bytes. A
// group file whose copies hold them already stays as it is, and so does one that ntdlib can't read. Throws
// xsflib::FormatError for a group file that can't take the bytes.
std::vector<uint8_t> WithSequenceCopies(std::vector<uint8_t> group_file, sar::Flavor flavor,
                                        std::span<const sar::GroupMember> sequence_files)
{
    std::vector<sar::GroupCopy> copies;
    try
    {
        copies = sar::GroupCopies(group_file, flavor);
    }
    catch (const xsflib::FormatError&)
    {
        return group_file;
    }

    std::vector<sar::GroupMember> changed;
    for (const sar::GroupCopy& copy : copies)
    {
        const auto file = std::ranges::find(sequence_files, copy.file, &sar::GroupMember::file);
        if (file != sequence_files.end() &&
            !std::ranges::equal(*file->copy, std::span(group_file).subspan(copy.start, copy.size)))
        {
            changed.push_back(*file);
        }
    }
    if (changed.empty())
    {
        return group_file;
    }

    try
    {
        return sar::GroupWithCopies(group_file, flavor, changed);
    }
    catch (const xsflib::FormatError& error)
    {
        throw xsflib::FormatError(std::string("Can't update the sequence files inside a group file: ") + error.what() +
                                  ".");
    }
}

} // namespace

namespace model_parts
{

// The sequences of `file` that the model's sequence files `sequences` hold, by the files' IDs. The cut of the archive
// that the model lays out takes them (ntdlib::sar::Cut). A sequence whose commands run on past a return to commands
// that no label or start leads to then comes back from the cut too.
sar::KnownSequences KnownOf(const File& file, const std::map<uint32_t, ModelSequence>& sequences)
{
    sar::KnownSequences known;
    for (const auto& [id, held] : sequences)
    {
        if (held.sequence < file.sequences.size())
        {
            known[id] = *file.sequences[held.sequence];
        }
    }

    return known;
}

// Throws xsflib::FormatError for `bytes`, an archive whose header gives a larger size than the bytes hold. The tools
// write the size at `at`: at 8 in the Wii's archives, and at 12 in the later consoles'. A reader of the model would
// stop at its first read past the end, with another message on each console.
void RequireWhole(std::span<const uint8_t> bytes, std::size_t at, bool big_endian)
{
    if (bytes.size() < at + 4)
    {
        return;
    }

    const uint32_t size = big_endian ? xsflib::LoadBe32(bytes.data() + at) : xsflib::LoadLe32(bytes.data() + at);
    if (size > bytes.size())
    {
        throw xsflib::FormatError("the archive is cut short, and a file in the model form requires the whole archive");
    }
}

// The version of each sequence of `native`: the version of its first sequence piece in the archive, or else in the
// group files. A sequence file that only group files hold has its pieces there.
std::map<uint32_t, uint32_t> SequenceVersions(const NativeArchive& native)
{
    std::map<uint32_t, uint32_t> versions;
    const auto add = [&versions](const std::vector<sar::Piece>& pieces)
    {
        for (const sar::Piece& piece : pieces)
        {
            if (const auto* sequence = std::get_if<sar::SequencePiece>(&piece))
            {
                versions.emplace(sequence->sequence, sequence->version);
            }
        }
    };
    add(*native.pieces);
    for (const NativeGroupFile& group_file : native.group_files)
    {
        add(*group_file.pieces);
    }

    return versions;
}

// `pieces` with each sequence piece naming the sequence that `map` gives for the one that it names.
std::vector<sar::Piece> Renamed(std::vector<sar::Piece> pieces, const std::map<uint32_t, uint32_t>& map)
{
    for (sar::Piece& piece : pieces)
    {
        if (auto* sequence = std::get_if<sar::SequencePiece>(&piece))
        {
            sequence->sequence = map.at(sequence->sequence);
        }
    }

    return pieces;
}

// `fields` with each place that names a sequence naming the sequence that `map` gives for the one that it names.
std::vector<sar::Field> Renamed(std::vector<sar::Field> fields, const std::map<uint32_t, uint32_t>& map)
{
    for (sar::Field& field : fields)
    {
        for (sar::Place* place : {&field.from, &field.to})
        {
            if (place->sequence)
            {
                place->piece = map.at(place->piece);
            }
        }
    }

    return fields;
}

// A list of 32-bit values that starts with their 32-bit count. `what` names the values in a message.
void WriteList(Out& out, const std::vector<uint32_t>& values, const std::string& what)
{
    out.Count(values.size(), what);
    for (const uint32_t value : values)
    {
        out.U32(value);
    }
}

// A list that WriteList wrote.
std::vector<uint32_t> ReadList(In& in)
{
    const uint32_t count = in.U32();
    std::vector<uint32_t> values;
    for (uint32_t i = 0; i < count; ++i)
    {
        values.push_back(in.U32());
    }

    return values;
}

// Bytes that start with their 64-bit size.
void WriteBytes(Out& out, std::span<const uint8_t> bytes)
{
    out.U64(bytes.size());
    out.Bytes(bytes);
}

// Bytes that WriteBytes wrote.
std::vector<uint8_t> ReadBytes(In& in)
{
    const std::span<const uint8_t> bytes = in.Take(in.U64());
    return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

// Maps each sequence of `built`, the cut of the archive that `model` lays out, to the sequence of `file` that it is.
// `cut` contains the sequence of each file of the cut.
std::map<uint32_t, uint32_t> CutOrder(const File& file, const File& built, const Model& model,
                                      const std::vector<std::optional<uint32_t>>& cut)
{
    std::map<uint32_t, uint32_t> map;
    for (const auto& [id, held] : model.sequences)
    {
        const std::optional<uint32_t> found = id < cut.size() ? cut[id] : std::nullopt;
        if (!found || map.emplace(*found, held.sequence).first->second != held.sequence)
        {
            Damaged("the model's file " + std::to_string(id) + " doesn't cut as sequence " +
                    std::to_string(held.sequence));
        }
    }

    for (uint32_t c = 0; c < built.sequences.size(); ++c)
    {
        const auto mapped = map.find(c);
        if (mapped == map.end() || !(*built.sequences[c] == *file.sequences[mapped->second]))
        {
            Damaged("a sequence file of the model doesn't give its sequence back");
        }
    }

    return map;
}

} // namespace model_parts

namespace
{

// The sequence that each file holds. The FILE block's copy of a sequence file takes its bytes from the sequence.
void SetModelSequenceFiles(Model& model, const File& file, const File& plain, const std::vector<uint8_t>& bytes,
                           sar::Flavor flavor)
{
    sar::Options options;
    options.flavor = flavor;
    const sar::SoundArchive read = sar::SoundArchive::Load(bytes, options);
    const std::vector<std::optional<uint32_t>> sequences = SequencesOfFiles(plain, bytes, read);
    const std::map<uint32_t, uint32_t> versions = SequenceVersions(*file.native);
    for (uint32_t id = 0; id < sequences.size() && id < model.archive.files.size(); ++id)
    {
        if (!sequences[id])
        {
            continue;
        }

        model.sequences[id] = {*sequences[id], versions.at(*sequences[id])};
        sar::FileModel& held = model.archive.files[id];
        if (held.place == sar::FilePlace::kBytes)
        {
            held.bytes.clear();
        }
    }
}

// Renames the sequences of the group files of `native` by `map`. The group files keep none of the game's sizes or
// hashes. Each one gets the group that `model` names for it.
void RenameGroupFiles(NativeArchive& native, const Model& model, const std::map<uint32_t, uint32_t>& map)
{
    for (std::size_t g = 0; g < native.group_files.size(); ++g)
    {
        NativeGroupFile& group_file = native.group_files[g];
        group_file.pieces = Renamed(*group_file.pieces, map);
        group_file.fields = Renamed(std::move(group_file.fields), map);
        group_file.game_size = 0;
        group_file.game_hash = {};
        if (g < model.group_file_groups.size() && model.group_file_groups[g])
        {
            group_file.group = model.group_file_groups[g];
        }
    }
}

// The bytes and the offsets of the sequence files that `model` writes from the sequences of `file`.
struct WrittenSequences
{
    std::map<uint32_t, std::vector<uint32_t>> offsets; // the offset of each item of each sequence
    std::vector<sar::GroupMember> files;               // each sequence file's bytes, by its ID
};

// Writes each sequence file of `model` from the sequences of `file`. A file that the archive contains gets its bytes in
// `archive`.
WrittenSequences WriteSequenceFiles(const File& file, const Model& model, sar::ArchiveModel& archive)
{
    const sar::Flavor flavor = archive.flavor;
    WrittenSequences sequences;
    for (const auto& [id, held] : model.sequences)
    {
        if (id >= archive.files.size() || held.sequence >= file.sequences.size())
        {
            Damaged("file " + std::to_string(id) + " of the model names no file or no sequence");
        }

        std::pair<std::vector<uint8_t>, std::vector<uint32_t>> written;
        try
        {
            written = SequenceFileOf(*file.sequences[held.sequence], flavor, held.version);
        }
        catch (const std::exception& error)
        {
            throw xsflib::FormatError("Can't write sequence " + std::to_string(held.sequence) + " for this console. " +
                                      AsSentence(error.what()));
        }

        sequences.files.push_back({id, written.first});
        if (archive.files[id].place == sar::FilePlace::kBytes)
        {
            archive.files[id].bytes = std::move(written.first);
        }

        sequences.offsets[held.sequence] = std::move(written.second);
    }

    return sequences;
}

// The archive that `archive` lays out.
std::vector<uint8_t> ModelArchiveBytes(const sar::ArchiveModel& archive)
{
    try
    {
        return sar::BuildArchive(archive);
    }
    catch (const std::invalid_argument& error)
    {
        throw xsflib::FormatError(AsSentence(std::string("Can't lay out the archive from the model: ") + error.what()));
    }
}

} // namespace

Model ModelOf(const File& file)
{
    if (!file.native)
    {
        throw std::invalid_argument("The file contains no archive.");
    }
    if (file.native->engine == Engine::kWii)
    {
        return WiiModelOf(file);
    }
    if (file.native->engine == Engine::kGba || file.native->driver)
    {
        throw xsflib::FormatError(
            "a GBA ROM has no model form because its songs are mixed in with the game code and data");
    }
    if (file.native->engine == Engine::kDs)
    {
        throw xsflib::FormatError("Hokuto keeps DS sound archives only in their native form");
    }
    if (file.native->engine == Engine::kDse)
    {
        throw xsflib::FormatError("Hokuto keeps DSE files only in their native form");
    }

    // The archive that the sequences without the encoder's choices build, and its items.
    File plain = file;
    plain.sequences = WithoutChoices(file.sequences);
    const std::vector<uint8_t> bytes = ExportArchive(plain);
    const sar::Flavor flavor = FlavorOf(*file.native);
    RequireWhole(bytes, 12, flavor == sar::Flavor::kCafe);
    Model model;
    model.archive = sar::ReadArchiveModel(bytes, flavor);

    SetModelSequenceFiles(model, file, plain, bytes, flavor);
    TakeSequenceStarts(model.archive, model, plain, GrammarOf(flavor), sar::SoundType::kSequence);

    model.group_files = ExportGroupFiles(plain);
    for (const NativeGroupFile& group_file : file.native->group_files)
    {
        model.group_file_groups.push_back(group_file.group);
    }

    for (const auto& [id, stream] : file.native->streams)
    {
        model.streams[id] = *stream.bytes;
    }

    return model;
}

File WithModel(File file, const Model& model)
{
    if (model.wii)
    {
        return WithWiiModel(std::move(file), model);
    }

    // Each sequence file's bytes, and each sequence sound's start.
    sar::ArchiveModel archive = model.archive;
    const sar::Flavor flavor = archive.flavor;
    const WrittenSequences written = WriteSequenceFiles(file, model, archive);
    SetSequenceStarts(archive, model, written.offsets, sar::SoundType::kSequence);

    // The archive that the model lays out, cut as an import cuts a game's archive.
    const std::vector<uint8_t> bytes = ModelArchiveBytes(archive);

    // The group files' copies of the sequence files take the sequences' bytes too. A sequence file that only group
    // files hold has its copies there.
    std::vector<std::vector<uint8_t>> group_files;
    for (const std::vector<uint8_t>& group_file : model.group_files)
    {
        group_files.push_back(WithSequenceCopies(group_file, flavor, written.files));
    }

    std::map<uint32_t, std::vector<uint8_t>> streams = model.streams;
    File built =
        ImportArchive(bytes, flavor, std::move(group_files), std::move(streams), {}, KnownOf(file, model.sequences));

    // The cut's sequences are the file's sequences, in another order.
    sar::Options options;
    options.flavor = flavor;
    const sar::SoundArchive read = sar::SoundArchive::Load(bytes, options);
    const std::vector<std::optional<uint32_t>> cut = SequencesOfFiles(built, bytes, read);
    const std::map<uint32_t, uint32_t> map = CutOrder(file, built, model, cut);

    // The native archive names the file's sequences. Its group files and streams are Hokuto's.
    NativeArchive native = *built.native;
    native.pieces = Renamed(*native.pieces, map);
    native.fields = Renamed(std::move(native.fields), map);
    RenameGroupFiles(native, model, map);
    for (auto& [id, stream] : native.streams)
    {
        stream.from_game = false;
    }

    native.game_sizes.reset();
    file.native = std::move(native);
    file.form = ArchiveForm::kModel;

    return file;
}

ntdlib::nwsnd::Grammar GrammarOf(const Model& model)
{
    if (model.wii)
    {
        return ntdlib::nwsnd::GrammarOf<ntdlib::nwsnd::RevolutionPolicy>(
            model.wii_build ? *model.wii_build : BuildOfWiiVersion(model.wii->version));
    }

    return GrammarOf(model.archive.flavor);
}

File InModelForm(File file)
{
    const Model model = ModelOf(file);
    file.sequences = WithoutChoices(file.sequences);

    return WithModel(std::move(file), model);
}

namespace
{

// A new file of the DS: a bare sound archive that the SDK's tools could have laid out, with names, kDefaultPlayer,
// kDefaultDsStreamPlayer and kDefaultDsBank. The player plays one sound at a time on any channel, and asks for no heap
// of its own. The stream player plays two channels on channels 4 and 5. Games' archives hold players with those values.
File NewDsFile()
{
    namespace sdat = ntdlib::sdat;

    sdat::ArchiveModel model;
    model.players.emplace_back(sdat::PlayerInfo{.max_sequences = 1});
    sdat::StreamPlayerInfo stream_player;
    stream_player.channel_count = 2;
    stream_player.channels.fill(0xFF);
    stream_player.channels[0] = 4;
    stream_player.channels[1] = 5;
    model.stream_players.emplace_back(std::move(stream_player));
    model.banks.emplace_back(sdat::BankInfo{.file_id = 0, .wave_archives = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF}});
    model.files.push_back(sdat::BankFile(sdat::Bank{}));
    sdat::Symbols symbols;
    symbols.names[2] = {std::string(kDefaultDsBank)};
    symbols.names[4] = {std::string(kDefaultPlayer)};
    symbols.names[6] = {std::string(kDefaultDsStreamPlayer)};
    model.symbols = std::move(symbols);

    // A new file has no game's sizes: its export measures no growth.
    File file = ImportDsArchive(sdat::BuildArchive(model));
    NativeArchive native = *file.native;
    native.game_sizes.reset();
    file.native = std::move(native);

    return file;
}

} // namespace

File NewFile(Engine engine)
{
    if (engine == Engine::kDs)
    {
        return NewDsFile();
    }
    if (FactsOf(engine).family == Family::kGbaDriver)
    {
        throw std::invalid_argument("Can't create a new file for " + std::string(FactsOf(engine).title) +
                                    ". Such files come from a game ROM.");
    }

    Model model;
    switch (engine)
    {
    case Engine::kWiiU:
        model.archive.flavor = sar::Flavor::kCafe;
        model.archive.version = kOldestWiiUArchive;
        break;

    case Engine::k3ds:
        model.archive.flavor = sar::Flavor::kCtr;
        model.archive.version = sar::kCtrFirstVersion;
        break;

    case Engine::kSwitch:
        model.archive.flavor = sar::Flavor::kNx;
        model.archive.version = kOldestSwitchArchive;
        break;

    case Engine::kWii:
        model.wii.emplace();
        model.wii->version = kNewestWiiArchive;
        model.wii->players.push_back({.name = std::string(kDefaultPlayer), .playable_sound_count = 1});
        break;

    case Engine::kGba:
        throw std::invalid_argument("Can't create a new GBA file. GBA files come from a game ROM.");

    case Engine::kDs:
        break;

    case Engine::kDse:
        throw std::invalid_argument("Can't create a new DSE file. DSE files come from a game image.");

    // The GBA's other drivers are refused above.
    case Engine::kKonami:
    case Engine::kRd2:
    case Engine::kBrownie:
    case Engine::kUbiMilan:
    case Engine::kRare:
    case Engine::kQuintet:
    case Engine::kKrawall:
        break;
    }

    if (!model.wii)
    {
        model.archive.players.push_back({.name = std::string(kDefaultPlayer), .playable_sound_max = 1});
    }

    File file;
    file.engine = engine;

    return WithModel(std::move(file), model);
}

std::vector<Shared<Sequence>> WithoutChoices(const std::vector<Shared<Sequence>>& sequences)
{
    std::vector<Shared<Sequence>> plain;
    for (const Shared<Sequence>& sequence : sequences)
    {
        const std::vector<ntdlib::commands::Command>& items = sequence->items;
        plain.push_back(std::any_of(items.begin(), items.end(), HasChoices) ? Shared<Sequence>(Plain(*sequence))
                                                                            : sequence);
    }

    return plain;
}

Fixed LevelOfVolume(uint8_t volume)
{
    return std::llround(std::sqrt(127.0 * volume) * static_cast<double>(kOne));
}

uint8_t VolumeOfLevel(Fixed level)
{
    const double gain = static_cast<double>(level) / static_cast<double>(kOne);
    return static_cast<uint8_t>(std::clamp<long long>(std::llround(gain * gain / 127.0), 0, 255));
}

} // namespace hokuto::format
