// SPDX-License-Identifier: MIT

#include "format/file.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iterator>
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
#include "ntdlib/sar/layout.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/sha1.h"

#include "format/binary.h"
#include "format/codes.h"
#include "format/model.h"

namespace hokuto::format
{
namespace
{

using ntdlib::commands::Argument;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::Sequence;
using ntdlib::commands::Source;

constexpr std::string_view kEngineChunk = "ENGN";
constexpr std::string_view kMixerChunk = "MIXR";
constexpr std::string_view kSequencesChunk = "SEQS";
constexpr std::string_view kNativeChunk = "NATV";
constexpr std::string_view kBuildChunk = "BULD";
constexpr std::string_view kTablesChunk = "TABL";
constexpr std::string_view kScenesChunk = "SCEN";
constexpr std::string_view kTrackColoursChunk = "TCOL";
constexpr std::string_view kEditorChunk = "EDTR";
constexpr std::string_view kTagsChunk = "TAGS";
constexpr std::string_view kLengthsChunk = "LNTH";
constexpr std::string_view kSizesChunk = "SIZE";
constexpr std::string_view kTimelinesChunk = "TMLN";

// Whether Hokuto reads the chunk `id`. A file holds each such chunk once at most.
bool IsHokutoChunk(std::string_view id)
{
    return id == kEngineChunk || id == kMixerChunk || id == kSequencesChunk || id == kNativeChunk ||
           id == kBuildChunk || id == kTablesChunk || id == kSizesChunk || id == kGroupFilesChunk ||
           id == kStreamsChunk || id == kScenesChunk || id == kTrackColoursChunk || id == kEditorChunk ||
           id == kTagsChunk || id == kLengthsChunk || id == kTimelinesChunk || id == kSoundsChunk ||
           id == kItemsChunk || id == kBanksChunk || id == kWavesChunk;
}

// The byte of the view that stands for no track.
constexpr uint8_t kNoTrack = 0xFF;

// The flag of a stream that the game lacks, in the STRM chunk.
constexpr uint32_t kHokutoStream = 1;

// The flag of the build of NW4R SND that comes with AX of October 2006, in the BULD chunk.
constexpr uint8_t kOctober2006 = 1;

// The flags of a wave in the WAVE chunk: a wave that loops, and one whose samples follow. A wave holds one or two
// channels.
constexpr uint8_t kLoopingWave = 1;
constexpr uint8_t kWaveSamples = 2;
constexpr std::size_t kMostWaveChannels = 2;

// The flags of a sound's lengths in the LNTH chunk: a sound that never makes a sound, and the ticks of a loop.
constexpr uint8_t kSilentSound = 1;
constexpr uint8_t kLoopTicks = 2;

// The header: the signature, the version, the flags and the number of chunks, then an entry for each chunk: its ID, its
// CRC-32, and where it lies. Each chunk starts on a multiple of 8 bytes.
constexpr std::size_t kHeaderSize = 24;
constexpr std::size_t kEntrySize = 24;
constexpr std::size_t kChunkAlignment = 8;

// The flags of an item. The flags say which of the item's parts follow its index.
constexpr uint16_t kConditional = 1 << 0;
constexpr uint16_t kHasValue = 1 << 1;
constexpr int kValueSourceShift = 2; // two bits: the value's Source
constexpr uint16_t kHasTime = 1 << 4;
constexpr int kTimeSourceShift = 5; // two bits: the time's Source
constexpr uint16_t kHasNote = 1 << 7;
constexpr uint16_t kHasTarget = 1 << 8;
constexpr uint16_t kTargetOutside = 1 << 9;
constexpr uint16_t kHasOpcode = 1 << 10;
constexpr uint16_t kHasBytes = 1 << 11;
constexpr uint16_t kItemFlags = (1 << 12) - 1;

// The types of the native archive's pieces.
constexpr uint8_t kBytesPiece = 0;
constexpr uint8_t kSequencePiece = 1;
constexpr uint8_t kPaddingPiece = 2;

// The flags of a place in the native archive.
constexpr uint8_t kItemPlace = 1 << 0;
constexpr uint8_t kSequencePlace = 1 << 1;

// The flags of an item's encoding.
constexpr uint8_t kUnusedRandom = 1 << 0;
constexpr uint8_t kUnusedVariable = 1 << 1;

// The flags of a sound in the native archive's tables.
constexpr uint8_t kReleasePriorityFix = 1 << 0;

// The group of a group file whose group Hokuto couldn't find.
constexpr uint32_t kNoGroup = 0xFFFFFFFF;

using binary::Damaged;
using binary::In;
using binary::Newer;
using binary::Out;

uint16_t SourceCode(Source source)
{
    return static_cast<uint16_t>(source);
}

Source SourceOfCode(int code)
{
    if (code > static_cast<int>(Source::kVariable))
    {
        Newer("a source of a value");
    }

    return static_cast<Source>(code);
}

// An argument's value, and its highest value when it's random.
void WriteArgument(Out& out, const Argument& argument)
{
    if (argument.source != Source::kRandom && argument.high != 0)
    {
        throw std::invalid_argument("a value with a highest random value, and no random source");
    }

    out.I64(argument.value);
    if (argument.source == Source::kRandom)
    {
        out.I64(argument.high);
    }
}

Argument ReadArgument(In& in, Source source)
{
    Argument argument;
    argument.source = source;
    argument.value = in.I64();
    if (source == Source::kRandom)
    {
        argument.high = in.I64();
    }

    return argument;
}

// An item: its kind, its flags and its index, then the parts that its flags name. A part that the kind has is always
// there. Another part is there only when it isn't empty.
void WriteItem(Out& out, const Command& item)
{
    const bool value = ntdlib::commands::UnitOf(item.kind).has_value() || item.value != Argument{};
    const bool note = item.kind == Kind::kNote || item.key != 0 || item.velocity != 0;
    const bool target = ntdlib::commands::HasTarget(item.kind) || item.target != 0 || item.target_outside;
    const bool opcode = item.kind == Kind::kOther || item.encoding.opcode != 0;
    const bool bytes = item.kind == Kind::kData || !item.bytes.empty();

    uint16_t flags = 0;
    flags |= item.conditional ? kConditional : 0;
    flags |= value ? static_cast<uint16_t>(kHasValue | SourceCode(item.value.source) << kValueSourceShift) : 0;
    flags |= item.time ? static_cast<uint16_t>(kHasTime | SourceCode(item.time->source) << kTimeSourceShift) : 0;
    flags |= note ? kHasNote : 0;
    flags |= target ? kHasTarget : 0;
    flags |= item.target_outside ? kTargetOutside : 0;
    flags |= opcode ? kHasOpcode : 0;
    flags |= bytes ? kHasBytes : 0;

    out.U16(CodeOf(item.kind));
    out.U16(flags);
    out.U8(item.index);
    if (value)
    {
        WriteArgument(out, item.value);
    }
    if (item.time)
    {
        WriteArgument(out, *item.time);
    }
    if (note)
    {
        out.U8(item.key);
        out.I64(item.velocity);
    }
    if (target)
    {
        out.U32(item.target);
    }
    if (opcode)
    {
        out.U16(item.encoding.opcode);
    }
    if (bytes)
    {
        out.Count(item.bytes.size(), "bytes in an item");
        out.Bytes(item.bytes);
    }
}

Command ReadItem(In& in)
{
    Command item;
    const uint16_t code = in.U16();
    const std::optional<Kind> kind = KindOfCode(code);
    if (!kind)
    {
        Newer("a command (" + std::to_string(code) + ")");
    }

    item.kind = *kind;
    const uint16_t flags = in.U16();
    if ((flags & ~kItemFlags) != 0)
    {
        Newer("a part of a command");
    }

    item.conditional = (flags & kConditional) != 0;
    item.index = in.U8();
    if ((flags & kHasValue) != 0)
    {
        item.value = ReadArgument(in, SourceOfCode(flags >> kValueSourceShift & 3));
    }
    if ((flags & kHasTime) != 0)
    {
        item.time = ReadArgument(in, SourceOfCode(flags >> kTimeSourceShift & 3));
    }
    if ((flags & kHasNote) != 0)
    {
        item.key = in.U8();
        item.velocity = in.I64();
    }
    if ((flags & kHasTarget) != 0)
    {
        item.target = in.U32();
    }
    item.target_outside = (flags & kTargetOutside) != 0;
    if ((flags & kHasOpcode) != 0)
    {
        item.encoding.opcode = in.U16();
    }
    if ((flags & kHasBytes) != 0)
    {
        const std::span<const uint8_t> bytes = in.Take(in.U32());
        item.bytes.assign(bytes.begin(), bytes.end());
    }

    return item;
}

// SEQS: the number of sequences, then each sequence as a record: its items and its labels.
std::vector<uint8_t> SequencesChunk(const File& file)
{
    Out out;
    out.Count(file.sequences.size(), "sequences");
    for (const Shared<Sequence>& shared : file.sequences)
    {
        const Sequence& sequence = *shared;
        Out record;
        record.Count(sequence.items.size(), "items in a sequence");
        for (const Command& item : sequence.items)
        {
            WriteItem(record, item);
        }

        record.Count(sequence.labels.size(), "labels in a sequence");
        for (const ntdlib::commands::Label& label : sequence.labels)
        {
            record.U32(label.item);
            record.Name(label.name, "bytes in a label's name");
        }

        out.Count(record.Size(), "bytes in a sequence");
        out.Bytes(record.Take());
    }

    return out.Take();
}

std::vector<Sequence> ReadSequences(std::span<const uint8_t> chunk)
{
    In in(chunk, "the sequences");
    const uint32_t count = in.U32();
    std::vector<Sequence> sequences;
    for (uint32_t s = 0; s < count; ++s)
    {
        In record = in.Record("sequence " + std::to_string(s));
        Sequence& sequence = sequences.emplace_back();
        const uint32_t items = record.U32();
        if (items > record.Left() / 5)
        {
            Damaged("sequence " + std::to_string(s) + " gives more items than it holds");
        }

        sequence.items.reserve(items);
        for (uint32_t i = 0; i < items; ++i)
        {
            sequence.items.push_back(ReadItem(record));
        }

        const uint32_t labels = record.U32();
        for (uint32_t i = 0; i < labels; ++i)
        {
            ntdlib::commands::Label& label = sequence.labels.emplace_back();
            label.item = record.U32();
            label.name = record.Name();
            if (label.item > items)
            {
                Damaged("label " + label.name + " of sequence " + std::to_string(s) + " is past its end");
            }
        }

        for (const Command& item : sequence.items)
        {
            if (ntdlib::commands::HasTarget(item.kind) && !item.target_outside && item.target > items)
            {
                Damaged("a command of sequence " + std::to_string(s) + " goes past its end");
            }
        }
    }

    return sequences;
}

// A sequence piece: the sequence, the file's version, and the encodings of its items. The first piece of each sequence
// holds the encodings, and the others none.
void WriteSequencePiece(Out& out, const ntdlib::sar::SequencePiece& piece, const Sequence& sequence, bool first)
{
    out.U32(piece.sequence);
    out.U32(piece.version);
    std::vector<uint32_t> encoded;
    for (std::size_t i = 0; i < sequence.items.size() && first; ++i)
    {
        if (HasChoices(sequence.items[i]))
        {
            encoded.push_back(static_cast<uint32_t>(i));
        }
    }

    out.Count(encoded.size(), "encodings");
    for (const uint32_t i : encoded)
    {
        const ntdlib::commands::Encoding& encoding = sequence.items[i].encoding;
        out.U32(i);
        out.U8(encoding.value_bytes);
        out.U8(encoding.unused_time);
        out.U8(static_cast<uint8_t>((encoding.unused_random ? kUnusedRandom : 0) |
                                    (encoding.unused_variable ? kUnusedVariable : 0)));
        out.U8(0);
    }
}

// Pieces and their count. Each piece is its type, three zeros, its size and its body: a run's bytes, a sequence piece,
// or padding's alignment. `written` marks the sequences whose encodings an earlier piece holds.
void WritePieces(Out& out, std::span<const ntdlib::sar::Piece> pieces, const File& file, std::vector<bool>& written,
                 const std::string& what)
{
    out.Count(pieces.size(), what);
    for (const ntdlib::sar::Piece& piece : pieces)
    {
        const auto* run = std::get_if<std::vector<uint8_t>>(&piece);
        const auto* sequence_piece = std::get_if<ntdlib::sar::SequencePiece>(&piece);
        out.U8(run ? kBytesPiece : sequence_piece ? kSequencePiece : kPaddingPiece);
        out.Zeros(3);
        const std::size_t size = out.Reserve64();
        const std::size_t start = out.Size();
        if (run)
        {
            out.Bytes(*run);
        }
        else if (sequence_piece)
        {
            const uint32_t sequence = sequence_piece->sequence;
            if (sequence >= file.sequences.size())
            {
                throw std::invalid_argument("a piece of the native archive names sequence " + std::to_string(sequence) +
                                            ", and the file has " + std::to_string(file.sequences.size()));
            }

            WriteSequencePiece(out, *sequence_piece, *file.sequences[sequence], !written[sequence]);
            written[sequence] = true;
        }
        else
        {
            out.U32(std::get<ntdlib::sar::Padding>(piece).alignment);
        }
        out.Patch64(size, out.Size() - start);
    }
}

// Fields and their count. Each field is three places: where it lies, and the two places whose distance it holds.
void WriteFields(Out& out, std::span<const ntdlib::sar::Field> fields, const std::string& what)
{
    out.Count(fields.size(), what);
    for (const ntdlib::sar::Field& field : fields)
    {
        for (const ntdlib::sar::Place& place : {field.at, field.from, field.to})
        {
            out.U32(place.piece);
            out.U32(place.offset);
            out.U8((place.item ? kItemPlace : 0) | (place.sequence ? kSequencePlace : 0));
            out.Zeros(3);
        }
    }
}

// NATV: the archive's kind, its pieces and its fields.
std::vector<uint8_t> NativeChunk(const File& file)
{
    const NativeArchive& native = *file.native;
    Out out;
    out.U8(native.wii ? kWiiArchiveCode : CodeOf(native.flavor));
    out.Zeros(3);
    std::vector<bool> written(file.sequences.size(), false);
    WritePieces(out, *native.pieces, file, written, "pieces of the native archive");
    WriteFields(out, native.fields, "fields of the native archive");

    return out.Take();
}

ntdlib::sar::SequencePiece ReadSequencePiece(In& in, std::vector<Sequence>& sequences)
{
    ntdlib::sar::SequencePiece piece;
    piece.sequence = in.U32();
    if (piece.sequence >= sequences.size())
    {
        Damaged("the native archive names sequence " + std::to_string(piece.sequence) + ", and the file has " +
                std::to_string(sequences.size()));
    }

    std::vector<Command>& items = sequences[piece.sequence].items;
    piece.version = in.U32();
    const uint32_t encodings = in.U32();
    for (uint32_t i = 0; i < encodings; ++i)
    {
        const uint32_t item = in.U32();
        if (item >= items.size())
        {
            Damaged("an encoding in sequence " + std::to_string(piece.sequence) + " is past its end");
        }

        ntdlib::commands::Encoding& encoding = items[item].encoding;
        encoding.value_bytes = in.U8();
        encoding.unused_time = in.U8();
        const uint8_t flags = in.U8();
        encoding.unused_random = (flags & kUnusedRandom) != 0;
        encoding.unused_variable = (flags & kUnusedVariable) != 0;
        in.U8();
    }

    return piece;
}

// Pieces that WritePieces wrote. Their sequence pieces name `sequences`, and give them the encodings that they hold.
std::vector<ntdlib::sar::Piece> ReadPieces(In& in, std::vector<Sequence>& sequences, const std::string& what)
{
    const uint32_t count = in.U32();
    std::vector<ntdlib::sar::Piece> pieces;
    for (uint32_t p = 0; p < count; ++p)
    {
        const uint8_t type = in.U8();
        in.Take(3);
        In body(in.Take(in.U64()), what);
        if (type == kBytesPiece)
        {
            const std::span<const uint8_t> bytes = body.Take(body.Left());
            pieces.emplace_back(std::vector<uint8_t>(bytes.begin(), bytes.end()));
        }
        else if (type == kSequencePiece)
        {
            pieces.emplace_back(ReadSequencePiece(body, sequences));
        }
        else if (type == kPaddingPiece)
        {
            const uint32_t alignment = body.U32();
            if (alignment == 0)
            {
                Damaged(what + " pads to a multiple of 0 bytes");
            }

            pieces.emplace_back(ntdlib::sar::Padding{alignment});
        }
        else
        {
            Newer("a piece of an archive (" + std::to_string(type) + ")");
        }
    }

    return pieces;
}

// Fields that WriteFields wrote, among `pieces`. `what` names their archive or group file in a message.
std::vector<ntdlib::sar::Field> ReadFields(In& in, std::span<const ntdlib::sar::Piece> pieces,
                                           std::span<const Sequence> sequences, const std::string& what)
{
    const uint32_t count = in.U32();
    std::vector<ntdlib::sar::Field> fields;
    for (uint32_t f = 0; f < count; ++f)
    {
        ntdlib::sar::Field& field = fields.emplace_back();
        for (ntdlib::sar::Place* place : {&field.at, &field.from, &field.to})
        {
            place->piece = in.U32();
            place->offset = in.U32();
            const uint8_t flags = in.U8();
            in.Take(3);
            if ((flags & ~(kItemPlace | kSequencePlace)) != 0)
            {
                Newer("a kind of place in an archive");
            }

            place->item = (flags & kItemPlace) != 0;
            place->sequence = (flags & kSequencePlace) != 0;
        }
        if (const std::optional<std::string> problem = FieldProblem(pieces, sequences, field))
        {
            Damaged("field " + std::to_string(f) + " of " + what + " " + *problem);
        }
    }

    return fields;
}

NativeArchive ReadNative(std::span<const uint8_t> chunk, std::vector<Sequence>& sequences)
{
    In in(chunk, "the native archive");
    NativeArchive native;
    const uint8_t code = in.U8();
    const std::optional<ntdlib::sar::Flavor> flavor = FlavorOfCode(code);
    if (!flavor && code != kWiiArchiveCode)
    {
        Newer("a kind of archive (" + std::to_string(code) + ")");
    }

    native.flavor = flavor.value_or(native.flavor);
    native.wii = code == kWiiArchiveCode;
    in.Take(3);
    std::vector<ntdlib::sar::Piece> pieces = ReadPieces(in, sequences, "a piece of the native archive");
    native.fields = ReadFields(in, pieces, sequences, "the native archive");
    native.pieces = std::move(pieces);

    return native;
}

// TABL: the native archive's tables. The sounds come first, then the names of the banks and the wave archives, the
// players, and the names of the groups. Each list starts with its length.
std::vector<uint8_t> TablesChunk(const NativeTables& tables)
{
    Out out;
    out.Count(tables.sounds.size(), "sounds");
    for (const NativeSound& sound : tables.sounds)
    {
        out.Name(sound.name, "bytes in a sound's name");
        out.U8(CodeOf(sound.kind));
        out.U8(sound.volume);
        out.U8(sound.pan_mode);
        out.U8(sound.pan_curve);
        out.U8(sound.player_priority);
        out.U8(sound.actor_player);
        out.U8(sound.channel_priority);
        out.U8(sound.release_priority_fix ? kReleasePriorityFix : 0);
        out.U32(sound.player);
        out.U32(sound.tracks);
        out.Count(sound.banks.size(), "banks of a sound");
        for (const uint32_t bank : sound.banks)
        {
            out.U32(bank);
        }
    }

    for (const std::vector<std::string>* names : {&tables.banks, &tables.wave_archives})
    {
        out.Count(names->size(), "items in the tables");
        for (const std::string& name : *names)
        {
            out.Name(name, "bytes in an item's name");
        }
    }

    out.Count(tables.players.size(), "players");
    for (const NativePlayer& player : tables.players)
    {
        out.Name(player.name, "bytes in a player's name");
        out.U32(player.sound_limit);
    }

    out.Count(tables.groups.size(), "groups");
    for (const std::string& name : tables.groups)
    {
        out.Name(name, "bytes in a group's name");
    }

    return out.Take();
}

NativeTables ReadTables(std::span<const uint8_t> chunk)
{
    In in(chunk, "the tables");
    NativeTables tables;
    const uint32_t sounds = in.U32();
    for (uint32_t i = 0; i < sounds; ++i)
    {
        NativeSound& sound = tables.sounds.emplace_back();
        sound.name = in.Name();
        const uint8_t code = in.U8();
        const std::optional<ntdlib::sar::SoundType> kind = SoundTypeOfCode(code);
        if (!kind)
        {
            Newer("a kind of sound (" + std::to_string(code) + ")");
        }

        sound.kind = *kind;
        sound.volume = in.U8();
        sound.pan_mode = in.U8();
        sound.pan_curve = in.U8();
        sound.player_priority = in.U8();
        sound.actor_player = in.U8();
        sound.channel_priority = in.U8();
        const uint8_t flags = in.U8();
        if ((flags & ~kReleasePriorityFix) != 0)
        {
            Newer("a flag of a sound");
        }

        sound.release_priority_fix = (flags & kReleasePriorityFix) != 0;
        sound.player = in.U32();
        sound.tracks = in.U32();
        const uint32_t banks = in.U32();
        for (uint32_t b = 0; b < banks; ++b)
        {
            sound.banks.push_back(in.U32());
        }
    }

    for (std::vector<std::string>* names : {&tables.banks, &tables.wave_archives})
    {
        const uint32_t count = in.U32();
        for (uint32_t i = 0; i < count; ++i)
        {
            names->push_back(in.Name());
        }
    }

    const uint32_t players = in.U32();
    for (uint32_t i = 0; i < players; ++i)
    {
        NativePlayer& player = tables.players.emplace_back();
        player.name = in.Name();
        player.sound_limit = in.U32();
    }

    const uint32_t groups = in.U32();
    for (uint32_t i = 0; i < groups; ++i)
    {
        tables.groups.push_back(in.Name());
    }

    return tables;
}

// BULD: the game's build of NW4R SND. The date of its banner, then the flags and 3 zeros.
std::vector<uint8_t> BuildChunk(const ntdlib::SndBuild& build)
{
    Out out;
    out.U32(build.date);
    out.U8(build.october_2006 ? kOctober2006 : 0);
    out.Zeros(3);

    return out.Take();
}

ntdlib::SndBuild ReadBuild(std::span<const uint8_t> chunk)
{
    In in(chunk, "the BULD chunk");
    ntdlib::SndBuild build;
    build.date = in.U32();
    const uint8_t flags = in.U8();
    if ((flags & ~kOctober2006) != 0)
    {
        Newer("a build of the Wii's sound library");
    }
    build.october_2006 = (flags & kOctober2006) != 0;

    return build;
}

// SIZE: the game's sizes. The archive's size, then the number of groups and each group's size.
std::vector<uint8_t> SizesChunk(const GameSizes& sizes)
{
    Out out;
    out.U64(sizes.archive);
    out.Count(sizes.groups.size(), "groups' sizes");
    for (const uint64_t size : sizes.groups)
    {
        out.U64(size);
    }

    return out.Take();
}

GameSizes ReadSizes(std::span<const uint8_t> chunk)
{
    In in(chunk, "the SIZE chunk");
    GameSizes sizes;
    sizes.archive = in.U64();
    const uint32_t groups = in.U32();
    for (uint32_t i = 0; i < groups; ++i)
    {
        sizes.groups.push_back(in.U64());
    }

    return sizes;
}

// The name of aux bus `bus`: A, B or C.
std::string BusName(std::size_t bus)
{
    return std::string(1, static_cast<char>('A' + bus));
}

// Whether an aux bus's volume differs from full volume.
bool NotUnity(float volume)
{
    return volume != 1.0f;
}

// MIXR: the effects on the aux buses, then their volumes. For each of the buses A to C, the number of its effects, then
// each effect: its kind, three zeros, the number of its parameters, and each parameter as a float. After bus C's
// effects, each bus's volume as a float, unless every bus is at full volume.
std::vector<uint8_t> MixerChunk(const File& file)
{
    Out out;
    for (std::size_t bus = 0; bus < kAuxBuses; ++bus)
    {
        out.Count(file.aux_effects[bus].size(), "effects on a bus");
        for (const Effect& effect : file.aux_effects[bus])
        {
            if (const std::optional<std::string> problem = EffectProblem(effect))
            {
                throw std::invalid_argument("an effect on aux bus " + BusName(bus) + " " + *problem);
            }

            out.U8(CodeOf(effect.kind));
            out.Zeros(3);
            out.Count(effect.parameters.size(), "parameters of an effect");
            for (const float parameter : effect.parameters)
            {
                out.F32(parameter);
            }
        }
    }
    for (std::size_t bus = 0; bus < kAuxBuses; ++bus)
    {
        if (const std::optional<std::string> problem = AuxVolumeProblem(file.aux_volumes[bus]))
        {
            throw std::invalid_argument("aux bus " + BusName(bus) + "'s volume " + *problem);
        }
    }
    if (std::any_of(file.aux_volumes.begin(), file.aux_volumes.end(), NotUnity))
    {
        for (const float volume : file.aux_volumes)
        {
            out.F32(volume);
        }
    }

    return out.Take();
}

void ReadMixer(std::span<const uint8_t> chunk, File& file)
{
    In in(chunk, "the mixer");
    for (std::size_t bus = 0; bus < kAuxBuses; ++bus)
    {
        const uint32_t count = in.U32();
        for (uint32_t e = 0; e < count; ++e)
        {
            const uint8_t code = in.U8();
            const std::optional<EffectKind> kind = EffectKindOfCode(code);
            if (!kind)
            {
                Newer("an effect (" + std::to_string(code) + ")");
            }

            Effect& effect = file.aux_effects[bus].emplace_back();
            effect.kind = *kind;
            in.Take(3);
            const uint32_t parameters = in.U32();
            for (uint32_t p = 0; p < parameters; ++p)
            {
                effect.parameters.push_back(in.F32());
            }
            if (const std::optional<std::string> problem = EffectProblem(effect))
            {
                Damaged("an effect on aux bus " + BusName(bus) + " " + *problem);
            }
        }
    }

    // A chunk that ends after bus C's effects leaves each bus at full volume.
    if (in.Left() == 0)
    {
        return;
    }

    for (std::size_t bus = 0; bus < kAuxBuses; ++bus)
    {
        file.aux_volumes[bus] = in.F32();
        if (const std::optional<std::string> problem = AuxVolumeProblem(file.aux_volumes[bus]))
        {
            Damaged("aux bus " + BusName(bus) + "'s volume " + *problem);
        }
    }
}

// SCEN: the scenes. Their number, then each scene: its sound, its name, the number of its starting values and each one,
// then the number of its changes and each one. A starting value is the variable's number, a zero and the value. A
// change is its time, then the same.
std::vector<uint8_t> ScenesChunk(const File& file)
{
    Out out;
    out.Count(file.scenes.size(), "scenes");
    for (std::size_t s = 0; s < file.scenes.size(); ++s)
    {
        const Scene& scene = file.scenes[s];
        if (const std::optional<std::string> problem = SceneProblem(scene))
        {
            throw std::invalid_argument("scene " + std::to_string(s) + " " + *problem);
        }

        out.U32(scene.sound);
        out.Name(scene.name, "a scene's name");
        out.Count(scene.values.size(), "starting values of a scene");
        for (const auto& [variable, value] : scene.values)
        {
            out.U8(variable);
            out.U8(0);
            out.U16(static_cast<uint16_t>(value));
        }
        out.Count(scene.changes.size(), "changes of a scene");
        for (const VariableChange& change : scene.changes)
        {
            out.U32(change.time);
            out.U8(change.variable);
            out.U8(0);
            out.U16(static_cast<uint16_t>(change.value));
        }
    }

    return out.Take();
}

// A variable's number and the zero after it. A later version may give the zero a meaning.
uint8_t ReadVariable(In& in)
{
    const uint8_t variable = in.U8();
    if (const uint8_t kind = in.U8(); kind != 0)
    {
        Newer("a kind of variable (" + std::to_string(kind) + ")");
    }

    return variable;
}

void ReadScenes(std::span<const uint8_t> chunk, File& file)
{
    In in(chunk, "the scenes");
    const uint32_t count = in.U32();
    for (uint32_t s = 0; s < count; ++s)
    {
        Scene& scene = file.scenes.emplace_back();
        scene.sound = in.U32();
        scene.name = in.Name();
        const uint32_t values = in.U32();
        for (uint32_t v = 0; v < values; ++v)
        {
            const uint8_t variable = ReadVariable(in);
            if (!scene.values.emplace(variable, static_cast<int16_t>(in.U16())).second)
            {
                Damaged("scene " + std::to_string(s) + " starts variable " + std::to_string(variable) + " twice");
            }
        }
        const uint32_t changes = in.U32();
        for (uint32_t c = 0; c < changes; ++c)
        {
            VariableChange& change = scene.changes.emplace_back();
            change.time = in.U32();
            change.variable = ReadVariable(in);
            change.value = static_cast<int16_t>(in.U16());
        }
        if (const std::optional<std::string> problem = SceneProblem(scene))
        {
            Damaged("scene " + std::to_string(s) + " " + *problem);
        }
    }
}

// TCOL: the tracks' colours. Their number, then each one: the sound, the track, and the colour's red, green and blue.
std::vector<uint8_t> TrackColoursChunk(const File& file)
{
    Out out;
    out.Count(file.track_colours.size(), "tracks' colours");
    for (const auto& [place, colour] : file.track_colours)
    {
        out.U32(place.sound);
        out.U8(place.track);
        out.U8(colour.red);
        out.U8(colour.green);
        out.U8(colour.blue);
    }

    return out.Take();
}

void ReadTrackColours(std::span<const uint8_t> chunk, File& file)
{
    In in(chunk, "the tracks' colours");
    const uint32_t count = in.U32();
    for (uint32_t i = 0; i < count; ++i)
    {
        TrackPlace place;
        place.sound = in.U32();
        place.track = in.U8();
        Colour colour;
        colour.red = in.U8();
        colour.green = in.U8();
        colour.blue = in.U8();
        if (!file.track_colours.emplace(place, colour).second)
        {
            Damaged("it gives track " + std::to_string(place.track) + " of sound " + std::to_string(place.sound) +
                    " two colours");
        }
    }
}

// EDTR: the program's view. The sound, the position and the cycle's locators, then a byte for whether the cycle is on,
// and the numbers of the selected track and of the editors' tracks. The arrangement's sizes, scroll and sets of tracks,
// the Key Editor's sizes and scroll, then the number of lanes and each lane. A byte for the window in front ends the
// chunk.
std::vector<uint8_t> EditorChunk(const EditorView& view)
{
    if (const std::optional<std::string> problem = ViewProblem(view))
    {
        throw std::invalid_argument("the view " + *problem);
    }

    Out out;
    out.U32(view.sound);
    out.F32(view.position);
    out.F32(view.cycle_start);
    out.F32(view.cycle_end);
    out.U8(view.cycle ? 1 : 0);
    out.U8(view.selected_track.value_or(kNoTrack));
    out.U8(view.key_editor_track.value_or(kNoTrack));
    out.U8(view.list_editor_track.value_or(kNoTrack));
    out.F32(view.beat_width);
    out.F32(view.track_height);
    out.F32(view.left);
    out.F32(view.top);
    out.U32(view.muted);
    out.U32(view.soloed);
    out.F32(view.key_beat_width);
    out.F32(view.key_height);
    out.F32(view.key_left);
    out.F32(view.key_top);
    out.Count(view.lanes.size(), "lanes");
    for (const ViewLane& lane : view.lanes)
    {
        out.U16(CodeOf(lane.kind));
        out.U8(lane.index);
        out.U8(0);
    }
    out.U8(static_cast<uint8_t>(view.front));

    return out.Take();
}

// A chunk without the byte for the window in front puts the List Editor in front: Hokuto opened such a file with the
// List Editor in front where it opened. Bytes after that byte belong to later versions. A reader skips them.
void ReadEditor(std::span<const uint8_t> chunk, File& file)
{
    In in(chunk, "the view");

    const auto track = [](uint8_t number)
    {
        return number == kNoTrack ? std::nullopt : std::optional<uint8_t>(number);
    };

    EditorView view;
    view.sound = in.U32();
    view.position = in.F32();
    view.cycle_start = in.F32();
    view.cycle_end = in.F32();
    const uint8_t cycle = in.U8();
    view.selected_track = track(in.U8());
    view.key_editor_track = track(in.U8());
    view.list_editor_track = track(in.U8());
    view.beat_width = in.F32();
    view.track_height = in.F32();
    view.left = in.F32();
    view.top = in.F32();
    view.muted = in.U32();
    view.soloed = in.U32();
    view.key_beat_width = in.F32();
    view.key_height = in.F32();
    view.key_left = in.F32();
    view.key_top = in.F32();
    if (cycle > 1)
    {
        Damaged("the view's cycle is neither on nor off");
    }

    view.cycle = cycle == 1;
    const uint32_t lanes = in.U32();
    for (uint32_t i = 0; i < lanes; ++i)
    {
        const uint16_t code = in.U16();
        const std::optional<Kind> kind = KindOfCode(code);
        if (!kind)
        {
            Newer("a lane of a command (" + std::to_string(code) + ")");
        }

        view.lanes.push_back({*kind, in.U8()});
        in.U8();
    }
    view.front = ViewFront::kListEditor;
    if (in.Left() > 0)
    {
        const uint8_t front = in.U8();
        if (front > static_cast<uint8_t>(ViewFront::kListEditor))
        {
            Newer("a window in front of the editors (" + std::to_string(front) + ")");
        }

        view.front = static_cast<ViewFront>(front);
    }
    if (const std::optional<std::string> problem = ViewProblem(view))
    {
        Damaged("the view " + *problem);
    }

    file.view = std::move(view);
}

// The number of `tags`, then each tag: its key, then its value. `whose` names the tags in a message, such as "sound 3's
// ".
void WriteTags(Out& out, const Tags& tags, const std::string& whose)
{
    out.Count(tags.size(), "tags");
    for (const auto& [key, value] : tags)
    {
        if (const std::optional<std::string> problem = TagProblem(key, value))
        {
            throw std::invalid_argument(whose + *problem);
        }

        out.Name(key, "a tag's key");
        out.Name(value, "a tag's value");
    }
}

// Tags that WriteTags wrote.
Tags ReadTagSet(In& in, const std::string& whose)
{
    Tags tags;
    const uint32_t count = in.U32();
    for (uint32_t i = 0; i < count; ++i)
    {
        std::string key = in.Name();
        std::string value = in.Name();
        if (const std::optional<std::string> problem = TagProblem(key, value))
        {
            Damaged(whose + *problem);
        }
        if (tags.contains(key))
        {
            Damaged(whose + "tag " + key + " comes twice");
        }

        tags.emplace(std::move(key), std::move(value));
    }

    return tags;
}

// TAGS: the file's tags, then the number of sounds with tags, and each sound's index and tags.
std::vector<uint8_t> TagsChunk(const File& file)
{
    Out out;
    WriteTags(out, file.tags, "");
    const auto tagged = [](const auto& entry)
    {
        return !entry.second.empty();
    };
    out.Count(static_cast<std::size_t>(std::count_if(file.sound_tags.begin(), file.sound_tags.end(), tagged)),
              "sounds with tags");
    for (const auto& [sound, tags] : file.sound_tags)
    {
        if (!tags.empty())
        {
            out.U32(sound);
            WriteTags(out, tags, "sound " + std::to_string(sound) + "'s ");
        }
    }

    return out.Take();
}

// A sound without tags adds nothing.
void ReadTags(std::span<const uint8_t> chunk, File& file)
{
    In in(chunk, "the tags");
    file.tags = ReadTagSet(in, "");
    const uint32_t sounds = in.U32();
    for (uint32_t i = 0; i < sounds; ++i)
    {
        const uint32_t sound = in.U32();
        const std::string whose = "sound " + std::to_string(sound) + "'s ";
        Tags tags = ReadTagSet(in, whose);
        if (file.sound_tags.contains(sound))
        {
            Damaged(whose + "tags come twice");
        }
        if (!tags.empty())
        {
            file.sound_tags.emplace(sound, std::move(tags));
        }
    }
}

// LNTH: the sounds' lengths. Their number, then each sound's: its index, its flags, its loops and two zeros, its length
// and its fade, then the ticks of its loop with the flag of them.
std::vector<uint8_t> LengthsChunk(const File& file)
{
    Out out;
    out.Count(file.lengths.size(), "sounds' lengths");
    for (const auto& [sound, length] : file.lengths)
    {
        if (const std::optional<std::string> problem = LengthProblem(length))
        {
            throw std::invalid_argument("sound " + std::to_string(sound) + "'s lengths " + *problem);
        }

        out.U32(sound);
        out.U8(static_cast<uint8_t>((length.silent ? kSilentSound : 0) | (length.loop ? kLoopTicks : 0)));
        out.U8(length.loops);
        out.U16(0);
        out.U32(length.length);
        out.U32(length.fade);
        if (length.loop)
        {
            out.U32(length.loop->start);
            out.U32(length.loop->end);
        }
    }

    return out.Take();
}

void ReadLengths(std::span<const uint8_t> chunk, File& file)
{
    In in(chunk, "the lengths");
    const uint32_t count = in.U32();
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t sound = in.U32();
        const uint8_t flags = in.U8();
        if ((flags & ~(kSilentSound | kLoopTicks)) != 0)
        {
            Newer("a sound's lengths with a flag");
        }

        SoundLength length;
        length.silent = (flags & kSilentSound) != 0;
        length.loops = in.U8();
        in.U16();
        length.length = in.U32();
        length.fade = in.U32();
        if ((flags & kLoopTicks) != 0)
        {
            length.loop = LoopTicks{in.U32(), in.U32()};
        }
        const std::string whose = "sound " + std::to_string(sound) + "'s lengths";
        if (const std::optional<std::string> problem = LengthProblem(length))
        {
            Damaged(whose + " " + *problem);
        }
        if (!file.lengths.emplace(sound, length).second)
        {
            Damaged(whose + " come twice");
        }
    }
}

// TMLN: the timelines. Their number, then each one: its sequence, the number of runs of bars and each run's length and
// number of bars, then the number of tracks with hints and each track's hints. A track's hints are its number and three
// zeros, then three lists: the blocks before the song's start, the bars kept in the track and the bars called. Each
// list is its length, then its numbers.
std::vector<uint8_t> TimelinesChunk(const File& file)
{
    if (const std::optional<std::string> problem = TimelinesProblem(file))
    {
        throw std::invalid_argument("the timelines " + *problem);
    }

    Out out;
    const auto list = [&out](const std::vector<uint32_t>& numbers, const std::string& what)
    {
        out.Count(numbers.size(), what);
        for (const uint32_t number : numbers)
        {
            out.U32(number);
        }
    };

    out.Count(file.timelines->size(), "timelines");
    for (const auto& [sequence, hints] : *file.timelines)
    {
        out.U32(sequence);
        out.Count(hints.bars.size(), "runs of bars");
        for (const BarRun& run : hints.bars)
        {
            out.U32(run.length);
            out.U32(run.count);
        }
        out.Count(hints.tracks.size(), "tracks' hints");
        for (const TrackHints& track : hints.tracks)
        {
            out.U8(track.track);
            out.Zeros(3);
            list(track.opening, "blocks before a song's start");
            list(track.kept_inline, "bars kept in a track");
            list(track.called, "bars called");
        }
    }

    return out.Take();
}

// The timelines need the sequences: each names one.
void ReadTimelines(std::span<const uint8_t> chunk, File& file)
{
    In in(chunk, "the timelines");
    const auto list = [&in]
    {
        std::vector<uint32_t> numbers(in.U32());
        for (uint32_t& number : numbers)
        {
            number = in.U32();
        }

        return numbers;
    };

    file.timelines.emplace();
    const uint32_t count = in.U32();
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t sequence = in.U32();
        ConverterHints hints;
        hints.bars.resize(in.U32());
        for (BarRun& run : hints.bars)
        {
            run.length = in.U32();
            run.count = in.U32();
        }
        hints.tracks.resize(in.U32());
        for (TrackHints& track : hints.tracks)
        {
            track.track = in.U8();
            in.Take(3);
            track.opening = list();
            track.kept_inline = list();
            track.called = list();
        }
        if (!file.timelines->emplace(sequence, std::move(hints)).second)
        {
            Damaged("sequence " + std::to_string(sequence) + " has two timelines");
        }
    }
    if (const std::optional<std::string> problem = TimelinesProblem(file))
    {
        Damaged("the timelines " + *problem);
    }
}

