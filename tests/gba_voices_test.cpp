// SPDX-License-Identifier: MIT

// Tests the new data of a GBA ROM's voices (src/edit/gba_voices.h). A sample's bytes have to hold MP2K's header and the
// samples rounded to 8 bits, worked out by hand. New bytes have to start where ntdlib::mp2k::Join finds its free space,
// and a song that moves has to go after them. A voice has to change its kind. A new voice in a table with other data
// after it has to move the table: each song's header names the new table, each song plays as before, and the voice
// plays from the new table. A key split and a drum kit have to play through their new tables. A WAV file's sample has
// to play as the same samples do in a cartridge that ntdlib's test code lays out. Each version has to come back from
// both forms of the file.

#include "edit/gba_voices.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/mp2k/archive_player.h"
#include "ntdlib/mp2k/layout.h"
#include "ntdlib/mp2k/song.h"
#include "ntdlib/testing/mp2k_cart.h"
#include "seqlib/midi/builder.h"
#include "seqlib/wav.h"
#include "xsflib/testing/check.h"

#include "cart_bytes.h"
#include "edit/files.h"
#include "edit/gba_midi.h"
#include "format/archive.h"
#include "format/file.h"
#include "format/text.h"
#include "song/gba.h"

namespace
{

namespace mp2k = ntdlib::mp2k;
namespace mp2k_cart = ntdlib::testing::mp2k_cart;
using hokuto::format::File;
using xsflib::testing::ErrorOf;

constexpr uint32_t kRomBase = mp2k_cart::kRomBase;

// A cartridge of one song: two notes on a voice group with a sample at program 0 and a square wave at program 1, then
// `tail` bytes of 0xFF. The song's bytes follow the voice group. As in a game's shorter table, a short table contains
// those two entries, and data that plays nothing follows them. The sample's first byte is 0, and so is the byte after
// it: a zero between the sample and the voice group. A mixer that interpolates past the sample's last byte reads what a
// loop puts there, whatever the voice of the program contains.
struct SmallCart
{
    std::vector<uint8_t> bytes;
    uint32_t voices = 0;
    uint32_t sample = 0;
    uint32_t header = 0;
    std::size_t before_tail = 0;
};

std::vector<int8_t> Saw()
{
    std::vector<int8_t> saw(64);
    for (std::size_t i = 0; i < saw.size(); ++i)
    {
        const int at = static_cast<int>(i);
        saw[i] = static_cast<int8_t>((at < 32 ? at : at - 64) * 4);
    }
    return saw;
}

SmallCart MakeCart(std::size_t tail, const std::vector<int8_t>& pcm = Saw(), bool short_table = false)
{
    using mp2k_cart::Cart;
    using enum mp2k::Command;

    Cart cart;
    SmallCart made;
    made.sample = cart.Wave(pcm, 13379, 0);
    cart.Place({0, 0, 0, 0});
    const std::vector<uint8_t> sample_voice = Cart::Voice(0, made.sample, 60, {255, 0, 255, 165});
    const std::vector<uint8_t> square_voice = Cart::Voice(2, 2, 60, {0, 0, 15, 0});
    if (short_table)
    {
        std::vector<uint8_t> table = sample_voice;
        table.insert(table.end(), square_voice.begin(), square_voice.end());
        table.resize(128 * 12, 0x30);
        made.voices = cart.Place(table);
    }
    else
    {
        made.voices = cart.VoiceGroup({{0, sample_voice}, {1, square_voice}});
    }
    mp2k_cart::Track track;
    track.Cmd(kCmdTempo, 75).Cmd(kCmdVol, 100);
    track.Cmd(kCmdVoice, 0).Note(24, 60, 100).Wait(24);
    track.Cmd(kCmdVoice, 1).Note(24, 60, 100).Wait(24);
    track.Fine();
    made.header = cart.Song({track}, made.voices);

    const ntdlib::agb::Rom rom = cart.ToRom();
    made.bytes = hokuto::testing::RomBytes(rom);
    made.before_tail = made.bytes.size();
    made.bytes.resize(made.bytes.size() + tail, 0xFF);
    return made;
}

// `count` stereo samples of the ROM that `file` builds, playing its first song on ntdlib's player.
std::vector<int16_t> Played(const File& file, std::size_t count, bool sounds = true)
{
    const std::shared_ptr<const hokuto::song::GbaRom> rom =
        hokuto::song::LoadGbaRom(hokuto::format::ExportArchive(file));
    mp2k::ArchivePlayer player(rom->rom, rom->info);
    player.Start(hokuto::song::GbaSongs(*rom).front(), mp2k::ArchivePlayerOptions{});
    std::vector<int16_t> samples;
    while (samples.size() < count * 2)
    {
        player.RunFrame(samples);
    }
    samples.resize(count * 2);
    XSFLIB_CHECK(!sounds ||
                 std::ranges::any_of(samples, [](int16_t sample) { return sample > 1000 || sample < -1000; }));
    return samples;
}

// The bytes of the ROM that `file` builds, from `address`.
std::vector<uint8_t> BytesAt(const File& file, uint32_t address, std::size_t count)
{
    const std::vector<uint8_t> rom = hokuto::format::ExportArchive(file);
    const std::size_t at = address - kRomBase;
    if (at + count > rom.size())
    {
        XSFLIB_CHECK(!"the bytes are in the ROM");
        return {};
    }
    return std::vector<uint8_t>(rom.begin() + static_cast<std::ptrdiff_t>(at),
                                rom.begin() + static_cast<std::ptrdiff_t>(at + count));
}

uint32_t Word(const std::vector<uint8_t>& bytes, std::size_t at)
{
    return uint32_t{bytes[at]} | uint32_t{bytes[at + 1]} << 8 | uint32_t{bytes[at + 2]} << 16 |
           uint32_t{bytes[at + 3]} << 24;
}

// `file` has to come back from both of its forms.
void CheckForms(const File& file)
{
    const std::vector<uint8_t> rom = hokuto::format::ExportArchive(file);
    XSFLIB_CHECK(hokuto::format::ExportArchive(hokuto::format::Read(hokuto::format::Write(file))) == rom);
    XSFLIB_CHECK(hokuto::format::ExportArchive(hokuto::format::ReadText(hokuto::format::WriteText(file))) == rom);
}

// Seven samples worked out by hand: each sample is (value + 128) >> 8, within -128 to 127. The rate of 8,000 Hz is
// 0x7D0000 in 1024ths. The byte after the samples repeats the loop's first sample, or else the last sample.
void TestSampleBytes()
{
    using hokuto::edit::EncodeGbaSample;

    const std::vector<int16_t> samples = {0, 256, -256, 32767, -32768, 127, 128};
    const std::vector<uint8_t> looped = {0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x7D, 0x00, 0x02, 0x00, 0x00, 0x00,
                                         0x07, 0x00, 0x00, 0x00, 0x00, 0x01, 0xFF, 0x7F, 0x80, 0x00, 0x01, 0xFF};
    XSFLIB_CHECK(EncodeGbaSample(samples, 8000, 2) == looped);
    const std::vector<uint8_t> once = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7D, 0x00, 0x00, 0x00, 0x00, 0x00,
                                       0x07, 0x00, 0x00, 0x00, 0x00, 0x01, 0xFF, 0x7F, 0x80, 0x00, 0x01, 0x01};
    XSFLIB_CHECK(EncodeGbaSample(samples, 8000, std::nullopt) == once);

