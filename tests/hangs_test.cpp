// SPDX-License-Identifier: MIT

// Tests the refusal of a loop without a wait (src/edit/bytecode.h), and what happens when a song contains one all the
// same. NITRO-Composer, MP2K and DSE have no limit of commands in a tick. Such a loop hangs the console. An edit has to
// refuse to make one. A read of such a song has to fail with a message, and a model that meets one has to stop with its
// reason: a thread that never returns can't be stopped.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/dse/driver.h"
#include "ntdlib/sdat/layout.h"
#include "ntdlib/testing/dse_song_files.h"
#include "ntdlib/testing/mp2k_cart.h"
#include "ntdlib/testing/nds_files.h"
#include "ntdlib/testing/sdat_files.h"
#include "xsflib/testing/check.h"

#include "cart_bytes.h"
#include "edit/bytecode.h"
#include "edit/commands.h"
#include "format/archive.h"
#include "format/file.h"
#include "song/ds.h"
#include "song/dse.h"
#include "song/gba.h"
#include "song/sequence.h"
#include "song/sequence_model.h"

namespace
{

namespace files = ntdlib::testing::sdat_files;
namespace mp2k = ntdlib::mp2k;
namespace mp2k_cart = ntdlib::testing::mp2k_cart;
namespace sdat = ntdlib::sdat;
using hokuto::edit::CommandPlace;
using hokuto::edit::CommandsWithoutWayOut;
using hokuto::edit::RefuseLoopsWithoutWaits;
using hokuto::format::File;
using hokuto::song::Console;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::Sequence;
using ntdlib::commands::Source;
using xsflib::testing::ErrorOf;

constexpr uint16_t kNone = 0xFFFF;
const Console kDs{.engine = hokuto::format::Engine::kDs};
const Console kGba{.engine = hokuto::format::Engine::kGba};
const Console kDse{.engine = hokuto::format::Engine::kDse};

Command Plain(Kind kind, int64_t value = 1)
{
    Command command;
    command.kind = kind;
    command.value.value = value;
    return command;
}

Command To(Kind kind, uint32_t target)
{
    Command command;
    command.kind = kind;
    command.target = target;
    return command;
}

Sequence Of(std::vector<Command> items)
{
    Sequence sequence;
    sequence.items = std::move(items);
    return sequence;
}

// CommandsWithoutWayOut finds each loop that a track can't leave: one without a wait or an end on the way, through
// settings, waits of no ticks, notes that don't wait and calls.
void TestCommandsWithoutWayOut()
{
    // A setting that jumps to itself has no way out. The command before the loop leads only into it.
    const Sequence loop = Of({Plain(Kind::kPan), Plain(Kind::kVolume), To(Kind::kJump, 1)});
    XSFLIB_CHECK(CommandsWithoutWayOut(loop, kDs) == 3);
    XSFLIB_CHECK(CommandsWithoutWayOut(loop, kGba) == 3);

    // A wait or an end anywhere on the way lets the track go on.
    XSFLIB_CHECK(CommandsWithoutWayOut(Of({Plain(Kind::kWait), To(Kind::kJump, 0)}), kGba) == 0);
    XSFLIB_CHECK(CommandsWithoutWayOut(Of({Plain(Kind::kVolume), Plain(Kind::kEnd)}), kGba) == 0);

    // A wait of no ticks waits for nothing. A wait that a variable or a random number decides may wait.
    XSFLIB_CHECK(CommandsWithoutWayOut(Of({Plain(Kind::kWait, 0), To(Kind::kJump, 0)}), kDs) == 2);
    Command variable = Plain(Kind::kWait, 0);
    variable.value.source = Source::kVariable;
    XSFLIB_CHECK(CommandsWithoutWayOut(Of({variable, To(Kind::kJump, 0)}), kDs) == 0);
    Command random = Plain(Kind::kWait, 0);
    random.value.source = Source::kRandom;
    random.value.high = 3;
    XSFLIB_CHECK(CommandsWithoutWayOut(Of({random, To(Kind::kJump, 0)}), kGba) == 0);

    // On the DS, a note waits while its track's note wait is on. A track starts with it on. On the GBA, a note never
    // waits.
    const Sequence notes = Of({Plain(Kind::kNote), To(Kind::kJump, 0)});
    XSFLIB_CHECK(CommandsWithoutWayOut(notes, kDs) == 0);
    XSFLIB_CHECK(CommandsWithoutWayOut(notes, kGba) == 2);
    const Sequence off = Of({Plain(Kind::kNoteWait, 0), Plain(Kind::kNote), To(Kind::kJump, 1)});
    XSFLIB_CHECK(CommandsWithoutWayOut(off, kDs) == 3);
    const Sequence on_again =
        Of({Plain(Kind::kNoteWait, 0), Plain(Kind::kNoteWait, 1), Plain(Kind::kNote), To(Kind::kJump, 2)});
    XSFLIB_CHECK(CommandsWithoutWayOut(on_again, kDs) == 0);

    // A call comes back from its subroutine. A wait in the subroutine is a way out, and a subroutine without one
    // leaves the loop without one.
    const Sequence calls = Of({To(Kind::kCall, 2), To(Kind::kJump, 0), Plain(Kind::kReturn)});
    XSFLIB_CHECK(CommandsWithoutWayOut(calls, kDs) == 3);
    const Sequence waiting_call =
        Of({To(Kind::kCall, 2), To(Kind::kJump, 0), Plain(Kind::kWait), Plain(Kind::kReturn)});
    XSFLIB_CHECK(CommandsWithoutWayOut(waiting_call, kDs) == 0);

    // A comparison may leave the loop. So may a conditional jump, or a jump past the bytes.
    Command conditional = To(Kind::kJump, 3);
    conditional.conditional = true;
    const Sequence compares = Of({conditional, To(Kind::kJump, 0), Plain(Kind::kEnd), Plain(Kind::kWait)});
    XSFLIB_CHECK(CommandsWithoutWayOut(compares, kDs) == 0);
    Command outside = To(Kind::kJump, 0x1000);
    outside.target_outside = true;
    XSFLIB_CHECK(CommandsWithoutWayOut(Of({Plain(Kind::kVolume), outside}), kGba) == 0);

    // MP2K's comparisons and repeats jump to their targets.
    const Sequence if_equal =
        Of({To(Kind::kJumpIfEqual, 2), To(Kind::kJump, 0), Plain(Kind::kWait), Plain(Kind::kEnd)});
    XSFLIB_CHECK(CommandsWithoutWayOut(if_equal, kGba) == 0);
    const Sequence repeats = Of({Plain(Kind::kVolume), To(Kind::kRepeat, 0), Plain(Kind::kEnd)});
    XSFLIB_CHECK(CommandsWithoutWayOut(repeats, kGba) == 0);
    // A repeat's way out is at its target: the commands after it loop without a wait.
    const Sequence repeats_to_wait =
        Of({Plain(Kind::kWait), Plain(Kind::kVolume), To(Kind::kRepeat, 0), To(Kind::kJump, 1)});
    XSFLIB_CHECK(CommandsWithoutWayOut(repeats_to_wait, kGba) == 0);

    // A return without a call runs on past it, here into a loop without a wait.
    const Sequence runs_on = Of({Plain(Kind::kReturn), Plain(Kind::kWait), To(Kind::kJump, 0)});
    XSFLIB_CHECK(CommandsWithoutWayOut(runs_on, kDs) == 0);
    const Sequence runs_into_loop = Of({Plain(Kind::kReturn), Plain(Kind::kVolume), To(Kind::kJump, 0)});
    XSFLIB_CHECK(CommandsWithoutWayOut(runs_into_loop, kDs) == 3);

    // Bytes that no command reaches don't count.
    Command data = Plain(Kind::kData);
    data.bytes = {0x00};
    XSFLIB_CHECK(CommandsWithoutWayOut(Of({Plain(Kind::kWait), To(Kind::kJump, 0), data}), kDs) == 0);
}

// A DS archive of one sequence, with a bank and a wave archive.
std::vector<uint8_t> DsArchiveOf(const files::Bytes& commands)
{
    const files::Wave square{
        .loop = 1,
        .loop_length = 4,
        .data = {0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0}};
    sdat::ArchiveModel model;
    model.sequences = {sdat::SequenceInfo{0, 0, 127, 64, 64, 0, 0, ""}};
    model.banks = {sdat::BankInfo{1, {0, kNone, kNone, kNone}, ""}};
    model.wave_archives = {sdat::WaveArchiveInfo{2, ""}};
    model.players = {sdat::PlayerInfo{1, 0, 0, 0, ""}};
    model.files = {files::Sequence(commands), files::Bank({{.type = 1, .region = {.wave = 0}}}),
                   files::WaveArchive({square})};
    return sdat::BuildArchive(model);
}

// Whether `call` throws std::invalid_argument.
template <typename Call>
bool Refuses(Call&& call)
{
    return !ErrorOf<std::invalid_argument>(call).empty();
}

// Runs `model` for `frames` frames at most, and returns the frames that it ran.
int Run(hokuto::song::SequenceModel& model, int frames)
{
    int frame = 0;
    for (bool more = model.SeekFrame(0); more && frame < frames; more = model.RunFrame())
    {
        frame++;
    }

    return frame;
}

// An edit of a DS sequence that leaves a loop without a wait is refused, and one of a sequence that already contains
// such a loop isn't refused for it.
void TestDs()
{
    //    0: 3C 7F 01        note 60, velocity 127, 1 tick
    //    3: C1 40           volume 64
    //    5: 80 18           wait 24 ticks
    //    7: 94 03 00 00     jump to 3
    const files::Bytes waits = {0x3C, 0x7F, 0x01, 0xC1, 0x40, 0x80, 0x18, 0x94, 0x03, 0x00, 0x00};
    const std::vector<uint8_t> bytes = DsArchiveOf(waits);
    const File file = hokuto::format::ImportDsArchive(bytes);
    const std::shared_ptr<const hokuto::song::DsArchive> archive = hokuto::song::LoadDsArchive(bytes);
    const hokuto::song::Sequence read = hokuto::song::ReadSequence(*archive, 0);
    SEQLIB_REQUIRE(!read.tracks.empty() && !read.tracks[0].stretches.empty());
    const hokuto::song::Stretch stretch = read.tracks[0].stretches.back();

    // Without its wait, the loop has no way out. Without its volume, it still waits.
    const std::vector<CommandPlace> wait = {CommandPlace{5, stretch}};
    const std::vector<CommandPlace> volume = {CommandPlace{3, stretch}};
    const File hanging = hokuto::edit::RemoveCommands(file, 0, wait).file;
    const File still_waits = hokuto::edit::RemoveCommands(file, 0, volume).file;
    XSFLIB_CHECK(Refuses([&] { RefuseLoopsWithoutWaits(file, hanging); }));
    XSFLIB_CHECK(!Refuses([&] { RefuseLoopsWithoutWaits(file, still_waits); }));
    XSFLIB_CHECK(!Refuses([&] { RefuseLoopsWithoutWaits(file, file); }));

    // An edit of a sequence that already has such a loop isn't refused for it: the new version has no more commands
    // without a way out. A version that shares the sequence isn't looked at. The new version therefore contains a copy.
    File again = hanging;
    again.sequences[0] = Sequence(*hanging.sequences[0]);
    XSFLIB_CHECK(!Refuses([&] { RefuseLoopsWithoutWaits(hanging, again); }));

    // The driver's model runs the loop up to its budget of commands. The read of such a sequence fails with a message.
    //    0: C1 40           volume 64
    //    2: 94 00 00 00     jump to 0
    const files::Bytes loops = {0xC1, 0x40, 0x94, 0x00, 0x00, 0x00};
    const std::vector<uint8_t> hanging_bytes = DsArchiveOf(loops);
    const std::shared_ptr<const hokuto::song::DsArchive> hanging_archive = hokuto::song::LoadDsArchive(hanging_bytes);
    const std::string error = ErrorOf([&] { hokuto::song::ReadSequence(*hanging_archive, 0); });
    XSFLIB_CHECK(error.find("Can't read") != std::string::npos);
    XSFLIB_CHECK(error.find("hangs") != std::string::npos);

    // A model that meets such a loop stops in its first frames, with the reason.
    hokuto::song::SequenceModel model(hanging_archive, std::make_shared<const hokuto::song::Sequence>(read));
    XSFLIB_CHECK(Run(model, 400) < 400);
    XSFLIB_CHECK(model.Failure().find("hangs") != std::string::npos);

    // The model of a sequence that waits returns no reason.
    hokuto::song::SequenceModel fine(archive, std::make_shared<const hokuto::song::Sequence>(read));
    XSFLIB_CHECK(Run(fine, 40) == 40);
    XSFLIB_CHECK(fine.Failure().empty());
}

// A cartridge of one song with a track that loops over a volume. With `waits`, the loop waits 24 ticks.
std::vector<uint8_t> CartOf(bool waits)
{
    using enum mp2k::Command;

    mp2k_cart::Cart cart(mp2k_cart::kMode, {10});
    std::vector<int8_t> saw(64);
    for (std::size_t i = 0; i < saw.size(); ++i)
    {
        const int at = static_cast<int>(i);
        saw[i] = static_cast<int8_t>((at < 32 ? at : at - 64) * 4);
    }
    const uint32_t sample = cart.Wave(saw, 13379, 0);
    const uint32_t voices = cart.VoiceGroup({{0, mp2k_cart::Cart::Voice(0, sample, 60, {255, 0, 255, 165})}});
    mp2k_cart::Track track;
    track.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0);
    const std::size_t loop = track.Here();
    track.Cmd(kCmdVol, 100);
    if (waits)
    {
        track.Wait(24);
    }
    track.Goto(loop);
    cart.Song({track}, voices);

