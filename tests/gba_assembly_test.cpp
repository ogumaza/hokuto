// SPDX-License-Identifier: MIT

// Tests the assembly of GBA songs and voice groups, and the files of GBA samples. The expected bytes of a song come
// from the driver's command codes: a wait of n ticks is 0x80 and the code of its length, a note of n ticks is 0xCF and
// that code, and the codes count 0 to 24 ticks one by one. Those of a voice come from its layout of 12 bytes. Those of
// an AIFF file come from the format's definition of its chunks and its 80-bit rate.

#include "song/gba_assembly.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/mp2k/commands.h"
#include "ntdlib/mp2k/song.h"
#include "ntdlib/testing/mp2k_cart.h"
#include "seqlib/wav.h"
#include "xsflib/testing/check.h"

#include "cart_bytes.h"
#include "edit/files.h"
#include "edit/gba_midi.h"
#include "edit/gba_voices.h"
#include "edit/replace.h"
#include "format/archive.h"
#include "format/file.h"
#include "song/gba.h"
#include "song/gba_samples.h"

namespace
{

namespace mp2k = ntdlib::mp2k;
namespace mp2k_cart = ntdlib::testing::mp2k_cart;
using hokuto::song::GbaSongAssembly;
using hokuto::song::GbaSongSource;
using hokuto::song::ReadGbaAssembly;

using Bytes = std::vector<uint8_t>;

bool Refused(std::string_view text, const std::string& part)
{
    try
    {
        ReadGbaAssembly(text);
    }
    catch (const std::invalid_argument& error)
    {
        const std::string message = error.what();
        if (message.find(part) == std::string::npos)
        {
            std::printf("the message is: %s\n", message.c_str());
            return false;
        }
        return true;
    }

    return false;
}

// A hand-written file in the form that GBA projects use.
void TestHandWritten()
{
    constexpr std::string_view kText = R"(	.include "MPlayDef.s"

	.equ	song_grp, voicegroup_title
	.equ	song_pri, 3
	.equ	song_rev, reverb_set+50
	.equ	song_mvl, 127
	.equ	song_key, 0
	.equ	song_tbs, 1

	.section .rodata
	.global	song
	.align	2

@********************** Track  1 **********************@

song_1:
@ 000   ----------------------------------------
	.byte	KEYSH , song_key+0
	.byte	TEMPO , 120*song_tbs/2
	.byte		VOICE , 38
	.byte		VOL   , 100*song_mvl/mxv
	.byte		PAN   , c_v-12
	.byte		N24   , Cn3 , v100
	.byte	W24
song_1_B1:
	.byte		TIE   , Gn8 , v127 ; .byte W12
	.byte		EOT
	.byte	GOTO
	 .word	song_1_B1
	.byte	FINE

	.align	2

song:
	.byte	1	@ NumTrks
	.byte	0	@ NumBlks
	.byte	song_pri	@ Priority
	.byte	song_rev	@ Reverb

	.word	song_grp

	.word	song_1

	.end
	.byte	99
)";

    const GbaSongSource source = ReadGbaAssembly(kText, 0x08001000);
    XSFLIB_CHECK(source.name == "song");
    XSFLIB_CHECK(source.base == 0x08001000);
    XSFLIB_CHECK(source.entries == std::vector<uint32_t>{0x08001000});
    XSFLIB_CHECK(source.blocks == 0);
    XSFLIB_CHECK(source.priority == 3);
    XSFLIB_CHECK(source.reverb == 0x80 + 50);
    XSFLIB_CHECK(!source.voices.has_value());
    XSFLIB_CHECK(source.voices_symbol == "voicegroup_title");

    const Bytes expected = {0xBC, 0,   0xBB, 60,   0xBD, 38,   0xBE, 100,  0xBF, 52, 0xE7, 60, 100, 0x98,
                            0xCF, 127, 127,  0x8C, 0xCE, 0xB2, 0x0E, 0x10, 0,    8,  0xB1, 0,  0,   0};
    XSFLIB_CHECK(source.bytes == expected);
}

// The arithmetic, a voice group by its number, and a name that the label of another track defines.
void TestExpressions()
{
    const GbaSongSource source = ReadGbaAssembly(R"(
first:	.byte	(2+3)*4/2, -1, 0x10-1, +7
	.2byte	0x1234
	.balign	4
second:	.byte	W96, N96, N01, CnM2, Bn3, gtp3, mod_tre, xIECL
	.align	2
head:
	.byte	2, 1, 0, 0
	.word	0x08ABCDE0
	.word	second, first
)");

    XSFLIB_CHECK(source.name == "head");
    XSFLIB_CHECK(source.base == 0x0E000000);
    XSFLIB_CHECK(source.entries == (std::vector<uint32_t>{0x0E000008, 0x0E000000}));
    XSFLIB_CHECK(source.blocks == 1);
    XSFLIB_CHECK(source.voices == 0x08ABCDE0u);
    XSFLIB_CHECK(source.voices_symbol.empty());
    const Bytes expected = {10, 0xFF, 15, 7, 0x34, 0x12, 0, 0, 0xB0, 0xFF, 0xD0, 0, 71, 3, 1, 9};
    XSFLIB_CHECK(source.bytes == expected);
}

