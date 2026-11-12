// SPDX-License-Identifier: MIT

// Tests the rest of a File in both forms of Hokuto's files (src/format/file.cpp and text.cpp). A file made by hand
// holds every unusual part that a command can have, labels whose names take quotes, an empty sequence, effects with
// unusual floats, tables with unusual values and chunks that Hokuto doesn't read. Each comes back unchanged through
// both forms. Damaged files and unknown parts throw with a message. The text form reads a command's value as the
// nearest 2^-16, and an effect's parameter as the nearest float. Copies of a file share its parts. Both forms keep the
// tracks' colours, the instrument maps of the tracks and the banks, the timelines, and the models of edited banks and
// waves.

#include <clocale>

#include "format_testing.h"

namespace
{

struct UnusualFixture
{
    File file;
    std::vector<uint8_t> binary;
    std::string text;
};

// The file that Unusual makes, in each form.
UnusualFixture MakeUnusualFixture()
{
    const File file = Unusual();

    const std::vector<uint8_t> binary = format::Write(file);
    const std::string text = format::WriteText(file);

    return {file, binary, text};
}

// The file that Unusual makes comes back through both forms.
void TestUnusualRoundTrip()
{
    const UnusualFixture fixture = MakeUnusualFixture();
    const File& file = fixture.file;
    const std::vector<uint8_t>& binary = fixture.binary;
    const std::string& text = fixture.text;

    XSFLIB_CHECK(format::Read(binary) == file);
    XSFLIB_CHECK(format::ReadText(text) == file);
    XSFLIB_CHECK(format::WriteText(format::ReadText(text)) == text);
}

// The binary form holds version 1.0, the group files in GRPF and the effects in MIXR.
void TestUnusualChunks()
{
    const UnusualFixture fixture = MakeUnusualFixture();
    const std::vector<uint8_t>& binary = fixture.binary;
    const std::string& text = fixture.text;

    // Version 1.1, and the group files in the GRPF chunk before the other chunks: their count, then each one's group,
    // the game's size and SHA-1, its pieces and its fields. The pieces and the fields take the forms of those in NATV.
    // NATV holds the sequence's encodings.
    XSFLIB_CHECK(binary[8] == 1 && binary[9] == 0 && binary[10] == 1 && binary[11] == 0);
    XSFLIB_CHECK(text.starts_with("hok 1.1\n") &&
                 text.find(std::string("\nchunk GRPF 160\n") + kGroupFilesText + "\nchunk XTRA") != std::string::npos);
    XSFLIB_CHECK(std::ranges::equal(ChunkBytes(binary, 5), kGroupFilesChunk));

    // The effects in the MIXR chunk after ENGN: for each bus, the number of its effects, then each effect's kind, three
    // zeros, the number of its parameters and each parameter's 32 bits.
    const std::vector<std::string> kIds = {"ENGN", "MIXR", "SEQS", "NATV", "TABL", "GRPF", "XTRA", "x y!"};
    const std::span<const uint8_t> mixer = ChunkBytes(binary, 1);
    XSFLIB_CHECK(ChunkIds(binary) == kIds);
    SEQLIB_REQUIRE(mixer.size() == 4 + 56 + 4 + 4 + 56 * 2);
    XSFLIB_CHECK(xsflib::LoadLe32(&mixer[0]) == 1 && xsflib::LoadLe32(&mixer[4]) == 1 &&
                 xsflib::LoadLe32(&mixer[8]) == 12);
    XSFLIB_CHECK(xsflib::LoadLe32(&mixer[12]) == 0x80000000u && xsflib::LoadLe32(&mixer[16]) == 0x7F7FFFFFu &&
                 xsflib::LoadLe32(&mixer[20]) == 1);
    XSFLIB_CHECK(xsflib::LoadLe32(&mixer[60]) == 0 && xsflib::LoadLe32(&mixer[64]) == 2);
}

// The text form writes each float with the fewest digits that read back as the float. It gives the same text where the
// decimal point is a comma.
void TestUnusualFloats()
{
    const UnusualFixture fixture = MakeUnusualFixture();
    const File& file = fixture.file;
    const std::string& text = fixture.text;

    // The effects after the mixing mode, bus by bus. Each parameter takes the fewest digits that read back as its
    // float, with an exponent only for a size below 0.00001 or from 10^15.
    XSFLIB_CHECK(text.find("\nmixing enhanced\neffect A i3dl2_reverb room_gain -0 room_hf_gain 3.4028235e+38 "
                           "late_reverb_decay_time 1e-45 late_reverb_hf_decay_ratio 0.1 reflections_gain -1234.5 "
                           "reflections_delay 0.33333334 reverb_gain -3.4028235e+38 late_reverb_delay 2.5 "
                           "late_reverb_diffusion 100 late_reverb_density 0.00001 hf_reference 5000 dry_gain 1\n"
                           "effect C i3dl2_reverb room_gain 0 room_hf_gain 0 late_reverb_decay_time 0 "
                           "late_reverb_hf_decay_ratio 0 reflections_gain 0 reflections_delay 0 reverb_gain 0 "
                           "late_reverb_delay 0 late_reverb_diffusion 0 late_reverb_density 0 hf_reference 0 "
                           "dry_gain 0\neffect C i3dl2_reverb room_gain 1e+15 room_hf_gain 100000000000000 "
                           "late_reverb_decay_time 1e-06 late_reverb_hf_decay_ratio 123456790 reflections_gain -0.5 "
                           "reflections_delay 7 reverb_gain 65536 late_reverb_delay 0.0009765625 "
                           "late_reverb_diffusion 3 late_reverb_density 1e-38 hf_reference 20000 dry_gain 0.75\n\n"
                           "sequence 0\n") != std::string::npos);

    // The same text in a locale whose decimal point is a comma, where the system has one.
    for (const char* locale : {"de_DE.UTF-8", "German_Germany.1252"})
    {
        if (std::setlocale(LC_NUMERIC, locale) && std::string_view(std::localeconv()->decimal_point) == ",")
        {
            const std::string written = format::WriteText(file);
            const File read = format::ReadText(text);
            std::setlocale(LC_NUMERIC, "C");
            XSFLIB_CHECK(written == text && read == file);
        }
        std::setlocale(LC_NUMERIC, "C");
    }
}

// The text holds the tables' lines, the labels, the bracketed parts, and the native archive's pieces and fields.
void TestUnusualText()
{
    const UnusualFixture fixture = MakeUnusualFixture();
    const std::string& text = fixture.text;

    // The tables after the fields. Each line leaves out the defaults and gives the item's ID. Banks come last.
    XSFLIB_CHECK(text.find("\n    tables\n        sound Song sequence player 0x4000001 volume 0 pan_mode 1 pan_curve 2 "
                           "player_priority 127 actor_player 3 channel_priority 0 tracks 0xFFFF release_priority_fix "
                           "banks 0x3000000 none 0x3000001 # ID 0x1000000\n        sound \"\" unknown # ID 0x1000001\n"
                           "        sound \"two words\" wave_sound volume 255 # ID 0x1000002\n"
                           "        sound sound stream # ID 0x1000003\n        bank BANK # ID 0x3000000\n") !=
                 std::string::npos);
    XSFLIB_CHECK(text.find("\n        bank \"\\\"quoted\\\"\" # ID 0x3000001\n        player PLAYER sound_limit 4 # ID "
                           "0x4000000\n        player \"\" sound_limit 0 # ID 0x4000001\n        group GROUP # ID "
                           "0x6000000\n\nchunk GRPF") != std::string::npos);

    // The labels' order, the local label, the quoted names and the bracketed parts.
    XSFLIB_CHECK(text.find("\n    labels Song \"\\\"odd\\\" name\" ") != std::string::npos);
    XSFLIB_CHECK(text.find("\n    .L1:\n        pan") == std::string::npos);
    XSFLIB_CHECK(text.find("\n    .L1:\n        if pan -64\n") != std::string::npos);
    XSFLIB_CHECK(text.find("\n        open_track 1 .L1\n") != std::string::npos);
    XSFLIB_CHECK(text.find("\n    \"over\":\n") != std::string::npos);
    XSFLIB_CHECK(text.find("\n    \"\\xFF bad\":\n") != std::string::npos);
    XSFLIB_CHECK(text.find("volume random(64,127) over var(3)") != std::string::npos);
    XSFLIB_CHECK(text.find("call @100") != std::string::npos &&
                 text.find("pitch_bend 0.0078125\n") != std::string::npos);
    XSFLIB_CHECK(text.find("sweep_pitch 1.5625 [value_bytes 3] [unused_time 0xA3] [unused_random] [unused_variable]") !=
                 std::string::npos);
    XSFLIB_CHECK(
        text.find("end [index 3] [key 9] [velocity 1] [value var(2)] [target 1] [opcode 0x12] [bytes 01 02]") !=
        std::string::npos);
    XSFLIB_CHECK(text.find("transpose -12800\n") != std::string::npos);
    XSFLIB_CHECK(text.find("\nchunk \"x y!\" 0\n") != std::string::npos);

    // The native archive's pieces with their numbers, and its fields. Labels name the items past a bytecode's first: a
    // local label where the item has none, and a label whose name reads as a number in quotes.
    XSFLIB_CHECK(text.find("\n    sequence_file 0 version 0x20000 # piece 1\n    padding 64 # piece 2\n") !=
                 std::string::npos);
    XSFLIB_CHECK(text.find("\n    field 0 from 0 to end\n    field 0+0x4 from 1 item 0 to 1 item end_label\n") !=
                 std::string::npos);
    XSFLIB_CHECK(text.find("\n    field 0+0x1 from 2 to end+0x10\n") != std::string::npos);
    XSFLIB_CHECK(text.find("\n    field 0+0x2 from 1 item 0 to 1 item .L2\n") != std::string::npos);
    XSFLIB_CHECK(text.find("\n    .L2:\n        open_track 1 .L1\n") != std::string::npos);
    XSFLIB_CHECK(text.find("\n    field 0+0x3 from 3 item 0 to 3 item \"12\"\n") != std::string::npos);
}

// A command's value in the text form reads as the nearest 2^-16, with halfway away from zero. A reader takes the lowest
// value of Fixed, and refuses an exponent and a value past the range.
void TestNumbers()
{
    const auto value = [](std::string_view number)
    {
        const std::string text = "hok 1.0\nengine switch\nsequence 0\n    volume " + std::string(number) + "\n";
        return format::ReadText(text).sequences[0]->items[0].value.value;
    };

    // The nearest 2^-16. Halfway goes away from zero.
    XSFLIB_CHECK(value("0.1") == 6554 && value("-0.1") == -6554 && value("100") == 100 * kOne);
    XSFLIB_CHECK(value("0.00000762939453125") == 1 && value("-0.00000762939453125") == -1);
    XSFLIB_CHECK(value("0.0000076293945312") == 0 && value(".5") == kOne / 2 && value("7.") == 7 * kOne);
    XSFLIB_CHECK(!TextError("hok 1.0\nengine switch\nsequence 0\n    volume 1e5\n").empty());
    XSFLIB_CHECK(!TextError("hok 1.0\nengine switch\nsequence 0\n    volume 140737488355328\n").empty());
    XSFLIB_CHECK(value("-140737488355328") == std::numeric_limits<Fixed>::min());
}

// The tracks' colours, in both forms. TCOL follows SCEN.
void TestTrackColours()
{
    File file = WithScenes();
    file.track_colours = {
        {{.sound = 0, .track = 0}, {.red = 74, .green = 127, .blue = 224}},
        {{.sound = 0, .track = 15}, {}},
        {{.sound = 70000, .track = 255}, {.red = 255, .green = 255, .blue = 255}},
    };

    const std::vector<uint8_t> binary = format::Write(file);
    const std::string text = format::WriteText(file);

    XSFLIB_CHECK(format::Read(binary) == file);
    XSFLIB_CHECK(format::ReadText(text) == file);
    XSFLIB_CHECK(format::WriteText(format::ReadText(text)) == text);

    // The number of colours, then each one: the sound in 4 bytes, the track, and the red, the green and the blue.
    const std::vector<std::string> kIds = {"ENGN", "MIXR", "SEQS", "NATV", "TABL",
                                           "GRPF", "SCEN", "TCOL", "XTRA", "x y!"};
    const std::span<const uint8_t> colours = ChunkBytes(binary, 7);
    XSFLIB_CHECK(ChunkIds(binary) == kIds);
    SEQLIB_REQUIRE(colours.size() == 4 + 8 * 3);
    XSFLIB_CHECK(xsflib::LoadLe32(&colours[0]) == 3);
    XSFLIB_CHECK(xsflib::LoadLe32(&colours[4]) == 0 && colours[8] == 0 && colours[9] == 74 && colours[10] == 127 &&
                 colours[11] == 224);
    XSFLIB_CHECK(xsflib::LoadLe32(&colours[20]) == 70000 && colours[24] == 255 && colours[27] == 255);

    // A line for each colour after the scenes, with the sound's name where the tables give it.
    XSFLIB_CHECK(text.find("\n\ntrack_colour 0 0 74 127 224 # Song\ntrack_colour 0 15 0 0 0 # Song\n"
                           "track_colour 70000 255 255 255 255\n\nsequence 0\n") != std::string::npos);

    // Lines that a reader refuses.
    const auto error = [](std::string_view lines)
    {
        return TextError("hok 1.3\nengine switch\n" + std::string(lines));
    };
    XSFLIB_CHECK(error("track_colour 1 2 3 4 5\ntrack_colour 1 2 6 7 8\n")
                     .starts_with("Line 4: track 2 of sound 1 already has a colour"));
    XSFLIB_CHECK(error("track_colour 1 2 3 4\n").starts_with("Line 3: a track's colour is track_colour"));
    XSFLIB_CHECK(error("track_colour 1 256 0 0 0\n")
                     .starts_with("Line 3: a track's number has to be a whole number from 0 to 255, not 256"));
    XSFLIB_CHECK(error("track_colour 1 2 3 4 256\n")
                     .starts_with("Line 3: a colour's blue has to be a whole number from 0 to 255, not 256"));
    XSFLIB_CHECK(error("sequence 0\n    end\ntrack_colour 0 0 0 0 0\n")
                     .starts_with("Line 5: the tracks' colours come before the sequences"));

    // The binary form refuses a track with two colours. The second colour's track becomes the first one's.
    XSFLIB_CHECK(Patched(binary, 7, 16, 0) == "The file is damaged: it sets two colours for track 0 of sound 0.");

    // A file without colours has no TCOL chunk.
    XSFLIB_CHECK(ChunkIds(format::Write(WithScenes())).size() == kIds.size() - 1);
}

// The instrument maps, in both forms. IMAP follows TCOL.
void TestInstrumentMaps()
{
    File file = WithScenes();
    file.track_colours = {{{.sound = 0, .track = 0}, {.red = 74, .green = 127, .blue = 224}}};
    file.instrument_maps = {
        {0x08000014,
         {{{.drums = false, .program = 0}, 5},
          {{.drums = false, .program = 127}, 127},
          {{.drums = true, .program = 0}, 126}}},
        {0x08000010, {{{.drums = true, .program = 35}, 0}}},
    };

    const std::vector<uint8_t> binary = format::Write(file);
    const std::string text = format::WriteText(file);

    XSFLIB_CHECK(format::Read(binary) == file);
    XSFLIB_CHECK(format::ReadText(text) == file);
    XSFLIB_CHECK(format::WriteText(format::ReadText(text)) == text);

    // The number of maps, then each one: the voice group's address and the number of its pairings, then each pairing:
    // the drum channel, the program, the voice and a byte of 0.
    const std::vector<std::string> kIds = {"ENGN", "MIXR", "SEQS", "NATV", "TABL", "GRPF",
                                           "SCEN", "TCOL", "IMAP", "XTRA", "x y!"};
    const std::span<const uint8_t> maps = ChunkBytes(binary, 8);
    XSFLIB_CHECK(ChunkIds(binary) == kIds);
    SEQLIB_REQUIRE(maps.size() == 4 + 8 + 4 + 8 + 4 * 3);
    XSFLIB_CHECK(xsflib::LoadLe32(&maps[0]) == 2);
    XSFLIB_CHECK(xsflib::LoadLe32(&maps[4]) == 0x08000010 && xsflib::LoadLe32(&maps[8]) == 1);
    XSFLIB_CHECK(maps[12] == 1 && maps[13] == 35 && maps[14] == 0 && maps[15] == 0);
    XSFLIB_CHECK(xsflib::LoadLe32(&maps[16]) == 0x08000014 && xsflib::LoadLe32(&maps[20]) == 3);
    XSFLIB_CHECK(maps[24] == 0 && maps[25] == 0 && maps[26] == 5);
    XSFLIB_CHECK(maps[28] == 0 && maps[29] == 127 && maps[30] == 127);
    XSFLIB_CHECK(maps[32] == 1 && maps[33] == 0 && maps[34] == 126);

    // A line for each pairing after the tracks' colours.
    XSFLIB_CHECK(text.find("# Song\n\ninstrument_map 0x8000010 drums 35 voice 0\n"
                           "instrument_map 0x8000014 program 0 voice 5\n"
                           "instrument_map 0x8000014 program 127 voice 127\n"
                           "instrument_map 0x8000014 drums 0 voice 126\n\nsequence 0\n") != std::string::npos);

    // Lines that a reader refuses.
    const auto error = [](std::string_view lines)
    {
        return TextError("hok 1.3\nengine switch\n" + std::string(lines));
    };
    XSFLIB_CHECK(error("instrument_map 0x8000000 program 1 voice 2\ninstrument_map 0x8000000 program 1 voice 3\n")
                     .starts_with("Line 4: the instrument map of 0x8000000 already pairs program 1"));
    XSFLIB_CHECK(error("instrument_map 0x8000000 program 1\n")
                     .starts_with("Line 3: a pairing of an instrument map is instrument_map"));
    XSFLIB_CHECK(error("instrument_map 0x8000000 piano 1 voice 2\n")
                     .starts_with("Line 3: a pairing of an instrument map is instrument_map"));
    XSFLIB_CHECK(error("instrument_map 0x8000000 program 128 voice 0\n")
                     .starts_with("Line 3: a program has to be a whole number from 0 to 127, not 128"));
    XSFLIB_CHECK(error("instrument_map 0x8000000 drums 1 voice 128\n")
                     .starts_with("Line 3: a voice has to be a whole number from 0 to 127, not 128"));
    XSFLIB_CHECK(error("sequence 0\n    end\ninstrument_map 0x8000000 program 1 voice 2\n")
                     .starts_with("Line 5: the instrument maps come before the sequences"));

    // The binary form refuses a second map of a voice group, a voice past 127, a drum channel's flag past 1, and a
    // program that a map pairs twice.
    XSFLIB_CHECK(Patched(binary, 8, 16, 0x10) ==
                 "The file is damaged: it has two instrument maps for the voice group at address 134217744.");
    XSFLIB_CHECK(Patched(binary, 8, 26, 128) ==
                 "The file is damaged: an instrument map pairs a program or a voice past 127.");
    XSFLIB_CHECK(Patched(binary, 8, 12, 2) ==
                 "The file is damaged: an instrument map holds a drum flag other than 0 or 1.");
    XSFLIB_CHECK(Patched(binary, 8, 29, 0) == "The file is damaged: an instrument map pairs program 0 twice.");

    // A file without maps has no IMAP chunk.
    XSFLIB_CHECK(ChunkIds(format::Write(WithScenes())).size() == kIds.size() - 2);
}

// The banks' instrument maps, in both forms. BMAP follows SCEN in a file without TCOL and IMAP.
void TestBankMaps()
{
    File file = WithScenes();
    file.bank_maps = {
        {3, {.programs = {{33, 300}, {0, 65535}}, .drum_notes = {{36, {.program = 5, .key = 60}}}}},
        {1, {.programs = {{127, 0}}}},
    };

    const std::vector<uint8_t> binary = format::Write(file);
    const std::string text = format::WriteText(file);
    XSFLIB_CHECK(format::Read(binary) == file);
    XSFLIB_CHECK(format::ReadText(text) == file);
    XSFLIB_CHECK(format::WriteText(format::ReadText(text)) == text);

    // The number of maps, then each one: the bank, the numbers of its programs and its drum notes, then each pairing.
    const std::vector<std::string> ids = ChunkIds(binary);
    const auto at = static_cast<std::size_t>(std::ranges::find(ids, "BMAP") - ids.begin());
    SEQLIB_REQUIRE(at < ids.size());
    XSFLIB_CHECK(ids[at - 1] == "SCEN");
    const std::span<const uint8_t> maps = ChunkBytes(binary, at);
    SEQLIB_REQUIRE(maps.size() == 4 + 12 + 8 + 12 + 8 * 2 + 8);
    XSFLIB_CHECK(xsflib::LoadLe32(&maps[0]) == 2);
    XSFLIB_CHECK(xsflib::LoadLe32(&maps[4]) == 1 && xsflib::LoadLe32(&maps[8]) == 1 &&
                 xsflib::LoadLe32(&maps[12]) == 0);
    XSFLIB_CHECK(xsflib::LoadLe32(&maps[16]) == 127 && xsflib::LoadLe32(&maps[20]) == 0);
    XSFLIB_CHECK(xsflib::LoadLe32(&maps[24]) == 3 && xsflib::LoadLe32(&maps[28]) == 2 &&
                 xsflib::LoadLe32(&maps[32]) == 1);
    XSFLIB_CHECK(xsflib::LoadLe32(&maps[36]) == 0 && xsflib::LoadLe32(&maps[40]) == 65535);
    XSFLIB_CHECK(xsflib::LoadLe32(&maps[44]) == 33 && xsflib::LoadLe32(&maps[48]) == 300);
    XSFLIB_CHECK(maps[52] == 36 && maps[53] == 60 && maps[54] == 0 && maps[55] == 0);
    XSFLIB_CHECK(xsflib::LoadLe32(&maps[56]) == 5);

    // A line for each pairing after the scenes.
    XSFLIB_CHECK(text.find("bank_map 1 program 127 instrument 0\n"
                           "bank_map 3 program 0 instrument 65535\n"
                           "bank_map 3 program 33 instrument 300\n"
                           "bank_map 3 drum_note 36 instrument 5 key 60\n\nsequence 0\n") != std::string::npos);

    // Lines that a reader refuses.
    const auto error = [](std::string_view lines)
    {
        return TextError("hok 1.3\nengine switch\n" + std::string(lines));
    };
    XSFLIB_CHECK(error("bank_map 0 program 1 instrument 2\nbank_map 0 program 1 instrument 3\n")
                     .starts_with("Line 4: the instrument map of bank 0 already pairs program 1"));
    XSFLIB_CHECK(error("bank_map 0 drum_note 1 instrument 2 key 3\nbank_map 0 drum_note 1 instrument 3 key 3\n")
                     .starts_with("Line 4: the instrument map of bank 0 already pairs drum_note 1"));
    XSFLIB_CHECK(
        error("bank_map 0 program 1\n").starts_with("Line 3: a pairing of a bank's instrument map is bank_map"));
    XSFLIB_CHECK(error("bank_map 0 drum_note 1 instrument 2\n")
                     .starts_with("Line 3: a pairing of a bank's instrument map is bank_map"));
    XSFLIB_CHECK(error("bank_map 0 program 128 instrument 0\n")
                     .starts_with("Line 3: a program has to be a whole number from 0 to 127, not 128"));
    XSFLIB_CHECK(error("bank_map 0 drum_note 1 instrument 2 key 128\n")
                     .starts_with("Line 3: a key has to be a whole number from 0 to 127, not 128"));
    XSFLIB_CHECK(error("sequence 0\n    end\nbank_map 0 program 1 instrument 2\n")
                     .starts_with("Line 5: the instrument maps come before the sequences"));

    // The binary form refuses a second map of a bank, numbers past their ranges, and a pairing twice.
    XSFLIB_CHECK(Patched(binary, at, 24, 1) == "The file is damaged: it has two instrument maps for bank 1.");
    XSFLIB_CHECK(Patched(binary, at, 16, 128) ==
                 "The file is damaged: an instrument map pairs a MIDI program past 127 or a program past 65,535.");
    XSFLIB_CHECK(Patched(binary, at, 53, 128) ==
                 "The file is damaged: an instrument map pairs a note or a key past 127, or a program past 65,535.");
    XSFLIB_CHECK(Patched(binary, at, 44, 0) == "The file is damaged: an instrument map pairs program 0 twice.");
}

struct TimelinesFixture
{
    File file;
    File none;
    std::vector<uint8_t> binary;
    std::string text;
};

// A file with two timelines in each form, and a file without timelines that says so.
TimelinesFixture MakeTimelinesFixture()
{
    File file = WithScenes();
    file.timelines.emplace();
    (*file.timelines)[0] = {{{192, 12}, {144, 1}, {192, 1}},
                            {TrackHints{0, {8, 8}, {1, 4}, {}}, TrackHints{2, {}, {}, {7}}}};
    (*file.timelines)[1] = {{{96, 1}}, {}};
    File none = WithScenes();
    none.timelines.emplace();

    const std::vector<uint8_t> binary = format::Write(file);
    const std::string text = format::WriteText(file);

    return {file, none, binary, text};
}

// The timelines, in both forms.
void TestTimelinesRoundTrip()
{
    const TimelinesFixture fixture = MakeTimelinesFixture();
    const File& file = fixture.file;
    const File& none = fixture.none;
    const std::vector<uint8_t>& binary = fixture.binary;
    const std::string& text = fixture.text;

    XSFLIB_CHECK(format::Read(binary) == file);
    XSFLIB_CHECK(format::ReadText(text) == file);
    XSFLIB_CHECK(format::WriteText(format::ReadText(text)) == text);
    XSFLIB_CHECK(format::Read(format::Write(none)) == none);
    XSFLIB_CHECK(format::ReadText(format::WriteText(none)) == none);
    XSFLIB_CHECK(!format::Read(format::Write(WithScenes())).timelines);
    XSFLIB_CHECK(!format::ReadText(format::WriteText(WithScenes())).timelines);
}

// TMLN follows TAGS.
void TestTimelinesChunk()
{
    const TimelinesFixture fixture = MakeTimelinesFixture();
    const File& none = fixture.none;
    const std::vector<uint8_t>& binary = fixture.binary;

    // The number of timelines, then each one: its sequence, its runs of bars, and its tracks' hints. A track's hints
    // are its number and three zeros, then its three lists.
    std::vector<uint8_t> expected;
    const auto u32 = [&expected](std::initializer_list<uint32_t> values)
    {
        for (const uint32_t value : values)
        {
            std::array<uint8_t, 4> bytes{};
            xsflib::StoreLe32(bytes.data(), value);
            expected.insert(expected.end(), bytes.begin(), bytes.end());
        }
    };
    u32({2, 0, 3, 192, 12, 144, 1, 192, 1, 2, 0, 2, 8, 8, 2, 1, 4, 0, 2, 0, 0, 1, 7, 1, 1, 96, 1, 0});
    const std::vector<std::string> kIds = {"ENGN", "MIXR", "SEQS", "NATV", "TABL",
                                           "GRPF", "SCEN", "TMLN", "XTRA", "x y!"};
    XSFLIB_CHECK(ChunkIds(binary) == kIds);
    XSFLIB_CHECK(std::ranges::equal(ChunkBytes(binary, 7), expected));
    XSFLIB_CHECK(ChunkBytes(format::Write(none), 7).size() == 4);
}

// The text form writes the timelines' lines, and refuses damaged ones.
void TestTimelinesText()
{
    const TimelinesFixture fixture = MakeTimelinesFixture();
    const File& none = fixture.none;
    const std::string& text = fixture.text;

    // A line of each timeline's bars and of each track's hints, after the scenes. A file without timelines that says so
    // has a line of none.
    XSFLIB_CHECK(text.find("\n\ntimeline 0 bars 192 12 144 1 192 1\ntimeline 0 track 0 opening 8 8 kept 1 4\n"
                           "timeline 0 track 2 called 7\ntimeline 1 bars 96 1\n\nsequence 0\n") != std::string::npos);
    XSFLIB_CHECK(format::WriteText(none).find("\n\ntimeline none\n\nsequence 0\n") != std::string::npos);

    // Lines that a reader refuses.
    const auto error = [](std::string_view lines)
    {
        return TextError("hok 1.3\nengine switch\n" + std::string(lines) + "sequence 0\n    end\n");
    };
    XSFLIB_CHECK(error("timeline 0 track 0\n").starts_with("Line 3: a track's hints are timeline"));
    XSFLIB_CHECK(error("timeline 0 bars 192\n").starts_with("Line 3: a timeline's bars come in pairs"));
    XSFLIB_CHECK(error("timeline 0\n").starts_with("Line 3: a timeline's line is timeline"));
    XSFLIB_CHECK(error("timeline none\ntimeline 0 bars 192 1\n")
                     .starts_with("Line 4: a file with a line of none can't have timelines"));
    XSFLIB_CHECK(error("timeline 0 bars 192 1\ntimeline none\n")
                     .starts_with("Line 4: a file with timelines or a line of none can't have a line of none"));
    XSFLIB_CHECK(error("timeline 0 bars 192 1\ntimeline 0 bars 192 1\n")
                     .starts_with("Line 4: sequence 0 already has a timeline"));
    XSFLIB_CHECK(error("timeline 0 bars 192 1\ntimeline 0 track 0 4\n")
                     .starts_with("Line 4: a track's numbers follow opening, kept or called"));
    XSFLIB_CHECK(error("timeline 0 bars 192 1\ntimeline 0 track 0 kept 1 kept 2\n")
                     .starts_with("Line 4: a track's hints have kept once at most"));
    XSFLIB_CHECK(error("timeline 5 bars 192 1\n").starts_with("Line 3: the file has no sequence 5"));
    XSFLIB_CHECK(error("timeline 0 bars 192 1\ntimeline 0 track 0 kept 2 1\n")
                     .starts_with("Line 3: the timelines list the bars of track 0 of sequence 0 out of order"));
    XSFLIB_CHECK(error("timeline 0 bars 0 1\n")
                     .starts_with("Line 3: the timelines have a run of bars of sequence 0 with no ticks or no bars"));
    XSFLIB_CHECK(TextError("hok 1.0\nengine switch\nsequence 0\n    end\ntimeline 0 bars 192 1\n")
                     .starts_with("Line 5: the timelines come before the sequences"));
}

// The binary form refuses damaged timelines, and neither form writes a timeline of a missing sequence.
void TestTimelinesDamaged()
{
    const TimelinesFixture fixture = MakeTimelinesFixture();
    const File& file = fixture.file;
    const std::vector<uint8_t>& binary = fixture.binary;

    // The binary form refuses a run of no ticks, and a timeline of a sequence that the file lacks.
    XSFLIB_CHECK(Patched(binary, 7, 12, 0) ==
                 "The file is damaged: the timelines have a run of bars of sequence 0 with no ticks or no bars.");
    XSFLIB_CHECK(Patched(binary, 7, 4, 9) == "The file is damaged: the timelines name sequence 9, and the file has 2.");

    // Neither form writes a timeline of a sequence that the file lacks.
    File past = file;
    (*past.timelines)[2] = {};
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { format::Write(past); }));
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { format::WriteText(past); }));
}

