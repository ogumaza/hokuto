// SPDX-License-Identifier: MIT

// hokuto_check_project imports each GBA ROM that it's given as a project (format::ImportGbaArchives), and checks the
// archives of a ROM with more than one driver:
//
// - The project exports the ROM byte for byte, and both forms of the file give the project back.
// - Each song of each archive grows by data that no command reaches, archive by archive through format::WithArchive,
//   until it outgrows its place. The project then exports a ROM whose import gives the same archives, each with the
//   commands of every song. The songs that move share the one free space, and no driver's songs overwrite another's.
// - The exported ROM differs from the game's only in the places of the songs, in the fields of the tables, and in the
//   free space.
//
// A ROM with one driver only has to export itself. Usage: hokuto_check_project <rom>...

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/sar/layout.h"
#include "xsflib/console.h"
#include "xsflib/files.h"

#include "check_support.h"
#include "format/archive.h"
#include "format/engines.h"
#include "format/project.h"

namespace
{

namespace format = hokuto::format;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;

// The items of `sequence` without the data after its last command.
std::vector<Command> Commands(const ntdlib::commands::Sequence& sequence)
{
    std::vector<Command> items = sequence.items;
    while (!items.empty() && items.back().kind == Kind::kData)
    {
        items.pop_back();
    }

    return items;
}

// `file` with 64 bytes of data after the last item of each song.
format::File Grown(format::File file)
{
    for (auto& sequence : file.sequences)
    {
        ntdlib::commands::Sequence grown = *sequence;
        Command data;
        data.kind = Kind::kData;
        data.bytes.assign(64, 0x11);
        grown.items.push_back(std::move(data));
        sequence = std::move(grown);
    }

    return file;
}

// A range of bytes of a ROM, from `start` up to `end`.
struct Range
{
    uint64_t start = 0;
    uint64_t end = 0;
};

// The bytes of `project` that an edit of its songs may change: the places of the songs of each archive, the fields of
// each archive's tables, and the free space past the latest start that an archive gives.
std::vector<Range> ChangingRanges(const format::Project& project, uint64_t size)
{
    std::vector<Range> ranges;
    uint64_t free_start = 0;
    for (const format::File& file : project.archives)
    {
        const std::vector<uint64_t> starts = format::NativePieceStarts(file);
        const std::vector<ntdlib::sar::Piece>& pieces = *file.native->pieces;
        for (std::size_t p = 0; p < pieces.size() && p + 1 < starts.size(); ++p)
        {
            if (std::holds_alternative<ntdlib::sar::SequencePiece>(pieces[p]))
            {
                ranges.push_back({starts[p], starts[p + 1]});
            }
        }

        for (const ntdlib::sar::Field& field : file.native->fields)
        {
            if (!field.at.item && !field.at.sequence && field.at.piece < starts.size())
            {
                const uint64_t at = starts[field.at.piece] + field.at.offset;
                ranges.push_back({at, at + field.bytes});
            }
        }

        free_start =
            std::max(free_start, file.native->driver ? file.native->driver->free_start : file.native->gba_free_floor);
    }

    ranges.push_back({free_start, size});

    return ranges;
}

// `ranges` in the order of their starts, with the ranges that touch or overlap joined.
std::vector<Range> Joined(std::vector<Range> ranges)
{
    std::ranges::sort(ranges, {}, &Range::start);
    std::vector<Range> joined;
    for (const Range& range : ranges)
    {
        if (!joined.empty() && range.start <= joined.back().end)
        {
            joined.back().end = std::max(joined.back().end, range.end);
        }
        else
        {
            joined.push_back(range);
        }
    }

    return joined;
}

// The changes of a ROM: the ranges that differ, and those of them that lie outside the ranges allowed to change.
struct Changes
{
    std::size_t changed = 0;
    std::vector<Range> unexpected;
};

// The ranges in which `edited` differs from `rom` among the bytes of both. `changed` is set to the number of ranges
// that differ, and `unexpected` to the ranges outside `allowed`.
Changes ChangesOf(std::span<const uint8_t> rom, std::span<const uint8_t> edited, const std::vector<Range>& allowed)
{
    const std::vector<Range> joined = Joined(allowed);
    Changes changes;
    std::vector<Range>& unexpected = changes.unexpected;
    std::size_t next = 0;
    const std::size_t size = std::min(rom.size(), edited.size());
    for (std::size_t at = 0; at < size; ++at)
    {
        if (rom[at] == edited[at])
        {
            continue;
        }
        if (at == 0 || rom[at - 1] == edited[at - 1])
        {
            changes.changed++;
        }

        while (next < joined.size() && joined[next].end <= at)
        {
            ++next;
        }
        if (next < joined.size() && joined[next].start <= at)
        {
            continue;
        }
        if (!unexpected.empty() && unexpected.back().end == at)
        {
            unexpected.back().end = at + 1;
        }
        else
        {
            unexpected.push_back({at, at + 1});
        }
    }

    return changes;
}

// Checks the ROM at `path`. Returns the number of problems.
int CheckRom(const std::string& path)
{
    std::vector<uint8_t> rom;
    if (!xsflib::ReadFile(xsflib::PathFromUtf8(path), rom))
    {
        std::printf("%s: can't read the file\n", path.c_str());
        return 1;
    }

    format::ImportedArchives imported;
    try
    {
        imported = format::ImportGbaArchives(rom);
    }
    catch (const std::exception& error)
    {
        std::printf("%s: can't import the ROM: %s\n", path.c_str(), error.what());
        return 1;
    }

    format::Project project = imported.project;
    int problems = 0;
    const auto problem = [&](const std::string& what)
    {
        std::printf("%s: %s\n", path.c_str(), what.c_str());
        problems++;
    };
    for (const std::string& note : imported.notes)
    {
        std::printf("%s: note: %s\n", path.c_str(), note.c_str());
    }

    // The round trips.
    if (format::ExportProject(project) != rom)
    {
        problem("the project doesn't export the ROM byte for byte");
    }

    const std::vector<uint8_t> binary = format::WriteProject(project);
    const std::string text = format::WriteProjectText(project);
    if (format::ReadProject(binary) != project ||
        format::ReadProject(std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size())) != project)
    {
        problem("a form of the file doesn't give the project back");
    }