// A song's assembly is refused with a message that names the line or the fault: an .incbin, no song header, a header
// that ends too early, more than 16 tracks, a header that ends before its last track, a track that doesn't start before
// the header and a byte that isn't a number.
void TestRefusals()
{
    XSFLIB_CHECK(Refused("\t.incbin \"song.bin\"\n", "Line 1"));
    XSFLIB_CHECK(Refused("\n\t.incbin \"song.bin\"\n", "\".incbin\""));
    XSFLIB_CHECK(Refused("\t.byte 1\n", "no song header"));
    XSFLIB_CHECK(Refused("head:\n\t.byte 1\n", "ends too early"));
    XSFLIB_CHECK(Refused("head:\n\t.byte 17, 0, 0, 0\n\t.word 0\n", "lists 17 tracks. A song plays at most 16."));
    XSFLIB_CHECK(Refused("head:\n\t.byte 2, 0, 0, 0\n\t.word 0\n\t.word 0\n", "ends before the last of its 2 tracks"));
    XSFLIB_CHECK(Refused("head:\n\t.byte 1, 0, 0, 0\n\t.word 0\n\t.word 0\n", "doesn't start before the header"));
    XSFLIB_CHECK(Refused("track:\n\t.byte mystery\nhead:\n\t.byte 1, 0, 0, 0\n\t.word 0\n\t.word track\n", "Line 2"));
    XSFLIB_CHECK(
        Refused("track:\n\t.byte mystery\nhead:\n\t.byte 1, 0, 0, 0\n\t.word 0\n\t.word track\n", "\"mystery\""));
    XSFLIB_CHECK(Refused("\t.byte 1/0\nhead:\n", "divides by 0"));
    XSFLIB_CHECK(Refused("\t.byte (1\nhead:\n", "closing parenthesis"));
    XSFLIB_CHECK(Refused("\t.equ a\nhead:\n", "has no value"));
    XSFLIB_CHECK(Refused("\t.equ a, a\n\t.byte a\nhead:\n", "is defined in terms of itself"));
    XSFLIB_CHECK(Refused("\t.byte 08\nhead:\n", "Can't read the number"));
}

// GNU as reads a number that starts with 0 and another digit as octal (Using as, "Integers").
void TestOctal()
{
    const GbaSongSource source = ReadGbaAssembly(R"(
first:	.byte	010, 0x10, 10, 0
head:
	.byte	1, 0, 0, 0
	.word	0x08ABCDE0
	.word	first
)");

    XSFLIB_CHECK(source.bytes == (Bytes{8, 0x10, 10, 0}));
}

// A song of a cart: every kind of command, a repeated status, a pattern, a repeat, a comparison, a jump, a jump outside
// the song and bytes that no track reaches.
std::vector<uint8_t> MakeSong()
{
    using enum mp2k::Command;

    mp2k_cart::Cart cart(mp2k_cart::kMode);
    const std::vector<int8_t> saw = {0, 20, 40, 60, 80, 100, 120, -100, -80, -60, -40, -20, 0, 10, 20, 30};
    const uint32_t sample = cart.Wave(saw, 13379, 0);
    const uint32_t voices = cart.VoiceGroup({{0, mp2k_cart::Cart::Voice(0, sample, 60, {255, 0, 255, 165})}});

    mp2k_cart::Track track;
    track.Cmd(kCmdKeySh, 0xFD).Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Raw({90});
    track.Cmd(kCmdPan, 0x30).Cmd(kCmdBend, 0x50).Cmd(kCmdBendR, 2).Cmd(kCmdLfoS, 40).Cmd(kCmdLfoDl, 3);
    track.Cmd(kCmdMod, 10).Cmd(kCmdModT, 1).Cmd(kCmdTune, 0x41).Cmd(kCmdPrio, 7);
    track.Raw({kCmdPort, 3, 4});
    track.Raw({kCmdXcmd, mp2k::kXcmdEchoVol, 12}).Raw({mp2k::kXcmdEchoLen, 3});
    track.Note(24, 60, 100).Wait(1).Raw({62}).Wait(1).Raw({64, 90, 2}).Wait(22);
    track.Tie(72, 127).Wait(12).Eot(72).Wait(1);

    // A pattern that the track jumps over, and then calls.
    const size_t pattern = track.Here() + 5;
    track.Goto(pattern + 5);
    track.Note(12, 67, 80).Wait(12).Raw({kCmdPend});
    track.Patt(pattern);

    const size_t loop = track.Here();
    track.Note(12, 0, 1).Wait(12);
    track.Rept(2, loop);
    track.MemAccJump(6, 1, 2, loop);
    track.Goto(loop).Fine();
    track.Raw({0x90, 0x01, 0x02});

    mp2k_cart::Track other;
    other.Cmd(kCmdVol, 100).Cmd(kCmdVoice, 0).Note(12, 60, 100).Wait(12);
    other.Raw({kCmdPatt, 0x10, 0x20, 0x00, 0x08}).Fine();

    cart.Song({track, other}, voices, 0, 5, 0x80 + 40);

    const ntdlib::agb::Rom rom = cart.ToRom();
    return hokuto::testing::RomBytes(rom);
}