// ENGN: the engine's number, the mixing mode's number and two zeros. When sounds take another mode, their number
// follows, then each sound's index, its mode's number and three zeros. When sounds play on another engine, the number
// of sounds in another mode comes all the same. The number of sounds on another engine follows, then each sound's
// index, its engine's number and three zeros.
std::vector<uint8_t> EngineChunk(const File& file)
{
    Out out;
    out.U8(CodeOf(file.engine));
    out.U8(CodeOf(file.mixing));
    out.Zeros(2);
    if (!file.sound_mixing.empty() || !file.sound_engines.empty())
    {
        out.Count(file.sound_mixing.size(), "sounds in another mixing mode");
        for (const auto& [sound, mixing] : file.sound_mixing)
        {
            out.U32(sound);
            out.U8(CodeOf(mixing));
            out.Zeros(3);
        }
    }
    if (!file.sound_engines.empty())
    {
        out.Count(file.sound_engines.size(), "sounds on another engine");
        for (const auto& [sound, engine] : file.sound_engines)
        {
            out.U32(sound);
            out.U8(CodeOf(engine));
            out.Zeros(3);
        }
    }

    return out.Take();
}

void ReadEngine(std::span<const uint8_t> chunk, File& file)
{
    In in(chunk, "the engine");
    const uint8_t engine = in.U8();
    const uint8_t mixing = in.U8();
    const std::optional<Engine> e = EngineOfCode(engine);
    const std::optional<Mixing> m = MixingOfCode(mixing);
    if (!e)
    {
        Newer("an engine (" + std::to_string(engine) + ")");
    }
    if (!m)
    {
        Newer("a mixing mode (" + std::to_string(mixing) + ")");
    }

    file.engine = *e;
    file.mixing = *m;

    // A chunk of 4 bytes gives every sound the file's mode.
    in.U16();
    if (in.Left() == 0)
    {
        return;
    }

    const uint32_t count = in.U32();
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t sound = in.U32();
        const uint8_t code = in.U8();
        in.Take(3);
        const std::optional<Mixing> mode = MixingOfCode(code);
        if (!mode)
        {
            Newer("a mixing mode (" + std::to_string(code) + ")");
        }
        if (!file.sound_mixing.emplace(sound, *mode).second)
        {
            Damaged("sound " + std::to_string(sound) + "'s mixing mode comes twice");
        }
    }

    // A chunk without the sounds' engines plays every sound on the file's engine.
    if (in.Left() == 0)
    {
        return;
    }

    const uint32_t engines = in.U32();
    for (uint32_t i = 0; i < engines; ++i)
    {
        const uint32_t sound = in.U32();
        const uint8_t code = in.U8();
        in.Take(3);
        const std::optional<Engine> named = EngineOfCode(code);
        if (!named)
        {
            Newer("an engine (" + std::to_string(code) + ")");
        }
        if (!file.sound_engines.emplace(sound, *named).second)
        {
            Damaged("sound " + std::to_string(sound) + "'s engine comes twice");
        }
    }
}

} // namespace