    const auto refusal = [](std::span<const int16_t> given, uint32_t rate, std::optional<uint32_t> loop)
    {
        return ErrorOf<std::invalid_argument>([&] { EncodeGbaSample(given, rate, loop); });
    };
    XSFLIB_CHECK(refusal({}, 8000, std::nullopt) == "A sample must contain at least one sample point.");
    XSFLIB_CHECK(refusal(samples, 0, std::nullopt) == "The sample rate must be from 1 to 4194303 Hz.");
    XSFLIB_CHECK(refusal(samples, 4194304, std::nullopt) == "The sample rate must be from 1 to 4194303 Hz.");
    XSFLIB_CHECK(refusal(samples, 8000, 7) == "The loop must start before the end of the sample.");
}

// New bytes go where Join finds its free space, and the next ones go on from their end.
void TestNewBytes()
{
    using hokuto::edit::WithGbaNewBytes;

    const SmallCart cart = MakeCart(0x2000);
    const File file = hokuto::format::ImportGbaRom(cart.bytes);
    const std::vector<uint8_t> bytes = {1, 2, 3, 4, 5, 6, 7, 0xFF};
    const uint32_t free_space = kRomBase + ((static_cast<uint32_t>(cart.before_tail) + 511) & ~255U);
    const hokuto::edit::PlacedGbaBytes first = WithGbaNewBytes(file, bytes);
    XSFLIB_CHECK(first.address == free_space);
    XSFLIB_CHECK(hokuto::format::ExportArchive(first.file).size() == cart.bytes.size());
    std::vector<uint8_t> held = bytes;
    held.insert(held.end(), {0, 0, 0, 0});
    XSFLIB_CHECK(BytesAt(first.file, first.address, held.size()) == held);
    XSFLIB_CHECK(BytesAt(first.file, first.address + 12, 1) == std::vector<uint8_t>{0xFF});
    XSFLIB_CHECK(Played(first.file, 2000) == Played(file, 2000));
    CheckForms(first.file);

    const hokuto::edit::PlacedGbaBytes second = WithGbaNewBytes(first.file, bytes);
    XSFLIB_CHECK(second.address == first.address + 512);
    XSFLIB_CHECK(BytesAt(second.file, first.address, held.size()) == held);

    // The ROM's run of 0xFF bytes ends before the new bytes do: the ROM grows by the new bytes past the end of the run.
    const SmallCart short_tail = MakeCart(0x180);
    const File short_file = hokuto::format::ImportGbaRom(short_tail.bytes);
    const std::vector<uint8_t> large(0x400, 0x11);
    const hokuto::edit::PlacedGbaBytes grown = WithGbaNewBytes(short_file, large);
    XSFLIB_CHECK(grown.address >= kRomBase + short_tail.before_tail + mp2k::kFreeSpaceMargin);
    XSFLIB_CHECK(hokuto::format::ExportArchive(grown.file).size() == grown.address - kRomBase + 0x404);
    XSFLIB_CHECK(Played(grown.file, 2000) == Played(short_file, 2000));
    XSFLIB_CHECK(BytesAt(grown.file, grown.address, large.size()) == large);

    // Without a run of 0xFF bytes, the bytes go on a multiple of 4 after the ROM's end.
    const SmallCart no_tail = MakeCart(0);
    const File bare = hokuto::format::ImportGbaRom(no_tail.bytes);
    const hokuto::edit::PlacedGbaBytes after = WithGbaNewBytes(bare, bytes);
    XSFLIB_CHECK(after.address == kRomBase + ((static_cast<uint32_t>(no_tail.bytes.size()) + 3) & ~3U));
    XSFLIB_CHECK(Played(after.file, 2000) == Played(bare, 2000));

    // A song that outgrows its place moves after the new bytes, and they stay.
    namespace midi = seqlib::midi;
    midi::FileBuilder long_song(48);
    midi::TrackBuilder& track = long_song.AddTrack();
    for (int note = 0; note < 200; note++)
    {
        track.NoteOn(0, 0, 40 + note % 40, 100);
        track.NoteOff(24, 0, 40 + note % 40);
    }
    const std::shared_ptr<const hokuto::song::GbaRom> built =
        hokuto::song::LoadGbaRom(hokuto::format::ExportArchive(first.file));
    const std::vector<std::optional<uint32_t>> sequences = {
        hokuto::format::GbaSongSequence(first.file, built->info.song_table, 0)};
    const auto places =
        hokuto::edit::GbaSoundPlaces(sequences, hokuto::format::GbaTrackFields(first.file, built->info.song_table, 1));
    const hokuto::edit::GbaMidiSong midi_song = hokuto::edit::GbaSongOfMidi(long_song.Encode());
    const File moved = hokuto::edit::WithGbaMidiSong(first.file, places, 0, midi_song);
    XSFLIB_CHECK(BytesAt(moved, first.address, held.size()) == held);
    const std::shared_ptr<const hokuto::song::GbaRom> rom =
        hokuto::song::LoadGbaRom(hokuto::format::ExportArchive(moved));
    mp2k::SongHeader header;
    SEQLIB_REQUIRE(mp2k::ReadSong(rom->rom, rom->info, 0, header));
    XSFLIB_CHECK(header.address > first.address + held.size());
}

