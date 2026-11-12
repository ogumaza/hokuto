// SPDX-License-Identifier: MIT

#include "app/recent_projects.h"

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <utility>

#include "xsflib/files.h"

namespace hokuto::app
{
namespace
{

// `path` with its symbolic links resolved and its "." and ".." steps gone. The links resolve in the part of the path
// that exists. Where the file system can't resolve the path, only the steps go.
std::filesystem::path CanonicalOf(const std::string& path)
{
    const std::filesystem::path given = xsflib::PathFromUtf8(path);
    std::error_code error;
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(given, error);

    return error ? given.lexically_normal() : canonical;
}

// `list` without the paths of the file that `path` names.
std::vector<std::string> WithoutFile(std::vector<std::string> list, const std::string& path)
{
    const std::filesystem::path file = CanonicalOf(path);
    std::erase_if(list, [&file](const std::string& listed) { return CanonicalOf(listed) == file; });

    return list;
}

} // namespace

bool SameProject(const std::string& a, const std::string& b)
{
    return a == b || CanonicalOf(a) == CanonicalOf(b);
}

std::vector<std::string> WithRecentProject(std::vector<std::string> list, const std::string& path)
{
    if (path.empty() || path.find_first_of("\r\n") != std::string::npos)
    {
        return list;
    }

    list = WithoutFile(std::move(list), path);
    list.insert(list.begin(), path);
    if (list.size() > kMostRecentProjects)
    {
        list.resize(kMostRecentProjects);
    }

    return list;
}

std::vector<std::string> WithoutRecentProject(std::vector<std::string> list, const std::string& path)
{
    return WithoutFile(std::move(list), path);
}

std::vector<std::string> RecentProjectTitles(const std::vector<std::string>& list)
{
    std::vector<std::string> names;
    names.reserve(list.size());
    for (const std::string& path : list)
    {
        names.push_back(xsflib::Utf8(xsflib::PathFromUtf8(path).filename()));
    }

    std::vector<std::string> titles = names;
    for (std::size_t i = 0; i < list.size(); i++)
    {
        if (std::count(names.begin(), names.end(), names[i]) > 1)
        {
            const std::filesystem::path folder = xsflib::PathFromUtf8(list[i]).parent_path().filename();
            titles[i] += " (" + xsflib::Utf8(folder) + ")";
        }
    }

    return titles;
}

} // namespace hokuto::app
