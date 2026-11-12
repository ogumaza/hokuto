// SPDX-License-Identifier: MIT

// Tests the edits of a GBA song's values (src/edit/gba_tables.h). The song table of MP2K has 8 bytes for each song, and
// the header of a song contains the priority and the reverb in its third and fourth bytes. An edit has to change those
// bytes and no others, and ntdlib's reader has to read the new values from the exported ROM. A song that moves keeps
// its new values, and a reverb that the song sets has to change what the model plays.

#include "edit/gba_tables.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/mp2k/archive_player.h"
#include "ntdlib/mp2k/song.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/testing/mp2k_cart.h"
#include "xsflib/testing/check.h"

#include "cart_bytes.h"
#include "format/archive.h"
#include "format/file.h"
#include "format/text.h"
#include "song/gba.h"

namespace
{

namespace mp2k = ntdlib::mp2k;
namespace mp2k_cart = ntdlib::testing::mp2k_cart;
using hokuto::edit::ChangeGbaSong;
using hokuto::edit::SoundChange;
using hokuto::edit::SoundValue;
using hokuto::format::File;
using xsflib::testing::ErrorOf;

constexpr uint32_t kRomBase = mp2k_cart::kRomBase;
constexpr uint32_t kSongTable = mp2k_cart::kSongTable;

// A cartridge of three songs on two music players, then room after its data. Songs 0 and 1 play a held sample. Song 2
// names song 0's header, on the second music player.
struct Cart
{
    std::vector<uint8_t> bytes;
    uint32_t header = 0;
};

Cart MakeCart(std::vector<int> players = {10, 3})
{
    using enum mp2k::Command;

    mp2k_cart::Cart cart(mp2k_cart::kMode, std::move(players));
    std::vector<int8_t> saw(64);
    for (std::size_t i = 0; i < saw.size(); ++i)
    {
        const int at = static_cast<int>(i);
        saw[i] = static_cast<int8_t>((at < 32 ? at : at - 64) * 4);
    }
    const uint32_t sample = cart.Wave(saw, 13379, 0);
    const uint32_t voices = cart.VoiceGroup({{0, mp2k_cart::Cart::Voice(0, sample, 60, {255, 0, 255, 165})}});
    mp2k_cart::Track track;
    track.Cmd(kCmdTempo, 75).Cmd(kCmdVol, 100).Cmd(kCmdVoice, 0);
    track.Note(48, 60, 100).Wait(48);
    track.Note(48, 64, 100).Wait(48);
    track.Fine();

    Cart made;
    made.header = cart.Song({track}, voices);
    cart.Song({track}, voices);
    cart.Song({track}, voices, 1);

    const ntdlib::agb::Rom rom = cart.ToRom();
    made.bytes = hokuto::testing::RomBytes(rom);
    const uint32_t entry = kSongTable + 16 - kRomBase;
    for (uint32_t i = 0; i < 4; i++)
    {
        made.bytes[entry + i] = static_cast<uint8_t>(made.header >> (i * 8));
    }
    made.bytes.resize(made.bytes.size() + 0x4000, 0xFF);
    return made;
}

// The positions where two ROMs of one size differ, from the ROM's start.
std::vector<std::size_t> DifferencesOf(const std::vector<uint8_t>& before, const std::vector<uint8_t>& after)
{
    XSFLIB_CHECK(before.size() == after.size());
    std::vector<std::size_t> places;
    for (std::size_t i = 0; i < std::min(before.size(), after.size()); i++)
    {
        if (before[i] != after[i])
        {
            places.push_back(i);
        }
    }
    return places;
}

mp2k::SongHeader HeaderOf(const std::vector<uint8_t>& bytes, int song)
{
    const std::shared_ptr<const hokuto::song::GbaRom> rom = hokuto::song::LoadGbaRom(bytes);
    mp2k::SongHeader header;
    XSFLIB_CHECK(mp2k::ReadSong(rom->rom, rom->info, song, header));
    return header;
}

std::vector<int16_t> Played(const std::vector<uint8_t>& bytes, int song)
{
    const std::shared_ptr<const hokuto::song::GbaRom> rom = hokuto::song::LoadGbaRom(bytes);
    mp2k::ArchivePlayer player(rom->rom, rom->info);
    player.Start(song, mp2k::ArchivePlayerOptions{});
    std::vector<int16_t> samples;
    while (samples.size() < 40000)
    {
        player.RunFrame(samples);
    }
    samples.resize(40000);
    return samples;
}

void CheckForms(const File& file)
{
    const std::vector<uint8_t> rom = hokuto::format::ExportArchive(file);
    XSFLIB_CHECK(hokuto::format::ExportArchive(hokuto::format::Read(hokuto::format::Write(file))) == rom);
    XSFLIB_CHECK(hokuto::format::ExportArchive(hokuto::format::ReadText(hokuto::format::WriteText(file))) == rom);
}

SoundChange ChangeOf(SoundValue value, uint32_t to)
{
    SoundChange change;
    change.value = value;
    change.to = to;
    return change;
}

// The table values of a GBA song: its name, player and priority, the player of a later song, a refusal for a song past
// the table, the players' names and the values that the GBA keeps fixed.
void TestValues()
{
    const Cart cart = MakeCart();
    const std::shared_ptr<const hokuto::song::GbaRom> rom = hokuto::song::LoadGbaRom(cart.bytes);

    const hokuto::format::NativeSound first = hokuto::edit::GbaNativeSoundOf(*rom, 0);
    XSFLIB_CHECK(first.name == hokuto::song::GbaSongName(*rom, 0));
    XSFLIB_CHECK(first.player == ntdlib::sar::ItemId(ntdlib::sar::kPlayerItem, 0));
    XSFLIB_CHECK(first.player_priority == 0 && first.reverb == 0);
    XSFLIB_CHECK(hokuto::edit::GbaNativeSoundOf(*rom, 2).player == ntdlib::sar::ItemId(ntdlib::sar::kPlayerItem, 1));
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::GbaNativeSoundOf(*rom, 3); }).find("song 3") !=
                 std::string::npos);

    const std::vector<std::string> names = hokuto::edit::GbaPlayerNames(*rom);
    XSFLIB_CHECK((names == std::vector<std::string>{"0 (10 tracks)", "1 (3 tracks)"}));

    const hokuto::edit::HeldValues held = hokuto::edit::GbaHeldValues();
    XSFLIB_CHECK(held.highest_priority == 255 && !held.pan && !held.tracks && !held.actor_player);
}