Engine EngineOf(ntdlib::sar::Flavor flavor)
{
    switch (flavor)
    {
    case ntdlib::sar::Flavor::kCafe:
        return Engine::kWiiU;
    case ntdlib::sar::Flavor::kCtr:
        return Engine::k3ds;
    case ntdlib::sar::Flavor::kNx:
        break;
    }

    return Engine::kSwitch;
}

std::optional<ntdlib::sar::Flavor> FlavorOf(Engine engine)
{
    switch (engine)
    {
    case Engine::kSwitch:
        return ntdlib::sar::Flavor::kNx;
    case Engine::kWiiU:
        return ntdlib::sar::Flavor::kCafe;
    case Engine::k3ds:
        return ntdlib::sar::Flavor::kCtr;
    case Engine::kWii:
        break;
    }

    return std::nullopt;
}

std::vector<Shared<Sequence>> ShareSequences(std::vector<Sequence> sequences)
{
    return {std::make_move_iterator(sequences.begin()), std::make_move_iterator(sequences.end())};
}

std::vector<Sequence> CopySequences(const File& file)
{
    std::vector<Sequence> sequences;
    sequences.reserve(file.sequences.size());
    for (const Shared<Sequence>& sequence : file.sequences)
    {
        sequences.push_back(*sequence);
    }

    return sequences;
}

