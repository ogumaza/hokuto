// SPDX-License-Identifier: MIT

// Tests the edits of a file with more than one archive on a made-up ROM with two drivers (two_driver_cart.h). The ROM
// contains no game data.
//
// New bytes of MusicPlayer2000 go past the bytes of R&D2's driver (edit::WithGbaNewBytes and the floor). An edit of
// one of MusicPlayer2000's voices reaches the runs of R&D2's archive through format::WithArchive. The ROM that the
// project exports then contains the new bytes once, and both archives import from it with their songs. The history
// refuses an edit that changes another archive's song, and stays as it was. A move through the history brings the
// archive that Undo or Redo would end on to the front.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/mp2k/song.h"
#include "ntdlib/sar/layout.h"
#include "xsflib/testing/check.h"

#include "edit/gba_voices.h"
#include "edit/history.h"
#include "format/archive.h"
#include "format/file.h"
#include "format/project.h"
#include "song/gba.h"
#include "two_driver_cart.h"

namespace
{

namespace format = hokuto::format;
using format::File;
using format::Project;
using hokuto::edit::Action;
using hokuto::edit::History;
using xsflib::testing::Check;

constexpr uint32_t kRomBase = 0x08000000;

// The bytes of `rom` at the ROM address `address`.
std::vector<uint8_t> BytesAt(const std::vector<uint8_t>& rom, uint32_t address, std::size_t size)
{
    const std::size_t offset = address - kRomBase;
    return offset + size <= rom.size() ? std::vector<uint8_t>(rom.begin() + static_cast<std::ptrdiff_t>(offset),
                                                              rom.begin() + static_cast<std::ptrdiff_t>(offset + size))
                                       : std::vector<uint8_t>();
}

// Whether `body` throws an exception of type `Error`.
template <typename Error, typename Body>
bool Throws(Body body)
{
    try
    {
        body();
    }
    catch (const Error&)
    {
        return true;
    }

    return false;
}

Action Make(const std::string& name, const File& file)
{
    return {name, "", std::chrono::system_clock::time_point(), std::make_shared<const File>(file)};
}

// The number of sequences of each archive of the import of `rom`.
std::vector<std::size_t> SongCounts(const std::vector<uint8_t>& rom)
{
    std::vector<std::size_t> counts;
    for (const File& archive : format::ImportGbaArchives(rom).project.archives)
    {
        counts.push_back(archive.sequences.size());
    }

    return counts;
}

void TestNewBytes()
{
    const std::vector<uint8_t> rom = hokuto::testing::TwoDriverCart().rom;
    const Project project = format::ImportGbaArchives(rom).project;
    const std::vector<uint8_t> bytes(64, 0x12);

    const hokuto::edit::PlacedGbaBytes placed = hokuto::edit::WithGbaNewBytes(project.archives[0], bytes);
    const Project edited = format::WithArchive(project, 0, placed.file);
    const std::vector<uint8_t> exported = format::ExportProject(edited);

    Check(placed.address - kRomBase >= project.archives[1].native->driver->free_start,
          "MusicPlayer2000's new bytes go past the bytes of R&D2's driver");
    Check(edited.archives[1].native->driver->free_start >= placed.address - kRomBase + bytes.size(),
          "R&D2's free space moves past them");
    Check(BytesAt(exported, placed.address, bytes.size()) == bytes, "the exported ROM contains the new bytes");
    Check(SongCounts(exported) == SongCounts(rom), "both archives import from the exported ROM");
}

void TestVoiceEdit()
{
    const hokuto::testing::TwoDriverRom cart = hokuto::testing::TwoDriverCart();
    const Project project = format::ImportGbaArchives(cart.rom).project;
    agblib::mp2k::Voice voice;
    const std::shared_ptr<const hokuto::song::GbaRom> rom = hokuto::song::LoadGbaRom(cart.rom);
    Check(agblib::mp2k::ReadVoice(rom->rom, cart.voices, voice), "the voice of program 0 can be read");
    voice.release = static_cast<uint8_t>(voice.release == 100 ? 101 : 100);

    const File changed = hokuto::edit::WithGbaVoice(project.archives[0], voice);
    const Project edited = format::WithArchive(project, 0, changed);
    const std::vector<uint8_t> exported = format::ExportProject(edited);
    const std::vector<uint8_t> voice_bytes = BytesAt(format::ExportArchive(changed), cart.voices, 12);

    Check(voice_bytes != BytesAt(cart.rom, cart.voices, 12), "the edit changes the voice's bytes");
    Check(BytesAt(format::ExportArchive(edited.archives[1]), cart.voices, 12) == voice_bytes,
          "R&D2's archive takes the voice's new bytes in its runs");
    Check(BytesAt(exported, cart.voices, 12) == voice_bytes, "the exported ROM contains them");
    Check(!format::ProjectProblem(edited), "the runs of the two archives still agree");
}

void TestRefusedEdit()
{
    const Project project = format::ImportGbaArchives(hokuto::testing::TwoDriverCart().rom).project;
    History history(std::make_shared<const Project>(project));

    // A byte of R&D2's first song, changed in MusicPlayer2000's run that contains it.
    const std::vector<uint64_t> starts = format::NativePieceStarts(project.archives[1]);
    std::size_t song = 0;
    while (!std::holds_alternative<ntdlib::sar::SequencePiece>((*project.archives[1].native->pieces)[song]))
    {
        ++song;
    }
    std::vector<ntdlib::sar::Piece> pieces = *project.archives[0].native->pieces;
    std::get<std::vector<uint8_t>>(pieces.front())[starts[song]] ^= 0xFF;
    File clashing = project.archives[0];
    clashing.native.Edit([&pieces](format::NativeArchive& native) { native.pieces = pieces; });
    const std::shared_ptr<const File> before = history.Current();

    Check(Throws<std::invalid_argument>([&] { history.Do(Make("Clash", clashing)); }),
          "the history refuses an edit of another archive's song");
    Check(history.Done() == 0 && history.Actions().empty() && history.Current() == before && !history.Modified(),
          "the refused edit leaves the history as it was");
}

// New bytes of MusicPlayer2000 move the free space of R&D2's archive, and with it MusicPlayer2000's floor. The archive
// in front of the history then has the floor of the project's copy.
void TestFloor()
{
    const Project project = format::ImportGbaArchives(hokuto::testing::TwoDriverCart().rom).project;
    History history(std::make_shared<const Project>(project));
    const std::vector<uint8_t> bytes(64, 0x34);
    const hokuto::edit::PlacedGbaBytes placed = hokuto::edit::WithGbaNewBytes(project.archives[0], bytes);
    history.Do(Make("New bytes", placed.file));

    Check(history.CurrentProject().archives[0].native->gba_free_floor != placed.file.native->gba_free_floor,
          "the new bytes move MusicPlayer2000's floor");
    Check(*history.Current() == history.CurrentProject().archives[history.Front()],
          "the archive in front is the project's copy");
}

void TestGoTo()
{
    const Project project = format::ImportGbaArchives(hokuto::testing::TwoDriverCart().rom).project;
    History history(std::make_shared<const Project>(project));
    const auto tagged = [&history](const std::string& value)
    {
        File file = *history.Current();
        file.sound_tags[0]["title"] = value;
        history.Do(Make("Tag", file));
    };

    // Actions on archives 0, 1, 1 and 0.
    tagged("a");
    history.SetFront(1);
    tagged("b");
    tagged("c");
    history.SetFront(0);
    tagged("d");

    history.GoTo(1);
    const std::size_t back = history.Front();
    history.GoTo(4);
    const std::size_t forward = history.Front();
    history.GoTo(2);
    const std::size_t one_back = history.Front();

    Check(back == 1, "a move back to action 1 ends on the archive of action 2, as repeated Undo does");
    Check(forward == 0, "a move forward ends on the archive of the last action redone");
    Check(one_back == 1, "a move back by two actions ends on the archive of action 3");
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("project_edits",
                                    []
                                    {
                                        TestNewBytes();
                                        TestVoiceEdit();
                                        TestRefusedEdit();
                                        TestFloor();
                                        TestGoTo();
                                    });
}