// A bank of the Switch: no instrument at program 0, and at program 1 one region with a value other than the default in
// each field.
ntdlib::sar::BankFile SmallBank()
{
    ntdlib::sar::BankFile bank;
    bank.waves = {{0x05000001, 3}};
    bank.instruments.resize(2);
    ntdlib::sar::BankFile::Region region;
    region.values = {.wave_id_index = 0,
                     .original_key = 62,
                     .volume = 100,
                     .pan = 20,
                     .pitch = 0.5f,
                     .ignore_note_off = true,
                     .key_group = 2,
                     .interpolation = 1,
                     .adshr = {127, 100, 90, 80, 70}};
    bank.instruments[1].emplace().key_regions = {{0, 127, {region}}};

    return bank;
}

// A bank of the Wii U with two instruments, laid out in another order than their programs: two key regions, the second
// with two velocity regions, and an instrument of one region.
ntdlib::sar::BankFile WideBank()
{
    ntdlib::sar::BankFile bank;
    bank.flavor = Flavor::kCafe;
    bank.waves = {{0x05000000, 0}, {0x05000000, 1}};
    bank.instruments.resize(3);
    ntdlib::sar::BankFile::Region low;
    ntdlib::sar::BankFile::Region soft;
    soft.last_velocity = 63;
    soft.values.wave_id_index = 1;
    ntdlib::sar::BankFile::Region loud;
    loud.first_velocity = 64;
    loud.values.original_key = 72;
    bank.instruments[0].emplace().key_regions = {{0, 59, {low}}, {60, 127, {soft, loud}}};
    bank.instruments[2].emplace().key_regions = {{0, 127, {loud}}};
    bank.order = {2, 0};

    return bank;
}

