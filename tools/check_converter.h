// SPDX-License-Identifier: MIT

// The parts of hokuto_check_converter that its files share: the totals, the helpers and the check of the arrangement's
// edits. check_converter.cpp describes the checks.

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "checked_archive.h"
#include "format/file.h"
#include "song/sequence.h"

namespace hokuto::tools::converter
{

// What the check counted, for the summary.
struct Totals
{
    int archives = 0;
    int problems = 0;
    int sequences = 0;
    int songs = 0;
    int read = 0;
    int with_calls = 0;
    int whole = 0;
    int opened = 0;
    int excepted = 0;
    int kept_inline = 0;
    int called = 0;
    int files = 0;
    int timelines = 0;
    int played = 0;
    int edited = 0;
    int edits = 0;
    int loops_apart = 0;
    int silenced = 0;
    int fewer = 0;
    int more = 0;
    std::map<std::string, int> refused;
    std::set<uint64_t> unique_songs;
    std::set<uint64_t> unique_read;
    std::map<std::string, std::set<uint64_t>> reasons;
};

// `text` with a # for each run of digits. Reasons of the same kind then count together.
std::string WithoutNumbers(const std::string& text);

// Prints problem `what` at `where`, and counts it.
void Problem(Totals& totals, const std::string& where, const std::string& what);

// The notes of `song` without the places of their commands. The layouts move those places.
std::vector<hokuto::song::Note> NotesOf(const hokuto::song::Sequence& song);

// The sequence of `file` that sound `sound` of `archive` plays, or nothing for a sound of another kind. `sequences`
// gives each file's sequence.
std::optional<uint32_t> SequenceOfSound(const CheckedArchive& archive, uint32_t sound,
                                        const std::vector<std::optional<uint32_t>>& sequences);

// Settles each of the arrangement's edits of the first sound of each timeline of `opened`, and checks the saved archive
// and file (see the top of the file). `unfolded` is the archive that the program plays, and `game` the game's.
void CheckEdits(const std::string& name, const hokuto::format::File& opened, const CheckedArchive& game,
                const CheckedArchive& unfolded, const std::vector<std::optional<uint32_t>>& sequences, Totals& totals);

} // namespace hokuto::tools::converter