    const ntdlib::agb::Rom rom = cart.ToRom();
    return hokuto::testing::RomBytes(rom);
}

// An edit of a GBA song that leaves a loop without a wait is refused. ntdlib's sequencer stops such a song at its
// budget of commands, and the read and the model fail with a message.
void TestGba()
{
    const std::vector<uint8_t> bytes = CartOf(true);
    const File file = hokuto::format::ImportGbaRom(bytes);
    const std::shared_ptr<const hokuto::song::GbaRom> rom = hokuto::song::LoadGbaRom(bytes);
    const hokuto::song::Sequence read = hokuto::song::ReadSequence(*rom, 0);
    SEQLIB_REQUIRE(!read.tracks.empty() && !read.tracks[0].stretches.empty());

    // The wait's offset: past the tempo, the voice and the volume, each of two bytes.
    const hokuto::song::Stretch stretch = read.tracks[0].stretches.back();
    const std::vector<CommandPlace> wait = {CommandPlace{6, stretch}};
    const File hanging = hokuto::edit::RemoveCommands(file, 0, wait).file;
    XSFLIB_CHECK(Refuses([&] { RefuseLoopsWithoutWaits(file, hanging); }));
    XSFLIB_CHECK(!Refuses([&] { RefuseLoopsWithoutWaits(file, file); }));

    // ntdlib's sequencer stops at its budget of commands. The read fails with a message, and so does a model.
    const std::vector<uint8_t> hanging_bytes = CartOf(false);
    const std::shared_ptr<const hokuto::song::GbaRom> hanging_rom = hokuto::song::LoadGbaRom(hanging_bytes);
    const std::string error = ErrorOf([&] { hokuto::song::ReadSequence(*hanging_rom, 0); });
    XSFLIB_CHECK(error.find("hangs") != std::string::npos);

    hokuto::song::SequenceModel model(hanging_rom, std::make_shared<const hokuto::song::Sequence>(read));
    XSFLIB_CHECK(Run(model, 400) < 400);
    XSFLIB_CHECK(model.Failure().find("hangs") != std::string::npos);
}