// At the 32 MiB that the GBA maps, the bytes go into a filler run, and only where the file allows it.
void TestFullRom()
{
    using hokuto::edit::WithGbaNewBytes;

    SmallCart cart = MakeCart(0x10000);
    cart.bytes.resize(mp2k::kLargestRom, 0x22);
    File file = hokuto::format::ImportGbaRom(cart.bytes);
    const std::vector<uint8_t> bytes(0x100, 0x33);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { WithGbaNewBytes(file, bytes); }) ==
                 "No room for 260 more bytes. The ROM can't exceed 32 MiB. To use runs of 0xFF bytes inside the ROM, "
                 "turn on Move songs into filler runs in the Export window.");

    file.native.Edit([](hokuto::format::NativeArchive& native) { native.gba_filler = true; });
    const hokuto::edit::PlacedGbaBytes placed = WithGbaNewBytes(file, bytes);
    XSFLIB_CHECK(placed.address >= kRomBase + cart.before_tail + mp2k::kFreeSpaceMargin);
    XSFLIB_CHECK(placed.address + 0x104 < kRomBase + cart.before_tail + 0x10000);
    XSFLIB_CHECK(hokuto::format::ExportArchive(placed.file).size() == mp2k::kLargestRom);
    XSFLIB_CHECK(BytesAt(placed.file, placed.address, bytes.size()) == bytes);
}

// A voice changes its kind. The key, a drum kit's pan and a fixed pitch stay.
void TestKinds()
{
    using hokuto::edit::GbaVoiceOfKind;
    using hokuto::edit::WithGbaVoice;

    mp2k::Voice sample;
    sample.address = 0x08000100;
    sample.type = mp2k::kVoiceFixed;
    sample.key = 38;
    sample.pan_sweep = 0xA0;
    sample.wave = 0x08001000;
    sample.attack = 255;
    sample.decay = 250;
    sample.sustain = 200;
    sample.release = 165;

    const mp2k::Voice square = GbaVoiceOfKind(sample, 2, 0);
    XSFLIB_CHECK(square.type == (2 | mp2k::kVoiceFixed) && square.PsgChannel() == 2);
    XSFLIB_CHECK(square.address == sample.address && square.key == 38 && square.pan_sweep == 0xA0);
    XSFLIB_CHECK(square.wave == 2 && square.attack == 0 && square.decay == 0 && square.sustain == 15);
    XSFLIB_CHECK(square.release == 0 && square.length == 0);

    mp2k::Voice shaped = square;
    shaped.wave = 1;
    shaped.attack = 3;
    shaped.sustain = 9;
    shaped.length = 12;
    const mp2k::Voice first = GbaVoiceOfKind(shaped, 1, 0);
    XSFLIB_CHECK(first.PsgChannel() == 1 && first.wave == 1 && first.attack == 3 && first.sustain == 9);
    XSFLIB_CHECK(first.length == 12);
    const mp2k::Voice wave = GbaVoiceOfKind(shaped, 3, 0x08002000);
    XSFLIB_CHECK(wave.PsgChannel() == 3 && wave.wave == 0x08002000 && wave.attack == 3 && wave.length == 12);
    const mp2k::Voice noise = GbaVoiceOfKind(wave, 4, 0);
    XSFLIB_CHECK(noise.PsgChannel() == 4 && noise.wave == 0 && noise.sustain == 9);
    const mp2k::Voice back = GbaVoiceOfKind(noise, 2, 0);
    XSFLIB_CHECK(back.PsgChannel() == 2 && back.wave == 2);

    const mp2k::Voice again = GbaVoiceOfKind(noise, 0, 0x08001000);
    XSFLIB_CHECK(again.PsgChannel() == 0 && again.type == mp2k::kVoiceFixed && again.wave == 0x08001000);
    XSFLIB_CHECK(again.attack == 255 && again.decay == 0 && again.sustain == 255 && again.release == 165);
    XSFLIB_CHECK(again.length == 0);

    // A sample that stays one changes only its sample.
    const mp2k::Voice other = GbaVoiceOfKind(sample, 0, 0x08003000);
    XSFLIB_CHECK(other.wave == 0x08003000 && other.decay == 250 && other.sustain == 200);

    // The ROM stores the new kind. A square wave's bytes play as a voice of that kind in a cartridge.
    const SmallCart cart = MakeCart(0x2000);
    const File file = hokuto::format::ImportGbaRom(cart.bytes);
    mp2k::Voice voice;
    const std::shared_ptr<const hokuto::song::GbaRom> rom = hokuto::song::LoadGbaRom(cart.bytes);
    SEQLIB_REQUIRE(mp2k::ReadVoice(rom->rom, cart.voices, voice));
    const File edited = WithGbaVoice(file, GbaVoiceOfKind(voice, 2, 0));
    const std::vector<uint8_t> bytes = BytesAt(edited, cart.voices, 12);
    XSFLIB_CHECK((bytes == std::vector<uint8_t>{2, 60, 0, 0, 2, 0, 0, 0, 0, 0, 15, 0}));
    XSFLIB_CHECK(Played(edited, 2000) != Played(file, 2000));
    CheckForms(edited);

    // A value that the new kind doesn't take.
    mp2k::Voice wrong = GbaVoiceOfKind(voice, 2, 0);
    wrong.attack = 8;
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { WithGbaVoice(file, wrong); }) ==
                 "The attack of a PSG voice must be from 0 to 7.");
}

