// SPDX-License-Identifier: MIT

// The checks that a file saves and opens again as it was, with --model in both forms, and the messages that say what
// differs (tools/checked_archive.h).

#pragma once

#include "checked_archive.h"

namespace hokuto::tools
{

// The first index at which the elements of ranges `a` and `b` differ, or the shorter range's size when one starts the
// other.
template <class Range, class Other>
std::size_t FirstDifferingIndex(const Range& a, const Other& b)
{
    std::size_t index = 0;
    while (index < a.size() && index < b.size() && a[index] == b[index])
    {
        ++index;
    }

    return index;
}

// Where two ranges `a` and `b` first differ, for a check's message: their sizes when those differ, or else the first
// index at which their elements differ.
template <class Range>
std::string FirstDifference(const Range& a, const Range& b)
{
    if (a.size() != b.size())
    {
        return "sizes " + std::to_string(a.size()) + " and " + std::to_string(b.size());
    }

    return "first at " + std::to_string(FirstDifferingIndex(a, b));
}

// The parts in which files `a` and `b` differ, for a check's message: each part of the file and of its native archive
// that differs, with where a list in it first differs.
std::string PartsThatDiffer(const format::File& a, const format::File& b);

// The saves that CheckSaved checked, the saves of samples that it saw, and those that it passed over.
inline std::size_t checked_saves = 0;
inline std::size_t sampled_saves = 0;
inline std::size_t passed_saves = 0;

// The saves of a sample that CheckSaved checks: the first kAllSampledSaves of the run, then every kSampleStep-th.
inline constexpr std::size_t kAllSampledSaves = 64;
inline constexpr std::size_t kSampleStep = 64;

// With --model, saves `file`, a file as the program saves it, in both forms, and opens each form again. Each form has
// to open as `file`, and a save of the opened file has to give the same bytes again. Returns whether it checked the
// save. In a `sample` of many saves, it checks the first kAllSampledSaves of the run, then every kSampleStep-th. Throws
// std::runtime_error with what differs, and the exceptions of the saves and the opens.
bool CheckSaved(const format::File& file, bool sample = false);

// With --model, prints the line of `name` that counts CheckSaved's saves.
void PrintSaves(const std::string& name);

// Prints PrintSaves's line of `name` when it goes out of scope: after the lines that a check prints on its way out,
// whichever way it returns.
class SavesLine
{
public:
    // The line of the check `name`.
    explicit SavesLine(std::string name) : name_(std::move(name))
    {
    }

    SavesLine(const SavesLine&) = delete;
    SavesLine& operator=(const SavesLine&) = delete;

    // Prints the line.
    ~SavesLine()
    {
        PrintSaves(name_);
    }

private:
    std::string name_;
};

// Whether `file` saves and opens again as it was, in both forms. In the native form, each form has to open as `file`.
// With --model, CheckSaved checks the save instead. When `sample` is true, the save counts as one of a sample.
// CheckSaved's exceptions pass through. A save that the sample passes over counts as one that opens again.
bool SavesAndOpens(const format::File& file, bool sample = false);

// Whether `file` saves in the native form and opens again as it was, in both forms. --model doesn't change the check.
bool RoundTrips(const format::File& file);

} // namespace hokuto::tools