// A command of DSE that the common form has no kind for.
Command DseOther(uint16_t opcode)
{
    Command command = Plain(Kind::kOther);
    command.encoding.opcode = opcode;
    return command;
}

// On DSE, the end of a track jumps back to its last loop point, and the commands from the last wait on have no way out.
// The counts follow from each track's items.
void TestDseCommandsWithoutWayOut()
{
    const Command start = Plain(Kind::kData);
    const Command end = Plain(Kind::kEnd);
    const Command loop = DseOther(0x99);
    const Command volume = Plain(Kind::kVolume);

    // A track without a loop point ends, and a loop with a wait waits.
    XSFLIB_CHECK(CommandsWithoutWayOut(Of({start, volume, end}), kDse) == 0);
    XSFLIB_CHECK(CommandsWithoutWayOut(Of({start, loop, Plain(Kind::kWait), end}), kDse) == 0);

    // The commands that lead into the loop count from the last wait.
    XSFLIB_CHECK(CommandsWithoutWayOut(Of({start, volume, loop, volume, end}), kDse) == 4);
    XSFLIB_CHECK(CommandsWithoutWayOut(Of({start, Plain(Kind::kWait), volume, loop, end}), kDse) == 3);

    // A wait of no ticks and a note don't wait.
    XSFLIB_CHECK(CommandsWithoutWayOut(Of({start, loop, Plain(Kind::kWait, 0), end}), kDse) == 3);
    XSFLIB_CHECK(CommandsWithoutWayOut(Of({start, loop, Plain(Kind::kNote), end}), kDse) == 3);

    // A later loop point replaces an earlier one, past the wait between them.
    XSFLIB_CHECK(CommandsWithoutWayOut(Of({start, loop, Plain(Kind::kWait), loop, end}), kDse) == 2);

    // A wait for the channel's voices counts as a way out. An end with an invalid code stops the track.
    XSFLIB_CHECK(CommandsWithoutWayOut(Of({start, loop, DseOther(0x95), end}), kDse) == 0);
    Command invalid = end;
    invalid.encoding.opcode = 0x9A;
    XSFLIB_CHECK(CommandsWithoutWayOut(Of({start, loop, invalid}), kDse) == 0);

    // Each track counts apart, and a track's loop point doesn't reach the next track.
    XSFLIB_CHECK(CommandsWithoutWayOut(Of({start, loop, end, start, volume, end}), kDse) == 2);
}