struct BanksAndWavesFixture
{
    File file;
    std::vector<uint8_t> binary;
    std::string text;
};

// A file with two banks and two waves, in each form.
BanksAndWavesFixture MakeBanksAndWavesFixture()
{
    // A wave that keeps its wave archive's samples, and one with PCM16 samples in two channels.
    const WaveModel kLooped{ntdlib::sar::WaveEncoding::kDspAdpcm, 32000, true, 14, 28, 42, {}};
    const WaveModel kPcm{ntdlib::sar::WaveEncoding::kPcm16, 48000, false, 0, 2, 3, {{1, -2, 3}, {-4, 5, -6}}};
    File file = Unusual();
    file.banks = {{9, SmallBank()}, {12, WideBank()}};
    file.waves = {{WavePlace{4, 1}, kLooped}, {WavePlace{4, 2}, kPcm}};

    const std::vector<uint8_t> binary = format::Write(file);
    const std::string text = format::WriteText(file);

    return {file, binary, text};
}

// The models of edited banks and waves in both forms. BANK and WAVE follow GRPF.
void TestBanksAndWavesRoundTrip()
{
    const BanksAndWavesFixture fixture = MakeBanksAndWavesFixture();
    const File& file = fixture.file;
    const std::vector<uint8_t>& binary = fixture.binary;
    const std::string& text = fixture.text;

    XSFLIB_CHECK(format::Read(binary) == file && format::ReadText(text) == file);
    XSFLIB_CHECK(format::WriteText(format::ReadText(text)) == text);
    const std::vector<std::string> kIds = {"ENGN", "MIXR", "SEQS", "NATV", "TABL",
                                           "GRPF", "BANK", "WAVE", "XTRA", "x y!"};
    XSFLIB_CHECK(ChunkIds(binary) == kIds);
    XSFLIB_CHECK(text.find("\nchunk BANK ") != std::string::npos &&
                 text.find("\nchunk WAVE 76\n") != std::string::npos);
}

