// SPDX-License-Identifier: MIT

#include "format/project.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "agblib/mp2k/layout.h"
#include "ntdlib/sar/layout.h"
#include "xsflib/binary.h"

#include "format/archive.h"
#include "format/codes.h"
#include "format/drivers.h"
#include "format/engines.h"
#include "format/kinds.h"
#include "format/text.h"

namespace hokuto::format
{
namespace
{

// A span of an archive's bytes: a run or a sequence piece of its native layout, or a field of its tables.
struct Span
{
    uint64_t start = 0;
    uint64_t end = 0;
    std::size_t piece = 0;
    bool run = false;
};

// The bytes that an edit or a join changed, from the ROM's start.
struct ByteChange
{
    uint64_t start = 0;
    std::vector<uint8_t> bytes;
};

// The driver of `file`'s archive in a sentence (format::DriverTitle).
std::string TitleOf(const File& file)
{
    return std::string(DriverTitle(file.engine));
}

// The runs and the sequence pieces of `file`'s native layout, in their order. A sequence piece reaches to the start of
// the next piece: the room of a list of songs belongs to its songs.
std::vector<Span> PieceSpans(const File& file)
{
    std::vector<Span> spans;
    if (!file.native)
    {
        return spans;
    }

    const std::vector<uint64_t> starts = NativePieceStarts(file);
    const std::vector<ntdlib::sar::Piece>& pieces = *file.native->pieces;
    for (std::size_t p = 0; p < pieces.size() && p + 1 < starts.size(); ++p)
    {
        if (const auto* run = std::get_if<std::vector<uint8_t>>(&pieces[p]))
        {
            spans.push_back({starts[p], starts[p] + run->size(), p, true});
        }
        else if (std::holds_alternative<ntdlib::sar::SequencePiece>(pieces[p]))
        {
            spans.push_back({starts[p], starts[p + 1], p, false});
        }
    }

    return spans;
}

// Where `file`'s pieces end: the size of the ROM that they give before songs move.
uint64_t PiecesEnd(const File& file)
{
    const std::vector<uint64_t> starts = NativePieceStarts(file);
    return starts.empty() ? 0 : starts.back();
}

// The sequence pieces of `file`, and the fields of its tables that lie in its runs, in the order of their starts.
std::vector<Span> SongsAndFields(const File& file)
{
    std::vector<Span> spans;
    for (const Span& span : PieceSpans(file))
    {
        if (!span.run)
        {
            spans.push_back(span);
        }
    }

    const std::vector<uint64_t> starts = NativePieceStarts(file);
    for (const ntdlib::sar::Field& field : file.native->fields)
    {
        if (!field.at.item && !field.at.sequence && field.at.piece < starts.size())
        {
            const uint64_t at = starts[field.at.piece] + field.at.offset;
            spans.push_back({at, at + field.bytes, field.at.piece, false});
        }
    }

    std::ranges::sort(spans, {}, &Span::start);
    return spans;
}

// Whether a span of `a` and a span of `b` share a byte. Both lists are in the order of their starts.
bool Overlap(std::span<const Span> a, std::span<const Span> b)
{
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < a.size() && j < b.size())
    {
        if (a[i].start < b[j].end && b[j].start < a[i].end)
        {
            return true;
        }

        if (a[i].end <= b[j].end)
        {
            ++i;
        }
        else
        {
            ++j;
        }
    }

