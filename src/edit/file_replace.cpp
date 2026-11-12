// SPDX-License-Identifier: MIT

// The replacement of a file of the 3DS, the Wii U or the Switch in edit/files.h, and the changes to a wave archive:
// ReplaceFile, ChangesOfWave, WaveArchiveWith and WaveArchiveWithNew. files.cpp reads the incoming files and lists
// their changes.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/rsar/layout.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/layout.h"
#include "ntdlib/sar/wave_file.h"
#include "xsflib/binary.h"

#include "edit/files.h"
#include "edit/files_internal.h"
#include "edit/files_parts.h"
#include "format/archive.h"
#include "format/kinds.h"
#include "song/archive.h"

namespace hokuto::edit
{

namespace
{

namespace sar = ntdlib::sar;
using files_parts::BanksNaming;
using files_parts::BytesOfCopy;
using files_parts::Names;
using files_parts::WaveSoundsPlaying;

// The pieces in which each copy of file `file_id` of `file`, a file of the 3DS, the Wii U or the Switch, holds `bytes`
// in the place of the present file. A group's copy that names the present file's waves at other places takes `bytes`
// with those places (BytesOfCopy). The present file is the one that the archive that `file` builds gives, with the
// group files. Throws the exceptions of ntdlib's ReplaceFile.
sar::ReplacedPieces ReplacedCopies(const format::File& file, uint32_t file_id, std::span<const uint8_t> bytes)
{
    const format::NativeArchive& native = *file.native;
    const std::vector<ntdlib::commands::Sequence> sequences = format::CopySequences(file);
    const std::vector<sar::GroupFileLayout> group_files = format::GroupFileLayouts(native);
    const std::vector<sar::FileCopy> copies =
        sar::FileCopies(FlavorOf(native), sequences, *native.pieces, native.fields, group_files, file_id);
    const auto same = [&copies](const sar::FileCopy& copy)
    {
        return copy.bytes == copies.front().bytes;
    };
    if (copies.empty() || std::ranges::all_of(copies, same))
    {
        return sar::ReplaceFile(FlavorOf(native), sequences, *native.pieces, native.fields, group_files, file_id,
                                bytes);
    }

    // The copies differ. The archive that the file builds gives the present file.
    const std::shared_ptr<const sar::SoundArchive> archive =
        song::LoadArchive(format::ExportArchive(file), "the archive", format::ExportGroupFiles(file));
    const std::span<const uint8_t> present = archive->FileData(file_id);

    const auto copy_bytes = [&](const sar::FileCopy& copy)
    {
        return BytesOfCopy(copy.bytes, present, bytes, FlavorOf(native)).bytes;
    };
    return sar::ReplaceFile(FlavorOf(native), sequences, *native.pieces, native.fields, group_files, file_id,
                            copy_bytes);
}

// The pieces of `file`, a file of a game archive, with `bytes` in the place of file `file_id`, and on the Wii
// `wave_data` beside it, in the archive and in the copies in groups (ReplaceFile). Throws std::invalid_argument with
// ntdlib's reasons in words for the user.
sar::ReplacedPieces ReplacedPiecesOf(const format::File& file, uint32_t file_id, std::span<const uint8_t> bytes,
                                     std::optional<std::span<const uint8_t>> wave_data)
{
    const format::NativeArchive& native = *file.native;
    sar::ReplacedPieces pieces;
    try
    {
        if (native.engine == format::Engine::kWii)
        {
            pieces.pieces = ntdlib::rsar::ReplaceFile(format::BuildOfWiiArchive(native), format::CopySequences(file),
                                                      *native.pieces, native.fields, file_id, bytes, wave_data);
        }
        else
        {
            pieces = ReplacedCopies(file, file_id, bytes);
        }
    }
    catch (const std::invalid_argument& error)
    {
        const std::string reason = error.what();
        if (reason.find("shares its bytes") != std::string::npos)
        {
            throw std::invalid_argument(
                "Another file in the archive shares the same bytes. That file would change too.");
        }
        if (reason.find("isn't a piece of its own") != std::string::npos && native.engine == format::Engine::kWii)
        {
            throw std::invalid_argument(
                "The old file is mixed in with other data in the archive. Can't replace it there. An archive that is "
                "cut short can leave a file like this.");
        }
        if (reason.find("isn't a piece of its own") != std::string::npos)
        {
            throw std::invalid_argument(
                "The old file is mixed in with other data in the archive. Can't replace it there. A .hok file saved "
                "before 2026-10-17 stores every bank, wave archive and wave sound file this way. Import the game "
                "archive again to separate them.");
        }
        if (reason.find("holds no file") != std::string::npos)
        {
            throw std::invalid_argument("The old file is outside the archive. Can't replace it there.");
        }
        if (reason.find("no group of the archive holds the wave data") != std::string::npos)
        {
            throw std::invalid_argument("The old file has no wave data beside it.");
        }
        if (reason.find("no group of the archive holds") != std::string::npos)
        {
            throw std::invalid_argument("No group in the archive contains the old file. Can't replace it there.");
        }

        throw std::invalid_argument("Can't update the archive tables for the new file. " + format::AsSentence(reason));
    }
    catch (const xsflib::FormatError& error)
    {
        throw std::invalid_argument(std::string("Can't read the archive tables. ") + format::AsSentence(error.what()));
    }

    return pieces;
}

} // namespace

format::File ReplaceFile(const format::File& file, uint32_t file_id, std::span<const uint8_t> bytes,
                         std::optional<std::span<const uint8_t>> wave_data)
{
    if (!file.native)
    {
        throw std::invalid_argument("The file contains no game archive.");
    }
    if (wave_data && file.native->engine != format::Engine::kWii)
    {
        throw std::invalid_argument("Only a Wii file has wave data beside it.");
    }

    // ntdlib's reasons, in words for the user. The copies in the group files beside the archive take the bytes too. On
    // the Wii, every group that holds the file holds a copy.
    sar::ReplacedPieces pieces = ReplacedPiecesOf(file, file_id, bytes, wave_data);

    const auto take_pieces = [&pieces](format::NativeArchive& edited)
    {
        if (*edited.pieces != pieces.pieces)
        {
            edited.pieces = std::move(pieces.pieces);
        }

        for (std::size_t g = 0; g < edited.group_files.size() && g < pieces.group_files.size(); ++g)
        {
            if (*edited.group_files[g].pieces != pieces.group_files[g])
            {
                edited.group_files[g].pieces = std::move(pieces.group_files[g]);
            }
        }
    };
    format::File result = file;
    result.native.Edit(take_pieces);

    // The models of the file's bank or waves no longer give its bytes.
    result.banks.erase(file_id);
    const auto in_file = [file_id](const auto& wave)
    {
        return wave.first.file == file_id;
    };
    std::erase_if(result.waves, in_file);

    return result;
}

WaveChanges ChangesOfWave(const sar::SoundArchive& archive, uint32_t wave_archive, uint32_t wave)
{
    const sar::WaveArchiveFile file = WaveArchiveFileOf(archive, wave_archive);
    const sar::WaveFile present = WaveFileOf(file, wave);

    WaveChanges changes;
    changes.encoding = present.encoding;
    changes.channels = present.channels.size();
    changes.rate = present.sample_rate;
    changes.loop = present.loop;
    changes.loop_start = present.loop_start;
    changes.loop_end = present.loop_end;
    changes.size = file.waves[wave].size();
    const auto named = [&](const sar::WaveId& id)
    {
        return Names(id, wave_archive) && id.index == wave;
    };
    changes.banks = BanksNaming(archive, named);
    changes.sounds = WaveSoundsPlaying(archive, named);

    return changes;
}

std::vector<uint8_t> WaveArchiveWith(const sar::SoundArchive& archive, uint32_t wave_archive,
                                     std::optional<uint32_t> wave, std::span<const uint8_t> wave_file)
{
    sar::WaveArchiveFile file = WaveArchiveFileOf(archive, wave_archive);
    if (wave)
    {
        CheckWave(file, *wave);
        file.waves[*wave].assign(wave_file.begin(), wave_file.end());
    }
    else
    {
        file.waves.emplace_back(wave_file.begin(), wave_file.end());
    }

    return file.Write();
}

std::vector<uint8_t> WaveArchiveWithNew(const sar::SoundArchive& archive, uint32_t wave_archive,
                                        std::span<const std::vector<uint8_t>> wave_files)
{
    sar::WaveArchiveFile file = WaveArchiveFileOf(archive, wave_archive);
    file.waves.insert(file.waves.end(), wave_files.begin(), wave_files.end());

    return file.Write();
}

} // namespace hokuto::edit