// The bytes of the BANK and WAVE chunks.
void TestBanksAndWavesChunks()
{
    const BanksAndWavesFixture fixture = MakeBanksAndWavesFixture();
    const std::vector<uint8_t>& binary = fixture.binary;

    // BANK holds the number of banks, then each one's file ID, its console, its version, its table of waves, its number
    // of programs and the order of its layout. Each program's flags follow, with bit 0 for an instrument, and an
    // instrument's key regions with their velocity regions. A velocity region holds its velocities, its wave's entry,
    // its key, volume and pan, its flags with bit 0 for a note-off that it ignores, its pitch, its key group, its
    // interpolation and its envelope.
    File small = Unusual();
    small.banks = {{9, SmallBank()}};
    const std::vector<uint8_t> kBankBytes = {
        1, 0,   0, 0, 9, 0, 0, 0, 1,  0,   0,  0, 0, 0, 1, 0,    1, 0, 0, 0, 1,   0,   0,  5,  3,  0, 0, 0,
        2, 0,   0, 0, 0, 0, 0, 0, 0,  0,   0,  0, 1, 0, 0, 0,    1, 0, 0, 0, 0,   127, 0,  0,  1,  0, 0, 0,
        0, 127, 0, 0, 0, 0, 0, 0, 62, 100, 20, 1, 0, 0, 0, 0x3F, 2, 1, 0, 0, 127, 100, 90, 80, 70, 0, 0, 0};
    XSFLIB_CHECK(std::ranges::equal(ChunkBytes(format::Write(small), 6), kBankBytes));

    // WAVE holds the number of waves, then each one's wave archive and index, its encoding, its flags with bit 0 for a
    // loop and bit 1 for samples, its rate, its loop's start and end, its length, and its samples: the number of
    // channels, then each channel's samples in turn.
    const std::vector<uint8_t> kWaveBytes = {
        2,    0,    0,    0, // two waves
        4,    0,    0,    0,    1,  0, 0,    0,    2,  1, 0,    0,
        0,    0x7D, 0,    0,                                       // wave 1 of file 4: DSP-ADPCM, a loop, 32 kHz
        14,   0,    0,    0,    28, 0, 0,    0,    42, 0, 0,    0, // the loop's start and end, the length
        4,    0,    0,    0,    2,  0, 0,    0,    1,  2, 0,    0,
        0x80, 0xBB, 0,    0, // wave 2: PCM16, samples, 48 kHz
        0,    0,    0,    0,    2,  0, 0,    0,    3,  0, 0,    0,
        2,    0,    0,    0,                                           // no loop, the end, the length, 2 channels
        1,    0,    0xFE, 0xFF, 3,  0, 0xFC, 0xFF, 5,  0, 0xFA, 0xFF}; // the samples
    XSFLIB_CHECK(std::ranges::equal(ChunkBytes(binary, 7), kWaveBytes));
}

