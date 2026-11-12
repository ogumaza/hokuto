// SPDX-License-Identifier: MIT

// Checks the import and the export of the DSE files of DS cartridge images. For each image, it checks:
//
// - The import makes a sequence of each song and each sound effect (ntdlib::dse::Cut). The tool counts them, and names
//   the songs and the banks of sound effects that stay in the set's bytes.
// - The export reproduces the image byte for byte, with no stale hash.
// - The binary form reproduces the file. With --text, so does the text form.
// - Longer sequences: in each sequence that waits, kCopies copies of the first wait follow it. A bank of sound effects
//   that the longer effects would take past the 16 bits of its table keeps its sequences. The exported image then
//   contains each edited song and sound effect with the commands of the file. Each file with sequences keeps its
//   header, its size aside, and the header contains the new size of the file. A song keeps its settings. A bank of
//   sound effects keeps its two tables and its slots, each with its settings. Every other file of the image keeps its
//   bytes. An import of the export returns the edited sequences.
// - The browser's Export… of each song as its file (song::DseItemBytes) reproduces the file of the image byte for byte.
//
// The tool prints a line for each image. It returns 1 when any check fails, and 2 without an image.
//
//     check_dse_layout [--text] <image.nds>...

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/dse/commands.h"
#include "ntdlib/dse/formats.h"
#include "ntdlib/dse/layout.h"
#include "ntdlib/dse/sets.h"
#include "ntdlib/ntr/rom.h"
#include "xsflib/bytes.h"
#include "xsflib/console.h"
#include "xsflib/files.h"

#include "format/archive.h"
#include "format/file.h"
#include "format/text.h"
#include "layout_problems.h"
#include "song/dse.h"
#include "song/items.h"