// An image with one song. Its second track contains a volume, a loop point, a note and a wait of 24 ticks when `waits`,
// in the loop.
std::vector<uint8_t> DseImageOf(bool waits)
{
    namespace files = ntdlib::testing::dse_song_files;
    namespace nds_files = ntdlib::testing::nds_files;
    const files::Bytes conductor = {0, 0, 0, 0, 0xA4, 0x78, 0x92, 0x30, 0x98};
    const files::Bytes wait = waits ? files::Bytes{0x92, 0x18} : files::Bytes{};
    const files::Bytes track = files::Join({{1, 1, 0, 0},
                                            {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x64, 0x99},
                                            files::Note(0x7F, 0, 0, 0x30, 1),
                                            wait,
                                            {0x98}});
    nds_files::Image image;
    image.files = {{"SOUND/BGM/bgm.swd", files::MainBankFile()},
                   {"SOUND/BGM/bgm0001.smd", files::SongFile({conductor, track})},
                   {"SOUND/BGM/bgm0001.swd", files::BankFile()}};
    return nds_files::BuildImage(image).first;
}

// An edit of a DSE song that leaves a loop without a wait is refused, and one that keeps the wait isn't. ntdlib's
// driver stops such a song at its budget of commands, and the read and the model fail with the reason.
void TestDse()
{
    const File file = hokuto::format::ImportDseImage(DseImageOf(true));
    const std::vector<uint8_t> set_bytes = hokuto::format::ExportArchive(file);
    const std::shared_ptr<const hokuto::song::DseSet> set =
        hokuto::song::LoadDseSet(set_bytes, ntdlib::dse::Build::kLater);
    const hokuto::song::Sequence read = hokuto::song::ReadSequence(*set, 0);

    // Without its wait, the loop has no way out. An edit that leaves the wait isn't refused.
    const Sequence& commands = *file.sequences.at(0);
    const std::vector<uint32_t> offsets = hokuto::edit::Offsets(commands, hokuto::edit::ConsoleOf(file));
    std::size_t wait = 0;
    std::size_t volume = 0;
    for (std::size_t item = 0; item < commands.items.size(); ++item)
    {
        if (commands.items[item].kind == Kind::kWait)
        {
            wait = item;
        }
        if (commands.items[item].kind == Kind::kVolume)
        {
            volume = item;
        }
    }
    SEQLIB_REQUIRE(wait != 0 && volume != 0);
    const CommandPlace wait_place{offsets[wait], {}};
    const CommandPlace volume_place{offsets[volume], {}};
    const File hanging = hokuto::edit::RemoveCommands(file, 0, {&wait_place, 1}).file;
    const File still_waits = hokuto::edit::RemoveCommands(file, 0, {&volume_place, 1}).file;
    XSFLIB_CHECK(CommandsWithoutWayOut(*file.sequences.at(0), kDse) == 0);
    XSFLIB_CHECK(Refuses([&] { RefuseLoopsWithoutWaits(file, hanging); }));
    XSFLIB_CHECK(!Refuses([&] { RefuseLoopsWithoutWaits(file, still_waits); }));
    File again = hanging;
    again.sequences[0] = Sequence(*hanging.sequences[0]);
    XSFLIB_CHECK(!Refuses([&] { RefuseLoopsWithoutWaits(hanging, again); }));

    // ntdlib's driver stops at its budget of commands. The read fails with a message, and a model stops with the
    // reason.
    const std::vector<uint8_t> hanging_bytes =
        hokuto::format::ExportArchive(hokuto::format::ImportDseImage(DseImageOf(false)));
    const std::shared_ptr<const hokuto::song::DseSet> hanging_set =
        hokuto::song::LoadDseSet(hanging_bytes, ntdlib::dse::Build::kLater);
    const std::string error = ErrorOf([&] { hokuto::song::ReadSequence(*hanging_set, 0); });
    XSFLIB_CHECK(error.find("Can't read") != std::string::npos);
    XSFLIB_CHECK(error.find("hangs") != std::string::npos);

    hokuto::song::SequenceModel model(hanging_set, std::make_shared<const hokuto::song::Sequence>(read));
    XSFLIB_CHECK(Run(model, 400) < 400);
    XSFLIB_CHECK(model.Failure().find("hangs") != std::string::npos);

    hokuto::song::SequenceModel fine(set, std::make_shared<const hokuto::song::Sequence>(read));
    XSFLIB_CHECK(Run(fine, 40) == 40);
    XSFLIB_CHECK(fine.Failure().empty());
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("hangs",
                                    []
                                    {
                                        TestCommandsWithoutWayOut();
                                        TestDs();
                                        TestGba();
                                        TestDseCommandsWithoutWayOut();
                                        TestDse();
                                    });
}
