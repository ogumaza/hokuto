// SPDX-License-Identifier: MIT

// Konami's driver for the GBA (agblib/konami/layout.h) as one of the drivers of format/drivers.h.

#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/konami/commands.h"
#include "agblib/konami/driver.h"
#include "agblib/konami/layout.h"
#include "agblib/konami/seqformat.h"
#include "xsflib/binary.h"

#include "format/archive.h"
#include "format/drivers.h"
#include "format/engines.h"
#include "format/file.h"

namespace hokuto::format
{
namespace
{

agblib::konami::Revision RevisionOf(uint8_t revision)
{
    return static_cast<agblib::konami::Revision>(revision);
}

// The driver that `rom` contains, or nothing.
std::optional<agblib::konami::DriverInfo> Detect(const agblib::agb::Rom& rom, std::string& error)
{
    agblib::konami::DriverInfo info;
    if (!agblib::konami::DetectDriver(rom, {}, info, error))
    {
        return std::nullopt;
    }

    return info;
}

// The place of song `song`'s entry in the song table at `song_table`, a ROM address, from the ROM's start.
uint64_t EntryPlace(const File& file, uint32_t song_table, int song)
{
    return uint64_t{song_table} - agblib::agb::kRomBase +
           agblib::konami::SongEntrySize(RevisionOf(file.native->driver->revision)) * static_cast<uint64_t>(song);
}

class Konami final : public Driver
{
public:
    const DriverFacts& Facts() const override
    {
        static const DriverFacts facts{
            .engine = Engine::kKonami,
            .title = FactsOf(Engine::kKonami).title,
            .engine_name = "Konami's",
            .most_revision = static_cast<uint8_t>(agblib::konami::Revision::kDungeonDiceMonsters),
            .two_byte_fields = true,
            .key_holds_byte = true,
            .choices = 0}; // Konami's bytes make no choice of an encoder.
        return facts;
    }

    bool Holds(std::span<const uint8_t> rom) const override
    {
        std::string error;
        return Detect(RomOf(rom), error).has_value();
    }

    File Import(std::span<const uint8_t> rom) const override
    {
        const agblib::agb::Rom read = RomOf(rom);
        std::string error;
        const std::optional<agblib::konami::DriverInfo> info = Detect(read, error);
        if (!info)
        {
            throw xsflib::FormatError(
                error.empty() ? "the ROM doesn't use Konami's driver"
                              : "the song table of Konami's driver has no song that Hokuto can read: " + error);
        }

        agblib::konami::Layout layout = agblib::konami::Cut(read, *info);

        // The field of a track is 16 bits wide. The file sets the field to that size and checks that the 16 bits fall
        // within their run.
        for (ntdlib::sar::Field& field : layout.fields)
        {
            field.bytes = field.from.item ? 2 : field.bytes;
        }

        NativeDriver driver{.revision = static_cast<uint8_t>(layout.revision),
                            .free_start = layout.free.start,
                            .padding = layout.free.padding};
        for (const agblib::konami::Span& run : layout.free.filler)
        {
            driver.filler.push_back({run.start, run.size});
        }

        File file;
        file.engine = Engine::kKonami;
        file.sequences = ShareSequences(std::move(layout.sequences));
        file.native = NativeArchive{.engine = Engine::kKonami,
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
        agblib::konami::FreeSpace free{.start = driver.free_start, .padding = driver.padding};
        for (const FillerRun& run : driver.filler)
        {
            free.filler.push_back({run.start, run.size});
        }

        return agblib::konami::Join(RevisionOf(driver.revision), sequences, pieces, fields, free,
                                    {.filler = native.gba_filler});
    }

    std::vector<uint32_t> ItemOffsets(const ntdlib::commands::Sequence& sequence, uint8_t revision) const override
    {
        return agblib::konami::ItemOffsets(sequence, RevisionOf(revision));
    }

    ntdlib::commands::Scale ScaleOf(ntdlib::commands::Kind kind, uint8_t revision) const override
    {
        return agblib::konami::ScaleOf(kind, RevisionOf(revision));
    }

    std::vector<std::vector<uint32_t>> TrackEntries(std::span<const ntdlib::sar::Piece> pieces,
                                                    std::span<const ntdlib::sar::Field> fields,
                                                    uint8_t revision) const override
    {
        return agblib::konami::TrackEntries(pieces, fields, RevisionOf(revision));
    }

    void Encode(const ntdlib::commands::Sequence& sequence, std::span<const uint32_t> entries,
                uint8_t revision) const override
    {
        agblib::konami::EncodeSequence(sequence, entries, RevisionOf(revision));
    }
};

} // namespace

const Driver& KonamiDriver()
{
    static const Konami driver;
    return driver;
}

bool HoldsKonamiDriver(std::span<const uint8_t> rom)
{
    return KonamiDriver().Holds(rom);
}

File ImportKonamiRom(std::span<const uint8_t> rom)
{
    return KonamiDriver().Import(rom);
}

std::optional<uint32_t> KonamiSongSequence(const File& file, uint32_t song_table, int song)
{
    if (!file.native || !file.native->driver || file.native->engine != Engine::kKonami || song < 0)
    {
        return std::nullopt;
    }

    // A song's entry of the song table contains a field that names the start of the song's piece (agblib::konami::Cut).
    return SequenceNamedAt(file, EntryPlace(file, song_table, song));
}

std::vector<uint32_t> KonamiTrackFields(const File& file, uint32_t song_table, int song)
{
    if (!file.native || !file.native->driver || file.native->engine != Engine::kKonami || song < 0)
    {
        return {};
    }

    // The entry's other fields follow its first: 2 bytes for each track's offset.
    const uint64_t entry = EntryPlace(file, song_table, song);
    std::vector<uint64_t> places;
    for (int track = 0; track < agblib::konami::TrackCount(RevisionOf(file.native->driver->revision)); ++track)
    {
        places.push_back(entry + 4 + 2 * static_cast<uint64_t>(track));
    }

    return FieldsAt(file, places);
}

} // namespace hokuto::format