// A new music player gets both numbers of the entry. A new priority and a new reverb take their bytes in the header,
// and every number that names the header follows.
void TestEdits()
{
    const Cart cart = MakeCart();
    const File file = hokuto::format::ImportGbaRom(cart.bytes);
    const std::shared_ptr<const hokuto::song::GbaRom> rom = hokuto::song::LoadGbaRom(cart.bytes);
    XSFLIB_CHECK(hokuto::format::ExportArchive(file) == cart.bytes);

    const uint32_t player = ntdlib::sar::ItemId(ntdlib::sar::kPlayerItem, 1);
    const File moved = ChangeGbaSong(file, *rom, 1, ChangeOf(SoundValue::kPlayer, player));
    const std::vector<uint8_t> moved_rom = hokuto::format::ExportArchive(moved);
    const std::size_t entry = kSongTable - kRomBase + 8 + 4;
    XSFLIB_CHECK((DifferencesOf(cart.bytes, moved_rom) == std::vector<std::size_t>{entry, entry + 2}));
    XSFLIB_CHECK(moved_rom[entry] == 1 && moved_rom[entry + 2] == 1);
    CheckForms(moved);

    const File back = ChangeGbaSong(moved, *hokuto::song::LoadGbaRom(moved_rom), 1,
                                    ChangeOf(SoundValue::kPlayer, ntdlib::sar::ItemId(ntdlib::sar::kPlayerItem, 0)));
    XSFLIB_CHECK(hokuto::format::ExportArchive(back) == cart.bytes);

    const File priority = ChangeGbaSong(file, *rom, 0, ChangeOf(SoundValue::kPlayerPriority, 200));
    const std::vector<uint8_t> priority_rom = hokuto::format::ExportArchive(priority);
    XSFLIB_CHECK((DifferencesOf(cart.bytes, priority_rom) == std::vector<std::size_t>{cart.header - kRomBase + 2}));
    XSFLIB_CHECK(HeaderOf(priority_rom, 0).priority == 200);
    XSFLIB_CHECK(HeaderOf(priority_rom, 2).priority == 200);
    XSFLIB_CHECK(HeaderOf(priority_rom, 1).priority == 0);
    CheckForms(priority);

    const uint32_t reverb_on = hokuto::edit::kGbaReverbOn | 100;
    const File reverb = ChangeGbaSong(file, *rom, 2, ChangeOf(SoundValue::kReverb, reverb_on));
    const std::vector<uint8_t> reverb_rom = hokuto::format::ExportArchive(reverb);
    XSFLIB_CHECK((DifferencesOf(cart.bytes, reverb_rom) == std::vector<std::size_t>{cart.header - kRomBase + 3}));
    XSFLIB_CHECK(HeaderOf(reverb_rom, 0).reverb == reverb_on);
    XSFLIB_CHECK(hokuto::edit::GbaNativeSoundOf(*hokuto::song::LoadGbaRom(reverb_rom), 0).reverb == reverb_on);
    XSFLIB_CHECK(Played(reverb_rom, 0) != Played(cart.bytes, 0));
    XSFLIB_CHECK(Played(reverb_rom, 1) == Played(cart.bytes, 1));

    // A level without the bit leaves the driver's reverb.
    const File level = ChangeGbaSong(file, *rom, 0, ChangeOf(SoundValue::kReverb, 100));
    XSFLIB_CHECK(Played(hokuto::format::ExportArchive(level), 0) == Played(cart.bytes, 0));

    XSFLIB_CHECK(
        ErrorOf<std::invalid_argument>([&] { ChangeGbaSong(file, *rom, 0, ChangeOf(SoundValue::kReverb, 256)); }) ==
        "The reverb of a GBA song must be from 0 to 255.");
    XSFLIB_CHECK(
        ErrorOf<std::invalid_argument>([&] { ChangeGbaSong(file, *rom, 0, ChangeOf(SoundValue::kVolume, 100)); }) ==
        "GBA songs have no volume setting.");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>(
                     [&]
                     {
                         ChangeGbaSong(file, *rom, 0,
                                       ChangeOf(SoundValue::kPlayer, ntdlib::sar::ItemId(ntdlib::sar::kPlayerItem, 2)));
                     }) == "The driver has no such music player.");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { ChangeGbaSong(file, *rom, 3, ChangeOf(SoundValue::kReverb, 1)); })
                     .find("song 3") != std::string::npos);
}

