// SPDX-License-Identifier: MIT

// Brownie Brown's driver for the GBA (ntdlib/brownie/layout.h) as one of the drivers of format/drivers.h.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/brownie/commands.h"
#include "ntdlib/brownie/driver.h"
#include "ntdlib/brownie/layout.h"
#include "ntdlib/brownie/song.h"
#include "xsflib/binary.h"

#include "format/archive.h"
#include "format/brownie_items.h"
#include "format/drivers.h"
#include "format/engines.h"
#include "format/file.h"

namespace hokuto::format
{
namespace
{

ntdlib::brownie::Revision RevisionOf(uint8_t revision)
{
    return static_cast<ntdlib::brownie::Revision>(revision);
}

// Sets the label of the list of each sound in `sequence` to the number of the sound with as many digits as its name
// shows, such as SOUND_007. Puts the labels in the order of their names. The edits keep them in that order
// (edit::RenameLabel).
void NameLists(ntdlib::commands::Sequence& sequence, int sounds)
{
    std::size_t digits = 2;
    for (int n = sounds - 1; n >= 100; n /= 10)
    {
        digits++;
    }

    const std::string prefix = "SOUND_";
    for (ntdlib::commands::Label& label : sequence.labels)
    {
        const std::string number = label.name.substr(std::min(prefix.size(), label.name.size()));
        if (label.name.starts_with(prefix) && !number.empty() && number.size() < digits &&
            std::ranges::all_of(number, [](char c) { return c >= '0' && c <= '9'; }))
        {
            label.name = prefix + std::string(digits - number.size(), '0') + number;
        }
    }

    std::ranges::stable_sort(sequence.labels, {}, &ntdlib::commands::Label::name);
}

class Brownie final : public Driver
{
public:
    const DriverFacts& Facts() const override
    {
        static const DriverFacts facts{
            .engine = Engine::kBrownie,
            .title = FactsOf(Engine::kBrownie).title,
            .engine_name = "Brownie Brown's",
            .most_revision = static_cast<uint8_t>(ntdlib::brownie::Revision::kMagicalVacation),
            .two_byte_fields = false,
            .key_holds_byte = false,
            .filler_runs = false,
            // The driver's bytes make no choice of an encoder. An item's opcode keeps the byte that reads as a
            // command in revision 1.
            .choices = 0};
        return facts;
    }

    bool Holds(std::span<const uint8_t> rom) const override
    {
        ntdlib::brownie::DriverInfo info;
        std::string error;
        return ntdlib::brownie::DetectDriver(RomOf(rom), {}, info, error);
    }

    File Import(std::span<const uint8_t> rom) const override
    {
        const ntdlib::agb::Rom read = RomOf(rom);
        ntdlib::brownie::DriverInfo info;
        std::string error;
        if (!ntdlib::brownie::DetectDriver(read, {}, info, error))
        {
            throw xsflib::FormatError(error.empty()
                                          ? "the ROM shows no sign of Brownie Brown's driver"
                                          : "Hokuto can't read the tables of Brownie Brown's driver: " + error);
        }

        ntdlib::brownie::Layout layout = ntdlib::brownie::Cut(read, info);

        // Each command that waits gets the frames that its channels wait there (FillBrownieWaits).
        const BrownieLength length = [&read, &info](uint8_t byte, int set)
        {
            const uint8_t frames = ntdlib::brownie::NoteLength(read, info, byte, set);
            return frames != 0 ? uint32_t{frames} : 256u;
        };
        for (ntdlib::commands::Sequence& sequence : layout.sequences)
        {
            FillBrownieWaits(sequence, layout.revision, length);
            NameLists(sequence, info.song_count);
        }

        NativeDriver driver{.revision = static_cast<uint8_t>(layout.revision),
                            .free_start = layout.free.start,
                            .padding = layout.free.padding};

        File file;
        file.engine = Engine::kBrownie;
        file.sequences = ShareSequences(std::move(layout.sequences));
        file.native = NativeArchive{.engine = Engine::kBrownie,
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
        return ntdlib::brownie::Join(RevisionOf(driver.revision), sequences, pieces, fields,
                                     {.start = driver.free_start, .padding = driver.padding});
    }

    std::vector<uint32_t> ItemOffsets(const ntdlib::commands::Sequence& sequence, uint8_t revision) const override
    {
        return BrownieItemOffsets(sequence, RevisionOf(revision));
    }

    ntdlib::commands::Scale ScaleOf(ntdlib::commands::Kind kind, uint8_t revision) const override
    {
        return ntdlib::brownie::ScaleOf(kind, RevisionOf(revision));
    }

    // The list of a sound names the starts of its channels, in the sequence. No field of the ROM's tables does.
    std::vector<std::vector<uint32_t>> TrackEntries(std::span<const ntdlib::sar::Piece> pieces,
                                                    std::span<const ntdlib::sar::Field>, uint8_t) const override
    {
        return std::vector<std::vector<uint32_t>>(pieces.size());
    }

    // The encoder checks that each list's channels read the commands as the items say. A sequence keeps the remainder
    // of its start modulo 4 in the ROM, and its first item is a word of a list wherever the ROM laid one out first.
    void Encode(const ntdlib::commands::Sequence& sequence, std::span<const uint32_t>, uint8_t revision) const override
    {
        const std::vector<uint32_t> offsets = BrownieItemOffsets(sequence, RevisionOf(revision));
        uint32_t remainder = 0;
        for (std::size_t item = 0; item < sequence.items.size(); item++)
        {
            if (BrownieIsWord(sequence.items[item]))
            {
                remainder = (4 - offsets[item] % 4) % 4;
                break;
            }
        }

        ntdlib::brownie::EncodeSequence(sequence, RevisionOf(revision), ntdlib::agb::kRomBase + remainder);
    }
};

} // namespace

const Driver& BrownieDriver()
{
    static const Brownie driver;
    return driver;
}

} // namespace hokuto::format
