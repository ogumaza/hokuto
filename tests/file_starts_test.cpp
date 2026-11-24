// SPDX-License-Identifier: MIT

// Tests the starts of a File in both forms of Hokuto's files (src/format/file_starts.cpp and text_starts.cpp), the
// edits of the starts, the version that a file gives, and the scenes' variables of every track. Each start comes back
// unchanged through both forms. Damaged starts and refused lines throw with a message. Neither form writes STRT for a
// file without starts. A change of a start that a player plays drops the lengths. A change of another state keeps them.

#include "format/text.h"
#include "format_testing.h"

namespace
{

using format::SoundStart;
using format::StartVariable;

struct StartsFixture
{
    File file;
    std::vector<uint8_t> binary;
    std::string text;
};

// A file with the starts of three sounds, in each form: a start that sets every field but the banks and the low-pass,
// a state of the same sound with a low-pass, a start that quiets tracks only, and a start of a sound past the tables.
StartsFixture MakeStartsFixture()
{
    File file = WithScenes();
    file.starts = {
        {.sound = 0,
         .muted = 0xFF80,
         .track_volumes = {{2, 0.5f}, {31, 0.0f}},
         .tempo_ratio = 1.125f,
         .pitch = 0.5f,
         .volume = 0.7f,
         .sends = format::StartSends{0.4f, 0.0f, 1.0f},
         .seed = 0x12345678,
         .variables = {{.variable = 0, .value = 3},
                       {.variable = 34, .value = 1},
                       {.variable = 47, .track = 3, .value = -2},
                       {.time = 4, .variable = 16, .value = 1},
                       {.time = 90000, .variable = 32, .value = 7}}},
        {.sound = 0, .name = "_LAP", .silenced = 0x1, .tempo_ratio = 1.23f, .low_pass = -0.40625f},
        {.sound = 3, .silenced = 0x0006},
        {.sound = 70000, .muted = 0x80000000},
    };

    return {file, format::Write(file), format::WriteText(file)};
}

// The offset of the first start's variables in STRT: after the number of starts, the sound, the name, the flags, the
// mask, the tracks and their two volumes, the three ratios, the three sends, the seed and the number of variables.
constexpr std::size_t kFirstVariables = 4 + 4 + 4 + 4 + 4 + 4 + 2 * 4 + 3 * 4 + 3 * 4 + 4 + 4;

// The bytes of STRT in `binary`.
std::span<const uint8_t> StartsBytes(std::span<const uint8_t> binary)
{
    const std::vector<std::string> ids = ChunkIds(binary);

    return ChunkBytes(binary, static_cast<std::size_t>(std::ranges::find(ids, "STRT") - ids.begin()));
}

// The bytes of `text`, for MajorVersionOf.
std::span<const uint8_t> BytesOf(std::string_view text)
{
    return {reinterpret_cast<const uint8_t*>(text.data()), text.size()};
}

// The starts, in both forms. The text form writes each field in the order of its flag.
void TestStartsRoundTrip()
{
    const StartsFixture fixture = MakeStartsFixture();
    const File& file = fixture.file;

    XSFLIB_CHECK(format::Read(fixture.binary) == file);
    XSFLIB_CHECK(format::ReadText(fixture.text) == file);
    XSFLIB_CHECK(format::WriteText(format::ReadText(fixture.text)) == fixture.text);
}

// STRT follows SCEN. A start that quiets tracks only takes 20 bytes: its sound, an empty name, the flags, the mask and
// no variables. A variable takes its time, its number, its track and its value: a track variable of every track has
// track 0xFF and its number from 0 to 15, and one of track N has track 1 + N.
void TestStartsChunk()
{
    const StartsFixture fixture = MakeStartsFixture();

    const std::span<const uint8_t> starts = StartsBytes(fixture.binary);
    const std::vector<std::string> ids = ChunkIds(fixture.binary);

    XSFLIB_CHECK(std::ranges::find(ids, "STRT") - std::ranges::find(ids, "SCEN") == 1);
    XSFLIB_CHECK(xsflib::LoadLe32(&starts[0]) == 4);
    XSFLIB_CHECK(xsflib::LoadLe32(&starts[4]) == 0 && xsflib::LoadLe32(&starts[8]) == 0);
    XSFLIB_CHECK(xsflib::LoadLe32(&starts[12]) == 0x17D && xsflib::LoadLe32(&starts[16]) == 0xFF80);
    XSFLIB_CHECK(xsflib::LoadLe32(&starts[20]) == 0x80000004);
    XSFLIB_CHECK(xsflib::LoadLe32(&starts[kFirstVariables - 4]) == 5);
    XSFLIB_CHECK(starts[kFirstVariables + 8 + 4] == 2 && starts[kFirstVariables + 8 + 5] == 0xFF);
    XSFLIB_CHECK(starts[kFirstVariables + 16 + 4] == 15 && starts[kFirstVariables + 16 + 5] == 4);
    const std::span<const uint8_t> last = starts.subspan(starts.size() - 20);
    XSFLIB_CHECK(xsflib::LoadLe32(&last[0]) == 70000 && xsflib::LoadLe32(&last[4]) == 0 &&
                 xsflib::LoadLe32(&last[8]) == 1 && xsflib::LoadLe32(&last[12]) == 0x80000000 &&
                 xsflib::LoadLe32(&last[16]) == 0);
}

// A start's line, then a line for each field and each variable.
void TestStartsLines()
{
    const StartsFixture fixture = MakeStartsFixture();

    XSFLIB_CHECK(fixture.text.find("\nstart 0 \"\" # Song\n    muted 0xFF80\n    track_volume 2 0.5\n"
                                   "    track_volume 31 0\n    tempo_ratio 1.125\n    pitch 0.5\n    volume 0.7\n"
                                   "    sends 0.4 0 1\n    seed 0x12345678\n    variable 0 3\n    variable 34 1\n"
                                   "    track 3 variable 47 -2\n    at 0.004 variable 16 1\n"
                                   "    at 90 variable 32 7\nstart 0 _LAP # Song\n    silenced 0x1\n"
                                   "    tempo_ratio 1.23\n    low_pass -0.40625\n") != std::string::npos);
    XSFLIB_CHECK(fixture.text.find("\nstart 70000 \"\"\n    muted 0x80000000\neffect A") != std::string::npos);
}

// Only a file of the 3DS keeps a start's banks.
void TestStartsBanks()
{
    File file;
    file.engine = format::Engine::k3ds;
    file.starts = {{.sound = 1, .banks = format::StartBanks{0x05000002, format::kNoStartBank, 0x05000000, 1}}};
    File other = file;
    other.engine = format::Engine::kWiiU;

    const std::string text = format::WriteText(file);
    const File without = format::ReadText(format::WriteText(other));

    XSFLIB_CHECK(format::Read(format::Write(file)) == file && format::ReadText(text) == file);
    XSFLIB_CHECK(text.find("\nstart 1 \"\"\n    banks 0x5000002 none 0x5000000 0x1\n") != std::string::npos);
    XSFLIB_CHECK(!without.starts.at(0).banks && !format::Read(format::Write(other)).starts.at(0).banks);
}

// Only a file of DSE keeps a start's level. STRT gives it flag 0x400 and 4 bytes after the low-pass, and the text form
// a dse_level line. Both forms refuse a level past 256.
void TestStartsDseLevel()
{
    File file;
    file.engine = format::Engine::kDse;
    file.starts = {{.sound = 0, .dse_level = 180}, {.sound = 0, .name = "_LEVEL256", .dse_level = 256}};
    File other = file;
    other.engine = format::Engine::kWiiU;

    const std::vector<uint8_t> binary = format::Write(file);
    const std::string text = format::WriteText(file);
    const std::span<const uint8_t> starts = StartsBytes(binary);

    XSFLIB_CHECK(format::Read(binary) == file && format::ReadText(text) == file);
    XSFLIB_CHECK(text.find("\nstart 0 \"\"\n    dse_level 180\nstart 0 _LEVEL256\n    dse_level 256\n") !=
                 std::string::npos);
    XSFLIB_CHECK(xsflib::LoadLe32(&starts[12]) == 0x400 && xsflib::LoadLe32(&starts[16]) == 180);
    XSFLIB_CHECK(!format::Read(format::Write(other)).starts.at(0).dse_level &&
                 !format::ReadText(format::WriteText(other)).starts.at(0).dse_level);

    XSFLIB_CHECK(TextError("hok 2.0\nengine dse\nstart 1 \"\"\n    dse_level 257\n")
                     .starts_with("Line 4: a level of DSE has to be a whole number from 0 to 256, not 257"));
    XSFLIB_CHECK(TextError("hok 2.0\nengine dse\nstart 1 \"\"\n    dse_level 1\n    dse_level 2\n")
                     .starts_with("Line 5: the start gives its dse_level twice"));
    XSFLIB_CHECK(Patched(binary, "STRT", 17, 2) == "The file is damaged: sound 0's start has a level of DSE of 692.");
}

// Both forms refuse damaged starts. A field or a timed variable of a single track that this version doesn't define gets
// a message that a later version saved the file.
void TestStartsRefused()
{
    const StartsFixture fixture = MakeStartsFixture();
    const auto error = [](std::string_view lines)
    {
        return TextError("hok 2.0\nengine 3ds\n" + std::string(lines));
    };

    XSFLIB_CHECK(error("start 1 \"\"\n    muted 0xFFFFFFFF\nstart 1 a\n    silenced 1\n").empty());
    XSFLIB_CHECK(error("start 1 \"\"\nstart 1 \"\"\n").starts_with("Line 4: sound 1 already has a start named"));
    XSFLIB_CHECK(
        error("start 1 \"\"\n    pitch 1\n    pitch 2\n").starts_with("Line 5: the start gives its pitch twice"));
    XSFLIB_CHECK(error("start 1 \"\"\n    variable 48 0\n").starts_with("Line 4: a variable's number"));
    XSFLIB_CHECK(error("start 1 \"\"\n    track 1 variable 3 0\n")
                     .starts_with("Line 3: the start gives variable 3 a track, and only a track variable has one"));
    XSFLIB_CHECK(error("start 1 \"\"\n    at 1 track 1 variable 33 0\n")
                     .starts_with("Line 3: a later version of Hokuto saved the file"));
    XSFLIB_CHECK(error("start 1 \"\"\n    at 1 variable 3 0\n    variable 4 0\n")
                     .starts_with("Line 3: the start sets its variables out of the order of their times"));
    XSFLIB_CHECK(error("start 1 \"\"\n    variable 3 0\n    variable 3 1\n")
                     .starts_with("Line 3: the start sets variable 3 twice at the start"));
    XSFLIB_CHECK(error("start 1 \"\"\n    sends 0 2 0\n").starts_with("Line 3: the start has a send that isn't"));
    XSFLIB_CHECK(error("start 1 \"\"\n    low_pass 0.5\n")
                     .starts_with("Line 3: the start has a low-pass that isn't a number from -1 to 0"));
    XSFLIB_CHECK(error("start 1 \"\"\n    low_pass -1.5\n")
                     .starts_with("Line 3: the start has a low-pass that isn't a number from -1 to 0"));
    XSFLIB_CHECK(error("start 1 \"\"\n    banks 1 2 3\n").starts_with("Line 4: a start's banks are banks"));
    XSFLIB_CHECK(
        error("sequence 0\n    end\nstart 0 \"\"\n").starts_with("Line 5: the starts come before the sequences"));

    // The flags of the first start, the number of its second variable, the track of its last variable, and the sound of
    // the third start.
    const std::size_t third = StartsBytes(fixture.binary).size() - 40;
    XSFLIB_CHECK(Patched(fixture.binary, "STRT", 14, 2).starts_with("The file contains a sound's start with a field"));
    XSFLIB_CHECK(Patched(fixture.binary, "STRT", kFirstVariables + 8 + 4, 40) ==
                 "The file is damaged: sound 0's start sets track variable 40.");
    XSFLIB_CHECK(Patched(fixture.binary, "STRT", kFirstVariables + 32 + 5, 4)
                     .starts_with("The file contains a change of a single track's variable after a sound's start"));
    XSFLIB_CHECK(Patched(fixture.binary, "STRT", third, 0) ==
                 "The file is damaged: it gives sound 0 two starts named \"\".");
}

// Neither form writes STRT for a file without starts. StartOf gives the start without a name. WithStart puts a new
// start without a name before the sound's states, a state after them, and drops a start without a name that sets
// nothing. A change of a start without a name drops the lengths, and a change of a state keeps them.
void TestStartsEdits()
{
    const StartsFixture fixture = MakeStartsFixture();
    const File& file = fixture.file;
    File measured = file;
    measured.lengths = {{0, {.length = 4500}}};

    const File cleared = format::WithStart(file, {.sound = 3});
    const File added = format::WithStart(file, {.sound = 0, .name = "_GOAL", .muted = 2});
    const File first = format::WithStart(cleared, {.sound = 3, .pitch = 2.0f});
    const File changed = format::WithStart(measured, {.sound = 0, .silenced = 1});
    const File state = format::WithStart(measured, {.sound = 0, .name = "_LAP"});

    XSFLIB_CHECK(ChunkIds(format::Write(WithScenes())).size() + 1 == ChunkIds(fixture.binary).size());
    XSFLIB_CHECK(format::WriteText(WithScenes()).find("\nstart ") == std::string::npos);
    XSFLIB_CHECK((format::StartOf(file, 3) == SoundStart{.sound = 3, .silenced = 0x0006}));
    XSFLIB_CHECK((format::StartOf(file, 9) == SoundStart{.sound = 9}));
    XSFLIB_CHECK((cleared.starts.size() == 3 && format::StartOf(cleared, 3) == SoundStart{.sound = 3}));
    XSFLIB_CHECK(added.starts.size() == 5 && added.starts[2].name == "_GOAL" && added.starts[3].sound == 3);
    XSFLIB_CHECK(first.starts.size() == 4 && first.starts[2].sound == 3 && first.starts[2].pitch == 2.0f);
    XSFLIB_CHECK(format::WithoutStaleLengths(measured, changed).lengths.empty());
    XSFLIB_CHECK(format::WithoutStaleLengths(measured, state).lengths == measured.lengths);
}

// Both forms write version 2.0 and read version 1. A later major version gets a message. MajorVersionOf reads the major
// version of either form, and gives nothing for other bytes. A file of version 1 whose archive no list knows gets no
// starts on opening.
void TestVersions()
{
    const StartsFixture fixture = MakeStartsFixture();
    std::vector<uint8_t> older = format::Write(WithScenes());
    older[8] = 1;
    older[10] = 2;
    std::vector<uint8_t> later = older;
    later[8] = 3;
    const std::string older_text = "hok 1.2\nengine 3ds\n";

    XSFLIB_CHECK(format::kMajorVersion == 2 && format::kMinorVersion == 0);
    XSFLIB_CHECK(fixture.text.starts_with("hok 2.0\n"));
    XSFLIB_CHECK(format::MajorVersionOf(fixture.binary) == 2 && format::MajorVersionOf(BytesOf(fixture.text)) == 2);
    XSFLIB_CHECK(format::MajorVersionOf(older) == 1 && format::MajorVersionOf(BytesOf(older_text)) == 1);
    XSFLIB_CHECK(!format::MajorVersionOf(std::vector<uint8_t>{1, 2, 3}).has_value());
    XSFLIB_CHECK(format::Read(older) == WithScenes() && format::ReadText(older_text).engine == format::Engine::k3ds);
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { format::Read(later); }) ==
                 "A later version of Hokuto saved the file, in version 3 of the format. This version reads versions 1 "
                 "and 2.");
    XSFLIB_CHECK(TextError("hok 3.0\nengine 3ds\n").starts_with("Line 1: a later version of Hokuto saved the file"));
    XSFLIB_CHECK(format::WithOlderStarts(WithScenes(), older).starts.empty());
}