// The assembly of a song names its parts as users of such files expect. A jump outside the song stays an address. From
// another address, the commands give the ROM's bytes at the song's address, and the definitions hold what the text
// names.
void TestOfSong()
{
    const std::shared_ptr<const hokuto::song::GbaRom> rom = hokuto::song::LoadGbaRom(MakeSong());
    mp2k::SongHeader header;
    SEQLIB_REQUIRE(mp2k::ReadSong(rom->rom, rom->info, 0, header));
    const std::string text = GbaSongAssembly(*rom, 0, "the song!");

    // The names stand as the file's users expect them.
    for (const std::string_view line : {"the_song_:",
                                        "the_song__1:",
                                        ".equ\tthe_song__pri, 5",
                                        ".equ\tthe_song__rev, reverb_set+40",
                                        "KEYSH , the_song__key-3",
                                        "TEMPO , 150*the_song__tbs/2",
                                        "VOL , 100*the_song__mvl/mxv",
                                        "90*the_song__mvl/mxv",
                                        "PAN , c_v-16",
                                        "BEND , c_v+16",
                                        "TUNE , c_v+1",
                                        "MODT , mod_tre",
                                        "N24 , Cn3 , v100",
                                        "Dn3",
                                        "En3 , v090 , gtp2",
                                        "TIE , Cn4 , v127",
                                        "EOT , Cn4",
                                        "N12 , CnM2 , v001",
                                        "the_song__1_B1:",
                                        ".word\tthe_song__1_B1"})
    {
        if (text.find(line) == std::string::npos)
        {
            std::printf("missing from the text: %s\n", std::string(line).c_str());
            XSFLIB_CHECK(false);
        }
    }

    // A jump outside the song stays an address, and the second track has its label.
    XSFLIB_CHECK(text.find(".word\t0x08002010") != std::string::npos);
    XSFLIB_CHECK(text.find("the_song__2:") != std::string::npos);

    const uint32_t base = *std::ranges::min_element(header.tracks);
    Bytes expected;
    for (uint32_t address = base; address < header.address; address++)
    {
        expected.push_back(rom->rom.U8(address));
    }

    const GbaSongSource source = ReadGbaAssembly(text, base);
    XSFLIB_CHECK(source.name == "the_song_");
    XSFLIB_CHECK(source.bytes == expected);
    XSFLIB_CHECK(source.entries == header.tracks);
    XSFLIB_CHECK(source.priority == 5);
    XSFLIB_CHECK(source.reverb == 0x80 + 40);
    XSFLIB_CHECK(!source.voices_symbol.empty());
    XSFLIB_CHECK(source.voices_symbol.find("voicegroup_") == 0);

    // From another address, the commands give the ROM's bytes at the song's address.
    const GbaSongSource away = ReadGbaAssembly(text);
    const mp2k::Decoded decoded = mp2k::DecodeSequence(away.bytes, away.base, away.entries);
    XSFLIB_CHECK(!decoded.opaque);
    XSFLIB_CHECK(mp2k::EncodeSequence(decoded.sequence, base).bytes == expected);

    // The definitions hold what the text names.
    const std::string definitions = hokuto::song::GbaAssemblyDefinitions();
    for (const std::string_view line :
         {".equ\tW96, 176", ".equ\tN96, 255", ".equ\tCn3, 60", ".equ\tv127, 127", ".equ\tGOTO, 178"})
    {
        XSFLIB_CHECK(definitions.find(line) != std::string::npos);
    }
}

// The text of a song imports as its commands, and the export returns the ROM unchanged. A note with another key changes
// one byte. A file with more tracks than the header is refused.
void TestImport()
{
    const std::vector<uint8_t> bytes = MakeSong();
    const std::shared_ptr<const hokuto::song::GbaRom> rom = hokuto::song::LoadGbaRom(bytes);
    const hokuto::format::File file = hokuto::format::ImportGbaRom(bytes);
    const std::vector<std::optional<uint32_t>> sequences = {
        hokuto::format::GbaSongSequence(file, rom->info.song_table, 0)};
    const auto places =
        hokuto::edit::GbaSoundPlaces(sequences, hokuto::format::GbaTrackFields(file, rom->info.song_table, 1));

    std::string text = GbaSongAssembly(*rom, 0, "song");
    XSFLIB_CHECK(hokuto::edit::IsGbaAssembly(std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size())));
    const hokuto::edit::GbaMidiSong same = hokuto::edit::GbaSongOfAssembly(text);
    XSFLIB_CHECK(same.entries.size() == 2);
    XSFLIB_CHECK(same.priority == 5);
    XSFLIB_CHECK(same.reverb == 0x80 + 40);
    XSFLIB_CHECK(hokuto::format::ExportArchive(hokuto::edit::WithGbaMidiSong(file, places, 0, same)) == bytes);

    // Key 60 of the first note becomes key 62.
    const std::size_t at = text.find("N24 , Cn3 , v100");
    SEQLIB_REQUIRE(at != std::string::npos);
    text.replace(at, 16, "N24 , Dn3 , v100");
    const std::vector<uint8_t> changed = hokuto::format::ExportArchive(
        hokuto::edit::WithGbaMidiSong(file, places, 0, hokuto::edit::GbaSongOfAssembly(text)));
    SEQLIB_REQUIRE(changed.size() == bytes.size());
    std::vector<std::size_t> differences;
    for (std::size_t i = 0; i < bytes.size(); i++)
    {
        if (bytes[i] != changed[i])
        {
            differences.push_back(i);
        }
    }
    SEQLIB_REQUIRE(differences.size() == 1);
    XSFLIB_CHECK(bytes[differences[0]] == 60);
    XSFLIB_CHECK(changed[differences[0]] == 62);

    // A file of three tracks for a header of two.
    const std::string three = "a:\n\t.byte FINE\n\t.align 2\nhead:\n\t.byte 3, 0, 0, 0\n\t.word 0\n\t.word a, a, a\n";
    bool refused = false;
    try
    {
        static_cast<void>(hokuto::edit::WithGbaMidiSong(file, places, 0, hokuto::edit::GbaSongOfAssembly(three)));
    }
    catch (const std::invalid_argument& error)
    {
        refused =
            std::string(error.what()) ==
            "The song header has room for 2 tracks, but the new song has 3. Hokuto can't add tracks to a header yet.";
    }
    XSFLIB_CHECK(refused);

    // Bytes of a MIDI file aren't assembly.
    const std::vector<uint8_t> midi = {'M', 'T', 'h', 'd', 0, 0, 0, 6};
    XSFLIB_CHECK(!hokuto::edit::IsGbaAssembly(midi));
    const std::vector<uint8_t> binary = {'a', 0, 'b'};
    XSFLIB_CHECK(!hokuto::edit::IsGbaAssembly(binary));
}

// The assembly of a song past the ROM's songs is refused with a message that names the song.
void TestRefusedSong()
{
    const std::shared_ptr<const hokuto::song::GbaRom> rom = hokuto::song::LoadGbaRom(MakeSong());
    bool refused = false;
    try
    {
        GbaSongAssembly(*rom, 200, "none");
    }
    catch (const std::invalid_argument& error)
    {
        refused = std::string(error.what()).find("Song 200") != std::string::npos;
    }
    XSFLIB_CHECK(refused);
}