// A new voice moves a table with other data after it, once.
void TestMovedGroup()
{
    using hokuto::edit::WithGbaVoicesAt;

    const SmallCart cart = MakeCart(0x4000);
    File file = hokuto::format::ImportGbaRom(cart.bytes);
    file.instrument_maps.emplace(cart.voices, hokuto::format::InstrumentMap{});
    const std::shared_ptr<const hokuto::song::GbaRom> rom = hokuto::song::LoadGbaRom(cart.bytes);
    XSFLIB_CHECK(hokuto::song::GbaTableHoldsVoices(*rom, cart.voices));

    const std::vector<uint32_t> free = hokuto::edit::FreeGbaPrograms(*rom, cart.voices);
    SEQLIB_REQUIRE(free.size() == 126);
    XSFLIB_CHECK(free.front() == 2 && free.back() == 127);

    const std::vector<std::pair<uint32_t, mp2k::Voice>> voices = {{5, hokuto::edit::NewGbaSampleVoice(cart.sample)}};
    const hokuto::edit::GbaGroupVersion moved = WithGbaVoicesAt(file, *rom, cart.voices, voices);
    XSFLIB_CHECK(moved.group != cart.voices);
    XSFLIB_CHECK(moved.group == kRomBase + ((static_cast<uint32_t>(cart.before_tail) + 511) & ~255U));
    XSFLIB_CHECK(moved.file.instrument_maps.contains(moved.group));
    XSFLIB_CHECK(!moved.file.instrument_maps.contains(cart.voices));

    // The header names the new table. The old table stays, and the song plays as before.
    XSFLIB_CHECK(Word(BytesAt(moved.file, cart.header, 8), 4) == moved.group);
    XSFLIB_CHECK(BytesAt(moved.file, moved.group, 24) == BytesAt(file, cart.voices, 24));
    XSFLIB_CHECK(BytesAt(moved.file, cart.voices, 24) == BytesAt(file, cart.voices, 24));
    XSFLIB_CHECK(Played(moved.file, 8000) == Played(file, 8000));

    const std::vector<uint8_t> entry = BytesAt(moved.file, moved.group + 60, 12);
    XSFLIB_CHECK(entry[0] == 0 && entry[1] == 60 && entry[2] == 0 && entry[3] == 0 && Word(entry, 4) == cart.sample);
    XSFLIB_CHECK(entry[8] == 255 && entry[9] == 0 && entry[10] == 255 && entry[11] == 165);
    XSFLIB_CHECK(BytesAt(moved.file, moved.group + 24, 36) == std::vector<uint8_t>(36, 0));
    CheckForms(moved.file);

    // The table has only voices and empty entries. The next voice goes into it in place, and the program is in use.
    const std::shared_ptr<const hokuto::song::GbaRom> built =
        hokuto::song::LoadGbaRom(hokuto::format::ExportArchive(moved.file));
    XSFLIB_CHECK(hokuto::song::GbaTableHoldsVoices(*built, moved.group));
    XSFLIB_CHECK(hokuto::song::GbaSongsWithOtherVoices(*rom, *built).empty());
    const hokuto::song::GbaVoiceGroup group = hokuto::song::GbaVoiceGroups(*built).front();
    XSFLIB_CHECK(group.address == moved.group);
    XSFLIB_CHECK((hokuto::song::GbaListedPrograms(*built, group) == std::vector<int>{0, 1, 5}));
    const std::vector<uint32_t> left = hokuto::edit::FreeGbaPrograms(*built, moved.group);
    XSFLIB_CHECK(left.size() == 125 && std::ranges::find(left, uint32_t{5}) == left.end());

    const std::vector<std::pair<uint32_t, mp2k::Voice>> more = {{6, hokuto::edit::NewGbaSquareVoice()}};
    const hokuto::edit::GbaGroupVersion in_place = WithGbaVoicesAt(moved.file, *built, moved.group, more);
    XSFLIB_CHECK(in_place.group == moved.group);
    const std::shared_ptr<const hokuto::song::GbaRom> with_more =
        hokuto::song::LoadGbaRom(hokuto::format::ExportArchive(in_place.file));
    XSFLIB_CHECK(hokuto::song::GbaSongsWithOtherVoices(*rom, *with_more).empty());
    XSFLIB_CHECK(hokuto::song::GbaSongsWithOtherVoices(
                     *rom, *hokuto::song::LoadGbaRom(hokuto::format::ExportArchive(hokuto::edit::WithoutGbaVoice(
                               in_place.file, moved.group + 12)))) == std::vector<int>{0});
    XSFLIB_CHECK(BytesAt(in_place.file, moved.group + 72, 12) ==
                 (std::vector<uint8_t>{2, 60, 0, 0, 2, 0, 0, 0, 0, 0, 15, 0}));
    XSFLIB_CHECK(hokuto::format::ExportArchive(in_place.file).size() == cart.bytes.size());

    // A deleted voice plays nothing, and its program is free again.
    const File cleared = hokuto::edit::WithoutGbaVoice(in_place.file, moved.group + 60);
    XSFLIB_CHECK(BytesAt(cleared, moved.group + 60, 12) == std::vector<uint8_t>(12, 0));
    XSFLIB_CHECK(Played(cleared, 8000) == Played(file, 8000));
    const std::shared_ptr<const hokuto::song::GbaRom> emptied =
        hokuto::song::LoadGbaRom(hokuto::format::ExportArchive(cleared));
    XSFLIB_CHECK((hokuto::song::GbaListedPrograms(*emptied, hokuto::song::GbaVoiceGroups(*emptied).front()) ==
                  std::vector<int>{0, 1, 6}));

    // A program past the table's end, and a value that the voice doesn't take.
    const std::vector<std::pair<uint32_t, mp2k::Voice>> past = {{128, hokuto::edit::NewGbaSquareVoice()}};
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { WithGbaVoicesAt(file, *rom, cart.voices, past); }) ==
                 "Programs in a voice group must be from 0 to 127.");
    mp2k::Voice wrong = hokuto::edit::NewGbaSquareVoice();
    wrong.sustain = 16;
    const std::vector<std::pair<uint32_t, mp2k::Voice>> values = {{5, wrong}};
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { WithGbaVoicesAt(file, *rom, cart.voices, values); }) ==
                 "The sustain of a PSG voice must be from 0 to 15.");
}