// The binary form refuses banks and waves out of order, parts of a later version, more than the chunk holds, a bank
// that can't be laid out and a loop past the samples.
void TestBanksAndWavesDamaged()
{
    const BanksAndWavesFixture fixture = MakeBanksAndWavesFixture();
    const std::vector<uint8_t>& binary = fixture.binary;

    // The binary form refuses banks whose IDs don't rise, a console, a flag of a later version, a bank that the tools
    // can't lay out and more programs than the chunk holds. The second bank's ID lies at byte 84 of BANK.
    XSFLIB_CHECK(Patched(binary, 6, 84, 9) == "The file is damaged: the file IDs of its banks don't rise.");
    XSFLIB_CHECK(Patched(binary, 6, 8, 9).starts_with("The file contains a bank for a console that this version"));
    XSFLIB_CHECK(
        Patched(binary, 6, 40, 3).starts_with("The file contains an instrument with a flag that this version"));
    XSFLIB_CHECK(Patched(binary, 6, 67, 2).starts_with("The file contains a region with a flag that this version"));
    XSFLIB_CHECK(Patched(binary, 6, 56, 0x80).starts_with("The file is damaged: bank 9 can't be laid out: "));
    XSFLIB_CHECK(Patched(binary, 6, 31, 0x40) ==
                 "The file is damaged: bank 9 has more programs than its chunk can contain.");

    // It refuses waves whose places don't rise, an encoding or a flag of a later version, more samples than the chunk
    // holds, and a loop past the samples. The second wave starts at byte 32 of WAVE.
    XSFLIB_CHECK(Patched(binary, 7, 36, 1) == "The file is damaged: the places of its waves don't rise.");
    XSFLIB_CHECK(Patched(binary, 7, 12, 3).starts_with("The file contains a wave in an encoding that this version"));
    XSFLIB_CHECK(Patched(binary, 7, 13, 4).starts_with("The file contains a wave with a flag that this version"));
    XSFLIB_CHECK(Patched(binary, 7, 56, 9) ==
                 "The file is damaged: wave 2 of file 4 has more samples than its chunk can contain.");
    XSFLIB_CHECK(Patched(binary, 7, 52, 4) ==
                 "The file is damaged: wave 2 of file 4 ends its loop after its last sample.");
    XSFLIB_CHECK(Patched(binary, 7, 20, 30).starts_with("The file is damaged: wave 1 of file 4 has a loop that"));
}

