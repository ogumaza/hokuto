// SPDX-License-Identifier: MIT

// Rare's driver for the GBA (ntdlib/rare/layout.h) as one of the drivers of format/drivers.h. The driver's revisions
// differ in the traits of their commands (ntdlib::rare::Traits). A file's revision of the driver stores them as bits
// (format/rare.h).

#include "format/rare.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/rare/commands.h"
#include "ntdlib/rare/driver.h"
#include "ntdlib/rare/layout.h"
#include "xsflib/binary.h"

#include "format/archive.h"
#include "format/drivers.h"
#include "format/engines.h"
#include "format/file.h"

namespace hokuto::format
{
namespace
{

// The driver that `rom` contains, or nothing.
std::optional<ntdlib::rare::DriverInfo> Detect(const ntdlib::agb::Rom& rom, std::string& error)
{
    ntdlib::rare::DriverInfo info;
    if (!ntdlib::rare::DetectDriver(rom, {}, info, error))
    {
        return std::nullopt;
    }

    return info;
}

class Rare final : public Driver
{
public:
    const DriverFacts& Facts() const override
    {
        static const DriverFacts facts{
            .engine = Engine::kRare,
            .title = FactsOf(Engine::kRare).title,
            .engine_name = "Rare's",
            .most_revision = kRareMostRevision,
            // The driver's bytes make a choice of a delay's bytes, and keep the byte that the driver ignores in a
            // note-off, a channel pressure and some controllers. They make none of the others.
            .choices = encoding_choices::kValueBytes | encoding_choices::kArguments};
        return facts;
    }

    bool Holds(std::span<const uint8_t> rom) const override
    {
        std::string error;
        return Detect(RomOf(rom), error).has_value();
    }

    File Import(std::span<const uint8_t> rom) const override
    {
        const ntdlib::agb::Rom read = RomOf(rom);
        std::string error;
        const std::optional<ntdlib::rare::DriverInfo> info = Detect(read, error);
        if (!info)
        {
            throw xsflib::FormatError(error.empty() ? "the ROM shows no sign of Rare's driver"
                                                    : "Rare's tune table holds no tune that Hokuto can read: " + error);
        }

        ntdlib::rare::Layout layout = ntdlib::rare::Cut(read, *info);
        NativeDriver driver{
            .revision = RareRevision(layout.traits), .free_start = layout.free.start, .padding = layout.free.padding};
        for (const ntdlib::rare::Span& run : layout.free.filler)
        {
            driver.filler.push_back({run.start, run.size});
        }

        File file;
        file.engine = Engine::kRare;
        file.sequences = ShareSequences(std::move(layout.sequences));
        file.native = NativeArchive{.engine = Engine::kRare,
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
        ntdlib::rare::FreeSpace free{.start = driver.free_start, .padding = driver.padding};
        for (const FillerRun& run : driver.filler)
        {
            free.filler.push_back({run.start, run.size});
        }

        return ntdlib::rare::Join(RareTraits(driver.revision), sequences, pieces, fields, free,
                                  {.filler = native.gba_filler});
    }

    std::vector<uint32_t> ItemOffsets(const ntdlib::commands::Sequence& sequence, uint8_t revision) const override
    {
        return ntdlib::rare::ItemOffsets(sequence, RareTraits(revision));
    }

    ntdlib::commands::Scale ScaleOf(ntdlib::commands::Kind kind, uint8_t) const override
    {
        return ntdlib::rare::ScaleOf(kind);
    }

    std::vector<std::vector<uint32_t>> TrackEntries(std::span<const ntdlib::sar::Piece> pieces,
                                                    std::span<const ntdlib::sar::Field> fields, uint8_t) const override
    {
        // The fields of the tunes' track lists name the items where the tracks start, once each, in the order of the
        // items (ntdlib::rare::Join).
        std::vector<std::vector<uint32_t>> entries(pieces.size());
        for (const ntdlib::sar::Field& field : fields)
        {
            if (field.to.item && !field.to.sequence && field.to.piece < pieces.size())
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
        ntdlib::rare::EncodeSequence(sequence, entries, RareTraits(revision));
    }
};

} // namespace

const Driver& RareDriver()
{
    static const Rare driver;
    return driver;
}

} // namespace hokuto::format
