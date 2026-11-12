// SPDX-License-Identifier: MIT

// The problems that hokuto_check_ds_layout and hokuto_check_dse_layout count, and their check of a file's forms.

#pragma once

#include <cstdio>
#include <string>

#include "format/file.h"
#include "format/text.h"

namespace hokuto::tools::layout
{

// The problems that the check found.
inline int problems = 0;

// Prints `message` about `what`, and counts a problem.
inline void Problem(const std::string& what, const std::string& message)
{
    std::printf("%s: %s\n", what.c_str(), message.c_str());
    problems++;
}

// The forms of `file`: the binary form, and the text form with `text`.
inline void CheckForms(const std::string& what, const format::File& file, bool text)
{
    if (format::Read(format::Write(file)) != file)
    {
        Problem(what, "the binary form doesn't give the file back");
    }
    if (text && format::ReadText(format::WriteText(file)) != file)
    {
        Problem(what, "the text form doesn't give the file back");
    }
}

} // namespace hokuto::tools::layout
