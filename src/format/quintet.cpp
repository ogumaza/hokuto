// SPDX-License-Identifier: MIT

// Quintet's driver for the GBA (ntdlib/quintet/layout.h) as one of the drivers of format/drivers.h. The driver's
// songs are back to back in lists, and each list keeps its place. A list can grow into its room: the bytes of one value
// after it, up to the end of its part of the music file. A file keeps the rooms (NativeDriver::rooms). The driver's
// revisions differ in the traits of their commands, and a file stores lengths in the performer's ticks
// (format/quintet.h).

#include "format/quintet.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/quintet/commands.h"
#include "ntdlib/quintet/driver.h"
#include "ntdlib/quintet/layout.h"
#include "xsflib/binary.h"

#include "format/archive.h"
#include "format/drivers.h"
#include "format/engines.h"
#include "format/file.h"

namespace hokuto::format
{
namespace
{

// The music files of the driver that `rom` contains, or none.
std::vector<ntdlib::quintet::DriverInfo> Detect(const ntdlib::agb::Rom& rom, std::string& error)
{
    ntdlib::quintet::DriverInfo info;
    if (!ntdlib::quintet::DetectDriver(rom, {}, info, error))
    {
        return {};
    }

    return ntdlib::quintet::ReadMusicFiles(rom, info);
}

// `fields` with each place that names an item of a sequence piece of `pieces` renumbered by `items`: the new index of
// each item of each sequence, and of its end.
void RenumberItems(std::vector<ntdlib::sar::Field>& fields, std::span<const ntdlib::sar::Piece> pieces,
                   const std::vector<std::vector<uint32_t>>& items)
{
    const auto renumber = [&](ntdlib::sar::Place& place)
    {
        if (!place.item || place.sequence || place.piece >= pieces.size())
        {
            return;
        }

        const auto* piece = std::get_if<ntdlib::sar::SequencePiece>(&pieces[place.piece]);
        if (piece != nullptr && piece->sequence < items.size() && place.offset < items[piece->sequence].size())
        {
            place.offset = items[piece->sequence][place.offset];
        }
    };
    for (ntdlib::sar::Field& field : fields)
    {
        renumber(field.at);
        renumber(field.from);
        renumber(field.to);
    }
}

class Quintet final : public Driver
{
public:
    const DriverFacts& Facts() const override
    {
        static const DriverFacts facts{
            .engine = Engine::kQuintet,
            .title = FactsOf(Engine::kQuintet).title,
            .engine_name = "Quintet's",
            .most_revision = kQuintetMostRevision,
            .two_byte_fields = true,
            .filler_runs = false,
            .rooms = true,
            // The driver's bytes make a choice of a note value's code. They make none of the others.
            .choices = encoding_choices::kLengthCode};
        return facts;
    }

    bool Holds(std::span<const uint8_t> rom) const override
    {
        std::string error;
        return !Detect(RomOf(rom), error).empty();
    }

    File Import(std::span<const uint8_t> rom) const override
    {
        const ntdlib::agb::Rom read = RomOf(rom);
        std::string error;
        const std::vector<ntdlib::quintet::DriverInfo> files = Detect(read, error);
        if (files.empty())
        {
            throw xsflib::FormatError(error.empty() ? "the ROM shows no sign of Quintet's driver"
                                                    : "Can't read the tables of Quintet's driver: " + error);
        }

        ntdlib::quintet::Layout layout = ntdlib::quintet::Cut(read, files);
        if (layout.sequences.empty())
        {
            throw xsflib::FormatError("Quintet's driver plays no song of this ROM that Hokuto can read");
        }

        const std::optional<uint8_t> revision = QuintetRevision(layout.traits);
        if (!revision)
        {
            throw xsflib::FormatError(
                "Quintet's driver in this ROM uses a table of note values that Hokuto can't "
                "read");
        }

        NativeDriver driver{.revision = *revision, .free_start = 0, .padding = 0};
        for (const ntdlib::quintet::Room& room : layout.rooms)
        {
            driver.rooms.push_back({.piece = room.piece, .end = room.end, .fill = room.fill});
        }

        // A file folds each dot into the item before it (QuintetTicks). The fields that name items take their new
        // indices.
        std::vector<std::vector<uint32_t>> folded;
        for (ntdlib::commands::Sequence& sequence : layout.sequences)
        {
            folded.push_back(QuintetFoldedItems(sequence));
            sequence = QuintetTicks(std::move(sequence));
        }

        RenumberItems(layout.fields, layout.pieces, folded);

        File file;
        file.engine = Engine::kQuintet;
        file.sequences = ShareSequences(std::move(layout.sequences));
        file.native = NativeArchive{.engine = Engine::kQuintet,
                                    .pieces = std::move(layout.pieces),
                                    .fields = std::move(layout.fields),
                                    .game_sizes = GameSizes{rom.size(), {}},
                                    .driver = std::move(driver)};

        return file;
    }