// A ROM of one song that plays a voice group of a sample, a wave voice, a key split and a drum kit.
struct GroupRom
{
    std::vector<uint8_t> bytes;
    uint32_t group = 0;
    uint32_t sample = 0;
    uint32_t pattern = 0;
    uint32_t split_table = 0;
    uint32_t key_table = 0;
    uint32_t kit = 0;
};

GroupRom MakeGroupRom()
{
    using enum mp2k::Command;
    using mp2k_cart::Cart;

    Cart cart(mp2k_cart::kMode);
    GroupRom made;
    made.sample = cart.Wave({1, 2, 3, 4, 5, 6, 7, 8}, 13379, 4);
    made.pattern =
        cart.Place({0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10});
    made.kit = cart.VoiceGroup({{60, Cart::Voice(0, made.sample, 72, {255, 0, 255, 165})}});
    made.split_table = cart.VoiceGroup({{0, Cart::Voice(0, made.sample, 48, {255, 0, 255, 165})},
                                        {1, Cart::Voice(0, made.sample, 72, {255, 0, 255, 165})}});
    const std::vector<uint8_t> split = cart.KeySplit(made.split_table, 59);
    made.key_table =
        uint32_t{split[8]} | uint32_t{split[9]} << 8 | uint32_t{split[10]} << 16 | uint32_t{split[11]} << 24;
    made.group = cart.VoiceGroup({{0, Cart::Voice(0, made.sample, 60, {255, 0, 255, 165})},
                                  {1, Cart::Voice(3, made.pattern, 60, {0, 0, 15, 0})},
                                  {2, split},
                                  {3, Cart::Voice(mp2k::kVoiceDrumKit, made.kit)}});

    mp2k_cart::Track track;
    track.Cmd(kCmdVol, 100);
    for (int program = 0; program < 4; program++)
    {
        track.Cmd(kCmdVoice, static_cast<uint8_t>(program)).Note(12, 60, 100).Wait(12);
    }
    track.Fine();
    cart.Song({track}, made.group);

    const ntdlib::agb::Rom rom = cart.ToRom();
    made.bytes = hokuto::testing::RomBytes(rom);
    return made;
}

// Ends the test where a function that returns a value can't go on.
void Must(bool holds)
{
    if (!holds)
    {
        throw std::logic_error("the test's setup doesn't hold");
    }
}

// `text` with the first `from` after `label` as `to`.
std::string Replaced(std::string text, std::string_view label, std::string_view from, std::string_view to)
{
    std::size_t at = text.find(label);
    Must(at != std::string::npos);
    at = text.find(from, at);
    Must(at != std::string::npos);
    text.replace(at, from.size(), to);
    return text;
}

// The name that the writer gives the address.
std::string NameOf(std::string_view prefix, uint32_t address)
{
    std::array<char, 16> digits{};
    std::snprintf(digits.data(), digits.size(), "%08X", address);
    return std::string(prefix) + digits.data();
}

// Voice `program` of the voice group at `group` in the ROM.
mp2k::Voice GroupVoice(const hokuto::song::GbaRom& from, uint32_t group, uint32_t program)
{
    mp2k::Voice voice;
    Must(mp2k::ReadVoice(from.rom, group + program * 12, voice));
    return voice;
}

// A ROM from MakeGroupRom, the ROM loaded and imported, and the voice groups in it.
struct VoiceGroupFileFixture
{
    GroupRom made;
    std::shared_ptr<const hokuto::song::GbaRom> rom;
    hokuto::format::File file;
    std::vector<hokuto::song::GbaVoiceGroup> groups;
};

// The fixture of the tests of the assembly file of a voice group.
VoiceGroupFileFixture MakeVoiceGroupFileFixture()
{
    const GroupRom made = MakeGroupRom();
    const std::shared_ptr<const hokuto::song::GbaRom> rom = hokuto::song::LoadGbaRom(made.bytes);
    const hokuto::format::File file = hokuto::format::ImportGbaRom(made.bytes);
    const std::vector<hokuto::song::GbaVoiceGroup> groups = hokuto::song::GbaVoiceGroups(*rom);

    return {made, rom, file, groups};
}

// A voice group's assembly file names each voice, the drum kit's 128 voices, the split's two, the key table and the
// pattern. The file as it is leaves the ROM unchanged.
void TestVoiceGroupFileUnedited()
{
    const VoiceGroupFileFixture fixture = MakeVoiceGroupFileFixture();
    const GroupRom& made = fixture.made;
    const std::shared_ptr<const hokuto::song::GbaRom>& rom = fixture.rom;
    const hokuto::format::File& file = fixture.file;
    const std::vector<hokuto::song::GbaVoiceGroup>& groups = fixture.groups;

    SEQLIB_REQUIRE(groups.size() == 1);
    SEQLIB_REQUIRE(groups[0].address == made.group);

    const std::string text = hokuto::song::GbaVoiceGroupAssembly(*rom, groups[0]);

    const hokuto::song::GbaVoiceGroupSource source = hokuto::song::ReadGbaVoiceGroup(text);
    XSFLIB_CHECK(source.name == NameOf("voicegroup_", made.group));
    XSFLIB_CHECK(source.voices == 4);

    // Program 0, with the sample's address from the name of the sample's word.
    const uint32_t sample = made.sample;
    const std::vector<uint8_t> first = {0,
                                        60,
                                        0,
                                        0,
                                        static_cast<uint8_t>(sample),
                                        static_cast<uint8_t>(sample >> 8),
                                        static_cast<uint8_t>(sample >> 16),
                                        static_cast<uint8_t>(sample >> 24),
                                        255,
                                        0,
                                        255,
                                        165};
    XSFLIB_CHECK(
        std::equal(first.begin(), first.end(), source.bytes.begin() + static_cast<std::ptrdiff_t>(source.table)));

    // The drum kit's 128 voices, the split's two, the key table's 128 keys and the pattern's 16 bytes.
    std::map<std::string, std::size_t> sizes;
    for (const hokuto::song::GbaVoiceGroupPart& part : source.parts)
    {
        sizes[part.label] = part.size;
    }
    XSFLIB_CHECK(sizes.size() == 4);
    XSFLIB_CHECK(sizes[NameOf("voicegroup_", made.kit)] == 128 * 12);
    XSFLIB_CHECK(sizes[NameOf("voicegroup_", made.split_table)] == 2 * 12);
    XSFLIB_CHECK(sizes[NameOf("KeySplitTable_", made.key_table)] == 128);
    XSFLIB_CHECK(sizes[NameOf("ProgrammableWaveData_", made.pattern)] == 16);

    // The file as it is leaves the ROM unchanged.
    const hokuto::edit::GbaGroupVersion same = hokuto::edit::WithGbaVoiceGroupFile(file, *rom, made.group, text);
    XSFLIB_CHECK(same.group == made.group);
    XSFLIB_CHECK(hokuto::format::ExportArchive(same.file) == made.bytes);
}