// A music player without tracks plays no song, and accepts none. An entry of zeros between two others in the table of
// music players is such a player.
void TestPlayerWithoutTracks()
{
    Cart cart = MakeCart({10, 3, 1, 3});
    const auto entry = static_cast<std::ptrdiff_t>(mp2k_cart::kPlayerTable - kRomBase + 2 * 12);
    std::fill_n(cart.bytes.begin() + entry, 12, uint8_t{0});
    const File file = hokuto::format::ImportGbaRom(cart.bytes);
    const std::shared_ptr<const hokuto::song::GbaRom> rom = hokuto::song::LoadGbaRom(cart.bytes);
    SEQLIB_REQUIRE(rom->info.players.size() == 4 && rom->info.players[2].track_count == 0);
    XSFLIB_CHECK(hokuto::edit::GbaPlayerNames(*rom)[2] == "2 (0 tracks)");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>(
                     [&]
                     {
                         ChangeGbaSong(file, *rom, 0,
                                       ChangeOf(SoundValue::kPlayer, ntdlib::sar::ItemId(ntdlib::sar::kPlayerItem, 2)));
                     }) == "Music player 2 has no tracks.");
}

// A song that outgrows its place moves its header with it, with the new values.
void TestMovedSong()
{
    const Cart cart = MakeCart();
    File file = hokuto::format::ImportGbaRom(cart.bytes);
    const std::optional<uint32_t> sequence =
        hokuto::format::GbaSongSequence(file, hokuto::song::LoadGbaRom(cart.bytes)->info.song_table, 0);
    SEQLIB_REQUIRE(sequence.has_value());
    file.sequences[*sequence].Edit([](auto& edited)
                                   { edited.items.insert(edited.items.begin() + 1, 40, edited.items[0]); });
    XSFLIB_CHECK(hokuto::format::MovedSongs(file) == std::vector<uint32_t>{*sequence});

    const std::vector<uint8_t> grown = hokuto::format::ExportArchive(file);
    const std::shared_ptr<const hokuto::song::GbaRom> rom = hokuto::song::LoadGbaRom(grown);
    XSFLIB_CHECK(HeaderOf(grown, 0).address != cart.header);

    const File edited = ChangeGbaSong(file, *rom, 0, ChangeOf(SoundValue::kPlayerPriority, 33));
    const File reverb = ChangeGbaSong(edited, *rom, 0, ChangeOf(SoundValue::kReverb, hokuto::edit::kGbaReverbOn | 9));
    const std::vector<uint8_t> built = hokuto::format::ExportArchive(reverb);
    XSFLIB_CHECK(HeaderOf(built, 0).priority == 33);
    XSFLIB_CHECK(HeaderOf(built, 0).reverb == (hokuto::edit::kGbaReverbOn | 9));
    XSFLIB_CHECK(HeaderOf(built, 0).address == HeaderOf(grown, 0).address);
    XSFLIB_CHECK(HeaderOf(built, 1).priority == 0);
    CheckForms(reverb);
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("gba_tables",
                                    []
                                    {
                                        TestValues();
                                        TestEdits();
                                        TestPlayerWithoutTracks();
                                        TestMovedSong();
                                    });
}