// A row of a drum kit's table can read a song's header as a wave voice's pattern. No note plays such a row. The table's
// move changes the word of the header that names it, and the song still counts as playing the same voices.
void TestRowOverHeader()
{
    using mp2k_cart::Cart;
    using enum mp2k::Command;

    Cart cart;
    const uint32_t sample = cart.Wave(Saw(), 13379, 0);
    const uint32_t group = cart.VoiceGroup(
        {{0, Cart::Voice(0, sample, 60, {255, 0, 255, 165})}, {1, Cart::Voice(mp2k::kVoiceDrumKit, 0)}});
    mp2k_cart::Track track;
    track.Cmd(kCmdTempo, 75).Cmd(kCmdVol, 100);
    track.Cmd(kCmdVoice, 1).Note(24, 60, 100).Wait(24);
    track.Fine();
    const uint32_t header = cart.Song({track}, group);
    const uint32_t kit =
        cart.VoiceGroup({{0, Cart::Voice(0, sample, 60, {255, 0, 255, 165})}, {60, Cart::Voice(3, header)}});

    const ntdlib::agb::Rom small = cart.ToRom();
    std::vector<uint8_t> bytes = hokuto::testing::RomBytes(small);
    const uint32_t kit_entry = group - kRomBase + 12 + 4;
    for (uint32_t i = 0; i < 4; i++)
    {
        bytes[kit_entry + i] = static_cast<uint8_t>(kit >> (i * 8));
    }
    bytes.resize(bytes.size() + 0x4000, 0xFF);

    const File file = hokuto::format::ImportGbaRom(bytes);
    const std::shared_ptr<const hokuto::song::GbaRom> rom = hokuto::song::LoadGbaRom(bytes);
    const std::vector<std::pair<uint32_t, mp2k::Voice>> voices = {{5, hokuto::edit::NewGbaSampleVoice(sample)}};
    const hokuto::edit::GbaGroupVersion moved = hokuto::edit::WithGbaVoicesAt(file, *rom, group, voices);
    XSFLIB_CHECK(moved.group != group);
    XSFLIB_CHECK(Word(BytesAt(moved.file, header, 8), 4) == moved.group);
    const std::shared_ptr<const hokuto::song::GbaRom> built =
        hokuto::song::LoadGbaRom(hokuto::format::ExportArchive(moved.file));
    XSFLIB_CHECK(hokuto::song::GbaSongsWithOtherVoices(*rom, *built).empty());

    // A row that a table's move doesn't explain still counts.
    File other = moved.file;
    mp2k::Voice row;
    SEQLIB_REQUIRE(mp2k::ReadVoice(built->rom, kit, row));
    row.sustain = 100;
    other = hokuto::edit::WithGbaVoice(other, row);
    XSFLIB_CHECK(hokuto::song::GbaSongsWithOtherVoices(
                     *rom, *hokuto::song::LoadGbaRom(hokuto::format::ExportArchive(other))) == std::vector<int>{0});
}