// An edited file puts the group's table in a new place, and only the parts that change move.
void TestVoiceGroupFileRelease()
{
    const VoiceGroupFileFixture fixture = MakeVoiceGroupFileFixture();
    const GroupRom& made = fixture.made;
    const std::shared_ptr<const hokuto::song::GbaRom>& rom = fixture.rom;
    const hokuto::format::File& file = fixture.file;
    const std::vector<hokuto::song::GbaVoiceGroup>& groups = fixture.groups;

    SEQLIB_REQUIRE(groups.size() == 1);
    SEQLIB_REQUIRE(groups[0].address == made.group);

    const std::string text = hokuto::song::GbaVoiceGroupAssembly(*rom, groups[0]);

    // A new release of program 0. The table of the group is written with the edit to a new place among the songs. The
    // song's header names the new place, and the other voices keep their tables and their pattern.
    const hokuto::edit::GbaGroupVersion release = hokuto::edit::WithGbaVoiceGroupFile(
        file, *rom, made.group,
        Replaced(text, "@ Program 0", "0xFF , 0x00 , 0xFF , 0xA5", "0xFF , 0x00 , 0xFF , 0xA6"));
    const std::shared_ptr<const hokuto::song::GbaRom> with_release =
        hokuto::song::LoadGbaRom(hokuto::format::ExportArchive(release.file));
    XSFLIB_CHECK(release.group != made.group);
    mp2k::SongHeader header;
    SEQLIB_REQUIRE(mp2k::ReadSong(with_release->rom, with_release->info, 0, header));
    XSFLIB_CHECK(header.voices == release.group);
    XSFLIB_CHECK(GroupVoice(*with_release, release.group, 0).release == 166);
    XSFLIB_CHECK(GroupVoice(*with_release, release.group, 0).wave == made.sample);
    XSFLIB_CHECK(GroupVoice(*with_release, release.group, 1).wave == made.pattern);
    XSFLIB_CHECK(GroupVoice(*with_release, release.group, 2).wave == made.split_table);
    XSFLIB_CHECK(GroupVoice(*with_release, release.group, 2).key_table == made.key_table);
    XSFLIB_CHECK(GroupVoice(*with_release, release.group, 3).wave == made.kit);
}

// An edited key table moves to a new place.
void TestVoiceGroupFileKeyTable()
{
    const VoiceGroupFileFixture fixture = MakeVoiceGroupFileFixture();
    const GroupRom& made = fixture.made;
    const std::shared_ptr<const hokuto::song::GbaRom>& rom = fixture.rom;
    const hokuto::format::File& file = fixture.file;
    const std::vector<hokuto::song::GbaVoiceGroup>& groups = fixture.groups;

    SEQLIB_REQUIRE(groups.size() == 1);
    SEQLIB_REQUIRE(groups[0].address == made.group);

    const std::string text = hokuto::song::GbaVoiceGroupAssembly(*rom, groups[0]);

    // Key 0 of the key table plays the split's second voice. Only the key table moves.
    const hokuto::edit::GbaGroupVersion keys = hokuto::edit::WithGbaVoiceGroupFile(
        file, *rom, made.group, Replaced(text, NameOf("KeySplitTable_", made.key_table) + ":", "0x00", "0x01"));
    const std::shared_ptr<const hokuto::song::GbaRom> with_keys =
        hokuto::song::LoadGbaRom(hokuto::format::ExportArchive(keys.file));
    const mp2k::Voice split = GroupVoice(*with_keys, keys.group, 2);
    XSFLIB_CHECK(split.wave == made.split_table);
    XSFLIB_CHECK(split.key_table != made.key_table);
    XSFLIB_CHECK(mp2k::SplitVoice(with_keys->rom, split, 0) == made.split_table + 12);
    XSFLIB_CHECK(mp2k::SplitVoice(with_keys->rom, split, 1) == made.split_table);
    XSFLIB_CHECK(mp2k::SplitVoice(with_keys->rom, split, 60) == made.split_table + 12);
    XSFLIB_CHECK(GroupVoice(*with_keys, keys.group, 1).wave == made.pattern);
    XSFLIB_CHECK(GroupVoice(*with_keys, keys.group, 3).wave == made.kit);
}

