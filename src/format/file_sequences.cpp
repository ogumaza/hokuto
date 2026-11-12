// SPDX-License-Identifier: MIT

// The chunks of the binary form that contain the sequences, the native archive and the group files: their items,
// pieces and fields.

#include "format/file_parts.h"

namespace hokuto::format
{
namespace
{

using file_parts::Argument;
using file_parts::Command;
using file_parts::Damaged;
using file_parts::In;
using file_parts::kBytesPiece;
using file_parts::kConditional;
using file_parts::kHasBytes;
using file_parts::kHasNote;
using file_parts::kHasOpcode;
using file_parts::kHasSample;
using file_parts::kHasTarget;
using file_parts::kHasTime;
using file_parts::kHasValue;
using file_parts::Kind;
using file_parts::kItemFlags;
using file_parts::kItemPlace;
using file_parts::kNoGroup;
using file_parts::kPaddingPiece;
using file_parts::kRepeated;
using file_parts::kSequencePiece;
using file_parts::kSequencePlace;
using file_parts::kTargetOutside;
using file_parts::kTimeSourceShift;
using file_parts::kTwoByteField;
using file_parts::kUnusedRandom;
using file_parts::kUnusedVariable;
using file_parts::kValueSourceShift;
using file_parts::Newer;
using file_parts::Out;
using file_parts::Sequence;
using file_parts::Source;

// The record of the encoding of each item that a sequence piece contains: the choices of NintendoWare and NW4R SND,
// those of the GBA's drivers (the bytes of the value and the length codes of MP2K, and the arguments of the others), or
// those of DSE.
enum class EncodingRecord
{
    kNwsnd,
    kGba,
    kDse,
};

// The record of the encodings of a native archive of `engine`, whose driver is `driver` on the GBA's other drivers.
EncodingRecord EncodingRecordOf(Engine engine, const Driver* driver)
{
    if (engine == Engine::kGba || driver != nullptr)
    {
        return EncodingRecord::kGba;
    }

    return engine == Engine::kDse ? EncodingRecord::kDse : EncodingRecord::kNwsnd;
}

uint16_t SourceCode(Source source)
{
    return static_cast<uint16_t>(source);
}

Source SourceOfCode(int code)
{
    if (code > static_cast<int>(Source::kVariable))
    {
        Newer("a value source");
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
    flags |= item.sample ? kHasSample : 0;

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

    if (item.sample)
    {
        out.U16(*item.sample);
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
        Newer("a command part");
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

    if ((flags & kHasSample) != 0)
    {
        item.sample = in.U16();
    }

    return item;
}

} // namespace

namespace file_parts
{

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
            record.Name(label.name, "bytes in a label name");
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
            Damaged("sequence " + std::to_string(s) + " lists more items than it contains");
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

} // namespace file_parts

namespace
{

// A sequence piece: the sequence, the file's version, and the encodings of its items in `record`. The first piece of
// each sequence holds the encodings, and the others none. In a GBA ROM, the room of the song's place is written in the
// place of the version.
void WriteSequencePiece(Out& out, const ntdlib::sar::SequencePiece& piece, const Sequence& sequence, bool first,
                        EncodingRecord record)
{
    out.U32(piece.sequence);
    out.U32(record == EncodingRecord::kGba ? piece.room : piece.version);
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
        if (record == EncodingRecord::kGba)
        {
            out.U8(encoding.arguments ? static_cast<uint8_t>(*encoding.arguments + 1) : 0);
            out.U8(encoding.length_code ? static_cast<uint8_t>(*encoding.length_code + 1) : 0);
            out.U8(encoding.repeated ? kRepeated : 0);
            out.U8(encoding.value_bytes);
            continue;
        }

        if (record == EncodingRecord::kDse)
        {
            out.U8(encoding.arguments ? static_cast<uint8_t>(*encoding.arguments + 1) : 0);
            out.U8(encoding.value_bytes);
            out.U8(encoding.repeated ? kRepeated : 0);
            out.U8(0);
            continue;
        }

        out.U8(encoding.value_bytes);
        out.U8(encoding.unused_time);
        out.U8(static_cast<uint8_t>((encoding.unused_random ? kUnusedRandom : 0) |
                                    (encoding.unused_variable ? kUnusedVariable : 0)));
        out.U8(0);
    }
}

// Pieces and their count. Each piece is its type, three zeros, its size and its body: a run's bytes, a sequence piece,
// or padding's alignment. `written` marks the sequences whose encodings an earlier piece holds. `record` selects the
// record of the encodings.
void WritePieces(Out& out, std::span<const ntdlib::sar::Piece> pieces, const File& file, std::vector<bool>& written,
                 const std::string& what, EncodingRecord record = EncodingRecord::kNwsnd)
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

            WriteSequencePiece(out, *sequence_piece, *file.sequences[sequence], !written[sequence], record);
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
        const auto write_place = [&out](const ntdlib::sar::Place& place, uint8_t flags)
        {
            out.U32(place.piece);
            out.U32(place.offset);
            out.U8((place.item ? kItemPlace : 0) | (place.sequence ? kSequencePlace : 0) | flags);
            out.Zeros(3);
        };
        write_place(field.at, field.bytes == 2 ? kTwoByteField : 0);
        write_place(field.from, 0);
        write_place(field.to, 0);
    }
}

} // namespace

