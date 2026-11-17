// SPDX-License-Identifier: MIT

#include "app/file_saves.h"

#include <optional>
#include <system_error>

#include "xsflib/files.h"

#if !defined(_WIN32)
#include <unistd.h>

#include <cerrno>
#endif

namespace hokuto::app
{

std::filesystem::path SaveTarget(const std::filesystem::path& file)
{
#if defined(_WIN32)
    return file;
#else
    // The most symbolic links that SaveTarget follows: the limit of Linux.
    constexpr int kMostLinks = 40;
    std::filesystem::path target = file;
    for (int links = 0; links <= kMostLinks; links++)
    {
        std::error_code error;
        if (!std::filesystem::is_symlink(std::filesystem::symlink_status(target, error)))
        {
            return target;
        }

        const std::filesystem::path link = std::filesystem::read_symlink(target, error);
        if (error)
        {
            return {};
        }
        target = link.is_absolute() ? link : target.parent_path() / link;
    }

    return {};
#endif
}

SaveResult SaveFile(const std::filesystem::path& file, std::span<const uint8_t> bytes)
{
    const std::filesystem::path target = SaveTarget(file);
    if (target.empty())
    {
        return SaveResult::kFailed;
    }

#if !defined(_WIN32)
    // A rename requires only the folder's write permission. A file that the user may not write to is refused first. On
    // Windows, the rename itself fails on a read-only file.
    std::error_code status_error;
    const std::filesystem::file_status status = std::filesystem::status(target, status_error);
    const bool exists = std::filesystem::exists(status);
    // A file of the Mac that Finder locked (the flag uchg) gives EPERM, and a file system mounted read-only EROFS.
    if (exists && access(target.c_str(), W_OK) != 0 && (errno == EACCES || errno == EPERM || errno == EROFS))
    {
        return SaveResult::kReadOnly;
    }
#endif

    std::filesystem::path saving = target;
    saving += ".saving";
    bool written = !xsflib::WriteFile(saving, bytes).has_value();

#if !defined(_WIN32)
    // The new file gets the old file's permissions: read, write and execute for the owner, the group and the others.
    if (written && exists)
    {
        std::error_code changed;
        std::filesystem::permissions(saving, status.permissions() & std::filesystem::perms::all,
                                     std::filesystem::perm_options::replace, changed);
        written = !changed;
    }
#endif

    std::error_code moved;
    if (written)
    {
        std::filesystem::rename(saving, target, moved);
    }
    if (!written || moved)
    {
        std::error_code removed;
        std::filesystem::remove(saving, removed);
        return SaveResult::kFailed;
    }

    return SaveResult::kSaved;
}

} // namespace hokuto::app