// An edited pattern moves to a new place.
void TestVoiceGroupFilePattern()
{
    const VoiceGroupFileFixture fixture = MakeVoiceGroupFileFixture();
    const GroupRom& made = fixture.made;
    const std::shared_ptr<const hokuto::song::GbaRom>& rom = fixture.rom;
    const hokuto::format::File& file = fixture.file;
    const std::vector<hokuto::song::GbaVoiceGroup>& groups = fixture.groups;

    SEQLIB_REQUIRE(groups.size() == 1);
    SEQLIB_REQUIRE(groups[0].address == made.group);

    const std::string text = hokuto::song::GbaVoiceGroupAssembly(*rom, groups[0]);

    // A new first byte of the pattern. Only the wave voice's pattern moves.
    const hokuto::edit::GbaGroupVersion pattern = hokuto::edit::WithGbaVoiceGroupFile(
        file, *rom, made.group, Replaced(text, NameOf("ProgrammableWaveData_", made.pattern) + ":", "0x01", "0x11"));
    const std::shared_ptr<const hokuto::song::GbaRom> with_pattern =
        hokuto::song::LoadGbaRom(hokuto::format::ExportArchive(pattern.file));
    const uint32_t moved = GroupVoice(*with_pattern, pattern.group, 1).wave;
    XSFLIB_CHECK(moved != made.pattern);
    SEQLIB_REQUIRE(with_pattern->rom.Contains(moved, 16));
    XSFLIB_CHECK(with_pattern->rom.U8(moved) == 0x11);
    XSFLIB_CHECK(with_pattern->rom.U8(moved + 1) == 0x23);
    XSFLIB_CHECK(with_pattern->rom.U8(moved + 15) == 0x10);
    XSFLIB_CHECK(with_pattern->rom.U8(made.pattern) == 0x01);
    XSFLIB_CHECK(GroupVoice(*with_pattern, pattern.group, 2).wave == made.split_table);

    // A voice of the game's can hold a length that an edit refuses. It keeps the length when its pattern moves.
    std::vector<uint8_t> long_bytes = made.bytes;
    long_bytes[made.group - ntdlib::agb::kRomBase + 12 + 2] = 0xE2;
    const std::shared_ptr<const hokuto::song::GbaRom> long_rom = hokuto::song::LoadGbaRom(long_bytes);
    const hokuto::edit::GbaGroupVersion long_pattern = hokuto::edit::WithGbaVoiceGroupFile(
        hokuto::format::ImportGbaRom(long_bytes), *long_rom, made.group,
        Replaced(hokuto::song::GbaVoiceGroupAssembly(*long_rom, groups[0]),
                 NameOf("ProgrammableWaveData_", made.pattern) + ":", "0x01", "0x11"));
    const std::shared_ptr<const hokuto::song::GbaRom> with_long =
        hokuto::song::LoadGbaRom(hokuto::format::ExportArchive(long_pattern.file));
    XSFLIB_CHECK(GroupVoice(*with_long, long_pattern.group, 1).wave != made.pattern);
    XSFLIB_CHECK(GroupVoice(*with_long, long_pattern.group, 1).length == 0xE2);
}

// An edited drum kit moves to a new place.
void TestVoiceGroupFileKit()
{
    const VoiceGroupFileFixture fixture = MakeVoiceGroupFileFixture();
    const GroupRom& made = fixture.made;
    const std::shared_ptr<const hokuto::song::GbaRom>& rom = fixture.rom;
    const hokuto::format::File& file = fixture.file;
    const std::vector<hokuto::song::GbaVoiceGroup>& groups = fixture.groups;

    SEQLIB_REQUIRE(groups.size() == 1);
    SEQLIB_REQUIRE(groups[0].address == made.group);

    const std::string text = hokuto::song::GbaVoiceGroupAssembly(*rom, groups[0]);

    // A sound of the kit at another key moves the kit's table, and the voice names the table's new place.
    const hokuto::edit::GbaGroupVersion kit = hokuto::edit::WithGbaVoiceGroupFile(
        file, *rom, made.group, Replaced(text, "@ Entry 60\n\t.byte\t0x00 , 0x48", "0x48", "0x49"));
    const std::shared_ptr<const hokuto::song::GbaRom> with_kit =
        hokuto::song::LoadGbaRom(hokuto::format::ExportArchive(kit.file));
    const mp2k::Voice drums = GroupVoice(*with_kit, kit.group, 3);
    XSFLIB_CHECK(drums.wave != made.kit);
    mp2k::Voice hit;
    SEQLIB_REQUIRE(mp2k::ReadVoice(with_kit->rom, mp2k::SplitVoice(with_kit->rom, drums, 60), hit));
    XSFLIB_CHECK(hit.key == 0x49);
    XSFLIB_CHECK(hit.wave == made.sample);
    XSFLIB_CHECK(GroupVoice(*with_kit, kit.group, 2).wave == made.split_table);
}

