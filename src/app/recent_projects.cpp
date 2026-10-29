// SPDX-License-Identifier: MIT

#include "app/recent_projects.h"

#include <algorithm>
#include <filesystem>

#include "xsflib/files.h"

namespace hokuto::app
{

std::vector<std::string> WithRecentProject(std::vector<std::string> list, const std::string& path)
{
    if (path.empty() || path.find_first_of("\r\n") != std::string::npos)
    {
        return list;
    }

    std::erase(list, path);
    list.insert(list.begin(), path);
    if (list.size() > kMostRecentProjects)
    {
        list.resize(kMostRecentProjects);
    }

    return list;
}

std::vector<std::string> WithoutRecentProject(std::vector<std::string> list, const std::string& path)
{
    std::erase(list, path);

    return list;
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
