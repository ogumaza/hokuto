// SPDX-License-Identifier: MIT

// Krawall as one of the drivers of format/drivers.h. Krawall is a player of XM and S3M modules for the GBA
// (ntdlib/krawall/layout.h). A sequence contains the patterns of a module. The fields of the module's table of patterns
// name the items where its patterns start. The revision is 1 for the builds from 2004/09/17 on, and 0 for the earlier
// builds. The patterns of the later builds hold wider fields (ntdlib::krawall::Traits::wide).

#include "format/krawall.h"

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/krawall/commands.h"
#include "ntdlib/krawall/driver.h"
#include "ntdlib/krawall/layout.h"
#include "xsflib/binary.h"

#include "format/archive.h"
#include "format/drivers.h"
#include "format/engines.h"
#include "format/file.h"

namespace hokuto::format
{
namespace
{

ntdlib::krawall::Traits TraitsOf(uint8_t revision)
{
    return {.wide = revision != 0};
}

class Krawall final : public Driver
{
public:
    const DriverFacts& Facts() const override
    {
        static const DriverFacts facts{.engine = Engine::kKrawall,
                                       .title = FactsOf(Engine::kKrawall).title,
                                       .engine_name = "Krawall's",
                                       .most_revision = 1,
                                       .two_byte_fields = false,
                                       .key_holds_byte = false,
                                       // A pattern's bytes make a choice of a third byte of an instrument
                                       // (encoding.value_bytes). They make none of the others.
                                       .choices = encoding_choices::kValueBytes};
        return facts;
    }

    bool Holds(std::span<const uint8_t> rom) const override
    {
        ntdlib::krawall::DriverInfo info;
        std::string error;
        return ntdlib::krawall::DetectDriver(RomOf(rom), {}, info, error);
    }

    File Import(std::span<const uint8_t> rom) const override
    {
        const ntdlib::agb::Rom read = RomOf(rom);
        ntdlib::krawall::DriverInfo info;
        std::string error;
        if (!ntdlib::krawall::DetectDriver(read, {}, info, error))
        {
            throw xsflib::FormatError(error.empty() ? "the ROM shows no sign of Krawall"
                                                    : "Hokuto can't read Krawall's tables: " + error);
        }

        ntdlib::krawall::Layout layout = ntdlib::krawall::Cut(read, info);
        NativeDriver driver{.revision = static_cast<uint8_t>(layout.traits.wide ? 1 : 0),
                            .free_start = layout.free.start,
                            .padding = layout.free.padding};
        for (const ntdlib::krawall::Span& run : layout.free.filler)
        {
            driver.filler.push_back({run.start, run.size});
        }

        File file;
        file.engine = Engine::kKrawall;
        file.sequences = ShareSequences(std::move(layout.sequences));
        file.native = NativeArchive{.engine = Engine::kKrawall,
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
        ntdlib::krawall::FreeSpace free{.start = driver.free_start, .padding = driver.padding};
        for (const FillerRun& run : driver.filler)
        {
            free.filler.push_back({run.start, run.size});
        }

        return ntdlib::krawall::Join(TraitsOf(driver.revision), sequences, pieces, fields, free,
                                     {.filler = native.gba_filler});
    }

    std::vector<uint32_t> ItemOffsets(const ntdlib::commands::Sequence& sequence, uint8_t revision) const override
    {
        return ntdlib::krawall::ItemOffsets(sequence, TraitsOf(revision));
    }

    ntdlib::commands::Scale ScaleOf(ntdlib::commands::Kind kind, uint8_t) const override
    {
        return ntdlib::krawall::ScaleOf(kind);
    }

    // The fields of the modules' tables of patterns name the items where the patterns start, in the order of each
    // piece's items. Two fields can name one pattern.
    std::vector<std::vector<uint32_t>> TrackEntries(std::span<const ntdlib::sar::Piece> pieces,
                                                    std::span<const ntdlib::sar::Field> fields, uint8_t) const override
    {
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
        ntdlib::krawall::EncodeSequence(sequence, entries, TraitsOf(revision));
    }

    ntdlib::commands::Sequence Alone(const ntdlib::commands::Command& item) const override
    {
        return KrawallAlone(item);
    }
};

} // namespace

const Driver& KrawallDriver()
{
    static const Krawall driver;
    return driver;
}

} // namespace hokuto::format