// A voice group's text names its table with `.global` or else with its first label, and a label in `placed` stands for
// that address. A project's macros and names don't come in, and the message names the line.
void TestVoiceGroupText()
{
    // Without `.global`, the first label names the table. A label can take two colons. The files of some projects write
    // labels that way.
    const hokuto::song::GbaVoiceGroupSource plain = hokuto::song::ReadGbaVoiceGroup(
        "\t.section .rodata\n"
        "vg::\n"
        "\t.byte 0, 60, 0, 0\n"
        "\t.4byte DirectSoundWaveData_08123456\n"
        "\t.byte 255, 0, 255, 165\n"
        "kit:\n"
        "\t.byte 1, 2\n");
    XSFLIB_CHECK(plain.name == "vg");
    XSFLIB_CHECK(plain.voices == 1);
    XSFLIB_CHECK(plain.table == 0);
    XSFLIB_CHECK(plain.bytes.size() == 14);
    XSFLIB_CHECK(plain.bytes[4] == 0x56 && plain.bytes[5] == 0x34 && plain.bytes[6] == 0x12 && plain.bytes[7] == 0x08);
    SEQLIB_REQUIRE(plain.parts.size() == 1);
    XSFLIB_CHECK(plain.parts[0].label == "kit");
    XSFLIB_CHECK(plain.parts[0].offset == 12);
    XSFLIB_CHECK(plain.parts[0].size == 2);

    // `.global` names the table, and a label in `placed` stands for that address.
    const std::string text = "\t.global b\na:\n\t.byte 9, 9, 9, 9\nb:\n\t.word a, 0, 0\n";
    const hokuto::song::GbaVoiceGroupSource global = hokuto::song::ReadGbaVoiceGroup(text);
    XSFLIB_CHECK(global.name == "b");
    XSFLIB_CHECK(global.table == 4);
    XSFLIB_CHECK(global.voices == 1);
    const hokuto::song::GbaVoiceGroupSource placed = hokuto::song::ReadGbaVoiceGroup(text, {{"a", 0x08000010}});
    XSFLIB_CHECK(placed.bytes[4] == 0x10 && placed.bytes[7] == 0x08);

    XSFLIB_CHECK(hokuto::song::GbaAddressOfName("voicegroup_08ABCDEF") == 0x08ABCDEF);
    XSFLIB_CHECK(!hokuto::song::GbaAddressOfName("voicegroup000").has_value());
    XSFLIB_CHECK(!hokuto::song::GbaAddressOfName("name_0812345").has_value());
    XSFLIB_CHECK(!hokuto::song::GbaAddressOfName("name_0812345G").has_value());

    const auto refusal = [](const std::string& refused)
    {
        try
        {
            hokuto::song::ReadGbaVoiceGroup(refused);
        }
        catch (const std::invalid_argument& error)
        {
            return std::string(error.what());
        }
        return std::string();
    };
    // A project's macros and its names don't come in, and the message names the line.
    XSFLIB_CHECK(refusal("vg:\n\tvoice_directsound 60, 0, sample, 255, 0, 255, 165\n").starts_with("Line 2 "));
    XSFLIB_CHECK(
        refusal("vg:\n\t.byte 0, 60, 0, 0\n\t.4byte some_sample\n\t.byte 255, 0, 255, 165\n").starts_with("Line 3 "));
    XSFLIB_CHECK(refusal("vg:\n\t.byte 0, 60, 0\n").find("whole number of voices") != std::string::npos);
    std::string too_many = "vg:\n";
    for (int i = 0; i < 129; i++)
    {
        too_many += "\t.word 0, 0, 0\n";
    }
    XSFLIB_CHECK(refusal(too_many).find("up to 128") != std::string::npos);
    XSFLIB_CHECK(refusal("\t.byte 1\n").find("no label") != std::string::npos);
}

// A ROM from MakeGroupRom, the ROM loaded, the bytes expected of the sample file and the bytes that GbaSampleBin
// returns.
struct SampleFixture
{
    GroupRom made;
    std::shared_ptr<const hokuto::song::GbaRom> rom;
    std::vector<uint8_t> expected;
    std::vector<uint8_t> bin;
};

// The fixture of the tests of GBA samples.
SampleFixture MakeSampleFixture()
{
    const GroupRom made = MakeGroupRom();
    const std::shared_ptr<const hokuto::song::GbaRom> rom = hokuto::song::LoadGbaRom(made.bytes);

    // The header, the 8 samples and the byte after them: the pattern's first.
    const std::vector<uint8_t> expected = {0, 0, 0, 0x40, 0, 0x0C, 0xD1, 0, 4, 0, 0, 0, 8,
                                           0, 0, 0, 1,    2, 3,    4,    5, 6, 7, 8, 1};
    const std::vector<uint8_t> bin = hokuto::song::GbaSampleBin(*rom, made.sample);

    return {made, rom, expected, bin};
}

// A GBA sample exports as its bytes, as an AIFF file with markers at the loop and as a WAV file, and the file of a
// sample imports as it is. Padding is dropped, and the byte after the samples repeats the first sample of the loop.
void TestSampleBin()
{
    const SampleFixture fixture = MakeSampleFixture();
    const GroupRom& made = fixture.made;
    const std::shared_ptr<const hokuto::song::GbaRom>& rom = fixture.rom;
    const std::vector<uint8_t>& expected = fixture.expected;
    const std::vector<uint8_t>& bin = fixture.bin;

    XSFLIB_CHECK(bin == expected);
    XSFLIB_CHECK(hokuto::song::IsGbaSampleFile(bin));
    XSFLIB_CHECK(hokuto::song::GbaSampleOfFile(bin) == expected);

    // Padding is dropped, and a file without the byte after the samples gets the first sample of the loop.
    std::vector<uint8_t> padded = bin;
    padded.insert(padded.end(), 3, 0);
    XSFLIB_CHECK(hokuto::song::GbaSampleOfFile(padded) == expected);
    std::vector<uint8_t> short_file(bin.begin(), bin.end() - 1);
    std::vector<uint8_t> filled = expected;
    filled.back() = 5;
    XSFLIB_CHECK(hokuto::song::GbaSampleOfFile(short_file) == filled);

    XSFLIB_CHECK(hokuto::song::GbaVoiceGroupSamples(*rom) == std::vector<uint32_t>{made.sample});
}

// A GBA sample exports as a WAV file with the loop.
void TestSampleWav()
{
    const SampleFixture fixture = MakeSampleFixture();
    const GroupRom& made = fixture.made;
    const std::shared_ptr<const hokuto::song::GbaRom>& rom = fixture.rom;
    const std::vector<uint8_t>& expected = fixture.expected;

    const seqlib::WavContents wav = seqlib::ReadWav(hokuto::song::GbaSampleWav(*rom, made.sample));
    SEQLIB_REQUIRE(wav.pcm.channels.size() == 1);
    XSFLIB_CHECK(wav.rate == 13379);
    XSFLIB_CHECK(wav.pcm.channels[0].size() == 8);
    for (std::size_t i = 0; i < wav.pcm.channels[0].size(); i++)
    {
        XSFLIB_CHECK(wav.pcm.channels[0][i] == static_cast<int8_t>(expected[16 + i]) * 256);
    }
    XSFLIB_CHECK(wav.pcm.loop);
    XSFLIB_CHECK(wav.pcm.loop_end == 8);
}

