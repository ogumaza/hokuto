// SPDX-License-Identifier: MIT

// Tests the save of a file over the last save (src/app/file_saves.h), without a window: the new bytes, the file left
// beside it, and on Linux and the Mac, symbolic links, read-only files and the file's permissions.

#include "app/file_saves.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "xsflib/files.h"
#include "xsflib/testing/check.h"

#include "temporary_files.h"

#if !defined(_WIN32)
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <sys/stat.h>
#endif

namespace hokuto
{
namespace
{

using app::SaveFile;
using app::SaveResult;
using app::SaveTarget;
using testing::TemporaryFolder;

const std::vector<uint8_t> kOld = {'o', 'l', 'd'};
const std::vector<uint8_t> kNew = {'n', 'e', 'w', '!'};

std::vector<uint8_t> BytesOf(const std::filesystem::path& file)
{
    std::vector<uint8_t> bytes;
    xsflib::ReadFile(file, bytes);
    return bytes;
}

// A save makes a new file and replaces an old one. No file stays beside it.
void TestSave()
{
    const TemporaryFolder folder;
    const std::filesystem::path file = folder.Path() / "a.hok";
    XSFLIB_CHECK(SaveTarget(file) == file);
    XSFLIB_CHECK(SaveFile(file, kOld) == SaveResult::kSaved);
    XSFLIB_CHECK(BytesOf(file) == kOld);
    XSFLIB_CHECK(SaveFile(file, kNew) == SaveResult::kSaved);
    XSFLIB_CHECK(BytesOf(file) == kNew);
    XSFLIB_CHECK(!std::filesystem::exists(folder.Path() / "a.hok.saving"));

    // A folder that doesn't exist fails the save.
    XSFLIB_CHECK(SaveFile(folder.Path() / "none" / "b.hok", kNew) == SaveResult::kFailed);
}

#if !defined(_WIN32)

// A save through a symbolic link keeps the link and changes the file that it leads to. A relative link starts at the
// link's folder, and a chain of links leads to its last file. A link to no file makes that file.
void TestLinks()
{
    const TemporaryFolder folder;
    const std::filesystem::path songs = folder.Path() / "songs";
    std::filesystem::create_directory(songs);
    const std::filesystem::path real = songs / "real.hok";
    XSFLIB_REQUIRE(!xsflib::WriteFile(real, kOld).has_value());

    const std::filesystem::path link = folder.Path() / "link.hok";
    std::filesystem::create_symlink("songs/real.hok", link);
    XSFLIB_CHECK(SaveTarget(link) == folder.Path() / "songs/real.hok");
    XSFLIB_CHECK(SaveFile(link, kNew) == SaveResult::kSaved);
    XSFLIB_CHECK(std::filesystem::is_symlink(std::filesystem::symlink_status(link)));
    XSFLIB_CHECK(BytesOf(real) == kNew);
    XSFLIB_CHECK(!std::filesystem::exists(songs / "real.hok.saving"));

    const std::filesystem::path chain = folder.Path() / "chain.hok";
    std::filesystem::create_symlink(link, chain);
    XSFLIB_CHECK(SaveFile(chain, kOld) == SaveResult::kSaved);
    XSFLIB_CHECK(std::filesystem::is_symlink(std::filesystem::symlink_status(chain)));
    XSFLIB_CHECK(std::filesystem::is_symlink(std::filesystem::symlink_status(link)));
    XSFLIB_CHECK(BytesOf(real) == kOld);

    const std::filesystem::path dangling = folder.Path() / "dangling.hok";
    std::filesystem::create_symlink("songs/new.hok", dangling);
    XSFLIB_CHECK(SaveFile(dangling, kNew) == SaveResult::kSaved);
    XSFLIB_CHECK(std::filesystem::is_symlink(std::filesystem::symlink_status(dangling)));
    XSFLIB_CHECK(BytesOf(songs / "new.hok") == kNew);

    // An absolute link keeps its link too.
    const std::filesystem::path absolute = folder.Path() / "absolute.hok";
    std::filesystem::create_symlink(real, absolute);
    XSFLIB_CHECK(SaveFile(absolute, kNew) == SaveResult::kSaved);
    XSFLIB_CHECK(std::filesystem::is_symlink(std::filesystem::symlink_status(absolute)));
    XSFLIB_CHECK(BytesOf(real) == kNew);

    // Links that lead round to each other lead to no file.
    std::filesystem::create_symlink("loop_b.hok", folder.Path() / "loop_a.hok");
    std::filesystem::create_symlink("loop_a.hok", folder.Path() / "loop_b.hok");
    XSFLIB_CHECK(SaveTarget(folder.Path() / "loop_a.hok").empty());
    XSFLIB_CHECK(SaveFile(folder.Path() / "loop_a.hok", kNew) == SaveResult::kFailed);
}

// A chain of 40 links leads to its last file. A chain of 41 leads to no file, and the file at its end stays as it is.
void TestChainLength()
{
    const TemporaryFolder folder;
    const std::filesystem::path real = folder.Path() / "real.hok";
    XSFLIB_REQUIRE(!xsflib::WriteFile(real, kOld).has_value());
    std::filesystem::path last = real;
    for (int links = 1; links <= 41; links++)
    {
        const std::filesystem::path link = folder.Path() / ("link_" + std::to_string(links) + ".hok");
        std::filesystem::create_symlink(last.filename(), link);
        last = link;
    }

    const std::filesystem::path forty = folder.Path() / "link_40.hok";
    XSFLIB_CHECK(SaveTarget(forty) == real);
    XSFLIB_CHECK(SaveFile(forty, kNew) == SaveResult::kSaved);
    XSFLIB_CHECK(BytesOf(real) == kNew);
    XSFLIB_CHECK(SaveTarget(last).empty());
    XSFLIB_CHECK(SaveFile(last, kOld) == SaveResult::kFailed);
    XSFLIB_CHECK(BytesOf(real) == kNew);
    XSFLIB_CHECK(std::filesystem::is_symlink(std::filesystem::symlink_status(last)));
}

// A link to a folder, and a link into a folder that is read-only or missing, fail the save. The link and the folder
// stay as they are. The superuser may write to any folder, and the read-only folder isn't checked then.
void TestLinksThatFail()
{
    using std::filesystem::perms;
    const TemporaryFolder folder;

    const std::filesystem::path songs = folder.Path() / "songs";
    std::filesystem::create_directory(songs);
    const std::filesystem::path to_folder = folder.Path() / "folder.hok";
    std::filesystem::create_symlink("songs", to_folder);
    XSFLIB_CHECK(SaveFile(to_folder, kNew) == SaveResult::kFailed);
    XSFLIB_CHECK(std::filesystem::is_directory(songs));
    XSFLIB_CHECK(std::filesystem::is_symlink(std::filesystem::symlink_status(to_folder)));
    XSFLIB_CHECK(!std::filesystem::exists(folder.Path() / "songs.saving"));

    const std::filesystem::path to_missing = folder.Path() / "missing.hok";
    std::filesystem::create_symlink("none/a.hok", to_missing);
    XSFLIB_CHECK(SaveFile(to_missing, kNew) == SaveResult::kFailed);
    XSFLIB_CHECK(std::filesystem::is_symlink(std::filesystem::symlink_status(to_missing)));
    XSFLIB_CHECK(!std::filesystem::exists(folder.Path() / "none"));

    const std::filesystem::path locked = folder.Path() / "locked";
    std::filesystem::create_directory(locked);
    const std::filesystem::path file = locked / "a.hok";
    XSFLIB_REQUIRE(!xsflib::WriteFile(file, kOld).has_value());
    const std::filesystem::path to_locked = folder.Path() / "locked.hok";
    std::filesystem::create_symlink("locked/a.hok", to_locked);
    std::filesystem::permissions(locked, perms::owner_read | perms::owner_exec);
    if (geteuid() != 0)
    {
        XSFLIB_CHECK(SaveFile(to_locked, kNew) == SaveResult::kFailed);
        XSFLIB_CHECK(BytesOf(file) == kOld);
        XSFLIB_CHECK(std::filesystem::is_symlink(std::filesystem::symlink_status(to_locked)));
        XSFLIB_CHECK(!std::filesystem::exists(locked / "a.hok.saving"));
    }
    std::filesystem::permissions(locked, perms::owner_all);
}

#if defined(__APPLE__)

// A file that Finder locked (the flag uchg) stays as it is. The test clears the flag afterwards, and the folder then
// goes.
void TestLockedFile()
{
    const TemporaryFolder folder;
    const std::filesystem::path file = folder.Path() / "a.hok";
    XSFLIB_REQUIRE(!xsflib::WriteFile(file, kOld).has_value());
    XSFLIB_REQUIRE(chflags(file.c_str(), UF_IMMUTABLE) == 0);
    XSFLIB_CHECK(SaveFile(file, kNew) == SaveResult::kReadOnly);
    XSFLIB_CHECK(BytesOf(file) == kOld);
    XSFLIB_CHECK(!std::filesystem::exists(folder.Path() / "a.hok.saving"));
    XSFLIB_CHECK(chflags(file.c_str(), 0) == 0);
}

#endif

// A file that the user may not write to stays as it is, in a folder that the user may write to. Its permissions stay
// too. The superuser may write to any file, and the refusal isn't checked then.
void TestReadOnly()
{
    using std::filesystem::perms;
    const TemporaryFolder folder;
    const std::filesystem::path file = folder.Path() / "a.hok";
    XSFLIB_REQUIRE(!xsflib::WriteFile(file, kOld).has_value());
    const perms read_only = perms::owner_read | perms::group_read | perms::others_read;
    std::filesystem::permissions(file, read_only);
    if (geteuid() != 0)
    {
        XSFLIB_CHECK(SaveFile(file, kNew) == SaveResult::kReadOnly);
        XSFLIB_CHECK(BytesOf(file) == kOld);
        XSFLIB_CHECK(std::filesystem::status(file).permissions() == read_only);
        XSFLIB_CHECK(!std::filesystem::exists(folder.Path() / "a.hok.saving"));

        // Through a link too.
        const std::filesystem::path link = folder.Path() / "link.hok";
        std::filesystem::create_symlink("a.hok", link);
        XSFLIB_CHECK(SaveFile(link, kNew) == SaveResult::kReadOnly);
        XSFLIB_CHECK(BytesOf(file) == kOld);
    }
    std::filesystem::permissions(file, perms::owner_all);
}

// The new file gets the old file's permissions, whatever the process's mask gives a new file.
void TestPermissions()
{
    using std::filesystem::perms;
    const TemporaryFolder folder;
    const std::filesystem::path file = folder.Path() / "a.hok";
    for (const perms kept :
         {perms::owner_read | perms::owner_write, perms::owner_read | perms::owner_write | perms::group_read,
          perms::owner_read | perms::owner_write | perms::group_read | perms::group_write | perms::others_read})
    {
        XSFLIB_REQUIRE(!xsflib::WriteFile(file, kOld).has_value());
        std::filesystem::permissions(file, kept);
        XSFLIB_CHECK(SaveFile(file, kNew) == SaveResult::kSaved);
        XSFLIB_CHECK(BytesOf(file) == kNew);
        XSFLIB_CHECK(std::filesystem::status(file).permissions() == kept);
    }
}

#endif

} // namespace
} // namespace hokuto

int main()
{
    return xsflib::testing::RunTest("file_saves",
                                    []
                                    {
                                        hokuto::TestSave();
#if !defined(_WIN32)
                                        hokuto::TestLinks();
                                        hokuto::TestReadOnly();
                                        hokuto::TestPermissions();
                                        hokuto::TestChainLength();
                                        hokuto::TestLinksThatFail();
#endif
#if defined(__APPLE__)
                                        hokuto::TestLockedFile();
#endif
                                    });
}