std::vector<uint8_t> GroupFilesChunk(const File& file)
{
    // NATV holds the encodings of the sequences that the archive's pieces name.
    const NativeArchive& native = *file.native;
    std::vector<bool> written(file.sequences.size(), false);
    for (const ntdlib::sar::Piece& piece : *native.pieces)
    {
        const auto* sequence_piece = std::get_if<ntdlib::sar::SequencePiece>(&piece);
        if (sequence_piece && sequence_piece->sequence < written.size())
        {
            written[sequence_piece->sequence] = true;
        }
    }

    Out out;
    out.Count(native.group_files.size(), "group files");
    for (const NativeGroupFile& group_file : native.group_files)
    {
        out.U32(group_file.group.value_or(kNoGroup));
        out.U64(group_file.game_size);
        out.Bytes(group_file.game_hash);
        WritePieces(out, *group_file.pieces, file, written, "pieces of a group file");
        WriteFields(out, group_file.fields, "fields of a group file");
    }

    return out.Take();
}

std::vector<NativeGroupFile> ReadGroupFiles(std::span<const uint8_t> chunk, std::vector<Sequence>& sequences)
{
    In in(chunk, "the GRPF chunk");
    const uint32_t count = in.U32();
    std::vector<NativeGroupFile> group_files;

    // An earlier Hokuto kept each group file whole: its size in 8 bytes, then its bytes. A group's file starts with its
    // magic. In the later form, a game's size below 2^62 bytes holds zeros there.
    const auto magic_at = [&](const char* magic)
    {
        return chunk.size() >= 16 && std::memcmp(chunk.data() + 12, magic, 4) == 0;
    };
    if (magic_at("FGRP") || magic_at("CGRP"))
    {
        for (uint32_t g = 0; g < count; ++g)
        {
            const std::span<const uint8_t> bytes = in.Take(in.U64());
            NativeGroupFile& group_file = group_files.emplace_back();
            group_file.game_size = bytes.size();
            group_file.game_hash = xsflib::Sha1::Of(bytes);
            if (!bytes.empty())
            {
                group_file.pieces = std::vector<ntdlib::sar::Piece>{std::vector<uint8_t>(bytes.begin(), bytes.end())};
            }
        }

        return group_files;
    }

    for (uint32_t g = 0; g < count; ++g)
    {
        NativeGroupFile& group_file = group_files.emplace_back();
        const uint32_t group = in.U32();
        group_file.group = group == kNoGroup ? std::nullopt : std::optional<uint32_t>(group);
        group_file.game_size = in.U64();
        const std::span<const uint8_t> hash = in.Take(group_file.game_hash.size());
        std::ranges::copy(hash, group_file.game_hash.begin());
        std::vector<ntdlib::sar::Piece> pieces = ReadPieces(in, sequences, "a piece of a group file");
        group_file.fields = ReadFields(in, pieces, sequences, "group file " + std::to_string(g));
        group_file.pieces = std::move(pieces);
    }

    return group_files;
}