// A scene keeps the track variables of every track as 32 to 47: in SCEN, the variable's number from 0 to 15 and 0xFF
// in the byte after it.
void TestSceneTrackVariables()
{
    File file = WithScenes();
    file.scenes[1].values = {{3, 1}, {34, 2}, {47, -1}};
    file.scenes[1].changes = {{1000, 32, 5}};

    const std::vector<uint8_t> binary = format::Write(file);
    const std::string text = format::WriteText(file);
    const std::span<const uint8_t> scenes = ChunkBytes(binary, 6);

    XSFLIB_CHECK(format::Read(binary) == file && format::ReadText(text) == file);
    XSFLIB_CHECK(text.find("\n    variable 34 2\n    variable 47 -1\n    at 1 variable 32 5\n") != std::string::npos);
    const std::array<uint8_t, 8> values = {3, 0, 1, 0, 2, 0xFF, 2, 0};
    XSFLIB_CHECK(std::ranges::search(scenes, values).begin() != scenes.end());
    XSFLIB_CHECK(TextError("hok 2.0\nengine 3ds\nscene 0 a\n    variable 48 0\n").starts_with("Line 4:"));
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("file_starts",
                                    []
                                    {
                                        TestStartsRoundTrip();
                                        TestStartsChunk();
                                        TestStartsLines();
                                        TestStartsBanks();
                                        TestStartsDseLevel();
                                        TestStartsRefused();
                                        TestStartsEdits();
                                        TestVersions();
                                        TestSceneTrackVariables();
                                    });
}
