// SPDX-License-Identifier: MIT

// The import and the export of the DS's sound archives, cartridge images and DSE's files (format/archive.h).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/dse/layout.h"
#include "ntdlib/dse/sets.h"
#include "ntdlib/ntr/rom.h"
#include "ntdlib/ntr/rom_writer.h"
#include "ntdlib/sdat/cut.h"
#include "ntdlib/sdat/formats.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"

#include "format/archive.h"
#include "format/file.h"

namespace hokuto::format
{

File ImportDsArchive(std::span<const uint8_t> sdat)
{
    // A reader's message for a read past the end contains only an offset. A file that passes the header's checks and
    // then ends early is cut short, or isn't a DS sound archive.
    ntdlib::sdat::Layout layout;
    try
    {
        layout = ntdlib::sdat::Cut(sdat);
    }
    catch (const xsflib::FormatError& error)
    {
        if (std::string_view(error.what()).starts_with("read out of bounds"))
        {
            throw xsflib::FormatError("the file is cut short, or it isn't a DS sound archive");
        }

        throw;
    }

    File file;
    file.engine = Engine::kDs;
    file.sequences = ShareSequences(std::move(layout.sequences));
    file.native = NativeArchive{.engine = Engine::kDs,
                                .pieces = std::move(layout.pieces),
                                .fields = std::move(layout.fields),
                                .game_sizes = GameSizes{xsflib::LoadLe32(&sdat[8]), {}}};

    return file;
}

bool IsDsArchive(std::span<const uint8_t> bytes)
{
    return bytes.size() >= 16 && std::memcmp(bytes.data(), "SDAT", 4) == 0;
}

std::vector<DsArchivePlace> DsArchivesIn(std::span<const uint8_t> image)
{
    const ntdlib::ntr::Rom rom = ntdlib::ntr::Rom::Parse(image);
    std::vector<DsArchivePlace> places;
    for (const ntdlib::ntr::RomFile& file : rom.Files())
    {
        const std::span<const uint8_t> data = rom.FileData(file.id);
        for (const ntdlib::sdat::Archive& archive : ntdlib::sdat::FindArchives(data))
        {
            places.push_back({file.id, file.path, static_cast<uint32_t>(archive.bytes.data() - data.data()),
                              static_cast<uint32_t>(archive.bytes.size())});
        }
    }

    return places;
}

namespace
{

// The cartridge that keeps `image`, without the 0xFF bytes at its end.
Cartridge CartridgeOf(std::span<const uint8_t> image)
{
    std::size_t kept = image.size();
    while (kept > 0 && image[kept - 1] == 0xFF)
    {
        kept--;
    }

    return {.image = std::vector<uint8_t>(image.begin(), image.begin() + std::ptrdiff_t(kept)),
            .size = static_cast<uint32_t>(image.size())};
}

} // namespace

File ImportDsImage(std::span<const uint8_t> image, const std::optional<DsArchivePlace>& place)
{
    DsArchivePlace taken;
    if (place)
    {
        taken = *place;
    }
    else
    {
        const std::vector<DsArchivePlace> places = DsArchivesIn(image);
        if (places.empty())
        {
            throw xsflib::FormatError("the image contains no DS sound archive");
        }

        taken = *std::ranges::max_element(places, {}, &DsArchivePlace::size);
    }

    const ntdlib::ntr::Rom rom = ntdlib::ntr::Rom::Parse(image);
    const std::span<const uint8_t> data = rom.FileData(taken.file);
    if (uint64_t(taken.offset) + taken.size > data.size())
    {
        throw xsflib::FormatError("the sound archive runs past the end of its file");
    }

    File file = ImportDsArchive(data.subspan(taken.offset, taken.size));
    Cartridge cartridge = CartridgeOf(image);
    cartridge.file = taken.file;
    cartridge.offset = taken.offset;
    file.native.Edit([&](NativeArchive& native) { native.cartridge = std::move(cartridge); });

    return file;
}

namespace
{

// A file's name in a set of DSE files: its path, or its number for a file without a path.
std::string DseFileName(const ntdlib::ntr::RomFile& file)
{
    return file.path.empty() ? "file " + std::to_string(file.id) : file.path;
}

bool IsDseFile(std::span<const uint8_t> bytes)
{
    return bytes.size() >= 4 &&
           (std::memcmp(bytes.data(), "smdl", 4) == 0 || std::memcmp(bytes.data(), "swdl", 4) == 0 ||
            std::memcmp(bytes.data(), "sedl", 4) == 0);
}

} // namespace

bool HoldsDseFiles(std::span<const uint8_t> image)
{
    const ntdlib::ntr::Rom rom = ntdlib::ntr::Rom::Parse(image);
    return std::ranges::any_of(rom.Files(),
                               [&rom](const ntdlib::ntr::RomFile& file) { return IsDseFile(rom.FileData(file.id)); });
}

namespace
{

// A file made from the set of `files`, the DSE files of a game. The file has no cartridge.
File FileOfDseFiles(std::span<const ntdlib::dse::NamedFile> files)
{
    ntdlib::dse::Layout layout = ntdlib::dse::Cut(files);
    const std::vector<uint8_t> set = ntdlib::dse::Join(layout.sequences, layout.pieces, layout.fields);
    File file;
    file.engine = Engine::kDse;
    file.sequences = ShareSequences(std::move(layout.sequences));
    file.native = NativeArchive{.engine = Engine::kDse,
                                .pieces = std::move(layout.pieces),
                                .fields = std::move(layout.fields),
                                .game_sizes = GameSizes{set.size(), {}}};

    return file;
}

} // namespace

File ImportDseImage(std::span<const uint8_t> image)
{
    const ntdlib::ntr::Rom rom = ntdlib::ntr::Rom::Parse(image);
    std::vector<ntdlib::dse::NamedFile> files;
    for (const ntdlib::ntr::RomFile& file : rom.Files())
    {
        if (const std::span<const uint8_t> data = rom.FileData(file.id); IsDseFile(data))
        {
            files.push_back({DseFileName(file), data});
        }
    }
    if (files.empty())
    {
        throw xsflib::FormatError("the image contains no DSE files");
    }

    File file = FileOfDseFiles(files);
    file.native.Edit([&image](NativeArchive& native) { native.cartridge = CartridgeOf(image); });

    return file;
}

File ImportDseSet(std::span<const uint8_t> set)
{
    return FileOfDseFiles(ntdlib::dse::ReadSet(set));
}

bool IsDsImage(std::span<const uint8_t> bytes)
{
    return bytes.size() >= 0x200 && xsflib::LoadLe16(&bytes[0x15C]) == 0xCF56;
}

bool IsDsiImage(std::span<const uint8_t> image)
{
    return image.size() > 0x12 && (image[0x12] & 0x02) != 0;
}

ntdlib::ntr::DsiKeys DsiKeysOf(std::span<const uint8_t> bytes)
{
    ntdlib::ntr::DsiKeys keys;
    if (bytes.size() != keys.digest.size())
    {
        throw std::invalid_argument("A key file must contain the 64-byte DSi digest key. This one has " +
                                    std::to_string(bytes.size()) + " bytes.");
    }

    std::ranges::copy(bytes, keys.digest.begin());

    return keys;
}

std::vector<uint8_t> CartridgeImage(const Cartridge& cartridge)
{
    if (cartridge.image->size() > cartridge.size)
    {
        throw std::invalid_argument("The cartridge image is larger than its stated size.");
    }

    std::vector<uint8_t> image(cartridge.size, 0xFF);
    std::ranges::copy(*cartridge.image, image.begin());

    return image;
}

namespace
{

// The cartridge image of a file of a set of DSE files: each file of the set whose bytes changed is written into the
// image.
DsImageExport ExportDseImage(const File& file, const std::optional<ntdlib::ntr::DsiKeys>& keys)
{
    std::vector<uint8_t> image = CartridgeImage(*file.native->cartridge);
    const ntdlib::ntr::Rom rom = ntdlib::ntr::Rom::Parse(image);
    std::map<std::string, const ntdlib::ntr::RomFile*> by_name;
    for (const ntdlib::ntr::RomFile& rom_file : rom.Files())
    {
        by_name.emplace(DseFileName(rom_file), &rom_file);
    }

    const std::vector<uint8_t> set = ExportArchive(file);
    std::map<uint32_t, std::vector<uint8_t>> changed;
    for (const ntdlib::dse::NamedFile& named : ntdlib::dse::ReadSet(set))
    {
        const auto found = by_name.find(named.path);
        if (found == by_name.end())
        {
            throw std::invalid_argument("The cartridge image contains no file " + named.path + ".");
        }

        if (!std::ranges::equal(named.bytes, rom.FileData(found->second->id)))
        {
            changed.emplace(found->second->id, std::vector<uint8_t>(named.bytes.begin(), named.bytes.end()));
        }
    }
    if (changed.empty())
    {
        return {std::move(image), {}};
    }

    ntdlib::ntr::RomWrite written = ntdlib::ntr::WithFiles(image, changed, keys);
    return {std::move(written.image), std::move(written.stale)};
}

} // namespace

DsImageExport ExportDsImage(const File& file, const std::optional<ntdlib::ntr::DsiKeys>& keys)
{
    if (!file.native || (file.native->engine != Engine::kDs && file.native->engine != Engine::kDse) ||
        !file.native->cartridge)
    {
        throw std::invalid_argument("The file contains no DS cartridge image to export.");
    }

    if (file.native->engine == Engine::kDse)
    {
        return ExportDseImage(file, keys);
    }

    // The new archive replaces the game's archive in its file.
    const Cartridge& cartridge = *file.native->cartridge;
    std::vector<uint8_t> image = CartridgeImage(cartridge);
    const std::vector<uint8_t> archive = ExportArchive(file);
    const ntdlib::ntr::Rom rom = ntdlib::ntr::Rom::Parse(image);
    const std::span<const uint8_t> data = rom.FileData(cartridge.file);
    if (uint64_t(cartridge.offset) + 16 > data.size())
    {
        throw std::invalid_argument("The cartridge image has no sound archive at the expected place in its file.");
    }

    const uint32_t old_size = xsflib::LoadLe32(&data[cartridge.offset + 8]);
    if (uint64_t(cartridge.offset) + old_size > data.size())
    {
        throw std::invalid_argument("The sound archive in the cartridge image runs past the end of its file.");
    }
    if (std::ranges::equal(archive, data.subspan(cartridge.offset, old_size)))
    {
        return {std::move(image), {}};
    }
    if (archive.size() != old_size && (cartridge.offset != 0 || old_size != data.size()))
    {
        throw std::invalid_argument(
            "The edits change the size of the sound archive. The archive shares its file in the image with other "
            "data, and that data can't be moved.");
    }

    std::vector<uint8_t> bytes(data.begin(), data.begin() + cartridge.offset);
    bytes.insert(bytes.end(), archive.begin(), archive.end());
    bytes.insert(bytes.end(), data.begin() + cartridge.offset + old_size, data.end());
    ntdlib::ntr::RomWrite written = ntdlib::ntr::WithFiles(image, {{cartridge.file, std::move(bytes)}}, keys);

    return {std::move(written.image), std::move(written.stale)};
}

bool DsArchiveSharesFile(const File& file)
{
    if (!file.native || !file.native->cartridge)
    {
        return false;
    }

    const Cartridge& cartridge = *file.native->cartridge;
    const std::vector<uint8_t> image = CartridgeImage(cartridge);
    const std::span<const uint8_t> data = ntdlib::ntr::Rom::Parse(image).FileData(cartridge.file);
    return cartridge.offset != 0 || data.size() < 16 || xsflib::LoadLe32(&data[8]) != data.size();
}

std::vector<std::optional<uint32_t>> DsSoundSequences(const File& file, std::span<const uint8_t> sdat)
{
    if (!file.native || file.native->engine != Engine::kDs)
    {
        return {};
    }

    // Each sequence piece's start.
    const std::vector<ntdlib::sar::Piece>& pieces = *file.native->pieces;
    const std::vector<uint64_t> starts = NativePieceStarts(file);
    const std::map<uint64_t, uint32_t> sequence_at = SequenceStarts<uint64_t>(pieces, starts);

    // A sequence file's bytecode starts at the file's offset in the FAT, after the file's headers.
    const ntdlib::sdat::Archive archive = ntdlib::sdat::Archive::Parse(sdat);
    std::vector<std::optional<uint32_t>> sequences(archive.sequences.size());
    for (std::size_t i = 0; i < archive.sequences.size(); i++)
    {
        const std::optional<ntdlib::sdat::SequenceInfo>& info = archive.sequences[i];
        if (!info || info->file_id >= archive.files.size())
        {
            continue;
        }

        try
        {
            const uint32_t data = ntdlib::sdat::Sequence::Parse(archive.File(info->file_id)).data_offset;
            const auto found = sequence_at.find(uint64_t{archive.files[info->file_id].offset} + data);
            if (found != sequence_at.end())
            {
                sequences[i] = found->second;
            }
        }
        catch (const xsflib::FormatError&)
        {
        }
    }

    return sequences;
}

std::vector<std::optional<uint32_t>> DseSoundSequences(const File& file, std::span<const uint8_t> set,
                                                       std::span<const ntdlib::dse::SequenceRef> sounds)
{
    if (!file.native || file.native->engine != Engine::kDse)
    {
        return {};
    }

    const std::vector<uint64_t> starts = NativePieceStarts(file);
    const std::map<uint64_t, uint32_t> sequence_at = SequenceStarts<uint64_t>(*file.native->pieces, starts);

    // A sound's tracks are in its file. The set's files point into the set.
    std::map<std::string_view, std::span<const uint8_t>> files;
    const std::vector<ntdlib::dse::NamedFile> named = ntdlib::dse::ReadSet(set);
    for (const ntdlib::dse::NamedFile& each : named)
    {
        files.emplace(each.path, each.bytes);
    }

    std::vector<std::optional<uint32_t>> sequences(sounds.size());
    for (std::size_t i = 0; i < sounds.size(); i++)
    {
        const auto found = files.find(sounds[i].path);
        const std::optional<uint32_t> offset =
            found == files.end() ? std::nullopt : ntdlib::dse::TracksOffset(found->second, sounds[i].effect);
        if (offset)
        {
            const auto sequence = sequence_at.find(uint64_t(found->second.data() - set.data()) + *offset);
            if (sequence != sequence_at.end())
            {
                sequences[i] = sequence->second;
            }
        }
    }

    return sequences;
}

} // namespace hokuto::format