std::vector<uint8_t> StreamsChunk(const NativeArchive& native)
{
    Out out;
    out.Count(native.streams.size(), "streams");
    for (const auto& [id, stream] : native.streams)
    {
        out.U32(id);
        out.U32(stream.from_game ? 0 : kHokutoStream);
        out.U64(stream.bytes->size());
        out.Bytes(*stream.bytes);
    }

    return out.Take();
}

std::vector<uint8_t> BanksChunk(const File& file)
{
    Out out;
    out.Count(file.banks.size(), "banks");
    for (const auto& [id, bank] : file.banks)
    {
        // The header's values, the table of waves and the order of the layout.
        out.U32(id);
        out.U8(CodeOf(bank->flavor));
        out.Zeros(3);
        out.U32(bank->version);
        out.Count(bank->waves.size(), "waves in a bank");
        for (const ntdlib::sar::WaveId& wave : bank->waves)
        {
            out.U32(wave.wave_archive_item);
            out.U32(wave.index);
        }
        out.Count(bank->instruments.size(), "programs in a bank");
        out.Count(bank->order.size(), "programs in a bank");
        for (const uint32_t program : bank->order)
        {
            out.U32(program);
        }

        // Each program's instrument, with its key regions and their velocity regions.
        for (const std::optional<ntdlib::sar::BankFile::Instrument>& instrument : bank->instruments)
        {
            out.U8(instrument ? 1 : 0);
            out.Zeros(3);
            if (!instrument)
            {
                continue;
            }

            out.Count(instrument->key_regions.size(), "key regions");
            for (const ntdlib::sar::BankFile::KeyRegion& keys : instrument->key_regions)
            {
                out.U8(keys.first_key);
                out.U8(keys.last_key);
                out.U16(0);
                out.Count(keys.regions.size(), "velocity regions");
                for (const ntdlib::sar::BankFile::Region& region : keys.regions)
                {
                    const ntdlib::sar::VelocityRegion& values = region.values;
                    out.U8(region.first_velocity);
                    out.U8(region.last_velocity);
                    out.U16(0);
                    out.U32(values.wave_id_index);
                    out.U8(values.original_key);
                    out.U8(values.volume);
                    out.U8(values.pan);
                    out.U8(values.ignore_note_off ? 1 : 0);
                    out.F32(values.pitch);
                    out.U8(values.key_group);
                    out.U8(values.interpolation);
                    out.U16(0);
                    out.U8(values.adshr.attack);
                    out.U8(values.adshr.decay);
                    out.U8(values.adshr.sustain);
                    out.U8(values.adshr.hold);
                    out.U8(values.adshr.release);
                    out.Zeros(3);
                }
            }
        }
    }

    return out.Take();
}

std::map<uint32_t, Shared<ntdlib::sar::BankFile>> ReadBanks(std::span<const uint8_t> chunk)
{
    In in(chunk, "the BANK chunk");
    const uint32_t count = in.U32();
    std::map<uint32_t, Shared<ntdlib::sar::BankFile>> banks;
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t id = in.U32();
        if (!banks.empty() && id <= banks.rbegin()->first)
        {
            Damaged("the file IDs of its banks don't rise");
        }
        const std::optional<ntdlib::sar::Flavor> flavor = FlavorOfCode(in.U8());
        in.Take(3);
        if (!flavor)
        {
            Newer("a bank of a console");
        }

        ntdlib::sar::BankFile bank;
        bank.flavor = *flavor;
        bank.version = in.U32();
        const uint32_t waves = in.U32();
        for (uint32_t w = 0; w < waves; ++w)
        {
            const uint32_t archive = in.U32();
            bank.waves.push_back({archive, in.U32()});
        }

        // Each program takes 4 bytes at least. The chunk has to hold them before they take memory.
        const uint32_t programs = in.U32();
        if (programs > in.Left() / 4)
        {
            Damaged("bank " + std::to_string(id) + " holds more programs than its chunk");
        }
        bank.instruments.resize(programs);
        const uint32_t laid = in.U32();
        for (uint32_t p = 0; p < laid; ++p)
        {
            bank.order.push_back(in.U32());
        }

        for (std::optional<ntdlib::sar::BankFile::Instrument>& instrument : bank.instruments)
        {
            const uint8_t flags = in.U8();
            in.Take(3);
            if ((flags & ~1u) != 0)
            {
                Newer("an instrument with a flag");
            }
            if (flags == 0)
            {
                continue;
            }

            instrument.emplace();
            const uint32_t key_regions = in.U32();
            for (uint32_t k = 0; k < key_regions; ++k)
            {
                ntdlib::sar::BankFile::KeyRegion keys;
                keys.first_key = in.U8();
                keys.last_key = in.U8();
                in.U16();
                const uint32_t regions = in.U32();
                for (uint32_t r = 0; r < regions; ++r)
                {
                    ntdlib::sar::BankFile::Region region;
                    ntdlib::sar::VelocityRegion& values = region.values;
                    region.first_velocity = in.U8();
                    region.last_velocity = in.U8();
                    in.U16();
                    values.wave_id_index = in.U32();
                    values.original_key = in.U8();
                    values.volume = in.U8();
                    values.pan = in.U8();
                    const uint8_t region_flags = in.U8();
                    if ((region_flags & ~1u) != 0)
                    {
                        Newer("a region with a flag");
                    }
                    values.ignore_note_off = region_flags != 0;
                    values.pitch = in.F32();
                    values.key_group = in.U8();
                    values.interpolation = in.U8();
                    in.U16();
                    values.adshr = {in.U8(), in.U8(), in.U8(), in.U8(), in.U8()};
                    in.Take(3);
                    keys.regions.push_back(region);
                }
                instrument->key_regions.push_back(std::move(keys));
            }
        }

        // The model has to lay a bank out.
        try
        {
            bank.Write();
        }
        catch (const std::invalid_argument& error)
        {
            Damaged("bank " + std::to_string(id) + " can't be laid out: " + error.what());
        }
        banks.emplace(id, std::move(bank));
    }

    return banks;
}