namespace file_parts
{

// NATV: the archive's kind, its flags, its pieces and its fields.
std::vector<uint8_t> NativeChunk(const File& file)
{
    const NativeArchive& native = *file.native;
    if (native.gba_filler && native.engine != Engine::kGba && !native.driver)
    {
        throw std::invalid_argument("only a GBA ROM's songs can go into filler runs");
    }
    if (native.gba_note_arguments < 2 || native.gba_note_arguments > 3)
    {
        throw std::invalid_argument("a note of MusicPlayer2000 reads 2 or 3 argument bytes");
    }
    if (native.gba_note_arguments == 2 && native.engine != Engine::kGba)
    {
        throw std::invalid_argument("only a GBA ROM with MusicPlayer2000 holds notes of 2 argument bytes");
    }

    const Driver* driver = WrittenDriverOf(native);

    Out out;
    out.U8(CodeOf(native.engine));
    out.U8(static_cast<uint8_t>((native.gba_filler ? kGbaFiller : 0) |
                                (native.gba_note_arguments == 2 ? kGbaTwoByteNotes : 0)));
    out.Zeros(2);
    std::vector<bool> written(file.sequences.size(), false);
    WritePieces(out, *native.pieces, file, written, "pieces of the native archive",
                EncodingRecordOf(native.engine, driver));
    WriteFields(out, native.fields, "fields of the native archive");
    if (native.driver)
    {
        // The driver's revision, and where the songs that move can go.
        out.U8(native.driver->revision);
        out.U8(native.driver->padding);
        out.Zeros(2);
        out.U32(native.driver->free_start);
        out.Count(native.driver->filler.size(), "filler runs");
        for (const FillerRun& run : native.driver->filler)
        {
            out.U32(run.start);
            out.U32(run.size);
        }

        if (driver->Facts().rooms)
        {
            // The rooms of the lists of songs.
            out.Count(native.driver->rooms.size(), "rooms");
            for (const DriverRoom& room : native.driver->rooms)
            {
                out.U32(room.piece);
                out.U32(room.end);
                out.U8(room.fill);
                out.Zeros(3);
            }
        }
    }

    return out.Take();
}

} // namespace file_parts

namespace
{

ntdlib::sar::SequencePiece ReadSequencePiece(In& in, std::vector<Sequence>& sequences, EncodingRecord record)
{
    ntdlib::sar::SequencePiece piece;
    piece.sequence = in.U32();
    if (piece.sequence >= sequences.size())
    {
        Damaged("the native archive names sequence " + std::to_string(piece.sequence) + ", and the file has " +
                std::to_string(sequences.size()));
    }

    std::vector<Command>& items = sequences[piece.sequence].items;
    (record == EncodingRecord::kGba ? piece.room : piece.version) = in.U32();
    const uint32_t encodings = in.U32();
    for (uint32_t i = 0; i < encodings; ++i)
    {
        const uint32_t item = in.U32();
        if (item >= items.size())
        {
            Damaged("an encoding in sequence " + std::to_string(piece.sequence) + " is past its end");
        }

        ntdlib::commands::Encoding& encoding = items[item].encoding;
        if (record == EncodingRecord::kGba)
        {
            const uint8_t arguments = in.U8();
            const uint8_t length_code = in.U8();
            encoding.arguments = arguments != 0 ? std::optional<uint8_t>(arguments - 1) : std::nullopt;
            encoding.length_code = length_code != 0 ? std::optional<uint8_t>(length_code - 1) : std::nullopt;
            encoding.repeated = (in.U8() & kRepeated) != 0;
            encoding.value_bytes = in.U8();
            continue;
        }

        if (record == EncodingRecord::kDse)
        {
            const uint8_t arguments = in.U8();
            encoding.arguments = arguments != 0 ? std::optional<uint8_t>(arguments - 1) : std::nullopt;
            encoding.value_bytes = in.U8();
            encoding.repeated = (in.U8() & kRepeated) != 0;
            in.U8();
            continue;
        }

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
std::vector<ntdlib::sar::Piece> ReadPieces(In& in, std::vector<Sequence>& sequences, const std::string& what,
                                           EncodingRecord record = EncodingRecord::kNwsnd)
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
            pieces.emplace_back(ReadSequencePiece(body, sequences, record));
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

// Fields that WriteFields wrote, among `pieces`. `what` names their archive or group file in a message. Fields of 16
// bits come only with `two_byte_fields`: in a set of DSE files, and on a driver whose facts allow them (Konami's and
// Quintet's).
std::vector<ntdlib::sar::Field> ReadFields(In& in, std::span<const ntdlib::sar::Piece> pieces,
                                           std::span<const Sequence> sequences, const std::string& what,
                                           bool two_byte_fields = false)
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
            const uint8_t known =
                kItemPlace | kSequencePlace | (place == &field.at && two_byte_fields ? kTwoByteField : 0);
            if ((flags & ~known) != 0)
            {
                Newer("a type of location in an archive");
            }

            place->item = (flags & kItemPlace) != 0;
            place->sequence = (flags & kSequencePlace) != 0;
            field.bytes = place == &field.at && (flags & kTwoByteField) != 0 ? 2 : field.bytes;
        }

        if (const std::optional<std::string> problem = FieldProblem(pieces, sequences, field))
        {
            Damaged("field " + std::to_string(f) + " of " + what + " " + *problem);
        }
    }