// A GBA sample exports as an AIFF file with markers at the loop. The AIFF file imports again as it is.
void TestSampleAiff()
{
    const SampleFixture fixture = MakeSampleFixture();
    const GroupRom& made = fixture.made;
    const std::shared_ptr<const hokuto::song::GbaRom>& rom = fixture.rom;
    const std::vector<uint8_t>& expected = fixture.expected;
    const std::vector<uint8_t>& bin = fixture.bin;

    const std::vector<uint8_t> aiff = hokuto::song::GbaSampleAiff(*rom, made.sample);
    const auto chunk = [&aiff](std::string_view id)
    {
        const auto found = std::search(aiff.begin(), aiff.end(), id.begin(), id.end(),
                                       [](uint8_t a, char b) { return static_cast<char>(a) == b; });
        Must(found != aiff.end());
        return static_cast<std::size_t>(found - aiff.begin()) + 8;
    };
    XSFLIB_CHECK(std::string(aiff.begin(), aiff.begin() + 4) == "FORM");
    XSFLIB_CHECK(std::string(aiff.begin() + 8, aiff.begin() + 12) == "AIFF");

    // One channel, 9 frames and 8 bits. 13,379 Hz is 0x3443: an exponent of 13 over 0x3FFF, and the mantissa from the
    // top bit on.
    const std::vector<uint8_t> common = {0, 1, 0, 0, 0, 9, 0, 8, 0x40, 0x0C, 0xD1, 0x0C, 0, 0, 0, 0, 0, 0};
    XSFLIB_CHECK(std::equal(common.begin(), common.end(), aiff.begin() + static_cast<std::ptrdiff_t>(chunk("COMM"))));

    // Two markers, at the loop's start and the end.
    const std::size_t marks = chunk("MARK");
    XSFLIB_CHECK(aiff[marks + 1] == 2);
    XSFLIB_CHECK(aiff[marks + 7] == 4);
    const std::size_t sound = chunk("SSND") + 8;
    XSFLIB_CHECK(std::equal(bin.begin() + 16, bin.end(), aiff.begin() + static_cast<std::ptrdiff_t>(sound)));

    XSFLIB_CHECK(hokuto::song::IsGbaSampleFile(aiff));
    XSFLIB_CHECK(hokuto::song::GbaSampleOfFile(aiff) == expected);

    // The file of a sample imports as it is.
    const hokuto::edit::IncomingWave incoming = hokuto::edit::ReadIncomingGbaSample(aiff);
    XSFLIB_CHECK(incoming.bytes == expected);
    XSFLIB_CHECK(incoming.rate == 13379);
    XSFLIB_CHECK(incoming.channels == 1);
    XSFLIB_CHECK(incoming.frames == 8);
    XSFLIB_CHECK(incoming.loop);
    XSFLIB_CHECK(incoming.loop_start == 4);
}

// Only the voice of program 0 plays a sample.
void TestVoiceSample()
{
    const SampleFixture fixture = MakeSampleFixture();
    const GroupRom& made = fixture.made;
    const std::shared_ptr<const hokuto::song::GbaRom>& rom = fixture.rom;

    // A voice's sample, for the Export window.
    const auto voice_at = [&rom, &made](uint32_t program)
    {
        mp2k::Voice voice;
        Must(mp2k::ReadVoice(rom->rom, made.group + program * 12, voice));
        return voice;
    };
    XSFLIB_CHECK(hokuto::song::GbaVoiceSample(*rom, voice_at(0)) == made.sample);
    XSFLIB_CHECK(!hokuto::song::GbaVoiceSample(*rom, voice_at(1)).has_value());
    XSFLIB_CHECK(!hokuto::song::GbaVoiceSample(*rom, voice_at(2)).has_value());
    XSFLIB_CHECK(!hokuto::song::GbaVoiceSample(*rom, voice_at(3)).has_value());
}

// GbaSampleBin refuses a compressed sample.
void TestCompressedSample()
{
    // A compressed sample exports to neither form.
    mp2k_cart::Cart cart(mp2k_cart::kMode);
    const uint32_t compressed = cart.CompressedWave(std::vector<uint8_t>(33, 0), 64);
    const uint32_t voices = cart.VoiceGroup({{0, mp2k_cart::Cart::Voice(mp2k::kVoiceCompressed, compressed)}});
    mp2k_cart::Track track;
    track.Cmd(mp2k::kCmdVol, 100).Cmd(mp2k::kCmdVoice, 0).Note(12, 60, 100).Wait(12).Fine();
    cart.Song({track}, voices);
    const ntdlib::agb::Rom raw = cart.ToRom();
    const std::vector<uint8_t> compressed_bytes = hokuto::testing::RomBytes(raw);
    const std::shared_ptr<const hokuto::song::GbaRom> compressed_rom = hokuto::song::LoadGbaRom(compressed_bytes);
    const bool refused = !xsflib::testing::ErrorOf<std::invalid_argument>(
                              [&] { hokuto::song::GbaSampleBin(*compressed_rom, compressed); })
                              .empty();
    XSFLIB_CHECK(refused);
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("gba_assembly",
                                    []
                                    {
                                        TestHandWritten();
                                        TestExpressions();
                                        TestRefusals();
                                        TestOctal();
                                        TestOfSong();
                                        TestImport();
                                        TestRefusedSong();
                                        TestVoiceGroupFileUnedited();
                                        TestVoiceGroupFileRelease();
                                        TestVoiceGroupFileKeyTable();
                                        TestVoiceGroupFilePattern();
                                        TestVoiceGroupFileKit();
                                        TestVoiceGroupText();
                                        TestSampleBin();
                                        TestSampleWav();
                                        TestSampleAiff();
                                        TestVoiceSample();
                                        TestCompressedSample();
                                    });
}
