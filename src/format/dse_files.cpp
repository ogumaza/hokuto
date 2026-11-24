// SPDX-License-Identifier: MIT

#include "format/dse_files.h"

#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/dse/files.h"
#include "ntdlib/dse/game_starts.h"
#include "ntdlib/sequence_export/dse/sequence_input.h"
#include "xsflib/bytes.h"

namespace hokuto::format
{
namespace
{

// Whether `inner` lies within `outer`.
bool Within(std::span<const uint8_t> inner, std::span<const uint8_t> outer)
{
    const std::less<const uint8_t*> before;
    return !before(inner.data(), outer.data()) && !before(outer.data() + outer.size(), inner.data() + inner.size());
}

// The bytes of the file of `holders` that holds the DSE file at `path`: the file of that path, or the file whose path
// comes before the last '/' that leaves one. Nothing where no file does.
std::span<const uint8_t> HolderOf(std::span<const ntdlib::dse::NamedFile> holders, std::string_view path)
{
    std::string_view holder = path;
    while (true)
    {
        const auto found = std::ranges::find(holders, holder, &ntdlib::dse::NamedFile::path);
        if (found != holders.end())
        {
            return found->bytes;
        }

        const std::size_t slash = holder.rfind('/');
        if (slash == std::string_view::npos)
        {
            return {};
        }

        holder = holder.substr(0, slash);
    }
}

// The names of the image's files (DseFileName), with their numbers.
std::map<std::string, uint32_t, std::less<>> FileIds(const ntdlib::ntr::Rom& rom)
{
    std::map<std::string, uint32_t, std::less<>> ids;
    for (const ntdlib::ntr::RomFile& file : rom.Files())
    {
        ids.emplace(DseFileName(file), file.id);
    }

    return ids;
}

} // namespace

std::string DseFileName(const ntdlib::ntr::RomFile& file)
{
    return file.path.empty() ? "file " + std::to_string(file.id) : file.path;
}

std::vector<DseImageFile> DseFilesOf(const ntdlib::ntr::Rom& rom, std::vector<std::vector<uint8_t>>& storage)
{
    std::vector<ntdlib::dse::NamedFile> named;
    for (const ntdlib::ntr::RomFile& file : rom.Files())
    {
        named.push_back({DseFileName(file), rom.FileData(file.id)});
    }
    const std::map<std::string, uint32_t, std::less<>> ids = FileIds(rom);
    const std::vector<ntdlib::dse::NamedFile> holders = ntdlib::dse::Decompressed(named, storage);

    // A file found keeps the number of the image's file that holds it: the file of its path, or the pack whose path
    // comes before one of its '/'. A file outside the bytes of the image's file sits in decompressed bytes.
    std::vector<DseImageFile> found;
    for (ntdlib::dse::NamedFile& file : ntdlib::dse::FindFiles(holders))
    {
        std::string_view holder = file.path;
        auto id = ids.find(holder);
        while (id == ids.end() && holder.rfind('/') != std::string_view::npos)
        {
            holder = holder.substr(0, holder.rfind('/'));
            id = ids.find(holder);
        }
        if (id == ids.end())
        {
            throw std::logic_error("ntdlib::dse::FindFiles gave " + file.path + ", which no file of the image holds");
        }

        const std::span<const uint8_t> data = rom.FileData(id->second);
        const bool loose = file.bytes.data() == data.data() && file.bytes.size() == data.size();
        const bool compressed = !Within(file.bytes, data);
        std::span<const uint8_t> base = data;
        if (compressed)
        {
            const auto holds = [&file](const ntdlib::dse::NamedFile& decompressed)
            {
                return Within(file.bytes, decompressed.bytes);
            };
            const auto decompressed = std::ranges::find_if(holders, holds);
            if (decompressed == holders.end())
            {
                throw std::logic_error("ntdlib::dse::FindFiles gave " + file.path + " outside the files it read");
            }

            base = decompressed->bytes;
        }

        const auto offset = static_cast<uint32_t>(file.bytes.data() - base.data());
        found.push_back({std::move(file), id->second, id->first, loose, offset, compressed});
    }

    return found;
}

std::vector<DseImageFile> DseFilesAt(const ntdlib::ntr::Rom& rom, std::span<const DsePlace> places,
                                     std::vector<std::vector<uint8_t>>& storage)
{
    const std::map<std::string, uint32_t, std::less<>> ids = FileIds(rom);

    // Each image's file of a compressed place is decompressed once.
    std::map<uint32_t, std::vector<ntdlib::dse::NamedFile>> decompressed;
    std::vector<DseImageFile> found;
    for (const DsePlace& place : places)
    {
        const auto id = ids.find(place.image_file);
        if (id == ids.end())
        {
            throw std::invalid_argument("The cartridge image contains no file " + place.image_file + ".");
        }

        const std::span<const uint8_t> data = rom.FileData(id->second);
        std::span<const uint8_t> base = data;
        if (place.compressed)
        {
            auto [holders, added] = decompressed.try_emplace(id->second);
            if (added)
            {
                const ntdlib::dse::NamedFile file{place.image_file, data};
                holders->second = ntdlib::dse::Decompressed(std::span(&file, 1), storage);
            }

            base = HolderOf(holders->second, place.path);
        }
        if (place.offset > base.size() || place.size > base.size() - place.offset)
        {
            throw std::invalid_argument("The cartridge image doesn't match the file: " + place.path +
                                        " isn't where the file puts it.");
        }

        const std::span<const uint8_t> bytes = base.subspan(place.offset, place.size);
        const bool loose = !place.compressed && bytes.data() == data.data() && bytes.size() == data.size();
        found.push_back({{place.path, bytes}, id->second, id->first, loose, place.offset, place.compressed});
    }

    return found;
}

DsePlace DsePlaceOf(const DseImageFile& file)
{
    return {.path = file.named.path,
            .image_file = file.image_path,
            .offset = file.offset,
            .size = static_cast<uint32_t>(file.named.bytes.size()),
            .compressed = file.compressed};
}

static_assert(kFullDseLevel == ntdlib::dse::kFullLevel);

DseLevels ReadDseLevels(const ntdlib::ntr::Rom& rom)
{
    DseLevels levels;
    try
    {
        levels.read.emplace(rom);
    }
    catch (const xsflib::FormatError& error)
    {
        // The code of the image differs from the code that ntdlib's profile of the game reads.
        levels.problem = error.what();
    }
    catch (const std::out_of_range&)
    {
        // A guard: no read on this path is known to throw it. The text of the exception means nothing to the user.
        levels.problem = "an unexpected error";
    }

    return levels;
}

std::optional<std::string> DseLevelsNote(const ntdlib::ntr::Rom& rom)
{
    const DseLevels levels = ReadDseLevels(rom);
    if (levels.read)
    {
        return std::nullopt;
    }

    return "Can't read the levels at which this game starts its songs (" + levels.problem +
           "). Its songs play at the levels that Hokuto's list gives the game, or at full level.";
}

std::vector<SoundStart> DseGameStarts(const ntdlib::ntr::Rom& rom, std::span<const ntdlib::dse::SequenceRef> sounds)
{
    // ntdlib's converter takes the table's rows where the game's code doesn't match the profile.
    const DseLevels levels = ReadDseLevels(rom);
    const std::optional<ntdlib::dse::GameStarts>& read = levels.read;

    // A sound effect starts at full level.
    const std::string game_code = rom.Header().game_code;
    std::vector<SoundStart> starts;
    for (std::size_t sound = 0; sound < sounds.size(); sound++)
    {
        if (sounds[sound].effect)
        {
            continue;
        }

        const std::vector<ntdlib::dse::SongState> states =
            read ? read->StatesOf(sounds[sound].path) : ntdlib::dse::SongStatesOf(game_code, sounds[sound].path);
        for (const ntdlib::dse::SongState& state : states)
        {
            if (state.suffix.empty() && state.level == kFullDseLevel)
            {
                continue;
            }

            starts.push_back({.sound = static_cast<uint32_t>(sound), .name = state.suffix, .dse_level = state.level});
        }
    }

    return starts;
}

std::vector<SoundStart> DseGameStarts(const ntdlib::ntr::Rom& rom, std::span<const ntdlib::dse::NamedFile> files)
{
    std::vector<std::string> warnings;
    std::vector<ntdlib::dse::SequenceRef> sounds;
    for (const ntdlib::sequence_export::dse::SequenceInput& sound :
         ntdlib::sequence_export::dse::ReadSequences(files, warnings))
    {
        sounds.push_back(sound.ref);
    }

    return DseGameStarts(rom, sounds);
}

} // namespace hokuto::format
