// SPDX-License-Identifier: MIT

// Checks the import and the export of the sound archives (SDAT) of DS cartridge images, and of sound archives alone.
// For each sound archive of an image (format::DsArchivesIn), it checks:
//
// - The import cuts the archive into a sequence for each sequence file that ntdlib's cut selects (ntdlib::sdat::Cut).
//   The tool counts the archive's sequence files, and names the ones that stay in the archive's bytes.
// - The export reproduces the image byte for byte, with no stale hash.
// - The binary form reproduces the file. With --text, so does the text form.
// - Longer sequences: in each sequence that waits, kCopies copies of the first wait follow it. The exported archive
//   then contains each edited sequence, and every other file of the archive keeps its bytes. The archive file contains
//   the new archive. The image keeps its number of files, and every other file of the image keeps its bytes. An import
//   of the export returns the edited sequences. The digest of a DSi image uses no keys here, and the tool lists the
//   stale hashes.
// - An archive that shares its file with other data keeps its size: the export has to refuse the longer sequences.
//
// A sound archive alone gets the same checks, without the checks of the image. The tool prints a line for each archive,
// and returns 1 when any check fails.
//
//     check_ds_layout [--text] <image.nds | archive.sdat>...

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/ntr/rom.h"
#include "ntdlib/ntr/snd/commands.h"
#include "ntdlib/sar/layout.h"
#include "ntdlib/sdat/formats.h"
#include "xsflib/binary.h"
#include "xsflib/console.h"
#include "xsflib/files.h"

#include "format/archive.h"
#include "format/file.h"
#include "format/text.h"
#include "layout_problems.h"