namespace
{

using hokuto::format::File;
using hokuto::tools::layout::CheckForms;
using hokuto::tools::layout::Problem;
using hokuto::tools::layout::problems;
using ntdlib::commands::Kind;
using ntdlib::commands::Sequence;

constexpr int kCopies = 8;

// Where a header of DSE files stores the size of the file, in 4 bytes.
constexpr std::size_t kSizeField = 0x08;

// `sequence` with kCopies copies of its first wait after that wait, or nothing for a sequence without a wait.
std::optional<Sequence> WithWaitCopies(const Sequence& sequence)
{
    const auto wait = std::ranges::find_if(
        sequence.items, [](const ntdlib::commands::Command& command) { return command.kind == Kind::kWait; });
    if (wait == sequence.items.end())
    {
        return std::nullopt;
    }

    Sequence longer = sequence;
    longer.items.insert(longer.items.begin() + (wait - sequence.items.begin()) + 1, kCopies, *wait);
    return longer;
}

// The files of a set, by their names.
std::map<std::string, std::span<const uint8_t>> ByName(std::span<const ntdlib::dse::NamedFile> files)
{
    std::map<std::string, std::span<const uint8_t>> named;
    for (const ntdlib::dse::NamedFile& file : files)
    {
        named.emplace(file.path, file.bytes);
    }
    return named;
}

// The files of the set that the image contains, under their names.
std::vector<ntdlib::dse::NamedFile> FilesOfImage(const ntdlib::ntr::Rom& rom,
                                                 std::span<const ntdlib::dse::NamedFile> set)
{
    std::map<std::string, uint32_t> ids;
    for (const ntdlib::ntr::RomFile& file : rom.Files())
    {
        ids.emplace(file.path.empty() ? "file " + std::to_string(file.id) : file.path, file.id);
    }

    std::vector<ntdlib::dse::NamedFile> files;
    for (const ntdlib::dse::NamedFile& file : set)
    {
        const auto id = ids.find(file.path);
        if (id == ids.end())
        {
            throw std::runtime_error("missing from the image: " + file.path);
        }
        files.push_back({file.path, rom.FileData(id->second)});
    }
    return files;
}

// Checks one sequence of `written`, a file of the export, against the commands that give it.
void CheckWritten(const std::string& what, std::span<const uint8_t> written, const Sequence& sequence,
                  std::optional<uint16_t> effect)
{
    const std::optional<uint32_t> at = ntdlib::dse::TracksOffset(written, effect);
    const std::vector<uint8_t> bytes = ntdlib::dse::EncodeTracks(sequence).bytes;
    if (!at || *at + bytes.size() > written.size() || !std::ranges::equal(bytes, written.subspan(*at, bytes.size())))
    {
        Problem(what, "the export doesn't hold the file's commands");
    }
}

// Whether two headers of DSE's files hold the same bytes, the file's size at 0x08 aside. The writers set the size.
bool SameHeader(std::span<const uint8_t> one, std::span<const uint8_t> other)
{
    return one.size() == other.size() && std::ranges::equal(one.first(kSizeField), other.first(kSizeField)) &&
           std::ranges::equal(one.subspan(kSizeField + 4), other.subspan(kSizeField + 4));
}

// Whether two banks of sound effects hold the same slots, each with the same settings. Their tracks can differ.
bool SameSlots(const ntdlib::dse::SoundEffects& one, const ntdlib::dse::SoundEffects& other)
{
    if (one.sequences.size() != other.sequences.size())
    {
        return false;
    }

    for (std::size_t slot = 0; slot < one.sequences.size(); ++slot)
    {
        const std::optional<ntdlib::dse::EffectSequence>& mine = one.sequences[slot];
        const std::optional<ntdlib::dse::EffectSequence>& theirs = other.sequences[slot];
        if (mine.has_value() != theirs.has_value() || (mine && mine->info != theirs->info))
        {
            return false;
        }
    }

    return true;
}

// Checks each file of `after`, the set's files in the export of `longer`, against its bytes in `named` before. The
// sequences that `edited` lists for a file are written in it.
void CheckWrittenFiles(const std::string& what, const File& longer, std::span<const ntdlib::dse::NamedFile> after,
                       const std::map<std::string, std::span<const uint8_t>>& named,
                       const std::map<std::string, std::vector<std::pair<std::optional<uint16_t>, uint32_t>>>& edited)
{
    for (const ntdlib::dse::NamedFile& written : after)
    {
        const std::span<const uint8_t> before = named.at(written.path);
        const auto sequences = edited.find(written.path);
        if (sequences == edited.end())
        {
            if (!std::ranges::equal(written.bytes, before))
            {
                Problem(what, written.path + " changes, and holds no sequence");
            }
            continue;
        }

        for (const auto& [effect, sequence] : sequences->second)
        {
            CheckWritten(what + ": " + longer.sequences[sequence]->labels.at(0).name, written.bytes,
                         *longer.sequences[sequence], effect);
        }

        // The readers read the new file. Its header keeps every byte but the size. The size follows the new file, and
        // the settings stay.
        if (xsflib::LoadLe32(&written.bytes[kSizeField]) != written.bytes.size())
        {
            Problem(what, written.path + "'s header gives another size");
        }
        if (std::memcmp(written.bytes.data(), "smdl", 4) == 0)
        {
            const ntdlib::dse::Song song = ntdlib::dse::Song::Parse(written.bytes);
            const ntdlib::dse::Song old = ntdlib::dse::Song::Parse(before);
            if (!SameHeader(song.header, old.header))
            {
                Problem(what, written.path + "'s header changes");
            }
            if (song.info != old.info)
            {
                Problem(what, written.path + "'s settings change");
            }
        }
        else
        {
            const ntdlib::dse::SoundEffects effects = ntdlib::dse::SoundEffects::Parse(written.bytes);
            const ntdlib::dse::SoundEffects old = ntdlib::dse::SoundEffects::Parse(before);
            if (!SameHeader(effects.header, old.header))
            {
                Problem(what, written.path + "'s header changes");
            }
            if (effects.macros != old.macros || effects.bank_lists != old.bank_lists || !SameSlots(effects, old))
            {
                Problem(what, written.path + "'s tables or the settings of its effects change");
            }
        }
    }
}

// Checks that each file of `rom`, the export of `image`, that `after` doesn't list keeps its bytes, and returns how
// many there are.
std::size_t CheckRomFilesKept(const std::string& what, const ntdlib::ntr::Rom& rom, const std::vector<uint8_t>& image,
                              std::span<const ntdlib::dse::NamedFile> after)
{
    const ntdlib::ntr::Rom old_rom = ntdlib::ntr::Rom::Parse(image);
    if (rom.Files().size() != old_rom.Files().size())
    {
        Problem(what, "the export has another number of files");
    }
    const std::map<std::string, std::span<const uint8_t>> set_files = ByName(after);
    std::size_t other = 0;
    for (const ntdlib::ntr::RomFile& rom_file : rom.Files())
    {
        if (set_files.contains(rom_file.path))
        {
            continue;
        }

        other++;
        if (!std::ranges::equal(rom.FileData(rom_file.id), old_rom.FileData(rom_file.id)))
        {
            Problem(what, rom_file.path + " changes, and the set doesn't hold it");
        }
    }

    return other;
}

// Checks that an import of `image`, the export of `longer`, returns the edited sequences.
void CheckImportAgain(const std::string& what, const std::vector<uint8_t>& image, const File& longer)
{
    const File again = hokuto::format::ImportDseImage(image);
    if (again.sequences.size() != longer.sequences.size())
    {
        Problem(what, "the export's import has another number of sequences");
    }
    else
    {
        for (std::size_t s = 0; s < again.sequences.size(); ++s)
        {
            if (again.sequences[s]->items != longer.sequences[s]->items)
            {
                Problem(what,
                        "the export's import gives " + longer.sequences[s]->labels.at(0).name + " other commands");
            }
        }
    }
}

void CheckLonger(const std::string& what, const File& file, std::span<const ntdlib::dse::NamedFile> original,
                 const std::vector<uint8_t>& image)
{
    // Each sequence with a wait grows, until a bank of sound effects' table can't reach its next effect.
    File longer = file;
    std::map<std::string, std::size_t> growth;
    std::map<std::string, std::vector<std::pair<std::optional<uint16_t>, uint32_t>>> edited;
    const std::map<std::string, std::span<const uint8_t>> named = ByName(original);
    std::size_t lengthened = 0;
    for (std::size_t s = 0; s < file.sequences.size(); ++s)
    {
        const Sequence& sequence = *file.sequences[s];
        const std::optional<ntdlib::dse::SequenceRef> ref =
            ntdlib::dse::SequenceNamed(sequence.labels.at(0).name, original);
        if (!ref)
        {
            Problem(what, "sequence " + std::to_string(s) + " names no file: " + sequence.labels.at(0).name);
            continue;
        }

        std::optional<Sequence> copies = WithWaitCopies(sequence);
        if (!copies)
        {
            edited[ref->path].emplace_back(ref->effect, static_cast<uint32_t>(s));
            continue;
        }

        // The effects table uses 16 bits. A bank that reaches 0xF000 bytes keeps the rest of its sequences.
        const std::size_t size =
            ntdlib::dse::EncodeTracks(*copies).bytes.size() - ntdlib::dse::EncodeTracks(sequence).bytes.size();
        std::size_t& grown = growth[ref->path];
        if (ref->effect && named.at(ref->path).size() + grown + size > 0xF000)
        {
            edited[ref->path].emplace_back(ref->effect, static_cast<uint32_t>(s));
            continue;
        }

        grown += size;
        longer.sequences[s] = std::move(*copies);
        edited[ref->path].emplace_back(ref->effect, static_cast<uint32_t>(s));
        lengthened++;
    }

    const hokuto::format::DsImageExport exported = hokuto::format::ExportDsImage(longer);
    if (exported.image == image && lengthened != 0)
    {
        Problem(what, "the longer sequences leave the image as it was");
    }

    const ntdlib::ntr::Rom rom = ntdlib::ntr::Rom::Parse(exported.image);
    const std::vector<ntdlib::dse::NamedFile> after = FilesOfImage(rom, original);
    CheckWrittenFiles(what, longer, after, named, edited);

    // Every file of the image that the set doesn't hold keeps its bytes.
    const std::size_t other = CheckRomFilesKept(what, rom, image, after);

    // An import of the export returns the edited sequences.
    CheckImportAgain(what, exported.image, longer);

    std::printf("  %zu sequences longer, the image %zu bytes against %zu, %zu other files, %zu stale hashes\n",
                lengthened, exported.image.size(), image.size(), other, exported.stale.size());
}

// Each song of `image` is exported as its file. The browser's Export… writes the same file. The file has to be `files`'
// file of the song, byte for byte.
void CheckSongFiles(const std::string& what, const std::vector<uint8_t>& image,
                    std::span<const ntdlib::dse::NamedFile> files)
{
    const std::shared_ptr<const hokuto::song::DseSet> set = hokuto::song::LoadDseSet(image);
    const std::map<std::string, std::span<const uint8_t>> named = ByName(files);
    std::size_t songs = 0;
    for (uint32_t sound = 0; sound < set->sounds.size(); ++sound)
    {
        const ntdlib::sequence_export::dse::SequenceInput& input = set->sounds[sound];
        if (!input.song)
        {
            continue;
        }

        songs++;
        const auto file = named.find(input.ref.path);
        const std::vector<uint8_t> exported =
            hokuto::song::DseItemBytes(*set, {.kind = hokuto::song::ItemKind::kSequence, .index = sound});
        if (file == named.end() || !std::ranges::equal(exported, file->second))
        {
            Problem(what, input.name + " goes out as another file than the image's");
        }
    }
    std::printf("  %zu songs went out as their files\n", songs);
}

void CheckImage(const std::string& path, const std::vector<uint8_t>& image, bool text)
{
    if (!hokuto::format::HoldsDseFiles(image))
    {
        Problem(path, "the image holds no files of DSE");
        return;
    }

    const File file = hokuto::format::ImportDseImage(image);
    const std::vector<uint8_t> set = hokuto::format::ExportArchive(file);
    const std::vector<ntdlib::dse::NamedFile> files = ntdlib::dse::ReadSet(set);
    if (set.size() != file.native->game_sizes->archive)
    {
        Problem(path, "the set has another size than the game's");
    }

    // The files that stay in the set's bytes.
    std::size_t with_sequences = 0;
    std::size_t kept = 0;
    std::map<std::string, std::size_t> cut;
    for (const auto& sequence : file.sequences)
    {
        if (const auto ref = ntdlib::dse::SequenceNamed(sequence->labels.at(0).name, files))
        {
            cut[ref->path]++;
        }
    }
    for (const ntdlib::dse::NamedFile& named : files)
    {
        if (std::memcmp(named.bytes.data(), "swdl", 4) == 0)
        {
            continue;
        }

        with_sequences++;
        if (!cut.contains(named.path))
        {
            kept++;
            std::printf("  %s stays in the bytes\n", named.path.c_str());
        }
    }

    const ntdlib::ntr::Rom rom = ntdlib::ntr::Rom::Parse(image);
    const std::vector<ntdlib::dse::NamedFile> in_image = FilesOfImage(rom, files);
    for (std::size_t f = 0; f < files.size(); f++)
    {
        if (!std::ranges::equal(files[f].bytes, in_image[f].bytes))
        {
            Problem(path, files[f].path + " doesn't come back");
        }
    }

    const hokuto::format::DsImageExport exported = hokuto::format::ExportDsImage(file);
    if (exported.image != image)
    {
        Problem(path, "the export doesn't give the image back");
    }
    if (!exported.stale.empty())
    {
        Problem(path, "the export leaves hashes stale, and changes nothing");
    }

    CheckForms(path, file, text);
    CheckLonger(path, file, files, image);
    CheckSongFiles(path, image, files);
    std::printf("%s: %zu files, %zu of them with sequences, %zu in the bytes; %zu sequences; the set takes %zu bytes\n",
                path.c_str(), files.size(), with_sequences, kept, file.sequences.size(), set.size());
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
        std::fputs("Usage: check_dse_layout [--text] <image.nds>...\n", stderr);
        return 2;
    }

    for (const std::string& path : paths)
    {
        const auto start = std::chrono::steady_clock::now();
        try
        {
            std::vector<uint8_t> image;
            if (!xsflib::ReadFile(xsflib::PathFromUtf8(path), image))
            {
                throw std::runtime_error("the file can't be read");
            }
            CheckImage(path, image, text);
        }
        catch (const std::exception& error)
        {
            Problem(path, error.what());
        }
        const std::chrono::duration<double> taken = std::chrono::steady_clock::now() - start;
        std::printf("  %.1f s\n", taken.count());
    }

    std::printf("%d problems\n", problems);
    return problems == 0 ? 0 : 1;
}