std::vector<uint8_t> WavesChunk(const File& file)
{
    Out out;
    out.Count(file.waves.size(), "waves");
    for (const auto& [place, wave] : file.waves)
    {
        if (const std::optional<std::string> problem = WaveModelProblem(*wave))
        {
            throw std::invalid_argument("wave " + std::to_string(place.index) + " of file " +
                                        std::to_string(place.file) + " " + *problem);
        }

        out.U32(place.file);
        out.U32(place.index);
        out.U8(static_cast<uint8_t>(wave->encoding));
        out.U8(static_cast<uint8_t>((wave->loop ? kLoopingWave : 0) | (wave->samples.empty() ? 0 : kWaveSamples)));
        out.U16(0);
        out.U32(wave->rate);
        out.U32(wave->loop_start);
        out.U32(wave->loop_end);
        out.U32(wave->length);
        if (!wave->samples.empty())
        {
            out.U32(static_cast<uint32_t>(wave->samples.size()));
            for (const std::vector<int16_t>& channel : wave->samples)
            {
                for (const int16_t sample : channel)
                {
                    out.U16(static_cast<uint16_t>(sample));
                }
            }
        }
    }

    return out.Take();
}

std::map<WavePlace, Shared<WaveModel>> ReadWaves(std::span<const uint8_t> chunk)
{
    In in(chunk, "the WAVE chunk");
    const uint32_t count = in.U32();
    std::map<WavePlace, Shared<WaveModel>> waves;
    for (uint32_t i = 0; i < count; ++i)
    {
        WavePlace place;
        place.file = in.U32();
        place.index = in.U32();
        if (!waves.empty() && place <= waves.rbegin()->first)
        {
            Damaged("the places of its waves don't rise");
        }

        WaveModel wave;
        const uint8_t encoding = in.U8();
        const uint8_t flags = in.U8();
        in.U16();
        if (encoding > static_cast<uint8_t>(ntdlib::sar::WaveEncoding::kDspAdpcm))
        {
            Newer("a wave of an encoding");
        }
        if ((flags & ~(kLoopingWave | kWaveSamples)) != 0)
        {
            Newer("a wave with a flag");
        }
        wave.encoding = static_cast<ntdlib::sar::WaveEncoding>(encoding);
        wave.loop = (flags & kLoopingWave) != 0;
        wave.rate = in.U32();
        wave.loop_start = in.U32();
        wave.loop_end = in.U32();
        wave.length = in.U32();

        // The PCM16 samples, channel after channel. The chunk has to hold them before they take memory.
        if ((flags & kWaveSamples) != 0)
        {
            const uint32_t channels = in.U32();
            if (channels == 0 || channels > kMostWaveChannels || wave.length > in.Left() / 2 / channels)
            {
                Damaged("wave " + std::to_string(place.index) + " of file " + std::to_string(place.file) +
                        " holds more samples than its chunk");
            }
            wave.samples.assign(channels, std::vector<int16_t>(wave.length));
            for (std::vector<int16_t>& channel : wave.samples)
            {
                for (int16_t& sample : channel)
                {
                    sample = static_cast<int16_t>(in.U16());
                }
            }
        }

        if (const std::optional<std::string> problem = WaveModelProblem(wave))
        {
            Damaged("wave " + std::to_string(place.index) + " of file " + std::to_string(place.file) + " " + *problem);
        }
        waves.emplace(place, std::move(wave));
    }

    return waves;
}

std::optional<std::string> WaveModelProblem(const WaveModel& wave)
{
    if (wave.encoding > ntdlib::sar::WaveEncoding::kDspAdpcm)
    {
        return std::string("is in IMA-ADPCM or another encoding that Hokuto doesn't write");
    }
    if (wave.rate == 0)
    {
        return std::string("plays at a rate of 0");
    }
    if (wave.loop_end == 0)
    {
        return std::string("plays no samples");
    }
    if (wave.loop_end > wave.length)
    {
        return std::string("ends its loop after its last sample");
    }
    if (wave.loop ? wave.loop_start >= wave.loop_end : wave.loop_start != 0)
    {
        return std::string("has a loop that doesn't end after its start");
    }
    if (wave.samples.empty())
    {
        return std::nullopt;
    }

    const auto whole = [&wave](const std::vector<int16_t>& channel)
    {
        return channel.size() == wave.length;
    };
    if (wave.samples.size() > kMostWaveChannels || !std::all_of(wave.samples.begin(), wave.samples.end(), whole))
    {
        return std::string("holds other samples than one or two channels of its length");
    }

    return std::nullopt;
}

std::map<uint32_t, NativeStream> ReadStreams(std::span<const uint8_t> chunk)
{
    In in(chunk, "the STRM chunk");
    const uint32_t count = in.U32();
    std::map<uint32_t, NativeStream> streams;
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t id = in.U32();
        const uint32_t flags = in.U32();
        if (!streams.empty() && id <= streams.rbegin()->first)
        {
            Damaged("the file IDs of its streams don't rise");
        }
        if ((flags & ~kHokutoStream) != 0)
        {
            Newer("a stream with a flag");
        }

        const std::span<const uint8_t> bytes = in.Take(in.U64());
        streams[id] = NativeStream{std::vector<uint8_t>(bytes.begin(), bytes.end()), (flags & kHokutoStream) == 0};
    }

    return streams;
}

std::vector<uint8_t> Write(const File& file)
{
    if (const std::optional<std::size_t> sequence = UnfiledChoices(file))
    {
        throw std::invalid_argument(
            "sequence " + std::to_string(*sequence) +
            " holds the encoder's choices, and no sequence file of the native archive or its group files names it");
    }

    // The chunks in order: Hokuto's chunks, then the chunks that Hokuto doesn't read.
    std::vector<std::pair<std::string, std::vector<uint8_t>>> chunks;
    chunks.emplace_back(kEngineChunk, EngineChunk(file));
    const auto has_effects = [](const std::vector<Effect>& effects)
    {
        return !effects.empty();
    };
    if (std::any_of(file.aux_effects.begin(), file.aux_effects.end(), has_effects) ||
        std::any_of(file.aux_volumes.begin(), file.aux_volumes.end(), NotUnity))
    {
        chunks.emplace_back(kMixerChunk, MixerChunk(file));
    }
    chunks.emplace_back(kSequencesChunk, SequencesChunk(file));

    // The archive in its form: the model's sounds and items, or the native layout.
    const NativeArchive* native = file.native && file.form == ArchiveForm::kNative ? &*file.native : nullptr;
    if (file.native && file.form == ArchiveForm::kModel)
    {
        const Model model = ModelOf(file);
        chunks.emplace_back(kSoundsChunk, SoundsChunk(model));
        chunks.emplace_back(kItemsChunk, ItemsChunk(model));
    }
    if (native)
    {
        chunks.emplace_back(kNativeChunk, NativeChunk(file));
    }
    if (native && native->wii_build)
    {
        if (!native->wii)
        {
            throw std::invalid_argument("a build of the Wii's sound library belongs to an archive of the Wii");
        }

        chunks.emplace_back(kBuildChunk, BuildChunk(*native->wii_build));
    }
    if (native && native->tables)
    {
        chunks.emplace_back(kTablesChunk, TablesChunk(*native->tables));
    }
    if (native && native->game_sizes)
    {
        if (const std::optional<std::string> problem = GameSizesProblem(*native))
        {
            throw std::invalid_argument("the game's sizes " + *problem);
        }

        chunks.emplace_back(kSizesChunk, SizesChunk(*native->game_sizes));
    }
    if (native && !native->group_files.empty())
    {
        chunks.emplace_back(kGroupFilesChunk, GroupFilesChunk(file));
    }
    if (native && !native->streams.empty())
    {
        chunks.emplace_back(kStreamsChunk, StreamsChunk(*native));
    }
    if (!file.banks.empty())
    {
        chunks.emplace_back(kBanksChunk, BanksChunk(file));
    }
    if (!file.waves.empty())
    {
        chunks.emplace_back(kWavesChunk, WavesChunk(file));
    }
    if (!file.scenes.empty())
    {
        chunks.emplace_back(kScenesChunk, ScenesChunk(file));
    }
    if (!file.track_colours.empty())
    {
        chunks.emplace_back(kTrackColoursChunk, TrackColoursChunk(file));
    }
    if (file.view)
    {
        chunks.emplace_back(kEditorChunk, EditorChunk(*file.view));
    }
    const auto tagged = [](const auto& entry)
    {
        return !entry.second.empty();
    };
    if (!file.tags.empty() || std::any_of(file.sound_tags.begin(), file.sound_tags.end(), tagged))
    {
        chunks.emplace_back(kTagsChunk, TagsChunk(file));
    }
    if (!file.lengths.empty())
    {
        chunks.emplace_back(kLengthsChunk, LengthsChunk(file));
    }
    if (file.timelines)
    {
        chunks.emplace_back(kTimelinesChunk, TimelinesChunk(file));
    }
    for (const OtherChunk& chunk : file.other_chunks)
    {
        if (chunk.id.size() != 4 || IsHokutoChunk(chunk.id))
        {
            throw std::invalid_argument("a chunk that Hokuto doesn't read can't have the ID " + chunk.id);
        }

        chunks.emplace_back(chunk.id, chunk.bytes);
    }

    // Where each chunk starts: after the header and the table of chunks, on a multiple of 8 bytes.
    std::vector<std::size_t> offsets;
    std::size_t end = kHeaderSize + chunks.size() * kEntrySize;
    for (const auto& [id, bytes] : chunks)
    {
        end = (end + kChunkAlignment - 1) / kChunkAlignment * kChunkAlignment;
        offsets.push_back(end);
        end += bytes.size();
    }

    Out out;
    out.Bytes(kSignature);
    out.U16(kMajorVersion);
    out.U16(kMinorVersion);
    out.U32(0);
    out.Count(chunks.size(), "chunks");
    out.U32(0);
    for (std::size_t i = 0; i < chunks.size(); ++i)
    {
        const auto& [id, bytes] = chunks[i];
        out.Bytes(std::span(reinterpret_cast<const uint8_t*>(id.data()), 4));
        out.U32(Crc32(bytes));
        out.U64(offsets[i]);
        out.U64(bytes.size());
    }
    for (std::size_t i = 0; i < chunks.size(); ++i)
    {
        out.Zeros(offsets[i] - out.Size());
        out.Bytes(chunks[i].second);
    }

    return out.Take();
}