    return fields;
}

// Reads the fields of `driver`'s ROM that follow the native archive's fields.
void ReadNativeDriver(In& in, NativeArchive& native, const Driver& driver)
{
    NativeDriver& read = *native.driver;
    read.revision = in.U8();
    if (read.revision > driver.Facts().most_revision)
    {
        Newer("a revision of " + std::string(driver.Facts().title) + " (" + std::to_string(read.revision) + ")");
    }

    read.padding = in.U8();
    in.Take(2);
    read.free_start = in.U32();
    const uint32_t runs = in.U32();
    for (uint32_t r = 0; r < runs; ++r)
    {
        FillerRun& run = read.filler.emplace_back();
        run.start = in.U32();
        run.size = in.U32();
    }

    if (!driver.Facts().filler_runs && (native.gba_filler || runs != 0))
    {
        Damaged(std::string(driver.Facts().title) + " can't put songs into filler runs");
    }

    if (driver.Facts().rooms)
    {
        const uint32_t rooms = in.U32();
        for (uint32_t r = 0; r < rooms; ++r)
        {
            DriverRoom& room = read.rooms.emplace_back();
            room.piece = in.U32();
            room.end = in.U32();
            room.fill = in.U8();
            in.Take(3);
            if (room.piece > native.pieces->size())
            {
                Damaged("a room of a list of songs names piece " + std::to_string(room.piece) +
                        ", and the native "
                        "archive has " +
                        std::to_string(native.pieces->size()));
            }
        }
    }
}

} // namespace

namespace file_parts
{

NativeArchive ReadNative(std::span<const uint8_t> chunk, std::vector<Sequence>& sequences)
{
    In in(chunk, "the native archive");
    NativeArchive native;
    // The kind of archive is the code of its engine: 1 to 3 for the flavors, 4 to 7 for the Wii, the GBA, the DS and
    // DSE, and 8 to 14 for the GBA's other drivers.
    const uint8_t code = in.U8();
    const std::optional<Engine> engine = EngineOfCode(code);
    if (!engine)
    {
        Newer("an archive type (" + std::to_string(code) + ")");
    }

    SetEngine(native, *engine);
    const Driver* driver = DriverOf(*engine);
    const uint8_t flags = in.U8();
    if ((flags & ~(kGbaFiller | kGbaTwoByteNotes)) != 0)
    {
        Newer("a native archive flag");
    }

    native.gba_filler = (flags & kGbaFiller) != 0;
    if (native.gba_filler && native.engine != Engine::kGba && !driver)
    {
        Damaged("the native archive uses filler runs, but isn't a GBA ROM");
    }

    native.gba_note_arguments = (flags & kGbaTwoByteNotes) != 0 ? 2 : 3;
    if (native.gba_note_arguments == 2 && native.engine != Engine::kGba)
    {
        Damaged("the native archive holds notes of 2 argument bytes, but it isn't a GBA ROM with MusicPlayer2000");
    }

    in.Take(2);
    std::vector<ntdlib::sar::Piece> pieces =
        ReadPieces(in, sequences, "a piece of the native archive", EncodingRecordOf(native.engine, driver));
    native.fields = ReadFields(in, pieces, sequences, "the native archive",
                               native.engine == Engine::kDse || (driver != nullptr && driver->Facts().two_byte_fields));
    native.pieces = std::move(pieces);
    if (driver)
    {
        ReadNativeDriver(in, native, *driver);
    }

    return native;
}

} // namespace file_parts

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

} // namespace hokuto::format