    return false;
}

// `native` with its free space past `end`, on the next multiple of kFreeSpaceStep, where bytes reach past its start.
// MusicPlayer2000's floor follows the other archives (WithFreeFloors).
void MoveFreeSpace(NativeArchive& native, uint64_t end)
{
    if (native.driver && end > native.driver->free_start)
    {
        native.driver->free_start = static_cast<uint32_t>((end + kFreeSpaceStep - 1) / kFreeSpaceStep * kFreeSpaceStep);
    }
}

// The bytes of the runs of `after` that differ from those of `before`, an earlier version of the same archive. A run
// that grew changed its new bytes, and a run without a run at its start in `before` changed whole.
std::vector<ByteChange> ChangedRunBytes(const File& before, const File& after)
{
    std::vector<ByteChange> changes;
    if (!before.native || !after.native || before.native->pieces.SharesWith(after.native->pieces))
    {
        return changes;
    }

    std::map<uint64_t, const std::vector<uint8_t>*> old_runs;
    const std::vector<ntdlib::sar::Piece>& old_pieces = *before.native->pieces;
    for (const Span& span : PieceSpans(before))
    {
        if (span.run)
        {
            old_runs[span.start] = &std::get<std::vector<uint8_t>>(old_pieces[span.piece]);
        }
    }

    const std::vector<ntdlib::sar::Piece>& pieces = *after.native->pieces;
    for (const Span& span : PieceSpans(after))
    {
        if (!span.run)
        {
            continue;
        }

        const std::vector<uint8_t>& run = std::get<std::vector<uint8_t>>(pieces[span.piece]);
        const auto old = old_runs.find(span.start);
        const std::size_t common = old == old_runs.end() ? 0 : std::min(run.size(), old->second->size());
        for (std::size_t i = 0; i < common;)
        {
            if (run[i] == (*old->second)[i])
            {
                ++i;
                continue;
            }

            const std::size_t first = i;
            while (i < common && run[i] != (*old->second)[i])
            {
                ++i;
            }
            changes.push_back(
                {span.start + first, std::vector<uint8_t>(run.begin() + static_cast<std::ptrdiff_t>(first),
                                                          run.begin() + static_cast<std::ptrdiff_t>(i))});
        }

        if (run.size() > common)
        {
            changes.push_back({span.start + common,
                               std::vector<uint8_t>(run.begin() + static_cast<std::ptrdiff_t>(common), run.end())});
        }
    }

    return changes;
}

// `other` with `changes` in its runs. Bytes past the end of its pieces go at the end of its last run, after 0xFF bytes
// up to them. Throws std::invalid_argument where a change reaches one of its sequence pieces.
File WithChanges(File other, std::span<const ByteChange> changes, const std::string& what)
{
    if (changes.empty() || !other.native)
    {
        return other;
    }

    const std::vector<Span> spans = PieceSpans(other);
    uint64_t end = PiecesEnd(other);
    uint64_t reached = 0;
    std::vector<ntdlib::sar::Piece> pieces = *other.native->pieces;
    for (const ByteChange& change : changes)
    {
        const uint64_t change_end = change.start + change.bytes.size();
        reached = std::max(reached, change_end);

        // The bytes inside the pieces.
        auto span = std::ranges::upper_bound(spans, change.start, {}, &Span::start);
        if (span != spans.begin())
        {
            --span;
        }
        for (; span != spans.end() && span->start < std::min(change_end, end); ++span)
        {
            if (span->end <= change.start)
            {
                continue;
            }
            if (!span->run)
            {
                throw std::invalid_argument(what + " changes bytes of a song of " + TitleOf(other) + ".");
            }

            auto& run = std::get<std::vector<uint8_t>>(pieces[span->piece]);
            const uint64_t from = std::max(change.start, span->start);
            const uint64_t to = std::min(change_end, span->end);
            std::copy(change.bytes.begin() + static_cast<std::ptrdiff_t>(from - change.start),
                      change.bytes.begin() + static_cast<std::ptrdiff_t>(to - change.start),
                      run.begin() + static_cast<std::ptrdiff_t>(from - span->start));
        }

        // The bytes past the pieces' end.
        if (change_end > end)
        {
            auto* last = pieces.empty() ? nullptr : std::get_if<std::vector<uint8_t>>(&pieces.back());
            if (!last)
            {
                throw std::invalid_argument(what + " adds bytes after a song of " + TitleOf(other) + ".");
            }

            const uint64_t last_start = end - last->size();
            last->resize(static_cast<std::size_t>(change_end - last_start), 0xFF);
            const uint64_t from = std::max(change.start, end);
            std::copy(change.bytes.begin() + static_cast<std::ptrdiff_t>(from - change.start), change.bytes.end(),
                      last->begin() + static_cast<std::ptrdiff_t>(from - last_start));
            end = change_end;
        }
    }

    const auto take = [&pieces, reached](NativeArchive& native)
    {
        native.pieces = std::move(pieces);
        MoveFreeSpace(native, reached);
    };
    other.native.Edit(take);

    return other;
}

// `later` with the bytes of `image`, the join of `joined`, in its runs. The image's growth goes at the end of its last
// run. Throws std::invalid_argument where the join changed a byte of a sequence piece of `later`.
File WithJoinedBytes(File later, const File& joined, std::span<const uint8_t> image)
{
    const std::vector<Span> spans = PieceSpans(later);
    const uint64_t end = PiecesEnd(later);
    std::vector<ntdlib::sar::Piece> pieces = *later.native->pieces;

    // The join's bytes in the runs, and the last byte that it changed.
    uint64_t reached = 0;
    for (const Span& span : spans)
    {
        if (!span.run)
        {
            continue;
        }

        auto& run = std::get<std::vector<uint8_t>>(pieces[span.piece]);
        const uint64_t to = std::min<uint64_t>(span.end, image.size());
        for (uint64_t at = span.start; at < to; ++at)
        {
            uint8_t& byte = run[static_cast<std::size_t>(at - span.start)];
            if (byte != image[static_cast<std::size_t>(at)])
            {
                byte = image[static_cast<std::size_t>(at)];
                reached = at + 1;
            }
        }
    }

    // The join left the songs of `later` as they were: its runs there give the image's bytes.
    const std::vector<ntdlib::sar::Piece>& joined_pieces = *joined.native->pieces;
    const std::vector<Span> joined_spans = PieceSpans(joined);
    for (const Span& song : spans)
    {
        if (song.run)
        {
            continue;
        }

        for (const Span& span : joined_spans)
        {
            if (span.end <= song.start || span.start >= song.end)
            {
                continue;
            }
            if (!span.run)
            {
                throw std::invalid_argument("The songs of " + TitleOf(joined) + " and " + TitleOf(later) +
                                            " share bytes.");
            }

            const auto& run = std::get<std::vector<uint8_t>>(joined_pieces[span.piece]);
            const uint64_t from = std::max(song.start, span.start);
            const uint64_t to = std::min({song.end, span.end, static_cast<uint64_t>(image.size())});
            if (!std::equal(image.begin() + static_cast<std::ptrdiff_t>(from),
                            image.begin() + static_cast<std::ptrdiff_t>(to),
                            run.begin() + static_cast<std::ptrdiff_t>(from - span.start)))
            {
                throw std::invalid_argument("The export of " + TitleOf(joined) + " changes bytes of a song of " +
                                            TitleOf(later) + ".");
            }
        }
    }

    // The image's growth.
    if (image.size() > end)
    {
        auto* last = pieces.empty() ? nullptr : std::get_if<std::vector<uint8_t>>(&pieces.back());
        if (!last)
        {
            throw std::invalid_argument("The export of " + TitleOf(joined) + " adds bytes after a song of " +
                                        TitleOf(later) + ".");
        }

        last->insert(last->end(), image.begin() + static_cast<std::ptrdiff_t>(end), image.end());
        reached = image.size();
    }

    const auto take = [&pieces, reached](NativeArchive& native)
    {
        native.pieces = std::move(pieces);
        MoveFreeSpace(native, reached);
    };
    later.native.Edit(take);

    return later;
}

// Whether the runs of `a` and `b` hold the same bytes wherever both have a run.
bool RunsAgree(const File& a, const File& b)
{
    std::vector<Span> a_runs;
    std::vector<Span> b_runs;
    std::ranges::copy_if(PieceSpans(a), std::back_inserter(a_runs), &Span::run);
    std::ranges::copy_if(PieceSpans(b), std::back_inserter(b_runs), &Span::run);
    const std::vector<ntdlib::sar::Piece>& a_pieces = *a.native->pieces;
    const std::vector<ntdlib::sar::Piece>& b_pieces = *b.native->pieces;

    std::size_t i = 0;
    std::size_t j = 0;
    while (i < a_runs.size() && j < b_runs.size())
    {
        const Span& x = a_runs[i];
        const Span& y = b_runs[j];
        const uint64_t from = std::max(x.start, y.start);
        const uint64_t to = std::min(x.end, y.end);
        if (from < to)
        {
            const auto& x_bytes = std::get<std::vector<uint8_t>>(a_pieces[x.piece]);
            const auto& y_bytes = std::get<std::vector<uint8_t>>(b_pieces[y.piece]);
            if (!std::equal(x_bytes.begin() + static_cast<std::ptrdiff_t>(from - x.start),
                            x_bytes.begin() + static_cast<std::ptrdiff_t>(to - x.start),
                            y_bytes.begin() + static_cast<std::ptrdiff_t>(from - y.start)))
            {
                return false;
            }
        }

        if (x.end <= y.end)
        {
            ++i;
        }
        else
        {
            ++j;
        }
    }

    return true;
}

// `file` with the values of the project from `values`: the mixing mode, the effects and the volumes of the buses, the
// file's tags and the choice of the filler runs.
File WithProjectValues(File file, const File& values)
{
    file.mixing = values.mixing;
    file.aux_effects = values.aux_effects;
    file.aux_volumes = values.aux_volumes;
    file.tags = values.tags;
    if (file.native && values.native && file.native->gba_filler != values.native->gba_filler)
    {
        file.native.Edit([&values](NativeArchive& native) { native.gba_filler = values.native->gba_filler; });
    }

    return file;
}

} // namespace

