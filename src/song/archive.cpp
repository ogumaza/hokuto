// SPDX-License-Identifier: MIT

#include "song/archive.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "ntdlib/nwsnd/console.h"
#include "ntdlib/sequence_export/archive_converter.h"
#include "ntdlib/sequence_export/consoles.h"
#include "xsflib/binary.h"
#include "xsflib/files.h"

#include "song/items.h"

namespace hokuto::song
{
namespace
{

// The converter's settings for the archives of `flavor`.
ntdlib::sequence_export::Settings SettingsOf(ntdlib::sar::Flavor flavor)
{
    switch (flavor)
    {
    case ntdlib::sar::Flavor::kCafe:
        return ntdlib::sequence_export::CafeSettings();
    case ntdlib::sar::Flavor::kCtr:
        return ntdlib::sequence_export::CtrSettings();
    case ntdlib::sar::Flavor::kNx:
        break;
    }

    return ntdlib::sequence_export::SwitchSettings();
}

// The sound archive of the Switch, the Wii U or the 3DS in `bytes`. Messages call it `name`.
std::shared_ptr<ntdlib::sar::SoundArchive> ReadTables(std::vector<uint8_t> bytes, const std::string& name)
{
    // FSAR on the Switch and the Wii U, and CSAR on the 3DS. Bytes of another kind name no console.
    if (bytes.size() < 4 || (std::memcmp(bytes.data(), "FSAR", 4) != 0 && std::memcmp(bytes.data(), "CSAR", 4) != 0))
    {
        throw std::runtime_error(name + " isn't a sound archive of the Switch, the Wii U, the 3DS or the Wii.");
    }

    const ntdlib::sar::Flavor flavor = FlavorOfArchive(bytes);
    const ntdlib::sequence_export::Settings settings = SettingsOf(flavor);
    try
    {
        return std::make_shared<ntdlib::sar::SoundArchive>(
            ntdlib::sar::SoundArchive::Load(std::move(bytes), settings.archive));
    }
    catch (const xsflib::FormatError& error)
    {
        throw std::runtime_error("Hokuto can't read " + name + " as a sound archive of the " + ConsoleName(flavor) +
                                 ": " + error.what() + ".");
    }
}

} // namespace

std::vector<std::pair<std::string, std::vector<uint8_t>>> SarcArchivesIn(std::span<const std::filesystem::path> folders)
{
    std::vector<std::pair<std::string, std::vector<uint8_t>>> sarcs;
    std::set<std::string> names;
    for (std::size_t f = 0; f < folders.size(); ++f)
    {
        const std::filesystem::path& folder = folders[f];
        const auto same = [&folder](const std::filesystem::path& earlier)
        {
            std::error_code error;
            return std::filesystem::equivalent(earlier, folder, error);
        };
        if (std::any_of(folders.begin(), folders.begin() + static_cast<std::ptrdiff_t>(f), same))
        {
            continue;
        }

        std::vector<std::string> found;
        std::error_code error;
        for (std::filesystem::directory_iterator entry(folder.empty() ? std::filesystem::path(".") : folder, error);
             !error && entry != std::filesystem::directory_iterator(); entry.increment(error))
        {
            if (entry->path().extension() == ".sarc")
            {
                found.push_back(xsflib::Utf8(entry->path().filename()));
            }
        }

        std::ranges::sort(found);
        for (const std::string& name : found)
        {
            std::vector<uint8_t> bytes;
            if (!names.contains(name) && xsflib::ReadFile(folder / xsflib::PathFromUtf8(name), bytes))
            {
                names.insert(name);
                sarcs.emplace_back(name, std::move(bytes));
            }
        }
    }

    return sarcs;
}

std::vector<std::pair<std::string, std::vector<uint8_t>>> PlaceGroupFiles(
    std::vector<std::pair<std::string, std::vector<uint8_t>>> outputs, std::span<const std::filesystem::path> folders)
{
    for (std::pair<std::string, std::vector<uint8_t>>& output : outputs)
    {
        const std::filesystem::path file = xsflib::PathFromUtf8(output.first);
        if (file.extension() != ".bcgrp" && file.extension() != ".bfgrp")
        {
            continue;
        }

        // The import reads the first file of the name that it finds: beside the archive, then in the folder.
        for (const std::filesystem::path& folder : folders)
        {
            std::error_code error;
            if (std::filesystem::exists(folder / file, error))
            {
                break;
            }
            if (std::filesystem::exists(folder / kUserManagementFolder / file, error))
            {
                output.first = std::string(kUserManagementFolder) + "/" + output.first;
                break;
            }
        }
    }

    return outputs;
}

ntdlib::sar::Flavor FlavorOfArchive(std::span<const uint8_t> bytes)
{
    // A 3DS archive's magic is CSAR. The byte-order mark follows the magic: FE FF in a big-endian archive.
    if (!bytes.empty() && bytes[0] == 'C')
    {
        return ntdlib::sar::Flavor::kCtr;
    }
    if (bytes.size() >= 6 && bytes[4] == 0xfe && bytes[5] == 0xff)
    {
        return ntdlib::sar::Flavor::kCafe;
    }

    return ntdlib::sar::Flavor::kNx;
}

ntdlib::sequence_export::Settings SettingsOf(const ntdlib::sar::SoundArchive& archive)
{
    return SettingsOf(archive.ReadOptions().flavor);
}

std::shared_ptr<const ntdlib::rsar::SoundArchive> LoadWiiArchive(std::vector<uint8_t> bytes, const std::string& name,
                                                                 const ntdlib::rsar::ReadOptions& options)
{
    try
    {
        return std::make_shared<ntdlib::rsar::SoundArchive>(
            ntdlib::rsar::SoundArchive::Load(std::move(bytes), options));
    }
    catch (const xsflib::FormatError& error)
    {
        throw std::runtime_error("Hokuto can't read " + name + " as a sound archive of the Wii: " + error.what() + ".");
    }
}

ntdlib::sequence_export::Settings SettingsOf(const ntdlib::rsar::SoundArchive& archive,
                                             const std::optional<ntdlib::SndBuild>& build)
{
    return ntdlib::sequence_export::RevolutionSettings(
        build.value_or(ntdlib::sequence_export::BuildOfRsarVersion(archive.Version())));
}

std::optional<ntdlib::SndBuild> ReadWiiBuild(std::span<const uint8_t> dol)
{
    // A damaged section of the DOL is left out, and so is the date of a damaged banner.
    const std::optional<ntdlib::Microcode> microcode = ntdlib::FindMicrocode(dol, {.skip_damaged_sections = true});
    if (!microcode)
    {
        return std::nullopt;
    }

    ntdlib::SndBuild build = ntdlib::FindSndBuild(dol, {.check_day = true});
    build.october_2006 = ntdlib::IdentifyBuild(*microcode) == ntdlib::AxBuild::kOctober2006;

    return build;
}

std::optional<std::filesystem::path> FindMainDol(const std::filesystem::path& archive)
{
    std::error_code error;
    const std::filesystem::path absolute = std::filesystem::absolute(archive, error);
    if (error)
    {
        return std::nullopt;
    }

    for (std::filesystem::path folder = absolute.parent_path(); folder.has_relative_path();
         folder = folder.parent_path())
    {
        const std::filesystem::path dol = folder.parent_path() / "sys" / "main.dol";
        if (folder.filename() == "files" && std::filesystem::is_regular_file(dol, error))
        {
            return dol;
        }
    }

    return std::nullopt;
}

ntdlib::nwsnd::Grammar GrammarOf(ntdlib::sar::Flavor flavor)
{
    switch (flavor)
    {
    case ntdlib::sar::Flavor::kCafe:
        return ntdlib::nwsnd::GrammarOf<ntdlib::nwsnd::CafePolicy>();
    case ntdlib::sar::Flavor::kCtr:
        return ntdlib::nwsnd::GrammarOf<ntdlib::nwsnd::CtrPolicy>();
    case ntdlib::sar::Flavor::kNx:
        break;
    }

    return ntdlib::nwsnd::GrammarOf<ntdlib::nwsnd::NxPolicy>();
}

ntdlib::nwsnd::Grammar GrammarOf(const Console& console)
{
    return console.wii ? ntdlib::nwsnd::GrammarOf<ntdlib::nwsnd::RevolutionPolicy>(*console.wii)
                       : GrammarOf(console.flavor);
}

bool Runs(ntdlib::sar::Flavor flavor, ntdlib::commands::Kind kind, uint8_t index)
{
    return Runs(Console{.flavor = flavor}, kind, index);
}

std::string SettingName(const ntdlib::commands::Command& command)
{
    using ntdlib::commands::Kind;
    if (command.kind == Kind::kSend)
    {
        return std::string("send ") + static_cast<char>('A' + command.index);
    }
    if (command.kind == Kind::kOther && command.encoding.opcode < 0xF000)
    {
        return "time prefixes";
    }
    if (command.kind == Kind::kOther || command.index > 0)
    {
        return "LFOs 1 to 3";
    }

    switch (command.kind)
    {
    case Kind::kLfoCurve:
        return "LFO 0's curve";
    case Kind::kLfoPhase:
        return "LFO 0's phase";
    case Kind::kLfoPeriod:
        return "LFO 0's period";
    default:
        break;
    }

    // The text form's name, such as bank_select, with spaces.
    std::string name(ntdlib::commands::Name(command.kind));
    std::ranges::replace(name, '_', ' ');

    return name;
}

bool Runs(const Console& console, ntdlib::commands::Kind kind, uint8_t index)
{
    using ntdlib::commands::Kind;
    const bool skips = console.wii || console.flavor == ntdlib::sar::Flavor::kCtr;
    if (!skips || kind == Kind::kNote || kind == Kind::kOther || kind == Kind::kData ||
        ntdlib::commands::HasTarget(kind))
    {
        return true;
    }

    // The opcode that the console's encoder writes for such a command: a byte, or 0xF000 plus the byte after 0xF0.
    ntdlib::commands::Sequence command;
    command.items.push_back(ntdlib::commands::Command{.kind = kind, .index = index});
    std::vector<uint8_t> bytes;
    try
    {
        bytes = ntdlib::nwsnd::EncodeSequence(command, GrammarOf(console)).bytes;
    }
    catch (const std::invalid_argument&)
    {
        return false;
    }
    if (bytes.empty())
    {
        return false;
    }

    const uint32_t opcode = bytes[0] == 0xF0 && bytes.size() > 1 ? 0xF000u | bytes[1] : bytes[0];
    return console.wii ? ntdlib::nwsnd::RevolutionPolicy::HasCommand(*console.wii, opcode)
                       : ntdlib::nwsnd::CtrPolicy::HasCommand({}, opcode);
}

std::shared_ptr<const ntdlib::sar::SoundArchive> OpenArchive(const std::filesystem::path& path)
{
    const std::string name = xsflib::Utf8(path.filename());
    std::vector<uint8_t> bytes;
    if (!xsflib::ReadFile(path, bytes))
    {
        throw std::runtime_error("Hokuto can't read " + name + ".");
    }

    return LoadArchive(std::move(bytes), name, path);
}

std::shared_ptr<const ntdlib::sar::SoundArchive> LoadArchive(std::vector<uint8_t> bytes, const std::string& name,
                                                             const std::filesystem::path& path)
{
    // The archive's tables, then the group files beside it. The groups have to come before any bank is read.
    std::shared_ptr<ntdlib::sar::SoundArchive> archive = ReadTables(std::move(bytes), name);
    ntdlib::sequence_export::AddGroupFiles(*archive, path, name);

    return archive;
}

std::shared_ptr<const ntdlib::sar::SoundArchive> LoadArchive(std::vector<uint8_t> bytes, const std::string& name,
                                                             std::span<const std::vector<uint8_t>> group_files)
{
    std::shared_ptr<ntdlib::sar::SoundArchive> archive = ReadTables(std::move(bytes), name);
    for (std::size_t i = 0; i < group_files.size(); ++i)
    {
        try
        {
            archive->AddGroupFile(group_files[i]);
        }
        catch (const xsflib::FormatError& error)
        {
            throw std::runtime_error("Group file " + std::to_string(i + 1) + " of " + name + " isn't a group file. " +
                                     error.what());
        }
    }

    return archive;
}

} // namespace hokuto::song
