// SPDX-License-Identifier: MIT

// The GBA's sound drivers other than MusicPlayer2000 whose ROMs a file can contain, such as Konami's. A file of such a
// ROM uses the driver's engine, and its native layout keeps the driver's revision and free space (NativeDriver).
// ntdlib's module of the driver finds the driver in a ROM, cuts the ROM into pieces and joins them again. The file of a
// driver in this folder contains its class, and DriverOf names each driver once. The song layer plays such a ROM
// (song/driver.h).

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/sar/layout.h"

#include "format/engine.h"

namespace hokuto::format
{

// `bytes` as a GBA ROM for ntdlib's readers of the drivers.
ntdlib::agb::Rom RomOf(std::span<const uint8_t> bytes);

struct File;
struct NativeArchive;

// What tells a driver's files apart, and what its bytes hold.
struct DriverFacts
{
    Engine engine = Engine::kKonami;
    std::string_view title;       // the driver in a sentence, such as "Konami's driver"
    std::string_view engine_name; // the engine in a sentence, such as "Konami's" for "Konami's engine"
    uint8_t most_revision = 0;    // the driver's last revision
    bool two_byte_fields = false; // whether a field of the ROM's tables can hold 2 bytes
    bool key_holds_byte = false;  // whether a note's key stores the byte of its command, up to 255
    bool filler_runs = true;      // whether an export can put songs into filler runs (NativeArchive::gba_filler)
    bool rooms = false;           // whether the songs are in lists that can't grow past their rooms (NativeDriver)
    uint8_t choices = 0;          // the encoder's choices that the driver's bytes make (encoding_choices)
};

// One of the drivers. Each method that takes a revision takes one of the revisions of the driver: up to
// DriverFacts::most_revision.
class Driver
{
public:
    virtual ~Driver() = default;

    virtual const DriverFacts& Facts() const = 0;

    // Whether ntdlib finds the driver in `rom`, a GBA ROM.
    virtual bool Holds(std::span<const uint8_t> rom) const = 0;

    // A file made from `rom`, a GBA ROM with the driver, with the driver's engine. Each song that ntdlib's cut finds
    // becomes a sequence, and the ROM's other bytes stay as they are. The file keeps the ROM's size as the game's size
    // and has no tables. Throws xsflib::FormatError if Hokuto finds no such driver in the ROM.
    virtual File Import(std::span<const uint8_t> rom) const = 0;

    // The ROM that ntdlib's join builds from `sequences` and from `pieces` and `fields` of `native`, an archive with
    // the driver. Throws std::invalid_argument with ntdlib's message when the join fails.
    virtual std::vector<uint8_t> Join(const NativeArchive& native,
                                      std::span<const ntdlib::commands::Sequence> sequences,
                                      std::span<const ntdlib::sar::Piece> pieces,
                                      std::span<const ntdlib::sar::Field> fields) const = 0;

    // The byte offset of each item of `sequence` in the bytes of `revision`, and the size of the bytes after them.
    // Throws std::invalid_argument with the reason when the bytes can't hold an item.
    virtual std::vector<uint32_t> ItemOffsets(const ntdlib::commands::Sequence& sequence, uint8_t revision) const = 0;

    // The bytes that `piece`, a piece of `sequence`, occupies in the ROM that the join builds in `revision`. By
    // default, a song keeps the size of its place (ntdlib::sar::SequencePiece::room). Throws std::invalid_argument as
    // ItemOffsets does.
    virtual uint64_t PieceBytes(const ntdlib::commands::Sequence& /*sequence*/, const ntdlib::sar::SequencePiece& piece,
                                uint8_t /*revision*/) const
    {
        return piece.room;
    }

    // The values that the bytes of `revision` hold for a command of `kind`.
    virtual ntdlib::commands::Scale ScaleOf(ntdlib::commands::Kind kind, uint8_t revision) const = 0;

    // The items at which the tracks of each piece of `pieces` start, by the pieces' order. An empty list for a piece
    // that has no sequence.
    virtual std::vector<std::vector<uint32_t>> TrackEntries(std::span<const ntdlib::sar::Piece> pieces,
                                                            std::span<const ntdlib::sar::Field> fields,
                                                            uint8_t revision) const = 0;

    // Encodes `sequence` in the bytes of `revision`. The tracks of `sequence` start at the items `entries`. Throws
    // std::invalid_argument with the encoder's reason when the bytes can't hold the sequence.
    virtual void Encode(const ntdlib::commands::Sequence& sequence, std::span<const uint32_t> entries,
                        uint8_t revision) const = 0;

    // Whether `item` has a choice of an encoder that the driver's bytes don't make (format/file.h).
    bool Foreign(const ntdlib::commands::Command& item) const;

    // A sequence of `item` alone. The text form encodes the sequence when it checks the values of an item. Most drivers
    // encode an item by itself. Krawall encodes a pattern's items after the pattern's start.
    virtual ntdlib::commands::Sequence Alone(const ntdlib::commands::Command& item) const
    {
        return ntdlib::commands::Sequence{{item}, {}};
    }
};

// The driver of `engine`, or nothing for an engine of another console.
const Driver* DriverOf(Engine engine);

// The driver that `rom`, a GBA ROM, contains, or nothing. Each driver's test runs in the order of the engines. No test
// image contains two drivers.
const Driver* DriverOfRom(std::span<const uint8_t> rom);

// The driver of `file`'s native archive, or nothing for a file of another kind.
const Driver* DriverOfFile(const File& file);

// The driver of `native`, a native archive that a writer of either form writes, or nothing for an engine of another
// console. Throws std::invalid_argument when the archive's driver doesn't fit its engine: a driver on another console's
// engine, no driver on a driver's engine, or filler runs or rooms that the driver doesn't keep.
const Driver* WrittenDriverOf(const NativeArchive& native);

// The sequence of `file`, a file of a ROM with one of the drivers, that the field at `place` names: the sequence of the
// piece that the field leads to. `place` counts in the bytes that the file's pieces give. Nothing where no such field
// starts at `place`, and where the field names no sequence. `starts` lists where the pieces start (NativePieceStarts).
// Without it, the function works them out.
std::optional<uint32_t> SequenceNamedAt(const File& file, uint64_t place);
std::optional<uint32_t> SequenceNamedAt(const File& file, uint64_t place, std::span<const uint64_t> starts);

// The fields of the native layout of `file` at `places`, in their order, without the places where no field starts.
// `starts` lists where the pieces start (as for SequenceNamedAt).
std::vector<uint32_t> FieldsAt(const File& file, std::span<const uint64_t> places);
std::vector<uint32_t> FieldsAt(const File& file, std::span<const uint64_t> places, std::span<const uint64_t> starts);

} // namespace hokuto::format