File Read(std::span<const uint8_t> bytes)
{
    if (!IsBinary(bytes))
    {
        throw xsflib::FormatError("It isn't a Hokuto file.");
    }

    In header(bytes.subspan(kSignature.size()), "the header");
    const uint16_t major = header.U16();
    header.U16();
    header.U32();
    const uint32_t count = header.U32();
    header.U32();
    if (major != kMajorVersion)
    {
        throw xsflib::FormatError("A later version of Hokuto made the file, with version " + std::to_string(major) +
                                  " of the format. This version reads version " + std::to_string(kMajorVersion) + ".");
    }

    // The chunks, checked against their CRC-32s.
    File file;
    std::map<std::string, std::span<const uint8_t>> known;
    for (uint32_t i = 0; i < count; ++i)
    {
        const std::span<const uint8_t> entry = header.Take(kEntrySize);
        const std::string id(reinterpret_cast<const char*>(entry.data()), 4);
        const uint32_t crc = xsflib::LoadLe32(&entry[4]);
        const uint64_t offset = xsflib::LoadLe64(&entry[8]);
        const uint64_t size = xsflib::LoadLe64(&entry[16]);
        if (offset > bytes.size() || size > bytes.size() - offset)
        {
            Damaged("chunk " + id + " lies past the end");
        }

        const std::span<const uint8_t> chunk =
            bytes.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(size));
        if (Crc32(chunk) != crc)
        {
            Damaged("chunk " + id + " doesn't match its CRC-32");
        }

        if (IsHokutoChunk(id))
        {
            if (!known.emplace(id, chunk).second)
            {
                Damaged("it holds two " + id + " chunks");
            }
        }
        else
        {
            file.other_chunks.push_back({id, std::vector<uint8_t>(chunk.begin(), chunk.end())});
        }
    }

    // The engine, its mixer, the scenes and the tracks' colours, then the sequences, the native archive that names
    // them, its tables and its group files.
    if (!known.contains(std::string(kEngineChunk)))
    {
        Damaged("it has no engine");
    }

    ReadEngine(known.at(std::string(kEngineChunk)), file);
    if (const auto mixer = known.find(std::string(kMixerChunk)); mixer != known.end())
    {
        ReadMixer(mixer->second, file);
    }
    if (const auto scenes = known.find(std::string(kScenesChunk)); scenes != known.end())
    {
        ReadScenes(scenes->second, file);
    }
    if (const auto colours = known.find(std::string(kTrackColoursChunk)); colours != known.end())
    {
        ReadTrackColours(colours->second, file);
    }
    if (const auto view = known.find(std::string(kEditorChunk)); view != known.end())
    {
        ReadEditor(view->second, file);
    }
    if (const auto tags = known.find(std::string(kTagsChunk)); tags != known.end())
    {
        ReadTags(tags->second, file);
    }
    if (const auto lengths = known.find(std::string(kLengthsChunk)); lengths != known.end())
    {
        ReadLengths(lengths->second, file);
    }
    std::vector<Sequence> sequences;
    if (const auto chunk = known.find(std::string(kSequencesChunk)); chunk != known.end())
    {
        sequences = ReadSequences(chunk->second);
    }
    std::optional<NativeArchive> native;
    if (const auto chunk = known.find(std::string(kNativeChunk)); chunk != known.end())
    {
        native = ReadNative(chunk->second, sequences);
    }
    if (const auto build = known.find(std::string(kBuildChunk)); build != known.end())
    {
        if (!native || !native->wii)
        {
            Damaged("it holds a build of the Wii's sound library without a native archive of the Wii");
        }

        native->wii_build = ReadBuild(build->second);
    }
    if (const auto tables = known.find(std::string(kTablesChunk)); tables != known.end())
    {
        if (!native)
        {
            Damaged("it holds tables without a native archive");
        }

        native->tables = ReadTables(tables->second);
    }
    if (const auto sizes = known.find(std::string(kSizesChunk)); sizes != known.end())
    {
        if (!native)
        {
            Damaged("it holds the game's sizes without a native archive");
        }

        native->game_sizes = ReadSizes(sizes->second);
        if (const std::optional<std::string> problem = GameSizesProblem(*native))
        {
            Damaged("the game's sizes " + *problem);
        }
    }
    if (const auto group_files = known.find(std::string(kGroupFilesChunk)); group_files != known.end())
    {
        if (!native)
        {
            Damaged("it holds group files without a native archive");
        }

        native->group_files = ReadGroupFiles(group_files->second, sequences);
    }
    if (const auto streams = known.find(std::string(kStreamsChunk)); streams != known.end())
    {
        if (!native)
        {
            Damaged("it holds streams without a native archive");
        }

        native->streams = ReadStreams(streams->second);
    }

    // A file in the model form holds the model's sounds and items, and no native layout.
    const auto sounds = known.find(std::string(kSoundsChunk));
    const auto items = known.find(std::string(kItemsChunk));
    std::optional<Model> model;
    if (sounds != known.end() || items != known.end())
    {
        if (sounds == known.end() || items == known.end())
        {
            Damaged("it holds only one of the model's SNDS and ITMS chunks");
        }
        if (native)
        {
            Damaged("it holds its archive in both forms");
        }

        model = ReadModel(sounds->second, items->second);
    }

    // Reading the native archive fills in the sequences' choices. The file shares the parts once they're whole. The
    // model builds the native archive from the sequences.
    file.sequences = ShareSequences(std::move(sequences));
    if (native)
    {
        file.native = std::move(*native);
    }
    if (model)
    {
        file = WithModel(std::move(file), *model);
    }
    if (const auto timelines = known.find(std::string(kTimelinesChunk)); timelines != known.end())
    {
        ReadTimelines(timelines->second, file);
    }

    // The models of the edited banks and waves name the archive's files.
    const auto banks = known.find(std::string(kBanksChunk));
    const auto waves = known.find(std::string(kWavesChunk));
    if (!file.native && (banks != known.end() || waves != known.end()))
    {
        Damaged("it holds banks or waves without an archive");
    }
    if (banks != known.end())
    {
        file.banks = ReadBanks(banks->second);
    }
    if (waves != known.end())
    {
        file.waves = ReadWaves(waves->second);
    }

    return file;
}

bool IsBinary(std::span<const uint8_t> bytes)
{
    return bytes.size() >= kHeaderSize && std::equal(kSignature.begin(), kSignature.end(), bytes.begin());
}

bool HasChoices(const Command& item)
{
    return item.encoding.value_bytes != 0 || item.encoding.unused_time != 0 || item.encoding.unused_random ||
           item.encoding.unused_variable;
}

std::optional<std::size_t> UnfiledChoices(const File& file)
{
    std::vector<bool> filed(file.sequences.size());
    const auto file_pieces = [&filed](std::span<const ntdlib::sar::Piece> pieces)
    {
        for (const ntdlib::sar::Piece& piece : pieces)
        {
            const auto* sequence_piece = std::get_if<ntdlib::sar::SequencePiece>(&piece);
            if (sequence_piece && sequence_piece->sequence < filed.size())
            {
                filed[sequence_piece->sequence] = true;
            }
        }
    };
    if (file.native)
    {
        file_pieces(*file.native->pieces);
        for (const NativeGroupFile& group_file : file.native->group_files)
        {
            file_pieces(*group_file.pieces);
        }
    }

    for (std::size_t s = 0; s < file.sequences.size(); ++s)
    {
        const std::vector<Command>& items = file.sequences[s]->items;
        if (!filed[s] && std::any_of(items.begin(), items.end(), HasChoices))
        {
            return s;
        }
    }

    return std::nullopt;
}

std::optional<std::string> EffectProblem(const Effect& effect)
{
    const std::size_t count = ParameterNamesOf(effect.kind).size();
    if (const std::size_t given = effect.parameters.size(); given != count)
    {
        return "has " + std::to_string(given) + (given == 1 ? " parameter" : " parameters") + ", and its kind has " +
               std::to_string(count);
    }

    const auto finite = [](float parameter)
    {
        return std::isfinite(parameter);
    };
    if (!std::all_of(effect.parameters.begin(), effect.parameters.end(), finite))
    {
        return std::string("has a parameter that isn't a finite number");
    }

    // A mode is a whole number from 0 to one less than its choices. A parameter with limits lies within them, a whole
    // number of steps from the lowest value where the parameter moves in steps.
    for (std::size_t p = 0; p < count; ++p)
    {
        const uint32_t choices = ChoicesOf(effect.kind, p);
        const float value = effect.parameters[p];
        const std::string name(ParameterNamesOf(effect.kind)[p]);
        if (choices > 0 && (value < 0.0f || value >= static_cast<float>(choices) || value != std::floor(value)))
        {
            return "has a value of " + name + " that isn't a whole number from 0 to " + std::to_string(choices - 1);
        }

        const std::optional<ParameterLimits> limits = LimitsOf(effect.kind, p);
        if (!limits)
        {
            continue;
        }

        // The limits are whole numbers.
        const auto whole = [](float bound)
        {
            return std::to_string(static_cast<int64_t>(bound));
        };

        const float steps = limits->step > 0.0f ? (value - limits->low) / limits->step : 0.0f;
        if (value < limits->low || value > limits->high || steps != std::floor(steps))
        {
            if (limits->step == 0.0f)
            {
                return "has a value of " + name + " that lies outside " + whole(limits->low) + " to " +
                       whole(limits->high);
            }

            const std::string range = " from " + whole(limits->low) + " to " + whole(limits->high);
            if (limits->step == 1.0f)
            {
                return "has a value of " + name + " that isn't a whole number" + range;
            }

            return "has a value of " + name + " that isn't a multiple of " + whole(limits->step) + range;
        }
    }

    return std::nullopt;
}

std::optional<std::string> AuxVolumeProblem(float volume)
{
    if (!std::isfinite(volume))
    {
        return std::string("isn't a finite number");
    }
    if (volume < 0.0f || volume > kMaxAuxVolume)
    {
        return std::string("lies outside 0 to 2");
    }

    return std::nullopt;
}