Project ProjectOf(File file)
{
    Project project;
    project.archives.push_back(std::move(file));

    return project;
}

ImportedArchives ImportGbaArchives(std::span<const uint8_t> rom)
{
    // Every detection: MusicPlayer2000's, then each other driver's, in the order of their engines.
    std::vector<File> found;
    try
    {
        found.push_back(ImportGbaRom(rom));
    }
    catch (const xsflib::FormatError&)
    {
    }

    for (const Driver* driver : DriversOfRom(rom))
    {
        try
        {
            found.push_back(driver->Import(rom));
        }
        catch (const xsflib::FormatError&)
        {
        }
    }

    // The archives whose cuts give songs.
    std::vector<File> with_songs;
    std::ranges::copy_if(found, std::back_inserter(with_songs),
                         [](const File& file) { return !file.sequences.empty(); });
    if (with_songs.empty())
    {
        const Driver* driver = DriverOfRom(rom);
        return {ProjectOf(driver ? driver->Import(rom) : ImportGbaRom(rom)), {}};
    }

    // Each archive whose songs and fields lie clear of those of the archives before it.
    ImportedArchives imported;
    std::vector<std::vector<Span>> taken;
    for (File& file : with_songs)
    {
        const std::vector<Span> spans = SongsAndFields(file);
        const auto clashes = [&spans](const std::vector<Span>& other)
        {
            return Overlap(spans, other);
        };
        const auto clash = std::ranges::find_if(taken, clashes);
        if (clash != taken.end())
        {
            const File& earlier = imported.project.archives[static_cast<std::size_t>(clash - taken.begin())];
            imported.notes.push_back("Hokuto leaves out the songs of " + TitleOf(file) +
                                     ". They share bytes with " + "those of " + TitleOf(earlier) + ".");
            continue;
        }

        taken.push_back(spans);
        imported.project.archives.push_back(std::move(file));
    }

    if (imported.project.archives.size() < 2)
    {
        return imported;
    }

    // One free space, from the latest of the starts that the archives give.
    uint64_t latest = agblib::mp2k::FreeSpaceStart(rom);
    for (const File& file : imported.project.archives)
    {
        if (file.native->driver)
        {
            latest = std::max<uint64_t>(latest, file.native->driver->free_start);
        }
    }

    for (File& file : imported.project.archives)
    {
        if (file.native->driver && file.native->driver->free_start != latest)
        {
            file.native.Edit([latest](NativeArchive& native)
                             { native.driver->free_start = static_cast<uint32_t>(latest); });
        }
    }

    imported.project = WithFreeFloors(std::move(imported.project));
    return imported;
}

