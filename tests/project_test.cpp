// SPDX-License-Identifier: MIT

// Tests the projects of format/project.h on a made-up ROM with two drivers: agblib's test cartridge of Nintendo R&D2's
// driver with an MP2K test cartridge after it (agblib/testing/two_drivers_cart.h). The ROM contains no game data.
//
// The import has to give an archive for each driver, MusicPlayer2000's first, and one free space past both drivers'
// data. Without edits, the project exports the ROM byte for byte, and both forms of the file give the project back.
// After edits that make songs of both drivers longer, the export has to keep the songs of the two drivers apart: the
// import of the new ROM gives each edited song. A change of a run's bytes reaches the other archive, and a change of
// another driver's song is refused. The readers refuse files that break the rules of
// docs/format.md: among them archives whose songs share bytes, and more than one archive in a file of version 1.

#include "format/project.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "agblib/testing/rd2_cart.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/sar/layout.h"
#include "xsflib/binary.h"
#include "xsflib/testing/check.h"

#include "cart_bytes.h"
#include "format/archive.h"
#include "format/file.h"
#include "format/text.h"
#include "two_driver_cart.h"

namespace
{

namespace format = hokuto::format;
namespace rd2_cart = agblib::testing::rd2_cart;
using format::Engine;
using format::File;
using format::Project;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using xsflib::testing::Check;

// The made-up ROM.
std::vector<uint8_t> TwoDriverRom()
{
    return hokuto::testing::TwoDriverCart().rom;
}

// `file` with `size` bytes of data after the last item of sequence `sequence`. No command reaches them, and the song
// outgrows its place.
File Grown(File file, std::size_t sequence, std::size_t size)
{
    ntdlib::commands::Sequence grown = *file.sequences.at(sequence);
    Command data;
    data.kind = Kind::kData;
    data.bytes.assign(size, 0x11);
    grown.items.push_back(std::move(data));
    file.sequences[sequence] = std::move(grown);

    return file;
}

// The items of `sequence` without the data after its last command. A cut keeps the bytes after a song's last command
// as data, or leaves them in a run.
std::vector<Command> Commands(const ntdlib::commands::Sequence& sequence)
{
    std::vector<Command> items = sequence.items;
    while (!items.empty() && items.back().kind == Kind::kData)
    {
        items.pop_back();
    }

    return items;
}

// Whether `read` holds the commands of `expected`.
bool SameCommands(const ntdlib::commands::Sequence& read, const ntdlib::commands::Sequence& expected)
{
    return Commands(read) == Commands(expected);
}

// The bytes of the first run of `file`.
std::vector<uint8_t> FirstRun(const File& file)
{
    return std::get<std::vector<uint8_t>>(file.native->pieces->front());
}

// Whether `body` throws an exception of type E.
template <typename E, typename Body>
bool Throws(Body body)
{
    try
    {
        body();
    }
    catch (const E&)
    {
        return true;
    }

    return false;
}

void TestImport()
{
    const std::vector<uint8_t> rom = TwoDriverRom();

    const format::ImportedArchives imported = format::ImportGbaArchives(rom);

    const Project& project = imported.project;
    Check(imported.notes.empty(), "the import leaves out no driver");
    Check(project.archives.size() == 2, "an archive for each driver");
    if (project.archives.size() != 2)
    {
        return;
    }

    const File& mp2k = project.archives[0];
    const File& rd2 = project.archives[1];
    Check(mp2k.engine == Engine::kGba && rd2.engine == Engine::kRd2, "MusicPlayer2000's archive comes first");
    Check(mp2k.sequences.size() == 2 && rd2.sequences.size() == 2, "each archive takes its driver's songs");
    Check(mp2k.native->gba_free_floor == rd2.native->driver->free_start, "one free space for both drivers");
    Check(mp2k.native->gba_free_floor >= hokuto::testing::kTwoDriverMp2kOrigin,
          "the free space starts past both drivers' data");
    Check(!format::ProjectProblem(project), "the project keeps the rules");
    Check(format::ExportProject(project) == rom, "the project exports the ROM byte for byte");
}

void TestForms()
{
    const Project project = format::ImportGbaArchives(TwoDriverRom()).project;
    Project tagged = project;
    for (File& archive : tagged.archives)
    {
        archive.tags["title"] = "Two drivers";
    }
    tagged.front = 1;

    const std::vector<uint8_t> binary = format::WriteProject(tagged);
    const std::string text = format::WriteProjectText(tagged);
    const std::span<const uint8_t> text_bytes(reinterpret_cast<const uint8_t*>(text.data()), text.size());

    Check(format::ReadProject(binary) == tagged, "the binary form gives the project back");
    Check(format::ReadProject(text_bytes) == tagged, "the text form gives the project back");
    Check(format::WriteProject(format::ReadProject(text_bytes)) == binary, "the text form gives the binary form back");
    Check(Throws<xsflib::FormatError>([&] { format::Read(binary); }), "a reader of one archive refuses the file");
    Check(Throws<xsflib::FormatError>([&] { format::ReadText(text); }),
          "a reader of one archive refuses the text form");

    // The lines of the project and of each archive take comments and spaces. The lines of a file of one archive take
    // them too.
    std::string spaced = text;
    const auto replace_all = [&spaced](const std::string& from, const std::string& to)
    {
        for (std::size_t at = spaced.find(from); at != std::string::npos; at = spaced.find(from, at + to.size()))
        {
            spaced.replace(at, from.size(), to);
        }
    };
    replace_all("\nengine archives\n", "\nengine   archives  # two drivers\n");
    replace_all("\nfront 1\n", "\n  front\t1 # the second driver\n");
    replace_all("\narchive\n", "\n archive   # the next driver\n");
    const std::span<const uint8_t> spaced_bytes(reinterpret_cast<const uint8_t*>(spaced.data()), spaced.size());
    Check(spaced != text, "the spaced text differs from the written text");
    Check(format::ReadProject(spaced_bytes) == tagged, "the text form reads comments and spaces in its lines");
}

void TestEditsOfSongs()
{
    const std::vector<uint8_t> rom = TwoDriverRom();
    Project project = format::ImportGbaArchives(rom).project;

    // Song 0 of each driver grows past its place, and both move into the one free space.
    project = format::WithArchive(project, 0, Grown(project.archives[0], 0, 40));
    project = format::WithArchive(project, 1, Grown(project.archives[1], 1, 40));
    const std::vector<uint8_t> exported = format::ExportProject(project);
    const Project again = format::ImportGbaArchives(exported).project;

    Check(exported.size() > rom.size(), "the songs that move go past the ROM's end");
    Check(again.archives.size() == 2, "the new ROM still gives an archive for each driver");
    // A cut takes the songs in the order of their places, and a song that moves comes after the others.
    for (std::size_t a = 0; a < again.archives.size() && again.archives.size() == 2; ++a)
    {
        for (const auto& expected : project.archives[a].sequences)
        {
            const auto found = [&expected](const auto& read)
            {
                return SameCommands(*read, *expected);
            };
            Check(std::ranges::any_of(again.archives[a].sequences, found), "each song of each driver comes back");
        }
    }

    // A file of the edited project exports the same ROM.
    Check(format::ExportProject(format::ReadProject(format::WriteProject(project))) == exported,
          "the saved project exports the same ROM");
}

void TestEditsOfRuns()
{
    Project project = format::ImportGbaArchives(TwoDriverRom()).project;

    // A byte of MusicPlayer2000's first run that R&D2's driver keeps in a run too: the byte of the header at 0xB2.
    File changed = project.archives[0];
    std::vector<ntdlib::sar::Piece> pieces = *changed.native->pieces;
    std::get<std::vector<uint8_t>>(pieces.front())[0xB2] = 0x96;
    changed.native.Edit([&pieces](format::NativeArchive& native) { native.pieces = pieces; });
    const Project edited = format::WithArchive(project, 0, changed);

    Check(FirstRun(edited.archives[1])[0xB2] == 0x96, "the other archive takes the changed byte");
    Check(!format::ProjectProblem(edited), "the runs still agree");

    // R&D2's first song lies in MusicPlayer2000's first run.
    const std::vector<uint64_t> starts = format::NativePieceStarts(project.archives[1]);
    std::size_t song = 0;
    while (!std::holds_alternative<ntdlib::sar::SequencePiece>((*project.archives[1].native->pieces)[song]))
    {
        ++song;
    }

    std::vector<ntdlib::sar::Piece> clash = *project.archives[0].native->pieces;
    std::get<std::vector<uint8_t>>(clash.front())[starts[song]] ^= 0xFF;
    File clashing = project.archives[0];
    clashing.native.Edit([&clash](format::NativeArchive& native) { native.pieces = clash; });
    Check(Throws<std::invalid_argument>([&] { format::WithArchive(project, 0, clashing); }),
          "an edit of another driver's song is refused");
}

void TestValuesOfTheProject()
{
    Project project = format::ImportGbaArchives(TwoDriverRom()).project;
    File louder = project.archives[1];
    louder.aux_volumes[0] = 0.5f;
    louder.tags["game"] = "A test";

    const Project edited = format::WithArchive(project, 1, louder);

    Check(edited.archives[0].aux_volumes[0] == 0.5f && edited.archives[0].tags.at("game") == "A test",
          "the values of the project reach every archive");
}

void TestRefusals()
{
    const Project project = format::ImportGbaArchives(TwoDriverRom()).project;
    Project mixing = project;
    mixing.archives[1].mixing = format::Mixing::kEnhanced;
    Project filler = project;
    filler.archives[0].native.Edit([](format::NativeArchive& native) { native.gba_filler = true; });
    Project order = project;
    std::swap(order.archives[0], order.archives[1]);
    Project runs = project;
    std::vector<ntdlib::sar::Piece> pieces = *runs.archives[1].native->pieces;
    std::get<std::vector<uint8_t>>(pieces.front())[0x10] ^= 1;
    runs.archives[1].native.Edit([&pieces](format::NativeArchive& native) { native.pieces = pieces; });
    Project front = project;
    front.front = 2;
    Project console = project;
    console.archives[1].engine = Engine::kDs;

    Check(format::ProjectProblem(mixing).has_value(), "archives of two mixing modes are refused");
    Check(format::ProjectProblem(filler).has_value(), "an archive that takes the filler runs is refused");
    Check(format::ProjectProblem(order).has_value(), "archives out of the engines' order are refused");
    Check(format::ProjectProblem(runs).has_value(), "runs that differ are refused");
    Check(format::ProjectProblem(front).has_value(), "a missing archive in front is refused");
    Check(format::ProjectProblem(console).value_or("").find("isn't an archive of the GBA") != std::string::npos,
          "an archive of another console is refused");
    Check(Throws<std::invalid_argument>([&] { format::WriteProject(runs); }), "the writer refuses such a project");

    // A text form without the archive in front, and one with an archive alone.
    std::string text = format::WriteProjectText(project);
    std::string without_front = text;
    without_front.erase(without_front.find("front 0\n"), 8);
    std::string one = text.substr(0, text.rfind("\narchive\n"));
    const auto read = [](const std::string& form)
    {
        return format::ReadProject(std::span(reinterpret_cast<const uint8_t*>(form.data()), form.size()));
    };
    Check(Throws<xsflib::FormatError>([&] { read(without_front); }), "the text form names the archive in front");
    Check(Throws<xsflib::FormatError>([&] { read(one); }), "the text form has two archives or more");

    // Two archives whose songs share bytes: the same archive twice.
    Project twice = project;
    twice.archives[1] = twice.archives[0];
    const std::optional<std::string> shared = format::ProjectProblem(twice);
    Check(shared && shared->find("share bytes of their songs or fields") != std::string::npos,
          "archives whose songs share bytes are refused");

    // A file of version 1 with more than one archive, in both forms.
    std::vector<uint8_t> binary = format::WriteProject(project);
    binary[8] = 1;
    binary[9] = 0;
    std::string version_1 = text;
    version_1.replace(version_1.find(' ') + 1, version_1.find('\n') - version_1.find(' ') - 1, "1.2");
    Check(Throws<xsflib::FormatError>([&] { format::ReadProject(binary); }),
          "the binary form of version 1 has one archive");
    Check(Throws<xsflib::FormatError>([&] { read(version_1); }), "the text form of version 1 has one archive");
}

void TestOneDriver()
{
    rd2_cart::Cart rd2;
    rd2.Sequences({{rd2_cart::TrackData().Wait(4).End()}});
    const std::vector<uint8_t> rom = hokuto::format::ExportArchive(
        format::ImportGbaArchives(hokuto::testing::RomBytes(rd2.ToRom())).project.archives.front());

    const format::ImportedArchives imported = format::ImportGbaArchives(rom);

    Check(imported.project.archives.size() == 1, "a ROM with one driver gives one archive");
    Check(format::WriteProject(imported.project) == format::Write(imported.project.archives.front()),
          "a project of one archive takes the form of a file");
}

} // namespace

int main()
{
    TestImport();
    TestForms();
    TestEditsOfSongs();
    TestEditsOfRuns();
    TestValuesOfTheProject();
    TestRefusals();
    TestOneDriver();

    return xsflib::testing::Summary("project");
}