// A game's shorter table moves with its two entries. The data after them stays behind, and zeros take its place.
void TestShortTable()
{
    const SmallCart cart = MakeCart(0x4000, Saw(), true);
    const File file = hokuto::format::ImportGbaRom(cart.bytes);
    const std::shared_ptr<const hokuto::song::GbaRom> rom = hokuto::song::LoadGbaRom(cart.bytes);
    XSFLIB_CHECK(!hokuto::song::GbaTableHoldsVoices(*rom, cart.voices));
    XSFLIB_CHECK(hokuto::edit::FreeGbaPrograms(*rom, cart.voices).size() == 126);
    const hokuto::song::GbaVoiceGroup group = hokuto::song::GbaVoiceGroups(*rom).front();
    XSFLIB_CHECK((hokuto::song::GbaListedPrograms(*rom, group) == std::vector<int>{0, 1}));

    const std::vector<std::pair<uint32_t, mp2k::Voice>> voices = {{2, hokuto::edit::NewGbaSquareVoice()}};
    const hokuto::edit::GbaGroupVersion moved = hokuto::edit::WithGbaVoicesAt(file, *rom, cart.voices, voices);
    XSFLIB_CHECK(moved.group != cart.voices);
    XSFLIB_CHECK(BytesAt(moved.file, moved.group, 24) == BytesAt(file, cart.voices, 24));
    XSFLIB_CHECK(BytesAt(moved.file, moved.group + 36, 127 * 12 - 24) == std::vector<uint8_t>(127 * 12 - 24, 0));
    XSFLIB_CHECK(BytesAt(moved.file, cart.voices + 24, 12) == std::vector<uint8_t>(12, 0x30));
    XSFLIB_CHECK(Played(moved.file, 8000) == Played(file, 8000));
    const std::shared_ptr<const hokuto::song::GbaRom> built =
        hokuto::song::LoadGbaRom(hokuto::format::ExportArchive(moved.file));
    XSFLIB_CHECK(hokuto::song::GbaTableHoldsVoices(*built, moved.group));
}

// The edits of a list of rows of a key split or a drum kit.
void TestRowLists()
{
    using hokuto::edit::GbaKeyRow;
    using hokuto::edit::GbaRowsWithKeys;
    using hokuto::edit::GbaRowsWithNew;
    using hokuto::edit::GbaSplitRowsWithout;
    using Keys = std::vector<std::pair<int, int>>;

    const auto keys = [](const std::vector<GbaKeyRow>& rows)
    {
        Keys shown;
        for (const GbaKeyRow& row : rows)
        {
            shown.emplace_back(row.first_key, row.last_key);
        }
        return shown;
    };

    mp2k::Voice marked;
    marked.key = 40;
    const std::vector<GbaKeyRow> whole = {{0, 127, marked}};
    const std::vector<GbaKeyRow> halves = GbaRowsWithNew(whole, false, 0);
    XSFLIB_CHECK(keys(halves) == (Keys{{0, 63}, {64, 127}}));
    XSFLIB_CHECK(halves[1].voice.key == 40);
    XSFLIB_CHECK(keys(GbaRowsWithNew(halves, false, 64)) == (Keys{{0, 63}, {64, 95}, {96, 127}}));
    XSFLIB_CHECK(keys(GbaRowsWithNew({}, false, std::nullopt)) == (Keys{{0, 127}}));
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([] { GbaRowsWithNew(std::vector<GbaKeyRow>{{5, 5, {}}}, false, 5); }) ==
                 "This row covers a single key and can't be split. Select a row with two or more keys.");

    const std::vector<GbaKeyRow> kit = {{36, 36, marked}, {37, 37, {}}, {127, 127, {}}};
    const std::vector<GbaKeyRow> added = GbaRowsWithNew(kit, true, 36);
    XSFLIB_CHECK(keys(added) == (Keys{{36, 36}, {37, 37}, {38, 38}, {127, 127}}));
    XSFLIB_CHECK(added[2].voice.key == 40);
    XSFLIB_CHECK(keys(GbaRowsWithNew(kit, true, 127)) == (Keys{{0, 0}, {36, 36}, {37, 37}, {127, 127}}));
    XSFLIB_CHECK(keys(GbaRowsWithNew({}, true, std::nullopt)) == (Keys{{60, 60}}));
    std::vector<GbaKeyRow> full;
    for (int key = 0; key < 128; key++)
    {
        full.push_back({key, key, {}});
    }
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { GbaRowsWithNew(full, true, 3); }) ==
                 "Every key of the drum kit already has a voice.");

    const std::vector<GbaKeyRow> three = {{0, 39, {}}, {40, 79, marked}, {80, 127, {}}};
    XSFLIB_CHECK(keys(GbaSplitRowsWithout(three, 40)) == (Keys{{0, 79}, {80, 127}}));
    XSFLIB_CHECK(keys(GbaSplitRowsWithout(three, 0)) == (Keys{{0, 79}, {80, 127}}));
    XSFLIB_CHECK(GbaSplitRowsWithout(three, 0)[0].voice.key == 40);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { GbaSplitRowsWithout(whole, 0); }) ==
                 "A key split must have at least one voice. To keep only this voice, use Make Single Voice.");

    // A row gains keys from the rows beside it, covers a row or cuts it in two. A key that it gives up plays nothing.
    XSFLIB_CHECK(keys(GbaRowsWithKeys(three, false, 40, 30, 79)) == (Keys{{0, 29}, {30, 79}, {80, 127}}));
    XSFLIB_CHECK(keys(GbaRowsWithKeys(three, false, 40, 50, 60)) == (Keys{{0, 39}, {50, 60}, {80, 127}}));
    XSFLIB_CHECK(keys(GbaRowsWithKeys(three, false, 40, 0, 100)) == (Keys{{0, 100}, {101, 127}}));
    XSFLIB_CHECK(keys(GbaRowsWithKeys(three, false, 0, 90, 100)) == (Keys{{40, 79}, {80, 89}, {90, 100}, {101, 127}}));
    XSFLIB_CHECK(GbaRowsWithKeys(three, false, 40, 30, 79)[1].voice.key == 40);

    XSFLIB_CHECK(keys(GbaRowsWithKeys(kit, true, 36, 50, 50)) == (Keys{{37, 37}, {50, 50}, {127, 127}}));
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { GbaRowsWithKeys(kit, true, 36, 37, 37); }) ==
                 "Another voice of the drum kit is already at that key.");
}