Project WithArchive(Project project, std::size_t index, File edited)
{
    if (index >= project.archives.size())
    {
        throw std::invalid_argument("The project has no archive " + std::to_string(index) + ".");
    }

    if (project.archives.size() > 1)
    {
        const std::vector<ByteChange> changes = ChangedRunBytes(project.archives[index], edited);
        const std::string what = "The edit of " + TitleOf(edited);
        for (std::size_t other = 0; other < project.archives.size(); ++other)
        {
            if (other != index)
            {
                project.archives[other] =
                    WithProjectValues(WithChanges(std::move(project.archives[other]), changes, what), edited);
            }
        }
    }

    project.archives[index] = std::move(edited);
    return WithFreeFloors(std::move(project));
}

std::vector<uint8_t> ExportProject(const Project& project)
{
    if (project.archives.size() == 1)
    {
        return ExportArchive(project.archives.front());
    }
    if (const std::optional<std::string> problem = ProjectProblem(project))
    {
        throw std::invalid_argument("The project " + *problem + ".");
    }

    // Each archive's join takes the joins before it in its runs.
    std::vector<File> archives = project.archives;
    std::vector<uint8_t> image;
    for (std::size_t k = 0; k < archives.size(); ++k)
    {
        image = ExportArchive(archives[k]);
        for (std::size_t later = k + 1; later < archives.size(); ++later)
        {
            archives[later] = WithJoinedBytes(std::move(archives[later]), archives[k], image);
        }

        if (k + 1 < archives.size())
        {
            Project moved;
            moved.archives = std::move(archives);
            archives = WithFreeFloors(std::move(moved)).archives;
        }
    }

    return image;
}

