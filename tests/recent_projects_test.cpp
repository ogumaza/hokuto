// SPDX-License-Identifier: MIT

// Tests the list of File > Recent Projects (src/app/recent_projects.h), without a window.

#include "app/recent_projects.h"

#include <cstddef>
#include <filesystem>
#include <random>
#include <string>
#include <system_error>
#include <vector>

#include "xsflib/files.h"
#include "xsflib/testing/check.h"

using hokuto::app::kMostRecentProjects;
using hokuto::app::RecentProjectTitles;
using hokuto::app::SameProject;
using hokuto::app::WithoutRecentProject;
using hokuto::app::WithRecentProject;

namespace
{

// A new path goes first, and a path that the list holds moves there. The list keeps the newest kMostRecentProjects
// paths. An empty path and a path with a line break leave the list as it is.
void TestWithRecentProject()
{
    std::vector<std::string> list;
    list = WithRecentProject(list, "/a/one.hok");
    list = WithRecentProject(list, "/a/two.hok");
    SEQLIB_CHECK((list == std::vector<std::string>{"/a/two.hok", "/a/one.hok"}));

    list = WithRecentProject(list, "/a/one.hok");
    SEQLIB_CHECK((list == std::vector<std::string>{"/a/one.hok", "/a/two.hok"}));

    for (const char* path : {"", "/a/new\nline.hok", "/a/return\r.hok"})
    {
        SEQLIB_CHECK(WithRecentProject(list, path) == list);
    }

    for (std::size_t i = 0; i < kMostRecentProjects + 5; i++)
    {
        list = WithRecentProject(list, "/b/" + std::to_string(i) + ".hok");
    }
    SEQLIB_CHECK(list.size() == kMostRecentProjects);
    SEQLIB_CHECK(list.front() == "/b/" + std::to_string(kMostRecentProjects + 4) + ".hok");
    SEQLIB_CHECK(list.back() == "/b/5.hok");
}

// A path leaves the list. A path that the list lacks changes nothing.
void TestWithoutRecentProject()
{
    const std::vector<std::string> kList = {"/a/one.hok", "/a/two.hok"};

    SEQLIB_CHECK((WithoutRecentProject(kList, "/a/one.hok") == std::vector<std::string>{"/a/two.hok"}));
    SEQLIB_CHECK(WithoutRecentProject(kList, "/a/three.hok") == kList);
}

// Two spellings of one file's path take one place in the list: with "." and ".." steps, and through a symbolic link to
// its folder. The newest spelling stays. A removal by either spelling removes the file. A system that refuses the link,
// such as Windows without the right to make one, checks the steps only.
void TestSameFile()
{
    namespace fs = std::filesystem;
    const fs::path folder =
        fs::temp_directory_path() / ("hokuto_recent_projects_" + std::to_string(std::random_device()()));
    fs::create_directories(folder / "sub");
    const std::string plain = xsflib::Utf8(folder / "a.hok");
    const std::string stepped = xsflib::Utf8(folder / "sub" / ".." / "." / "a.hok");
    const std::string other = xsflib::Utf8(folder / "sub" / "a.hok");

    SEQLIB_CHECK(SameProject(plain, stepped) && !SameProject(plain, other));
    std::vector<std::string> list = WithRecentProject({}, plain);
    list = WithRecentProject(list, other);
    list = WithRecentProject(list, stepped);
    SEQLIB_CHECK((list == std::vector<std::string>{stepped, other}));
    SEQLIB_CHECK((WithoutRecentProject(list, plain) == std::vector<std::string>{other}));

    std::error_code refused;
    fs::path link = folder;
    link += "_link";
    fs::create_directory_symlink(folder, link, refused);
    if (!refused)
    {
        const std::string linked = xsflib::Utf8(link / "a.hok");
        SEQLIB_CHECK(SameProject(plain, linked));
        SEQLIB_CHECK((WithRecentProject(list, linked) == std::vector<std::string>{linked, other}));
        fs::remove(link);
    }

    // Once the folder is gone, the two spellings still name one file.
    fs::remove_all(folder);
    SEQLIB_CHECK(SameProject(plain, stepped));
}

// Each title is the file's name. Two files of the same name take the names of their folders too. Japanese names stay as
// they are.
void TestTitles()
{
    const std::vector<std::string> kList = {"/songs/a/theme.hok", "/songs/曲/北斗.hok", "/songs/b/theme.hok",
                                            "/songs/c/other.hok"};

    SEQLIB_CHECK((RecentProjectTitles(kList) ==
                  std::vector<std::string>{"theme.hok (a)", "北斗.hok", "theme.hok (b)", "other.hok"}));
    SEQLIB_CHECK(RecentProjectTitles({}).empty());
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("recent_projects",
                                    []
                                    {
                                        TestWithRecentProject();
                                        TestWithoutRecentProject();
                                        TestSameFile();
                                        TestTitles();
                                    });
}