// A key split and a drum kit take new tables. The song plays through them.
void TestKeyRows()
{
    using hokuto::edit::GbaKeyRow;
    using hokuto::edit::WithGbaKeyRows;
    using Rows = std::vector<GbaKeyRow>;

    const SmallCart cart = MakeCart(0x4000);
    const File file = hokuto::format::ImportGbaRom(cart.bytes);
    const std::shared_ptr<const hokuto::song::GbaRom> rom = hokuto::song::LoadGbaRom(cart.bytes);
    mp2k::Voice sample;
    mp2k::Voice square;
    SEQLIB_REQUIRE(mp2k::ReadVoice(rom->rom, cart.voices, sample));
    SEQLIB_REQUIRE(mp2k::ReadVoice(rom->rom, cart.voices + 12, square));
    const std::vector<int16_t> before = Played(file, 8000);

    // One row over every key plays as the voice did. The tables follow the free space: the voice table with its empty
    // entry, then the key table.
    const mp2k::Voice split = hokuto::edit::GbaSplitOf(sample, mp2k::kVoiceKeySplit);
    XSFLIB_CHECK(split.address == cart.voices && split.IsSplit());
    const File one_row = WithGbaKeyRows(file, split, Rows{{0, 127, sample}});
    XSFLIB_CHECK(Played(one_row, 8000) == before);
    const std::vector<uint8_t> entry = BytesAt(one_row, cart.voices, 12);
    XSFLIB_CHECK(entry[0] == mp2k::kVoiceKeySplit);
    const uint32_t table = Word(entry, 4);
    const uint32_t key_table = Word(entry, 8);
    XSFLIB_CHECK(table == kRomBase + ((static_cast<uint32_t>(cart.before_tail) + 511) & ~255U));
    XSFLIB_CHECK(key_table == table + 512);
    XSFLIB_CHECK(BytesAt(one_row, table, 12) == BytesAt(file, cart.voices, 12));
    XSFLIB_CHECK(BytesAt(one_row, table + 12, 12) == std::vector<uint8_t>(12, 0));
    XSFLIB_CHECK(BytesAt(one_row, key_table, 128) == std::vector<uint8_t>(128, 0));
    CheckForms(one_row);

    // The notes at key 60 use the row that covers the key.
    const File upper = WithGbaKeyRows(file, split, Rows{{0, 59, square}, {60, 127, sample}});
    XSFLIB_CHECK(Played(upper, 8000) == before);
    const std::vector<uint8_t> keys = BytesAt(upper, Word(BytesAt(upper, cart.voices, 12), 8), 128);
    XSFLIB_CHECK(keys[0] == 0 && keys[59] == 0 && keys[60] == 1 && keys[127] == 1);
    const File lower = WithGbaKeyRows(file, split, Rows{{0, 60, square}, {61, 127, sample}});
    mp2k::Voice as_square = square;
    as_square.address = cart.voices;
    XSFLIB_CHECK(Played(lower, 8000) == Played(hokuto::edit::WithGbaProgramVoice(file, cart.voices, as_square), 8000));
    XSFLIB_CHECK(Played(lower, 8000) != before);

    // A key that no row plays uses the empty entry.
    const File gap = WithGbaKeyRows(file, split, Rows{{0, 10, square}, {70, 127, sample}});
    const std::vector<uint8_t> gap_keys = BytesAt(gap, Word(BytesAt(gap, cart.voices, 12), 8), 128);
    XSFLIB_CHECK(gap_keys[10] == 0 && gap_keys[11] == 2 && gap_keys[60] == 2 && gap_keys[69] == 2 && gap_keys[70] == 1);
    XSFLIB_CHECK(Played(gap, 8000, false) != before);

    // A drum kit's voice is at its key, with zeros at the others. It plays at its key.
    const mp2k::Voice kit = hokuto::edit::GbaSplitOf(sample, mp2k::kVoiceDrumKit);
    const File drums = WithGbaKeyRows(file, kit, Rows{{60, 60, sample}});
    const std::vector<uint8_t> kit_entry = BytesAt(drums, cart.voices, 12);
    XSFLIB_CHECK(kit_entry[0] == mp2k::kVoiceDrumKit);
    const uint32_t kit_table = Word(kit_entry, 4);
    XSFLIB_CHECK(BytesAt(drums, kit_table + 60 * 12, 12) == BytesAt(file, cart.voices, 12));
    XSFLIB_CHECK(BytesAt(drums, kit_table, 60 * 12) == std::vector<uint8_t>(60 * 12, 0));
    XSFLIB_CHECK(BytesAt(drums, kit_table + 61 * 12, 67 * 12) == std::vector<uint8_t>(67 * 12, 0));
    XSFLIB_CHECK(Played(drums, 8000) == before);
    CheckForms(drums);
    XSFLIB_CHECK(Played(WithGbaKeyRows(file, kit, Rows{{61, 61, sample}}), 8000, false) != before);

    // Make Single Voice leaves the tables behind.
    XSFLIB_CHECK(Played(hokuto::edit::WithGbaProgramVoice(drums, cart.voices, sample), 8000) == before);

    // Rows that the driver can't play.
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { WithGbaKeyRows(file, sample, Rows{{0, 127, square}}); }) ==
                 "This voice isn't a key split or a drum kit.");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { WithGbaKeyRows(file, split, Rows{}); }) ==
                 "A key split must have at least one voice.");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>(
                     [&] { WithGbaKeyRows(file, split, Rows{{0, 60, square}, {60, 127, sample}}); }) ==
                 "Key ranges can't overlap or go above key 127.");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { WithGbaKeyRows(file, kit, Rows{{60, 61, sample}}); }) ==
                 "Each voice of a drum kit must cover a single key.");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { WithGbaKeyRows(file, split, Rows{{0, 127, split}}); }) ==
                 "A key split or a drum kit can't contain another key split or drum kit.");
}