// A file without an archive can't hold them, and a wave at a rate of 0 doesn't save. The text form takes each chunk
// once, after the archive.
void TestBanksAndWavesUnsaved()
{
    // Banks and waves name an archive's files. A file without an archive can't hold them, and a model that the chunk
    // can't hold doesn't save.
    File loose;
    loose.banks = {{9, SmallBank()}};
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { format::Read(format::Write(loose)); }) ==
                 "The file is damaged: it contains banks or waves, but no archive.");
    File silent = Unusual();
    silent.waves = {{WavePlace{4, 1}, WaveModel{ntdlib::sar::WaveEncoding::kPcm16, 0, false, 0, 3, 3, {}}}};
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { format::Write(silent); }) ==
                 "wave 1 of file 4 plays at a rate of 0");

    // The text form takes each chunk once, after the archive.
    const std::string kEmptyChunk = "chunk BANK 4\n    AAAAAA==\n";
    XSFLIB_CHECK(TextError("hok 1.1\nengine switch\n" + kEmptyChunk)
                     .starts_with("Line 3: the BANK chunk comes once, after the archive"));
    XSFLIB_CHECK(TextError(kNativeOnly + kEmptyChunk + kEmptyChunk).starts_with("Line 8: the BANK chunk comes once"));
    XSFLIB_CHECK(TextError(kNativeOnly + kEmptyChunk).empty());
}

