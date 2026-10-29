// SPDX-License-Identifier: MIT

// The places where a song's tracks read variables or branch, for the arrangement's marker row. A branch is a command
// behind the sequence's if prefix: it runs only if the last comparison held. A read takes a variable's value. A
// comparison reads its variable, and so does an operation that works out the variable's new value from its value. An
// argument that comes from a variable reads that variable. The commands are those that the List Editor lists for each
// track (list_edits.h), up to the end of the loop's first pass. The commands that run at a place make a set. A set
// recurs wherever the tracks run the same commands again. The marker row's flags mark the sets' first places and the
// changes of the variables, laid out together (FlagWidths). The tests run without a window.

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/commands/sequence.h"

#include "app/song_view.h"
#include "format/file.h"
#include "song/sequence.h"

namespace hokuto::app
{

// The variables that `command` reads, in order. A set, a random number and a not give the variable a new value without
// its old one. nn::atk's commands work that way, and the analysis of the variables in song/sequence.cpp follows them
// too.
std::vector<int> VariablesRead(const ntdlib::commands::Command& command);

// The sets of commands that the tracks of `song` run where they read variables or branch, in the order of their first
// ticks. `song` is sequence `sequence` of `file` as ntdlib's performer played it. A command whose tick isn't known
// stays out (edit::CommandRow). So does each track whose commands the List Editor can't show.
std::vector<LogicMarkerView> LogicMarkers(const format::File& file, uint32_t sequence, const song::Sequence& song);

// The text of a place's label: the variables that its commands read, such as "Var 3" or "Var 3, 14". Empty for a place
// that reads none. The marker row puts "if" before it at a branch.
std::string VariablesLabel(std::span<const int> variables);

// A flag of the marker row: the column of pixels at its place, and the width that its label takes with its margins.
struct FlagSpan
{
    float x = 0.0f;
    float wanted = 0.0f;
};

// The widths of the marker row's flags `flags`: the flags of the sets of commands and of the changes alike. Each flag
// takes its `wanted` width, but stops `gap` short of the next flag to its right. No flag then covers the label of
// another. A flag keeps `least` at least: the line at its place. Of the flags at one place, the last in `flags` keeps
// its width, and the others keep `least`.
std::vector<float> FlagWidths(std::span<const FlagSpan> flags, float gap, float least);

} // namespace hokuto::app
