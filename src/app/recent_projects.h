// SPDX-License-Identifier: MIT

// The list of File > Recent Projects: the Hokuto files opened or saved last, newest first, as UTF-8 paths. The
// program's settings keep the list between runs. The list's code uses no window, and its tests run without one.

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace hokuto::app
{

// The most files that the list holds.
inline constexpr std::size_t kMostRecentProjects = 10;

// Whether paths `a` and `b` name the same file. Each path is compared with its symbolic links resolved and its "." and
// ".." steps gone (std::filesystem::weakly_canonical). The links resolve in the part of the path that exists. On macOS,
// a save panel can give /tmp/a.hok and a drop /private/tmp/a.hok for one file.
bool SameProject(const std::string& a, const std::string& b);

// `list` with `path` first. An earlier place of the same file leaves the list (SameProject), and the list keeps the
// first kMostRecentProjects paths. The settings file contains a path a line. An empty path, or a path with a line
// break, leaves `list` as it is.
std::vector<std::string> WithRecentProject(std::vector<std::string> list, const std::string& path);

// `list` without the file of `path` (SameProject).
std::vector<std::string> WithoutRecentProject(std::vector<std::string> list, const std::string& path);

// The titles of the list's items in the menu: each file's name. Where two files of the list share a name, each of their
// titles adds the name of the file's folder in parentheses.
std::vector<std::string> RecentProjectTitles(const std::vector<std::string>& list);

} // namespace hokuto::app