    std::printf("%s: %zu %s", path.c_str(), project.archives.size(),
                project.archives.size() == 1 ? "archive" : "archives");
    for (const format::File& archive : project.archives)
    {
        const std::string_view name = format::FactsOf(archive.engine).name;
        std::printf(", %.*s with %zu sequences", static_cast<int>(name.size()), name.data(), archive.sequences.size());
    }
    std::printf("\n");
    if (project.archives.size() < 2)
    {
        return problems;
    }

    // Every song of every archive grows and moves.
    const std::vector<Range> allowed = ChangingRanges(project, rom.size());
    try
    {
        for (std::size_t a = 0; a < project.archives.size(); ++a)
        {
            project = format::WithArchive(project, a, Grown(project.archives[a]));
        }

        const std::vector<uint8_t> exported = format::ExportProject(project);
        const format::Project again = format::ImportGbaArchives(exported).project;
        std::printf("%s: the ROM grew from %zu to %zu bytes\n", path.c_str(), rom.size(), exported.size());
        if (exported.size() < rom.size())
        {
            problem("the exported ROM is shorter than the game's");
        }

        const Changes changes = ChangesOf(rom, exported, allowed);
        std::printf("%s: %zu ranges of bytes changed, %zu outside the songs, the fields and the free space\n",
                    path.c_str(), changes.changed, changes.unexpected.size());
        for (const Range& range : changes.unexpected)
        {
            problem("bytes " + hokuto::tools::HexOf(static_cast<uint32_t>(range.start)) + " to " +
                    hokuto::tools::HexOf(static_cast<uint32_t>(range.end)) +
                    " changed outside the songs, the fields and the free space");
        }
        if (again.archives.size() != project.archives.size())
        {
            problem("the new ROM gives " + std::to_string(again.archives.size()) + " archives");
            return problems;
        }

        for (std::size_t a = 0; a < project.archives.size(); ++a)
        {
            std::vector<std::vector<Command>> expected;
            std::vector<std::vector<Command>> read;
            for (const auto& sequence : project.archives[a].sequences)
            {
                expected.push_back(Commands(*sequence));
            }
            for (const auto& sequence : again.archives[a].sequences)
            {
                read.push_back(Commands(*sequence));
            }

            // A cut takes the songs in the order of their places, and a song that moves comes after the others.
            bool same = expected.size() == read.size();
            for (const std::vector<Command>& song : expected)
            {
                const auto found = std::ranges::find(read, song);
                same = same && found != read.end();
                if (found != read.end())
                {
                    read.erase(found);
                }
            }

            if (!same)
            {
                problem("archive " + std::to_string(a) + " doesn't give its songs back after the edits");
            }
        }
    }
    catch (const std::exception& error)
    {
        problem(std::string("the edits fail: ") + error.what());
    }

    return problems;
}

} // namespace

int main(int argc, char** argv)
{
    const std::vector<std::string> arguments = xsflib::console::Arguments(argc, argv);
    if (arguments.size() < 2)
    {
        std::fprintf(stderr, "Usage: hokuto_check_project <rom>...\n");
        return 2;
    }

    int problems = 0;
    for (std::size_t i = 1; i < arguments.size(); ++i)
    {
        problems += CheckRom(arguments[i]);
    }

    std::printf("%d problems\n", problems);
    return problems == 0 ? 0 : 1;
}