File WithoutProjectValues(File archive)
{
    archive.aux_effects = {};
    archive.aux_volumes = {1.0f, 1.0f, 1.0f};
    archive.tags.clear();

    return archive;
}

std::optional<std::string> ProjectProblem(const Project& project)
{
    if (project.archives.empty())
    {
        return "has no archive";
    }
    if (project.front >= project.archives.size())
    {
        return "has no archive " + std::to_string(project.front) + " for the archive in front";
    }
    if (project.archives.size() == 1)
    {
        return project.other_chunks.empty() ? std::nullopt
                                            : std::optional<std::string>("keeps chunks outside its one archive");
    }

    const File& first = project.archives.front();
    for (std::size_t a = 0; a < project.archives.size(); ++a)
    {
        const File& file = project.archives[a];
        const std::string archive = "archive " + std::to_string(a);
        if (!file.native)
        {
            return archive + " has no native archive";
        }
        if (file.form != ArchiveForm::kNative)
        {
            return archive + " is in the model form";
        }
        if (FactsOf(file.engine).platform != Platform::kGba)
        {
            // Only a GBA ROM has a form with more than one archive: each archive cuts the whole ROM.
            return archive + " isn't an archive of the GBA";
        }
        if (a > 0 && CodeOf(file.engine) < CodeOf(project.archives[a - 1].engine))
        {
            return archive + " comes before an archive of a lower engine";
        }
        if (file.mixing != first.mixing)
        {
            return archive + " has another mixing mode than the file";
        }
        if (file.native->gba_filler)
        {
            return archive + " takes the filler runs";
        }
        if (file.aux_effects != first.aux_effects || file.aux_volumes != first.aux_volumes || file.tags != first.tags)
        {
            return archive + " has other effects, volumes of the buses or tags than the file";
        }
        if (file.native->game_sizes != first.native->game_sizes)
        {
            return archive + " gives another size of the ROM than archive 0";
        }
        for (std::size_t b = 0; b < a; ++b)
        {
            if (!RunsAgree(project.archives[b], file))
            {
                return "archives " + std::to_string(b) + " and " + std::to_string(a) +
                       " differ in a byte where both have a run";
            }
            if (Overlap(SongsAndFields(project.archives[b]), SongsAndFields(file)))
            {
                return "archives " + std::to_string(b) + " and " + std::to_string(a) +
                       " share bytes of their songs or fields";
            }
        }
    }

    return std::nullopt;
}

Project WithFreeFloors(Project project)
{
    if (project.archives.size() < 2)
    {
        return project;
    }

    uint64_t floor = 0;
    for (const File& file : project.archives)
    {
        if (file.native && file.native->driver)
        {
            floor = std::max<uint64_t>(floor, file.native->driver->free_start);
        }
    }

    for (File& file : project.archives)
    {
        if (file.native && file.native->engine == Engine::kGba && file.native->gba_free_floor != floor)
        {
            file.native.Edit([floor](NativeArchive& native) { native.gba_free_floor = floor; });
        }
    }

    return project;
}

Project ReadProject(std::span<const uint8_t> bytes)
{
    if (IsBinary(bytes))
    {
        return ReadBinaryProject(bytes);
    }
    if (IsText(bytes))
    {
        return ReadTextProject(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    }

    throw xsflib::FormatError(NotKindText(bytes, "a Hokuto file"));
}

} // namespace hokuto::format