namespace
{

using hokuto::format::DsArchivePlace;
using hokuto::format::File;
using hokuto::tools::layout::CheckForms;
using hokuto::tools::layout::Problem;
using hokuto::tools::layout::problems;
using ntdlib::commands::Kind;
using ntdlib::commands::Sequence;

// The copies of a wait that lengthen a sequence: 80 bytes or more, past the padding that a file can take up.
constexpr uint32_t kCopies = 40;

// `sequence` with kCopies copies of its first wait after it, or nothing for a sequence without a wait. The targets and
// labels after the wait move by the copies.
std::optional<Sequence> WithWaitCopies(const Sequence& sequence)
{
    const auto wait = std::ranges::find(sequence.items, Kind::kWait, &ntdlib::commands::Command::kind);
    if (wait == sequence.items.end())
    {
        return std::nullopt;
    }

    const auto at = static_cast<uint32_t>(wait - sequence.items.begin()) + 1;
    Sequence longer = sequence;
    longer.items.insert(longer.items.begin() + at, kCopies, *wait);
    for (ntdlib::commands::Command& item : longer.items)
    {
        if (ntdlib::commands::HasTarget(item.kind) && !item.target_outside && item.target >= at)
        {
            item.target += kCopies;
        }
    }
    for (ntdlib::commands::Label& label : longer.labels)
    {
        if (label.item >= at)
        {
            label.item += kCopies;
        }
    }

    return longer;
}

// The files of `sdat` by ID: the sequence files, the ones among them that `file`'s cut leaves in the archive's bytes,
// and the files that aren't sequence files. A sequence piece starts at the bytecode of its file, at the data offset of
// the file (format::NativePieceStarts). A sequence file that ntdlib can't read stays in the archive's bytes.
struct Files
{
    std::vector<uint32_t> sequences;
    std::vector<uint32_t> kept;
    std::vector<uint32_t> others;
};

Files FilesOf(std::span<const uint8_t> sdat, const File& file)
{
    const std::vector<ntdlib::sar::Piece>& pieces = *file.native->pieces;
    const std::vector<uint64_t> starts = hokuto::format::NativePieceStarts(file);
    std::set<uint64_t> cut;
    for (std::size_t p = 0; p < pieces.size(); p++)
    {
        if (std::holds_alternative<ntdlib::sar::SequencePiece>(pieces[p]))
        {
            cut.insert(starts[p]);
        }
    }

    const ntdlib::sdat::Archive archive = ntdlib::sdat::Archive::Parse(sdat);
    Files files;
    for (uint32_t id = 0; id < archive.files.size(); id++)
    {
        const std::span<const uint8_t> bytes = archive.File(id);
        if (bytes.size() < 4 || std::memcmp(bytes.data(), "SSEQ", 4) != 0)
        {
            files.others.push_back(id);
            continue;
        }

        files.sequences.push_back(id);
        try
        {
            const uint32_t data = ntdlib::sdat::Sequence::Parse(bytes).data_offset;
            if (!cut.contains(uint64_t{archive.files[id].offset} + data))
            {
                files.kept.push_back(id);
            }
        }
        catch (const xsflib::FormatError&)
        {
            files.kept.push_back(id);
        }
    }

    return files;
}

// Why the cut leaves sequence file `id` of `sdat` in the archive's bytes, as far as the decoder tells.
std::string KeptReason(std::span<const uint8_t> sdat, uint32_t id)
{
    try
    {
        const ntdlib::sdat::Archive archive = ntdlib::sdat::Archive::Parse(sdat);
        const std::span<const uint8_t> bytecode = ntdlib::sdat::Sequence::Parse(archive.File(id)).bytecode;
        constexpr uint32_t kEntry = 0;
        const ntdlib::ntr::snd::Decoded decoded = ntdlib::ntr::snd::DecodeSequence(bytecode, {}, std::span(&kEntry, 1));
        if (decoded.opaque)
        {
            return "the decoder keeps its bytecode as data";
        }
        if (!std::ranges::equal(ntdlib::ntr::snd::EncodeSequence(decoded.sequence).bytes, bytecode))
        {
            return "the encoding gives other bytes";
        }
        return "it shares bytes with another file, or its padding holds other bytes than zeros";
    }
    catch (const std::exception& error)
    {
        return error.what();
    }
}

// A line for each sequence file that the cut leaves in the archive's bytes, with the reason.
void PrintKept(const std::string& what, std::span<const uint8_t> sdat, const Files& files)
{
    for (const uint32_t id : files.kept)
    {
        std::printf("%s: sequence file %u stays in the archive's bytes: %s\n", what.c_str(), id,
                    KeptReason(sdat, id).c_str());
    }
}

// The archive's bytes at `place` in `rom`.
std::span<const uint8_t> ArchiveAt(const ntdlib::ntr::Rom& rom, const DsArchivePlace& place)
{
    return rom.FileData(place.file).subspan(place.offset, place.size);
}

// The longer sequences of `file`, the edited file and the number of sequences that took a wait.
struct Longer
{
    File file;
    std::size_t edited = 0;
};

Longer Lengthened(const File& file)
{
    Longer longer{file, 0};
    for (std::size_t s = 0; s < file.sequences.size(); s++)
    {
        if (std::optional<Sequence> sequence = WithWaitCopies(*file.sequences[s]))
        {
            longer.file.sequences[s] = std::move(*sequence);
            longer.edited++;
        }
    }

    return longer;
}

// Each other file of the archive keeps its bytes in `after`: each file of `before` that isn't a sequence file, and each
// sequence file that `file`'s cut leaves in the archive's bytes.
void CheckOtherFiles(const std::string& what, std::span<const uint8_t> before, const File& file,
                     std::span<const uint8_t> after)
{
    const ntdlib::sdat::Archive old = ntdlib::sdat::Archive::Parse(before);
    const ntdlib::sdat::Archive now = ntdlib::sdat::Archive::Parse(after);
    if (old.files.size() != now.files.size())
    {
        Problem(what, "the longer sequences change the number of files");
        return;
    }
    const Files files = FilesOf(before, file);
    std::vector<uint32_t> ids = files.others;
    ids.insert(ids.end(), files.kept.begin(), files.kept.end());
    for (const uint32_t id : ids)
    {
        if (!std::ranges::equal(old.File(id), now.File(id)))
        {
            Problem(what, "file " + std::to_string(id) + " of the archive changes");
        }
    }
}

// `again`, an import of the export of `longer`, returns the sequences of `longer`.
void CheckSequencesBack(const std::string& what, const File& longer, const File& again)
{
    if (again.sequences.size() != longer.sequences.size())
    {
        Problem(what, "the export's import holds " + std::to_string(again.sequences.size()) + " sequences, not " +
                          std::to_string(longer.sequences.size()));
        return;
    }
    for (std::size_t s = 0; s < longer.sequences.size(); s++)
    {
        if (*again.sequences[s] != *longer.sequences[s])
        {
            Problem(what, "sequence " + std::to_string(s) + " comes back otherwise from the export");
        }
    }
}

void CheckArchive(const std::string& path, std::span<const uint8_t> sdat, bool text)
{
    const auto start = std::chrono::steady_clock::now();
    const File file = hokuto::format::ImportDsArchive(sdat);
    const Files files = FilesOf(sdat, file);
    PrintKept(path, sdat, files);
    if (hokuto::format::ExportArchive(file) != std::vector<uint8_t>(sdat.begin(), sdat.end()))
    {
        Problem(path, "the export doesn't give the archive back");
    }
    CheckForms(path, file, text);

    const Longer longer = Lengthened(file);
    const std::vector<uint8_t> grown = hokuto::format::ExportArchive(longer.file);
    CheckOtherFiles(path, sdat, file, grown);
    CheckSequencesBack(path, longer.file, hokuto::format::ImportDsArchive(grown));

    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("%s: %zu bytes, %zu sequence files, %zu cut, %zu longer, %zu bytes longer, %.1f s\n", path.c_str(),
                sdat.size(), files.sequences.size(), file.sequences.size(), longer.edited, grown.size() - sdat.size(),
                seconds);
}

void CheckImage(const std::string& path, const std::vector<uint8_t>& image, bool text)
{
    const ntdlib::ntr::Rom rom = ntdlib::ntr::Rom::Parse(image);
    const std::vector<DsArchivePlace> places = hokuto::format::DsArchivesIn(image);
    if (places.empty())
    {
        std::printf("%s: no sound archive\n", path.c_str());
    }
    for (const DsArchivePlace& place : places)
    {
        const auto start = std::chrono::steady_clock::now();
        const std::string what = path + " " + place.path + "@" + std::to_string(place.offset);
        const File file = hokuto::format::ImportDsImage(image, place);
        const Files files = FilesOf(ArchiveAt(rom, place), file);
        PrintKept(what, ArchiveAt(rom, place), files);
        const hokuto::format::DsImageExport same = hokuto::format::ExportDsImage(file);
        if (same.image != image || !same.stale.empty())
        {
            Problem(what, "the export doesn't give the image back");
        }
        CheckForms(what, file, text);

        // An archive that shares its file with other data keeps its size.
        const Longer longer = Lengthened(file);
        if (hokuto::format::DsArchiveSharesFile(file))
        {
            const std::size_t file_size = rom.FileData(place.file).size();
            bool refused = false;
            try
            {
                hokuto::format::ExportDsImage(longer.file);
            }
            catch (const std::invalid_argument&)
            {
                refused = true;
            }
            if (longer.edited != 0 && !refused)
            {
                Problem(what, "the export takes a longer archive that shares its file");
            }
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            std::printf("%s: %u bytes in a file of %zu, %zu sequence files, %zu cut, %zu longer refused, %.1f s\n",
                        what.c_str(), place.size, file_size, files.sequences.size(), file.sequences.size(),
                        longer.edited, seconds);
            continue;
        }

        // The image keeps its number of files. Each file but the archive's keeps its bytes. The archive file contains
        // the new archive.
        const hokuto::format::DsImageExport grown = hokuto::format::ExportDsImage(longer.file);
        const ntdlib::ntr::Rom after = ntdlib::ntr::Rom::Parse(grown.image);
        const std::vector<uint8_t> archive = hokuto::format::ExportArchive(longer.file);
        if (after.Files().size() != rom.Files().size())
        {
            Problem(what, "the export's image holds " + std::to_string(after.Files().size()) + " files, not " +
                              std::to_string(rom.Files().size()));
        }
        for (const ntdlib::ntr::RomFile& other : rom.Files())
        {
            if (other.id != place.file && !std::ranges::equal(rom.FileData(other.id), after.FileData(other.id)))
            {
                Problem(what, "file " + other.path + " of the image changes");
            }
        }
        if (!std::ranges::equal(after.FileData(place.file), archive))
        {
            Problem(what, "the archive's file doesn't hold the new archive in the old one's place");
        }
        CheckOtherFiles(what, ArchiveAt(rom, place), file, archive);
        DsArchivePlace moved = place;
        moved.size = static_cast<uint32_t>(archive.size());
        CheckSequencesBack(what, longer.file, hokuto::format::ImportDsImage(grown.image, moved));

        std::string stale;
        for (const ntdlib::ntr::StaleHash& hash : grown.stale)
        {
            stale += ", " + std::to_string(hash.count) + " of " + hash.what + " stale";
        }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::printf(
            "%s: %u bytes, %zu sequence files, %zu cut, %zu longer, %zu bytes longer, image %td bytes longer%s, "
            "%.1f s\n",
            what.c_str(), place.size, files.sequences.size(), file.sequences.size(), longer.edited,
            archive.size() - place.size, static_cast<std::ptrdiff_t>(grown.image.size() - image.size()), stale.c_str(),
            seconds);
    }
}

} // namespace

int main(int argc, char** argv)
{
    const std::vector<std::string> arguments = xsflib::console::Arguments(argc, argv);
    bool text = false;
    std::vector<std::string> paths;
    for (std::size_t a = 1; a < arguments.size(); a++)
    {
        if (arguments[a] == "--text")
        {
            text = true;
        }
        else
        {
            paths.push_back(arguments[a]);
        }
    }
    if (paths.empty())
    {
        std::fputs("Usage: check_ds_layout [--text] <image.nds | archive.sdat>...\n", stderr);
        return 2;
    }

    for (const std::string& path : paths)
    {
        std::vector<uint8_t> bytes;
        if (!xsflib::ReadFile(xsflib::PathFromUtf8(path), bytes))
        {
            Problem(path, "can't be read");
            continue;
        }
        try
        {
            if (hokuto::format::IsDsArchive(bytes))
            {
                CheckArchive(path, bytes, text);
            }
            else
            {
                CheckImage(path, bytes, text);
            }
        }
        catch (const std::exception& error)
        {
            Problem(path, error.what());
        }
        std::fflush(stdout);
    }

    std::printf("%d problem%s\n", problems, problems == 1 ? "" : "s");
    return problems == 0 ? 0 : 1;
}