    std::vector<uint8_t> Join(const NativeArchive& native, std::span<const ntdlib::commands::Sequence> sequences,
                              std::span<const ntdlib::sar::Piece> pieces,
                              std::span<const ntdlib::sar::Field> fields) const override
    {
        const NativeDriver& driver = *native.driver;
        std::vector<ntdlib::quintet::Room> rooms;
        for (const DriverRoom& room : driver.rooms)
        {
            if (room.piece > pieces.size())
            {
                throw std::invalid_argument("a room of a list of songs names a piece that the archive doesn't have");
            }

            rooms.push_back({room.piece, room.end, room.fill});
        }

        std::vector<ntdlib::commands::Sequence> driver_ticks;
        std::vector<std::vector<uint32_t>> unfolded;
        for (const ntdlib::commands::Sequence& sequence : sequences)
        {
            driver_ticks.push_back(QuintetDriverTicks(sequence));
            unfolded.push_back(QuintetUnfoldedItems(sequence));
        }

        std::vector<ntdlib::sar::Field> driver_fields(fields.begin(), fields.end());
        RenumberItems(driver_fields, pieces, unfolded);

        return ntdlib::quintet::Join(QuintetTraits(driver.revision), driver_ticks, pieces, driver_fields, rooms);
    }

    std::vector<uint32_t> ItemOffsets(const ntdlib::commands::Sequence& sequence, uint8_t revision) const override
    {
        return QuintetFoldedOffsets(
            sequence, ntdlib::quintet::ItemOffsets(QuintetDriverTicks(sequence), QuintetTraits(revision)));
    }

    uint64_t PieceBytes(const ntdlib::commands::Sequence& sequence, const ntdlib::sar::SequencePiece& /*piece*/,
                        uint8_t revision) const override
    {
        // The join lays each song out at the size of its commands. A zero follows an odd size. The song's place
        // keeps no size (ntdlib::quintet::Cut).
        const uint64_t bytes = ItemOffsets(sequence, revision).back();
        return bytes + bytes % 2;
    }

    ntdlib::commands::Scale ScaleOf(ntdlib::commands::Kind kind, uint8_t) const override
    {
        return QuintetScale(kind);
    }

    std::vector<std::vector<uint32_t>> TrackEntries(std::span<const ntdlib::sar::Piece> pieces,
                                                    std::span<const ntdlib::sar::Field> fields, uint8_t) const override
    {
        // The fields of a song's header after its length name the items where the channels start (ntdlib::quintet::
        // Cut). The length's field, at the header's start, names the end of the song's items. Each item counts once, in
        // the order of the items.
        std::vector<std::vector<uint32_t>> entries(pieces.size());
        for (const ntdlib::sar::Field& field : fields)
        {
            if (field.to.item && !field.to.sequence && field.at.offset != 0 && field.to.piece < pieces.size())
            {
                entries[field.to.piece].push_back(field.to.offset);
            }
        }

        for (std::vector<uint32_t>& piece : entries)
        {
            std::ranges::sort(piece);
            piece.erase(std::unique(piece.begin(), piece.end()), piece.end());
        }

        return entries;
    }

    void Encode(const ntdlib::commands::Sequence& sequence, std::span<const uint32_t> entries,
                uint8_t revision) const override
    {
        const std::vector<uint32_t> unfolded = QuintetUnfoldedItems(sequence);
        std::vector<uint32_t> driver_entries;
        for (const uint32_t entry : entries)
        {
            driver_entries.push_back(entry < unfolded.size() ? unfolded[entry] : entry);
        }

        ntdlib::quintet::EncodeSequence(QuintetDriverTicks(sequence), driver_entries, QuintetTraits(revision));
    }
};

} // namespace

const Driver& QuintetDriver()
{
    static const Quintet driver;
    return driver;
}

} // namespace hokuto::format