// Copies of a file share its sequences and its native archive. An edit of a copy changes that copy alone.
void TestSharing()
{
    const File file = Unusual();
    File copy = file;
    XSFLIB_CHECK(copy.sequences[0].SharesWith(file.sequences[0]) && copy.native.SharesWith(file.native));
    XSFLIB_CHECK(copy == file);

    copy.sequences[0].Edit([](Sequence& sequence) { sequence.items.pop_back(); });

    XSFLIB_CHECK(!copy.sequences[0].SharesWith(file.sequences[0]) && copy.sequences[1].SharesWith(file.sequences[1]));
    XSFLIB_CHECK(copy.sequences[0]->items.size() + 1 == file.sequences[0]->items.size() && copy != file);
    XSFLIB_CHECK(copy.native.SharesWith(file.native));

    // Equal values are equal without sharing. A Shared without a value equals only another without one.
    const format::Shared<Sequence> first = *file.sequences[0];
    const format::Shared<Sequence> none;
    XSFLIB_CHECK(!first.SharesWith(file.sequences[0]) && first == file.sequences[0]);
    XSFLIB_CHECK(none == format::Shared<Sequence>() && !(none == first) && !none);

    // Edit starts from a value-initialised value when there's none.
    format::Shared<Sequence> empty;
    empty.Edit([](Sequence& sequence) { sequence.labels.push_back({"x", 0}); });
    XSFLIB_CHECK(empty && empty->labels.size() == 1 && empty->items.empty());
}

// Files that a read refuses. The binary form refuses a changed byte, a short file, a later major version, chunks
// without their archive, twice or cut short, parts of a later version and fields that name the wrong items.
void TestDamageBinary()
{
    const std::vector<uint8_t> binary = format::Write(Unusual());

    const auto refuses = [](std::vector<uint8_t> bytes)
    {
        return Throws<xsflib::FormatError>([&] { format::Read(bytes); });
    };

    // A changed byte, a short file, a later major version and bytes that aren't a file.
    std::vector<uint8_t> flipped = binary;
    flipped[flipped.size() - 40] ^= 1;
    XSFLIB_CHECK(refuses(flipped));
    XSFLIB_CHECK(refuses(std::vector<uint8_t>(binary.begin(), binary.end() - 1)));
    std::vector<uint8_t> later = binary;
    later[8] = 2;
    XSFLIB_CHECK(refuses(later));
    XSFLIB_CHECK(refuses({1, 2, 3}));
    XSFLIB_CHECK(Throws<xsflib::FormatError>([] { format::ReadEitherForm(std::vector<uint8_t>{1, 2}); }));

    // A file that the binary form can't hold.
    File missing = Unusual();
    missing.sequences.clear();
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { format::Write(missing); }));
    File clash = Unusual();
    clash.other_chunks.push_back({"SEQS", {}});
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { format::Write(clash); }));
    File twice = Unusual();
    twice.sequences[0].Edit([](Sequence& sequence) { sequence.labels.push_back({"Song", 1}); });
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { format::WriteText(twice); }));

    // The binary form with chunk `chunk` renamed `id`. The table of chunks holds ENGN, MIXR, SEQS, NATV, TABL, GRPF and
    // XTRA first, and each chunk's ID starts its entry.
    const auto renamed = [&](std::size_t chunk, std::string_view id)
    {
        std::vector<uint8_t> bytes = binary;
        std::copy(id.begin(), id.end(), bytes.begin() + 24 + 24 * static_cast<std::ptrdiff_t>(chunk));

        return bytes;
    };

    // Tables and group files without a native archive, two MIXR, TABL or GRPF chunks, and a MIXR, TABL or GRPF chunk
    // that ends early.
    XSFLIB_CHECK(refuses(renamed(3, "NATX")) && refuses(renamed(6, "MIXR")) && refuses(renamed(6, "TABL")) &&
                 refuses(renamed(6, "GRPF")));
    for (const std::size_t chunk : {1, 4, 5})
    {
        std::vector<uint8_t> short_chunk = renamed(chunk, "XXXX");
        std::copy_n(binary.begin() + 24 + 24 * static_cast<std::ptrdiff_t>(chunk), 4,
                    short_chunk.begin() + 24 + 24 * 6);
        XSFLIB_CHECK(refuses(short_chunk) && !refuses(renamed(chunk, "XXXX")));
    }

    // A sound of a later version's kind, and a flag of a later version. The first sound's kind follows the count of
    // sounds and its name, Song. Its flags come 7 bytes later.
    XSFLIB_CHECK(Patched(binary, 4, 12, 9).starts_with("The file contains a sound type (9) that this version"));
    XSFLIB_CHECK(Patched(binary, 4, 19, 3).starts_with("The file contains a sound flag that this version"));
    XSFLIB_CHECK(Patched(binary, 4, 12, 3).empty());
    XSFLIB_CHECK(
        TextError("hok 1.1\nengine switch\nchunk GRPF 4\n    AAAAAA==\n").starts_with("Line 3: the GRPF chunk"));
    XSFLIB_CHECK(TextError(kNativeOnly + "chunk GRPF 4\n    AAAAAA==\nchunk GRPF 4\n    AAAAAA==\n")
                     .starts_with("Line 8: the GRPF chunk comes once"));
    XSFLIB_CHECK(TextError(kNativeOnly + "chunk GRPF 3\n    AAAA\n").starts_with("Line 6: the GRPF chunk ends early"));
    XSFLIB_CHECK(TextError(kNativeOnly + "chunk GRPF 4\n    AAAAAA==\n").empty());

    // Fields that a reader refuses: one in a sequence file, and one that names an item of padding.
    File in_file = Unusual();
    in_file.native.Edit([](NativeArchive& native) { native.fields[0].at = {1, 0, false}; });
    XSFLIB_CHECK(refuses(format::Write(in_file)));
    File item = Unusual();
    item.native.Edit([](NativeArchive& native) { native.fields[1].to.piece = 2; });
    XSFLIB_CHECK(refuses(format::Write(item)));
    XSFLIB_CHECK(TextError(format::WriteText(item)).find("names an item of piece 2") != std::string::npos);
}

