// SPDX-License-Identifier: MIT

// Nintendo R&D2's driver for the GBA (ntdlib/rd2/layout.h) as one of the drivers of format/drivers.h.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/rd2/commands.h"
#include "ntdlib/rd2/driver.h"
#include "ntdlib/rd2/layout.h"
#include "xsflib/binary.h"

#include "format/archive.h"
#include "format/drivers.h"
#include "format/engines.h"
#include "format/file.h"

namespace hokuto::format
{
namespace
{

ntdlib::rd2::Revision RevisionOf(uint8_t revision)
{
    return static_cast<ntdlib::rd2::Revision>(revision);
}

class Rd2 final : public Driver
{
public:
    const DriverFacts& Facts() const override
    {
        static const DriverFacts facts{
            .engine = Engine::kRd2,
            .title = FactsOf(Engine::kRd2).title,
            .engine_name = "Nintendo R&D2's",
            .most_revision = static_cast<uint8_t>(ntdlib::rd2::Revision::kSuperMarioAdvance3),
            .two_byte_fields = false,
            .key_holds_byte = false,
            // The driver's bytes make a choice of a repeated note or wait, and of a length in two bytes. They make none
            // of the others.
            .choices = encoding_choices::kValueBytes | encoding_choices::kRepeated};
        return facts;
    }

    bool Holds(std::span<const uint8_t> rom) const override
    {
        ntdlib::rd2::DriverInfo info;
        std::string error;
        return ntdlib::rd2::DetectDriver(RomOf(rom), {}, info, error);
    }

    File Import(std::span<const uint8_t> rom) const override
    {
        const ntdlib::agb::Rom read = RomOf(rom);
        ntdlib::rd2::DriverInfo info;
        std::string error;
        if (!ntdlib::rd2::DetectDriver(read, {}, info, error))
        {
            throw xsflib::FormatError(error.empty()
                                          ? "the ROM shows no sign of Nintendo R&D2's driver"
                                          : "Hokuto can't read the tables of Nintendo R&D2's driver: " + error);
        }

        ntdlib::rd2::Layout layout = ntdlib::rd2::Cut(read, info);
        NativeDriver driver{.revision = static_cast<uint8_t>(layout.revision),
                            .free_start = layout.free.start,
                            .padding = layout.free.padding};
        for (const ntdlib::rd2::Span& run : layout.free.filler)
        {
            driver.filler.push_back({run.start, run.size});
        }

        File file;
        file.engine = Engine::kRd2;
        file.sequences = ShareSequences(std::move(layout.sequences));
        file.native = NativeArchive{.engine = Engine::kRd2,
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
        ntdlib::rd2::FreeSpace free{.start = driver.free_start, .padding = driver.padding};
        for (const FillerRun& run : driver.filler)
        {
            free.filler.push_back({run.start, run.size});
        }

        return ntdlib::rd2::Join(RevisionOf(driver.revision), sequences, pieces, fields, free,
                                 {.filler = native.gba_filler});
    }

    std::vector<uint32_t> ItemOffsets(const ntdlib::commands::Sequence& sequence, uint8_t revision) const override
    {
        return ntdlib::rd2::ItemOffsets(sequence, RevisionOf(revision));
    }

    ntdlib::commands::Scale ScaleOf(ntdlib::commands::Kind kind, uint8_t) const override
    {
        return ntdlib::rd2::ScaleOf(kind);
    }

    // The header of a sequence names the starts of its tracks. No field of the ROM's tables does.
    std::vector<std::vector<uint32_t>> TrackEntries(std::span<const ntdlib::sar::Piece> pieces,
                                                    std::span<const ntdlib::sar::Field>, uint8_t) const override
    {
        return std::vector<std::vector<uint32_t>>(pieces.size());
    }

    void Encode(const ntdlib::commands::Sequence& sequence, std::span<const uint32_t>, uint8_t revision) const override
    {
        ntdlib::rd2::EncodeSequence(sequence, RevisionOf(revision));
    }
};

} // namespace

const Driver& Rd2Driver()
{
    static const Rd2 driver;
    return driver;
}

} // namespace hokuto::format
