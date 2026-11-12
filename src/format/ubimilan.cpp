// SPDX-License-Identifier: MIT

// Ubisoft Milan's driver for the GBA (ntdlib/ubimilan/layout.h) as one of the drivers of format/drivers.h. A file
// stores each wait as a length in ticks (format/ubimilan.h).

#include "format/ubimilan.h"

#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/ubimilan/commands.h"
#include "ntdlib/ubimilan/driver.h"
#include "ntdlib/ubimilan/layout.h"
#include "xsflib/binary.h"

#include "format/archive.h"
#include "format/drivers.h"
#include "format/engines.h"
#include "format/file.h"

namespace hokuto::format
{
namespace
{

class UbiMilan final : public Driver
{
public:
    const DriverFacts& Facts() const override
    {
        static const DriverFacts facts{.engine = Engine::kUbiMilan,
                                       .title = FactsOf(Engine::kUbiMilan).title,
                                       .engine_name = "Ubisoft Milan's",
                                       .most_revision = 0,
                                       .two_byte_fields = false,
                                       .key_holds_byte = false,
                                       .filler_runs = false,
                                       .choices = 0}; // The driver's bytes make no choice of an encoder.
        return facts;
    }

    bool Holds(std::span<const uint8_t> rom) const override
    {
        ntdlib::ubimilan::DriverInfo info;
        std::string error;
        return ntdlib::ubimilan::DetectDriver(RomOf(rom), {}, info, error);
    }

    File Import(std::span<const uint8_t> rom) const override
    {
        const ntdlib::agb::Rom read = RomOf(rom);
        ntdlib::ubimilan::DriverInfo info;
        std::string error;
        if (!ntdlib::ubimilan::DetectDriver(read, {}, info, error))
        {
            throw xsflib::FormatError(error.empty() ? "the ROM shows no sign of Ubisoft Milan's driver"
                                                    : "Can't read the sound bank of Ubisoft Milan's driver: " + error);
        }

        ntdlib::ubimilan::Layout layout = ntdlib::ubimilan::Cut(read, info);
        for (ntdlib::commands::Sequence& sequence : layout.sequences)
        {
            sequence = UbiMilanTicks(std::move(sequence));
        }

        NativeDriver driver{.revision = 0, .free_start = layout.free.start, .padding = layout.free.padding};

        File file;
        file.engine = Engine::kUbiMilan;
        file.sequences = ShareSequences(std::move(layout.sequences));
        file.native = NativeArchive{.engine = Engine::kUbiMilan,
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
        const ntdlib::ubimilan::FreeSpace free{.start = driver.free_start, .padding = driver.padding};
        std::vector<ntdlib::commands::Sequence> countdowns;
        countdowns.reserve(sequences.size());
        for (const ntdlib::commands::Sequence& sequence : sequences)
        {
            countdowns.push_back(UbiMilanCountdowns(sequence));
        }

        return ntdlib::ubimilan::Join(countdowns, pieces, fields, free);
    }

    std::vector<uint32_t> ItemOffsets(const ntdlib::commands::Sequence& sequence, uint8_t) const override
    {
        return UbiMilanItemOffsets(sequence);
    }

    ntdlib::commands::Scale ScaleOf(ntdlib::commands::Kind kind, uint8_t) const override
    {
        return ntdlib::ubimilan::ScaleOf(kind);
    }

    // A sequence has one track of commands, and the resource table names its first item. No field names a track.
    std::vector<std::vector<uint32_t>> TrackEntries(std::span<const ntdlib::sar::Piece> pieces,
                                                    std::span<const ntdlib::sar::Field>, uint8_t) const override
    {
        return std::vector<std::vector<uint32_t>>(pieces.size());
    }

    void Encode(const ntdlib::commands::Sequence& sequence, std::span<const uint32_t>, uint8_t) const override
    {
        ntdlib::ubimilan::EncodeSequence(UbiMilanCountdowns(sequence));
    }
};

} // namespace

const Driver& UbiMilanDriver()
{
    static const UbiMilan driver;
    return driver;
}

} // namespace hokuto::format