// A WAV file's sample plays as the same samples do in a cartridge that ntdlib's test code lays out, and a sample of two
// channels becomes their mean.
void TestDroppedSample()
{
    const std::vector<int8_t> pcm = Saw();
    seqlib::Pcm source;
    source.channels.resize(2);
    for (const int8_t value : pcm)
    {
        const int sample = int{value} * 256;
        source.channels[0].push_back(static_cast<int16_t>(sample + 80));
        source.channels[1].push_back(static_cast<int16_t>(sample));
    }
    source.loop = true;
    source.loop_start = 0;
    source.loop_end = static_cast<uint32_t>(pcm.size());
    const std::vector<uint8_t> wav = seqlib::WavFile(source, 13379);

    const hokuto::edit::IncomingWave incoming = hokuto::edit::ReadIncomingGbaSample(wav);
    XSFLIB_CHECK(incoming.channels == 1 && incoming.rate == 13379 && incoming.loop && incoming.loop_start == 0);
    XSFLIB_CHECK(incoming.frames == pcm.size());

    // A cartridge whose sample contains other bytes gets the sample of the WAV file in the place of the voice.
    std::vector<int8_t> quiet(pcm.size());
    for (std::size_t i = 0; i < quiet.size(); i++)
    {
        quiet[i] = static_cast<int8_t>(pcm[i] / 8);
    }
    const SmallCart cart = MakeCart(0x2000, quiet);
    const File file = hokuto::format::ImportGbaRom(cart.bytes);
    const hokuto::edit::PlacedGbaBytes placed = hokuto::edit::WithGbaNewBytes(file, incoming.bytes);
    const std::shared_ptr<const hokuto::song::GbaRom> rom = hokuto::song::LoadGbaRom(cart.bytes);
    mp2k::Voice voice;
    SEQLIB_REQUIRE(mp2k::ReadVoice(rom->rom, cart.voices, voice));
    const File dropped =
        hokuto::edit::WithGbaVoice(placed.file, hokuto::edit::GbaVoiceOfKind(voice, 0, placed.address));
    CheckForms(dropped);

    const std::shared_ptr<const hokuto::song::GbaRom> built =
        hokuto::song::LoadGbaRom(hokuto::format::ExportArchive(dropped));
    mp2k::Wave wave;
    SEQLIB_REQUIRE(mp2k::ReadWave(built->rom, placed.address, wave));
    XSFLIB_CHECK(wave.loops && wave.loop_start == 0 && wave.size == pcm.size() && wave.frequency == 13379 * 1024);

    const File reference = hokuto::format::ImportGbaRom(MakeCart(0x2000, pcm).bytes);
    XSFLIB_CHECK(Played(dropped, 8000) == Played(reference, 8000));
    XSFLIB_CHECK(Played(dropped, 8000) != Played(file, 8000));
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("gba_voices",
                                    []
                                    {
                                        TestSampleBytes();
                                        TestNewBytes();
                                        TestFullRom();
                                        TestKinds();
                                        TestMovedGroup();
                                        TestRowOverHeader();
                                        TestShortTable();
                                        TestRowLists();
                                        TestKeyRows();
                                        TestDroppedSample();
                                    });
}