std::optional<std::string> SceneProblem(const Scene& scene)
{
    const auto past = [](int variable) -> std::optional<std::string>
    {
        if (variable < kSceneVariables)
        {
            return std::nullopt;
        }

        return "sets variable " + std::to_string(variable) + ", and a scene sets variables 0 to " +
               std::to_string(kSceneVariables - 1) + " only";
    };
    for (const auto& [variable, value] : scene.values)
    {
        if (std::optional<std::string> problem = past(variable))
        {
            return problem;
        }
    }
    for (const VariableChange& change : scene.changes)
    {
        if (std::optional<std::string> problem = past(change.variable))
        {
            return problem;
        }
    }

    const auto earlier = [](const VariableChange& first, const VariableChange& second)
    {
        return first.time < second.time;
    };
    if (!std::is_sorted(scene.changes.begin(), scene.changes.end(), earlier))
    {
        return std::string("changes its variables out of the order of their times");
    }

    return std::nullopt;
}

std::optional<std::string> GameSizesProblem(const NativeArchive& native)
{
    const std::size_t groups = native.tables ? native.tables->groups.size() : 0;
    if (!native.game_sizes || native.game_sizes->groups.size() == groups)
    {
        return std::nullopt;
    }

    const auto counted = [](std::size_t count)
    {
        return std::to_string(count) + (count == 1 ? " group" : " groups");
    };
    const std::string covered = "cover " + counted(native.game_sizes->groups.size());
    if (!native.tables)
    {
        return covered + ", and the native archive has no tables";
    }

    return covered + ", and the tables list " + counted(groups);
}

File WithTag(File file, std::optional<uint32_t> sound, const std::string& key, const std::string& value)
{
    Tags& tags = sound ? file.sound_tags[*sound] : file.tags;
    if (value.empty())
    {
        tags.erase(key);
    }
    else
    {
        tags[key] = value;
    }
    if (sound && tags.empty())
    {
        file.sound_tags.erase(*sound);
    }

    return file;
}

Mixing MixingOf(const File& file, uint32_t sound)
{
    const auto found = file.sound_mixing.find(sound);
    return found != file.sound_mixing.end() ? found->second : file.mixing;
}

File WithMixing(File file, uint32_t sound, Mixing mixing)
{
    if (mixing == file.mixing)
    {
        file.sound_mixing.erase(sound);
    }
    else
    {
        file.sound_mixing[sound] = mixing;
    }

    return file;
}

File WithMixing(File file, Mixing mixing)
{
    file.mixing = mixing;
    file.sound_mixing.clear();

    return file;
}

std::optional<std::string> MixingRefused(Engine engine, Mixing mixing)
{
    if (engine == Engine::kWii && mixing == Mixing::kEnhanced)
    {
        return "AX's microcode mixes the Wii's sounds. Hokuto has no enhanced mixing for the Wii yet.";
    }

    return std::nullopt;
}

Engine EngineOf(const File& file, uint32_t sound)
{
    const auto found = file.sound_engines.find(sound);
    return found != file.sound_engines.end() ? found->second : file.engine;
}

File WithEngine(File file, uint32_t sound, Engine engine)
{
    if (engine == file.engine)
    {
        file.sound_engines.erase(sound);
    }
    else
    {
        file.sound_engines[sound] = engine;
    }

    return file;
}

std::optional<std::string> LengthProblem(const SoundLength& length)
{
    if (length.loop && length.loops == 0)
    {
        return std::string("give a loop's ticks without loops");
    }
    if (length.loop && length.loop->end <= length.loop->start)
    {
        return "give a loop whose end at tick " + std::to_string(length.loop->end) +
               " doesn't come after its start at tick " + std::to_string(length.loop->start);
    }

    return std::nullopt;
}

File WithoutStaleLengths(const File& before, File after)
{
    const auto shared = [&before, &after]
    {
        for (std::size_t i = 0; i < after.sequences.size(); i++)
        {
            if (!after.sequences[i].SharesWith(before.sequences[i]))
            {
                return false;
            }
        }

        return true;
    };
    if (after.engine != before.engine || after.aux_effects != before.aux_effects ||
        after.aux_volumes != before.aux_volumes || after.sequences.size() != before.sequences.size() || !shared() ||
        !after.native.SharesWith(before.native))
    {
        after.lengths.clear();
    }

    return after;
}

std::optional<std::string> TagProblem(std::string_view key, std::string_view value)
{
    const auto lower = [](char c)
    {
        return c >= 'a' && c <= 'z';
    };
    const auto allowed = [&lower](char c)
    {
        return lower(c) || (c >= '0' && c <= '9') || c == '_';
    };
    if (key.empty())
    {
        return "a tag has no key";
    }
    if (key.size() > kLongestTagKey || !lower(key.front()) || !std::all_of(key.begin(), key.end(), allowed))
    {
        return "tag " + std::string(key) + " has a key other than 1 to " + std::to_string(kLongestTagKey) +
               " lower-case letters, digits and underscores from a letter on";
    }
    if (value.empty())
    {
        return "tag " + std::string(key) + " has no value";
    }

    return std::nullopt;
}

std::optional<std::string> TimelinesProblem(const File& file)
{
    if (!file.timelines)
    {
        return std::nullopt;
    }

    const auto rising = [](const std::vector<uint32_t>& numbers)
    {
        return std::adjacent_find(numbers.begin(), numbers.end(), std::greater_equal<>()) == numbers.end();
    };
    for (const auto& [sequence, hints] : *file.timelines)
    {
        const std::string of = " of sequence " + std::to_string(sequence);
        if (sequence >= file.sequences.size())
        {
            return "name sequence " + std::to_string(sequence) + ", and the file has " +
                   std::to_string(file.sequences.size());
        }
        for (const BarRun& run : hints.bars)
        {
            if (run.length == 0 || run.count == 0)
            {
                return "hold a run of bars" + of + " with no ticks or no bars";
            }
        }
        for (std::size_t i = 0; i < hints.tracks.size(); ++i)
        {
            const TrackHints& track = hints.tracks[i];
            if (i > 0 && track.track <= hints.tracks[i - 1].track)
            {
                return "give the hints of the tracks" + of + " out of the order of their numbers";
            }
            if (!rising(track.kept_inline) || !rising(track.called))
            {
                return "give the bars of track " + std::to_string(track.track) + of + " out of order";
            }
        }
    }

    return std::nullopt;
}

std::optional<std::string> ViewProblem(const EditorView& view)
{
    const std::array<float, 11> numbers = {view.position,     view.cycle_start, view.cycle_end, view.beat_width,
                                           view.track_height, view.left,        view.top,       view.key_beat_width,
                                           view.key_height,   view.key_left,    view.key_top};
    const std::array<float, 4> sizes = {view.beat_width, view.track_height, view.key_beat_width, view.key_height};
    const auto finite = [](float number)
    {
        return std::isfinite(number);
    };
    const auto positive = [](float size)
    {
        return size > 0.0f;
    };
    if (!std::all_of(numbers.begin(), numbers.end(), finite))
    {
        return "holds a number that isn't finite";
    }
    if (!std::all_of(sizes.begin(), sizes.end(), positive))
    {
        return "holds a size of 0 or less";
    }

    for (const std::optional<uint8_t> track : {view.selected_track, view.key_editor_track, view.list_editor_track})
    {
        if (track && *track > kLastViewTrack)
        {
            return "names track " + std::to_string(*track) + ", and tracks run from 0 to " +
                   std::to_string(kLastViewTrack);
        }
    }

    return std::nullopt;
}

std::optional<std::string> FieldProblem(const NativeArchive& native, std::span<const Sequence> sequences,
                                        const ntdlib::sar::Field& field)
{
    return FieldProblem(*native.pieces, sequences, field);
}

std::optional<std::string> FieldProblem(std::span<const ntdlib::sar::Piece> pieces, std::span<const Sequence> sequences,
                                        const ntdlib::sar::Field& field)
{
    const auto* at = field.at.piece < pieces.size() && !field.at.item && !field.at.sequence
                         ? std::get_if<std::vector<uint8_t>>(&pieces[field.at.piece])
                         : nullptr;
    if (!at || field.at.offset + std::size_t{4} > at->size())
    {
        return "lies outside a run of bytes";
    }

    // A field can name two items of a sequence without a piece among the pieces, such as a sequence file that only
    // group files hold.
    if (field.from.sequence || field.to.sequence)
    {
        const uint32_t sequence = field.from.piece;
        if (!field.from.sequence || !field.to.sequence || !field.from.item || !field.to.item ||
            field.to.piece != sequence)
        {
            return "names items of a sequence and something else";
        }
        if (sequence >= sequences.size())
        {
            return "names sequence " + std::to_string(sequence) + ", and the file has " +
                   std::to_string(sequences.size());
        }
        for (const ntdlib::sar::Place& place : {field.from, field.to})
        {
            if (place.offset > sequences[sequence].items.size())
            {
                return "names item " + std::to_string(place.offset) + ", past the end of sequence " +
                       std::to_string(sequence);
            }
        }

        return std::nullopt;
    }

    for (const ntdlib::sar::Place& place : {field.from, field.to})
    {
        const std::string piece = "piece " + std::to_string(place.piece);
        if (place.piece > pieces.size())
        {
            return "names " + piece + ", and the archive has " + std::to_string(pieces.size());
        }
        if (place.item)
        {
            const auto* file =
                place.piece < pieces.size() ? std::get_if<ntdlib::sar::SequencePiece>(&pieces[place.piece]) : nullptr;
            if (!file)
            {
                return "names an item of " + piece + ", and that piece isn't a sequence file";
            }
            if (file->sequence < sequences.size() && place.offset > sequences[file->sequence].items.size())
            {
                return "names item " + std::to_string(place.offset) + ", past the end of sequence " +
                       std::to_string(file->sequence);
            }
            continue;
        }

        const auto* run =
            place.piece < pieces.size() ? std::get_if<std::vector<uint8_t>>(&pieces[place.piece]) : nullptr;
        if (place.offset != 0 && place.piece < pieces.size() && (!run || place.offset >= run->size()))
        {
            return "names byte " + std::to_string(place.offset) + " of " + piece + ", outside its bytes";
        }
    }

    return std::nullopt;
}

uint32_t Crc32(std::span<const uint8_t> bytes)
{
    // The reflected polynomial 0x04C11DB7, a byte at a time.
    static const std::array<uint32_t, 256> kTable = []
    {
        std::array<uint32_t, 256> table{};
        for (uint32_t i = 0; i < 256; ++i)
        {
            uint32_t c = i;
            for (int bit = 0; bit < 8; ++bit)
            {
                c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            table[i] = c;
        }

        return table;
    }();

    uint32_t crc = 0xFFFFFFFFu;
    for (const uint8_t byte : bytes)
    {
        crc = kTable[(crc ^ byte) & 0xFF] ^ (crc >> 8);
    }

    return crc ^ 0xFFFFFFFFu;
}

} // namespace hokuto::format