// A file that a form can't hold throws, such as one whose encoder's choices lack their sequence file.
void TestDamageUnfiled()
{
    // The encoder's choices without the sequence file that holds them.
    File unfiled = Unusual();
    unfiled.native = {};
    XSFLIB_CHECK(format::UnfiledChoices(unfiled) == 0u);
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { format::Write(unfiled); }));
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { format::WriteText(unfiled); }));
    XSFLIB_CHECK(TextError("hok 1.0\nengine switch\nsequence 0\n    wait 5\n    wait 6 [value_bytes 2]\n")
                     .starts_with("Line 5: [value_bytes]"));
}

// A text with a mistake gives the mistake's line, in the sequences, the native archive and its tables.
void TestDamageText()
{
    // Texts with mistakes give the line.
    XSFLIB_CHECK(TextError("hok 1.0\nengine switch\nsequence 0\n    louder 5\n").starts_with("Line 4: no command"));
    XSFLIB_CHECK(TextError("hok 1.0\nengine switch\nsequence 0\n    jump nowhere\n").starts_with("Line 4: no label"));
    XSFLIB_CHECK(TextError("hok 2.0\nengine switch\n").starts_with("Line 1: a later version"));
    XSFLIB_CHECK(TextError("hok 1.0\nsequence 0\n").find("names no engine") != std::string::npos);
    XSFLIB_CHECK(TextError("hok 1.0\nengine switch\nnative switch\n    bytes 4\n        AAAA\n").starts_with("Line 4"));
    XSFLIB_CHECK(TextError("hok 1.0\nengine switch\nsequence 1\n").starts_with("Line 3"));
    const std::string kNative = "hok 1.0\nengine switch\nnative switch\n    bytes 3\n        AQID\n";
    XSFLIB_CHECK(TextError(kNative + "    field 0 from 0 to end\n").starts_with("Line 6: the field is outside a run"));
    XSFLIB_CHECK(TextError(kNative + "    field 0 from x to end\n").starts_with("Line 6: a place is"));
    XSFLIB_CHECK(TextError(kNative + "    field 0 from 0\n").starts_with("Line 6: a field's line"));
    const std::string kLabelled =
        "hok 1.0\nengine switch\nsequence 0\n    end\nnative switch\n    bytes 8\n"
        "        AAAAAAAAAAA=\n    sequence_file 0 version 0\n";
    XSFLIB_CHECK(TextError(kLabelled + "    field 0 from 1 item 0 to 1 item nowhere\n")
                     .starts_with("Line 9: sequence 0 has no label named nowhere"));
    XSFLIB_CHECK(TextError(kLabelled + "    field 0 from 1 item 0 to 1 item 1\n").empty());

    // Lines of the tables that a reader refuses.
    const std::string kTables = kNative + "    tables\n";
    XSFLIB_CHECK(TextError(kNative + "    sound A sequence\n").starts_with("Line 6: sound belongs to the tables"));
    XSFLIB_CHECK(TextError(kTables + "    tables\n").starts_with("Line 7: the native archive has one tables line"));
    XSFLIB_CHECK(TextError(kTables + "    sound A song\n").starts_with("Line 7: there's no sound type named song"));
    XSFLIB_CHECK(TextError(kTables + "    sound A sequence loudness 3\n")
                     .starts_with("Line 7: a sound has no value named loudness"));
    XSFLIB_CHECK(TextError(kTables + "    sound A sequence volume 256\n")
                     .starts_with("Line 7: volume has to be a whole number from 0 to 255, not 256"));
    XSFLIB_CHECK(TextError(kTables + "    sound A sequence volume 1 volume 2\n")
                     .starts_with("Line 7: the sound sets volume twice"));
    XSFLIB_CHECK(TextError(kTables + "    sound A sequence volume\n").starts_with("Line 7: volume has to be a number"));
    XSFLIB_CHECK(TextError(kTables + "    player A 4\n").starts_with("Line 7: a player's line"));
    XSFLIB_CHECK(TextError(kTables + "    bank A B\n").starts_with("Line 7: a line of bank is bank and its name"));
    XSFLIB_CHECK(TextError(kTables + "    sound A sequence banks 1 none\n    group G\n").empty());
    XSFLIB_CHECK(TextError("").find("empty") != std::string::npos);
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("file",
                                    []
                                    {
                                        TestUnusualRoundTrip();
                                        TestUnusualChunks();
                                        TestUnusualFloats();
                                        TestUnusualText();
                                        TestNumbers();
                                        TestTrackColours();
                                        TestInstrumentMaps();
                                        TestBankMaps();
                                        TestTimelinesRoundTrip();
                                        TestTimelinesChunk();
                                        TestTimelinesText();
                                        TestTimelinesDamaged();
                                        TestBanksAndWavesRoundTrip();
                                        TestBanksAndWavesChunks();
                                        TestBanksAndWavesDamaged();
                                        TestBanksAndWavesUnsaved();
                                        TestSharing();
                                        TestDamageBinary();
                                        TestDamageUnfiled();
                                        TestDamageText();
                                    });
}
