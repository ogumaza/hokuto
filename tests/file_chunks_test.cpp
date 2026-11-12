// SPDX-License-Identifier: MIT

// Tests three chunks of a File in both forms of Hokuto's files (src/format/file.cpp and text.cpp): the view, the
// tags and the lengths. Each comes back unchanged through both forms. Damaged chunks, refused lines and values that
// aren't finite throw with a message. Neither form writes a chunk for a file without tags or lengths. An edit that can
// change what a sound plays drops the lengths.

#include "format_testing.h"

namespace
{

struct ViewFixture
{
    File file;
    File closed;
    std::vector<uint8_t> binary;
    std::string text;
    std::string closed_text;
    std::vector<std::string> ids;
};

// A file with a view, a copy with no optional parts in its view, and both files in each form.
ViewFixture MakeViewFixture()
{
    File file = WithScenes();
    file.track_colours = {{{.sound = 0, .track = 0}, {.red = 74, .green = 127, .blue = 224}}};
    file.view = EditorView{.sound = 1,
                           .position = 4.5f,
                           .cycle_start = 2.0f,
                           .cycle_end = 18.25f,
                           .cycle = true,
                           .beat_width = 24.0f,
                           .track_height = 66.0f,
                           .left = 3.5f,
                           .top = 1.25f,
                           .selected_track = 3,
                           .muted = 0b1010,
                           .soloed = 1u << 31,
                           .key_beat_width = 64.0f,
                           .key_height = 12.0f,
                           .key_left = 0.5f,
                           .key_top = 96.0f,
                           .lanes = {{Kind::kVolume, 0}, {Kind::kSend, 2}},
                           .key_editor_track = 2,
                           .list_editor_track = 0,
                           .front = ViewFront::kKeyEditor};
    File closed = file;
    closed.view->selected_track.reset();
    closed.view->key_editor_track.reset();
    closed.view->list_editor_track.reset();
    closed.view->muted = 0;
    closed.view->soloed = 0;
    closed.view->lanes.clear();

    const std::vector<uint8_t> binary = format::Write(file);
    const std::string text = format::WriteText(file);
    const std::string closed_text = format::WriteText(closed);
    const std::vector<std::string> kIds = {"ENGN", "MIXR", "SEQS", "NATV", "TABL", "GRPF",
                                           "SCEN", "TCOL", "EDTR", "XTRA", "x y!"};

    return {file, closed, binary, text, closed_text, kIds};
}

// The program's view, in both forms.
void TestViewRoundTrip()
{
    const ViewFixture fixture = MakeViewFixture();
    const File& file = fixture.file;
    const File& closed = fixture.closed;
    const std::vector<uint8_t>& binary = fixture.binary;
    const std::string& text = fixture.text;
    const std::string& closed_text = fixture.closed_text;

    XSFLIB_CHECK(format::Read(binary) == file);
    XSFLIB_CHECK(format::ReadText(text) == file);
    XSFLIB_CHECK(format::WriteText(format::ReadText(text)) == text);
    XSFLIB_CHECK(format::Read(format::Write(closed)) == closed);
    XSFLIB_CHECK(format::ReadText(closed_text) == closed);
}

// EDTR follows TCOL.
void TestViewChunk()
{
    const ViewFixture fixture = MakeViewFixture();
    const File& closed = fixture.closed;
    const std::vector<uint8_t>& binary = fixture.binary;
    const std::vector<std::string>& kIds = fixture.ids;

    // The sound, the position and the cycle's locators, then a byte for the cycle and one for each track: the selected
    // one, the Key Editor's and the List Editor's, or 0xFF for none. The arrangement's sizes, scroll and sets of
    // tracks, the Key Editor's sizes and scroll, and the lanes: the number of each one's kind in 2 bytes, its bus and a
    // 0. A byte for the window in front ends the chunk: 1 for the Key Editor.
    const std::span<const uint8_t> view = ChunkBytes(binary, 8);
    const auto f32 = [&view](std::size_t at)
    {
        return std::bit_cast<float>(xsflib::LoadLe32(&view[at]));
    };

    XSFLIB_CHECK(ChunkIds(binary) == kIds);
    SEQLIB_REQUIRE(view.size() == 73);
    XSFLIB_CHECK(xsflib::LoadLe32(&view[0]) == 1);
    XSFLIB_CHECK(f32(4) == 4.5f && f32(8) == 2.0f && f32(12) == 18.25f);
    XSFLIB_CHECK(view[16] == 1 && view[17] == 3 && view[18] == 2 && view[19] == 0);
    XSFLIB_CHECK(f32(20) == 24.0f && f32(24) == 66.0f && f32(28) == 3.5f && f32(32) == 1.25f);
    XSFLIB_CHECK(xsflib::LoadLe32(&view[36]) == 10 && xsflib::LoadLe32(&view[40]) == 0x80000000u);
    XSFLIB_CHECK(f32(44) == 64.0f && f32(48) == 12.0f && f32(52) == 0.5f && f32(56) == 96.0f);
    XSFLIB_CHECK(xsflib::LoadLe32(&view[60]) == 2);
    XSFLIB_CHECK(xsflib::LoadLe16(&view[64]) == format::CodeOf(Kind::kVolume) && view[66] == 0 && view[67] == 0);
    XSFLIB_CHECK(xsflib::LoadLe16(&view[68]) == format::CodeOf(Kind::kSend) && view[70] == 2 && view[71] == 0);
    XSFLIB_CHECK(view[72] == 1);
    const std::vector<uint8_t> closed_binary = format::Write(closed);
    const std::span<const uint8_t> closed_view = ChunkBytes(closed_binary, 8);
    XSFLIB_CHECK(closed_view.size() == 65 && closed_view[17] == 0xFF && closed_view[18] == 0xFF &&
                 closed_view[19] == 0xFF && closed_view[64] == 1);
}

// The text form writes the view's lines, and leaves out the lines of the parts that are missing.
void TestViewLines()
{
    const ViewFixture fixture = MakeViewFixture();
    const File& file = fixture.file;
    const std::string& text = fixture.text;
    const std::string& closed_text = fixture.closed_text;

    // Lines after the tracks' colours. The tables give sound 1 no name, and its line no comment. Sound 0's line gives
    // its name. A closed editor, no selected track and no muted or soloed tracks leave out their lines.
    File named = file;
    named.view->sound = 0;
    XSFLIB_CHECK(text.find("\n\nview sound 1\nview position 4.5\nview cycle 2 18.25 on\nview zoom 24 66\n"
                           "view scroll 3.5 1.25\nview track 3\nview mute 1 3\nview solo 31\nview key_zoom 64 12\n"
                           "view key_scroll 0.5 96\nview lane volume 0\nview lane send 2\nview key_editor 2\n"
                           "view list_editor 0\nview front key_editor\n\nsequence 0\n") != std::string::npos);
    XSFLIB_CHECK(closed_text.find("view scroll 3.5 1.25\nview key_zoom 64 12\nview key_scroll 0.5 96\n"
                                  "view front key_editor\n\nsequence") != std::string::npos);
    XSFLIB_CHECK(format::WriteText(named).find("\n\nview sound 0 # Song\nview position") != std::string::npos);
}

// The text form refuses a view's damaged lines, and reads the window in front.
void TestViewRefusedLines()
{
    // Lines that a reader refuses. A view gives each part that the writers always write.
    const auto error = [](std::string_view lines)
    {
        return TextError("hok 1.0\nengine switch\n" + std::string(lines));
    };

    const std::string kParts = "view position 0\nview cycle 0 1 off\nview scroll 0 0\nview key_scroll 0 127\n";
    const std::string kSized = "view sound 0\nview zoom 1 1\nview key_zoom 1 1\n" + kParts;
    XSFLIB_CHECK(error(kSized).empty());
    XSFLIB_CHECK(error("view sound 0\nview sound 1\n").starts_with("Line 4: the view sets its sound twice"));
    XSFLIB_CHECK(error("view position 1\n").starts_with("Line 3: the view has no sound"));
    XSFLIB_CHECK(error("view sound 0\n").starts_with("Line 3: the view has no position"));
    XSFLIB_CHECK(error("view sound 0\nview zoom 1 1\nview key_zoom 1 1\nview position 0\nview cycle 0 1 off\n"
                       "view scroll 0 0\n")
                     .starts_with("Line 3: the view has no key_scroll"));
    XSFLIB_CHECK(error("view sound 0\nview zoom 1 0\nview key_zoom 1 1\n" + kParts)
                     .starts_with("Line 3: the view has a size of 0 or less"));
    XSFLIB_CHECK(error(kSized + "view track 32\n")
                     .starts_with("Line 10: a track's number has to be a whole number from 0 to 31, not 32"));
    XSFLIB_CHECK(error(kSized + "view lane loud 0\n").starts_with("Line 10: there's no command type named loud"));
    XSFLIB_CHECK(error("view cycle 0 1 maybe\n").starts_with("Line 3: the cycle is on or off, not maybe"));
    XSFLIB_CHECK(error(kSized + "view colour 1\n").starts_with("Line 10: the view has no part named colour"));
    XSFLIB_CHECK(error(kSized + "view front piano\n")
                     .starts_with("Line 10: the window in front is mixer, key_editor or list_editor, not piano"));
    XSFLIB_CHECK(
        error(kSized + "view front\n").starts_with("Line 10: the view's front is mixer, key_editor or list_editor"));
    XSFLIB_CHECK(
        error(kSized + "view mute\n").starts_with("Line 10: the view's mute has to list the numbers of tracks"));
    XSFLIB_CHECK(error(kSized + "view zoom 1\n").starts_with("Line 10: the view sets its zoom twice"));
    XSFLIB_CHECK(error("view sound 0\nview zoom 1\n")
                     .starts_with("Line 4: the view's zoom is the width of a beat and the height of a track"));
    XSFLIB_CHECK(error("view\n").starts_with("Line 3: a line of the view is view"));
    XSFLIB_CHECK(
        error("sequence 0\n    end\nview sound 0\n").starts_with("Line 5: the view comes before the sequences"));

    // A view without a window in front puts the List Editor in front. Hokuto opened such a file that way. Each window
    // reads by its name.
    const auto front = [&kSized](std::string_view line)
    {
        return format::ReadText("hok 1.0\nengine switch\n" + kSized + std::string(line)).view->front;
    };
    XSFLIB_CHECK(front("") == ViewFront::kListEditor && front("view front mixer\n") == ViewFront::kMixer &&
                 front("view front key_editor\n") == ViewFront::kKeyEditor &&
                 front("view front list_editor\n") == ViewFront::kListEditor);
}

// The binary form refuses a damaged view, and skips the bytes that a later version may add.
void TestViewDamagedChunk()
{
    const ViewFixture fixture = MakeViewFixture();
    const File& file = fixture.file;
    const std::vector<uint8_t>& binary = fixture.binary;
    const std::span<const uint8_t> view = ChunkBytes(binary, 8);

    // The binary form refuses a cycle that's neither on nor off, a track past 31, a size below 0, and a kind of command
    // or a window in front that this version doesn't define. A track's height of -66 has the sign bit in its last byte.
    // The form skips the bytes after the window in front: a count of one lane leaves the second lane's bytes after it,
    // and their first byte gives the window. A chunk without the window puts the List Editor in front.
    XSFLIB_CHECK(Patched(binary, 8, 16, 2) == "The file is damaged: the view's cycle is neither on nor off.");
    XSFLIB_CHECK(Patched(binary, 8, 17, 32) ==
                 "The file is damaged: the view names track 32, and tracks run from 0 to 31.");
    XSFLIB_CHECK(Patched(binary, 8, 27, 0xC2) == "The file is damaged: the view has a size of 0 or less.");
    XSFLIB_CHECK(Patched(binary, 8, 65, 0xFF).starts_with("The file contains a lane for a command ("));
    XSFLIB_CHECK(Patched(binary, 8, 72, 3) ==
                 "The file contains a window in front of the editors (3) that this version of "
                 "Hokuto doesn't support. A later version of Hokuto saved it.");
    std::vector<uint8_t> one_lane = binary;
    uint8_t* entry = &one_lane[24 + 24 * 8];
    const auto start = static_cast<std::size_t>(xsflib::LoadLe64(entry + 8));
    one_lane[start + 60] = 1;
    one_lane[start + 68] = 2;
    xsflib::StoreLe32(entry + 4, format::Crc32(std::span(one_lane).subspan(start, view.size())));
    File first_lane = file;
    first_lane.view->lanes.pop_back();
    first_lane.view->front = ViewFront::kListEditor;
    XSFLIB_CHECK(format::Read(one_lane) == first_lane);
    std::vector<uint8_t> without_front = binary;
    entry = &without_front[24 + 24 * 8];
    xsflib::StoreLe64(entry + 16, view.size() - 1);
    xsflib::StoreLe32(entry + 4, format::Crc32(std::span(without_front).subspan(start, view.size() - 1)));
    File listed = file;
    listed.view->front = ViewFront::kListEditor;
    XSFLIB_CHECK(format::Read(without_front) == listed);
}

// The forms refuse a view with a number that isn't finite, and write no EDTR chunk for a file without a view.
void TestViewNotFinite()
{
    const ViewFixture fixture = MakeViewFixture();
    const File& file = fixture.file;
    const std::vector<std::string>& kIds = fixture.ids;

    // Neither form writes a view with a number that isn't finite. A file without a view has no EDTR chunk.
    File infinite = file;
    infinite.view->left = std::numeric_limits<float>::infinity();
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { format::Write(infinite); }));
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { format::WriteText(infinite); }));
    File none = file;
    none.view.reset();
    XSFLIB_CHECK(ChunkIds(format::Write(none)).size() == kIds.size() - 1);
}

struct TagsFixture
{
    File file;
    File empty;
    std::vector<uint8_t> binary;
    std::string text;
    std::vector<uint8_t> expected;
    std::size_t last_key = 0;
    std::size_t last_value = 0;
    std::size_t second_sound = 0;
    std::vector<std::string> ids;
};

// A file with tags in each form, and the bytes that its TAGS chunk contains.
TagsFixture MakeTagsFixture()
{
    File file = WithScenes();
    file.tags = {{"title", "A \"quoted\" title"}, {"year", "2026"}, {"zz_8", "y"}, {"zz_9", "x"}};
    file.sound_tags = {{0,
                        {{"artist",
                          "\xC3\x9Cn\xC3\xAF"
                          "code"},
                         {"title", "Lap\nTwo"}}},
                       {7, {{"comment", "on"}}}};
    File empty = file;
    empty.sound_tags[3];

    const std::vector<uint8_t> binary = format::Write(file);
    const std::string text = format::WriteText(file);

    // The number of the file's tags and each tag's key and value, then the number of sounds with tags, and each sound's
    // index and tags. The keys come in order.
    std::vector<uint8_t> expected;
    const auto u32 = [&expected](uint32_t value)
    {
        std::array<uint8_t, 4> bytes{};
        xsflib::StoreLe32(bytes.data(), value);
        expected.insert(expected.end(), bytes.begin(), bytes.end());
    };
    const auto name = [&expected, &u32](std::string_view piece)
    {
        u32(static_cast<uint32_t>(piece.size()));
        expected.insert(expected.end(), piece.begin(), piece.end());
    };

    u32(4);
    name("title");
    name("A \"quoted\" title");
    name("year");
    name("2026");
    name("zz_8");
    name("y");
    const std::size_t last_key = expected.size() + 4;
    name("zz_9");
    const std::size_t last_value = expected.size();
    name("x");
    u32(2);
    u32(0);
    u32(2);
    name("artist");
    name(
        "\xC3\x9Cn\xC3\xAF"
        "code");
    name("title");
    name("Lap\nTwo");
    const std::size_t second_sound = expected.size();
    u32(7);
    u32(1);
    name("comment");
    name("on");
    const std::vector<std::string> kIds = {"ENGN", "MIXR", "SEQS", "NATV", "TABL",
                                           "GRPF", "SCEN", "TAGS", "XTRA", "x y!"};

    return {file, empty, binary, text, expected, last_key, last_value, second_sound, kIds};
}

// The tags of the file and of its sounds, in both forms.
void TestTagsRoundTrip()
{
    const TagsFixture fixture = MakeTagsFixture();
    const File& file = fixture.file;
    const File& empty = fixture.empty;
    const std::vector<uint8_t>& binary = fixture.binary;
    const std::string& text = fixture.text;

    XSFLIB_CHECK(format::Read(binary) == file);
    XSFLIB_CHECK(format::ReadText(text) == file);
    XSFLIB_CHECK(format::WriteText(format::ReadText(text)) == text);
    XSFLIB_CHECK(format::Read(format::Write(empty)) == file);
    XSFLIB_CHECK(format::WriteText(empty) == text);
}

// TAGS follows SCEN.
void TestTagsChunk()
{
    const TagsFixture fixture = MakeTagsFixture();
    const std::vector<uint8_t>& binary = fixture.binary;
    const std::vector<uint8_t>& expected = fixture.expected;
    const std::vector<std::string>& kIds = fixture.ids;

    XSFLIB_CHECK(ChunkIds(binary) == kIds);
    XSFLIB_CHECK(std::ranges::equal(ChunkBytes(binary, 7), expected));
}

// Sound 7 lies past the tables.
void TestTagsLines()
{
    const TagsFixture fixture = MakeTagsFixture();
    const std::string& text = fixture.text;

    // The lines follow the mixing line: the file's tags, then each sound's, with the sound's name where the tables give
    // one.
    const std::size_t mixing = text.find("\nmixing ");
    XSFLIB_CHECK(text.find("\ntag title") == text.find('\n', mixing + 1));
    XSFLIB_CHECK(text.find("\ntag title \"A \\\"quoted\\\" title\"\ntag year 2026\ntag zz_8 y\ntag zz_9 x\n"
                           "sound_tag 0 artist \"\xC3\x9Cn\xC3\xAF"
                           "code\" # Song\nsound_tag 0 title \"Lap\\x0ATwo\" # Song\n"
                           "sound_tag 7 comment on\neffect A") != std::string::npos);
}

// The text form refuses damaged tags.
void TestTagsRefusedLines()
{
    // Lines that a reader refuses.
    const auto error = [](std::string_view lines)
    {
        return TextError("hok 1.0\nengine switch\n" + std::string(lines));
    };

    const std::string kLongest(64, 'a');
    XSFLIB_CHECK(error("tag " + kLongest + " x\nsound_tag 3 t0_ \"\\x00\"\n").empty());
    XSFLIB_CHECK(
        error("tag " + kLongest + "a x\n").starts_with("Line 3: tag " + kLongest + "a has a key other than 1"));
    XSFLIB_CHECK(error("tag Title x\n")
                     .starts_with("Line 3: tag Title has a key other than 1 to 64 lower-case letters, digits and "
                                  "underscores from a letter on"));
    XSFLIB_CHECK(error("tag 9lives x\n").starts_with("Line 3: tag 9lives has a key other than"));
    XSFLIB_CHECK(error("tag title \"\"\n").starts_with("Line 3: tag title has no value"));
    XSFLIB_CHECK(error("tag title a\ntag title b\n").starts_with("Line 4: tag title comes twice"));
    XSFLIB_CHECK(
        error("sound_tag 3 title a\nsound_tag 3 title b\n").starts_with("Line 4: sound 3's tag title comes twice"));
    XSFLIB_CHECK(error("sound_tag 3 title a\nsound_tag 4 title b\ntag title c\n").empty());
    XSFLIB_CHECK(error("tag title\n").starts_with("Line 3: a tag is tag, its key and its value"));
    XSFLIB_CHECK(error("tag \"title\" x\n").starts_with("Line 3: a tag is tag, its key and its value"));
    XSFLIB_CHECK(error("sound_tag 3 title\n").starts_with("Line 3: a sound's tag is sound_tag, the sound's number"));
    XSFLIB_CHECK(error("sound_tag 3 Title x\n").starts_with("Line 3: sound 3's tag Title has a key other than"));
    XSFLIB_CHECK(error("sequence 0\n    end\ntag title x\n").starts_with("Line 5: the tags come before the sequences"));
}

// The binary form refuses damaged tags.
void TestTagsDamagedChunk()
{
    const TagsFixture fixture = MakeTagsFixture();
    const std::vector<uint8_t>& binary = fixture.binary;
    const std::size_t last_key = fixture.last_key;
    const std::size_t last_value = fixture.last_value;
    const std::size_t second_sound = fixture.second_sound;

    // The binary form refuses a key twice, a key with a capital, a value of no bytes and a sound's tags twice.
    XSFLIB_CHECK(Patched(binary, 7, last_key + 3, '8') == "The file is damaged: tag zz_8 comes twice.");
    XSFLIB_CHECK(Patched(binary, 7, last_key + 3, 'Z') ==
                 "The file is damaged: tag zz_Z has a key other than 1 to 64 lower-case letters, digits and "
                 "underscores from a letter on.");
    XSFLIB_CHECK(Patched(binary, 7, last_value, 0) == "The file is damaged: tag zz_9 has no value.");
    XSFLIB_CHECK(Patched(binary, 7, second_sound, 0) == "The file is damaged: sound 0's tags come twice.");
}

// The forms refuse to write tags that a reader refuses, and write no TAGS chunk for a file without tags.
void TestTagsNotWritten()
{
    const TagsFixture fixture = MakeTagsFixture();
    const File& file = fixture.file;
    const std::vector<std::string>& kIds = fixture.ids;

    // Neither form writes a tag without a value, or with a key that a reader refuses. A file without tags has no TAGS
    // chunk.
    File valueless = file;
    valueless.sound_tags[0]["title"].clear();
    File capital = file;
    capital.tags.emplace("Title", "x");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { format::Write(valueless); }) ==
                 "sound 0's tag title has no value");
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { format::WriteText(valueless); }));
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { format::Write(capital); }));
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { format::WriteText(capital); }));
    File none = file;
    none.tags.clear();
    none.sound_tags = {{2, {}}};
    XSFLIB_CHECK(ChunkIds(format::Write(none)).size() == kIds.size() - 1);
}

// WithTag sets, adds and removes a tag.
void TestTagsSet()
{
    const TagsFixture fixture = MakeTagsFixture();
    const File& file = fixture.file;

    // A tag that the program sets: a new value, a new key, and an empty value that removes the tag. A sound whose last
    // tag goes has no tags left.
    const File retitled = format::WithTag(file, std::nullopt, "title", "B");
    const File dated = format::WithTag(file, 3, "year", "1999");
    const File untitled = format::WithTag(file, 7, "comment", "");
    const File missing = format::WithTag(file, 5, "comment", "");
    XSFLIB_CHECK(retitled.tags.at("title") == "B" && retitled.tags.size() == 4 &&
                 retitled.sound_tags == file.sound_tags);
    XSFLIB_CHECK(dated.sound_tags.size() == 3 && (dated.sound_tags.at(3) == format::Tags{{"year", "1999"}}));
    XSFLIB_CHECK(untitled.sound_tags.size() == 1 && untitled.sound_tags.contains(0) && untitled.tags == file.tags);
    XSFLIB_CHECK(missing.sound_tags == file.sound_tags);
}

struct LengthsFixture
{
    File file;
    std::vector<uint8_t> binary;
    std::string text;
    std::vector<std::string> ids;
};

// A file with the lengths of four sounds, in each form.
LengthsFixture MakeLengthsFixture()
{
    File file = WithScenes();
    file.lengths = {
        {0, {.length = 151234, .fade = 10000, .loops = 2, .loop = format::LoopTicks{96, 1632}}},
        {3, {.length = 4500}},
        {9, {.length = 600000, .fade = 10000}},
        {70000, {.length = 500, .silent = true}},
    };

    const std::vector<uint8_t> binary = format::Write(file);
    const std::string text = format::WriteText(file);
    const std::vector<std::string> kIds = {"ENGN", "MIXR", "SEQS", "NATV", "TABL",
                                           "GRPF", "SCEN", "LNTH", "XTRA", "x y!"};

    return {file, binary, text, kIds};
}

// The sounds' lengths, in both forms.
void TestLengthsRoundTrip()
{
    const LengthsFixture fixture = MakeLengthsFixture();
    const File& file = fixture.file;
    const std::vector<uint8_t>& binary = fixture.binary;
    const std::string& text = fixture.text;

    XSFLIB_CHECK(format::Read(binary) == file);
    XSFLIB_CHECK(format::ReadText(text) == file);
    XSFLIB_CHECK(format::WriteText(format::ReadText(text)) == text);
}

// LNTH follows TAGS.
void TestLengthsChunk()
{
    const LengthsFixture fixture = MakeLengthsFixture();
    const std::vector<uint8_t>& binary = fixture.binary;
    const std::vector<std::string>& kIds = fixture.ids;

    // The number of sounds, then each one: the sound in 4 bytes, the flags, the loops and two zeros, the length and the
    // fade in 4 bytes each, and the loop's ticks with flag 2. Flag 1 marks a silent sound.
    const std::span<const uint8_t> lengths = ChunkBytes(binary, 7);
    XSFLIB_CHECK(ChunkIds(binary) == kIds);
    SEQLIB_REQUIRE(lengths.size() == 4 + 24 + 3 * 16);
    XSFLIB_CHECK(xsflib::LoadLe32(&lengths[0]) == 4);
    XSFLIB_CHECK(xsflib::LoadLe32(&lengths[4]) == 0 && lengths[8] == 2 && lengths[9] == 2 && lengths[10] == 0 &&
                 lengths[11] == 0 && xsflib::LoadLe32(&lengths[12]) == 151234 &&
                 xsflib::LoadLe32(&lengths[16]) == 10000 && xsflib::LoadLe32(&lengths[20]) == 96 &&
                 xsflib::LoadLe32(&lengths[24]) == 1632);
    XSFLIB_CHECK(xsflib::LoadLe32(&lengths[28]) == 3 && lengths[32] == 0 && lengths[33] == 0 &&
                 xsflib::LoadLe32(&lengths[36]) == 4500 && xsflib::LoadLe32(&lengths[40]) == 0);
    XSFLIB_CHECK(xsflib::LoadLe32(&lengths[44]) == 9 && xsflib::LoadLe32(&lengths[52]) == 600000 &&
                 xsflib::LoadLe32(&lengths[56]) == 10000);
    XSFLIB_CHECK(xsflib::LoadLe32(&lengths[60]) == 70000 && lengths[64] == 1 && xsflib::LoadLe32(&lengths[68]) == 500);
}

// Sound 70000 lies past the tables.
void TestLengthsLines()
{
    const LengthsFixture fixture = MakeLengthsFixture();
    const std::string& text = fixture.text;

    // A line for each sound after the tags, with the sound's name where the tables give it.
    XSFLIB_CHECK(text.find("\nsound_length 0 151234 10000 2 96 1632 # Song\nsound_length 3 4500 0 0 # sound\n"
                           "sound_length 9 600000 10000 0\nsound_length 70000 500 0 0 silent\neffect A") !=
                 std::string::npos);
}

// Both forms refuse damaged lengths.
void TestLengthsRefused()
{
    const LengthsFixture fixture = MakeLengthsFixture();
    const std::vector<uint8_t>& binary = fixture.binary;

    // Lines that a reader refuses.
    const auto error = [](std::string_view lines)
    {
        return TextError("hok 1.0\nengine switch\n" + std::string(lines));
    };
    XSFLIB_CHECK(error("sound_length 1 2 3 0 silent\nsound_length 2 2 3 2 4 5\n").empty());
    XSFLIB_CHECK(error("sound_length 1 2 3\n").starts_with("Line 3: a sound's lengths are sound_length"));
    XSFLIB_CHECK(error("sound_length 1 2 3 2 4\n").starts_with("Line 3: a sound's lengths are sound_length"));
    XSFLIB_CHECK(error("sound_length 1 2 3 0 4 5\n")
                     .starts_with("Line 3: sound 1's lengths have loop ticks but a loop count of 0"));
    XSFLIB_CHECK(error("sound_length 1 2 3 2 5 5\n")
                     .starts_with("Line 3: sound 1's lengths have a loop whose end at tick 5 isn't after its start at "
                                  "tick 5"));
    XSFLIB_CHECK(error("sound_length 1 2 3 256\n")
                     .starts_with("Line 3: a sound's number of loops has to be a whole number from 0 to 255, not 256"));
    XSFLIB_CHECK(
        error("sound_length 1 2 3 0\nsound_length 1 4 5 0\n").starts_with("Line 4: sound 1's lengths come twice"));
    XSFLIB_CHECK(error("sequence 0\n    end\nsound_length 0 1 0 0\n")
                     .starts_with("Line 5: the sounds' lengths come before the sequences"));

    // The binary form refuses a flag that this version doesn't define, a loop's ticks without loops, and a sound's
    // lengths twice.
    XSFLIB_CHECK(Patched(binary, 7, 8, 6) ==
                 "The file contains sound lengths with a flag that this version of Hokuto "
                 "doesn't support. A later version of Hokuto saved it.");
    XSFLIB_CHECK(Patched(binary, 7, 9, 0) ==
                 "The file is damaged: sound 0's lengths have loop ticks but a loop count of 0.");
    XSFLIB_CHECK(Patched(binary, 7, 28, 0) == "The file is damaged: sound 0's lengths come twice.");
}

// The forms refuse to write a backward loop, and write no LNTH chunk for a file without lengths.
void TestLengthsNotWritten()
{
    const LengthsFixture fixture = MakeLengthsFixture();
    const File& file = fixture.file;
    const std::vector<std::string>& kIds = fixture.ids;

    // Neither form writes a loop that ends before it starts. A file without lengths has no LNTH chunk.
    File backward = file;
    backward.lengths[0].loop = format::LoopTicks{1632, 96};
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { format::Write(backward); }) ==
                 "sound 0's lengths have a loop whose end at tick 96 isn't after its start at tick 1632");
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { format::WriteText(backward); }));
    XSFLIB_CHECK(ChunkIds(format::Write(WithScenes())).size() == kIds.size() - 1);
}

// WithoutStaleLengths drops the lengths after an edit that can change what a sound plays, and keeps them after others.
void TestLengthsStale()
{
    const LengthsFixture fixture = MakeLengthsFixture();
    const File& file = fixture.file;

    // An edit that can change what a sound plays drops the lengths: a new copy of a sequence or of the native archive,
    // a bus's volume, an effect or the file's engine. Hokuto measures the lengths in strict mixing. A tag, a scene, a
    // track's colour, a mixing mode or a sound's engine keeps them.
    const auto kept = [&file](const File& after)
    {
        return format::WithoutStaleLengths(file, after).lengths == file.lengths;
    };

    const auto dropped = [&file](const File& after)
    {
        return format::WithoutStaleLengths(file, after).lengths.empty();
    };

    File sequence = file;
    sequence.sequences[0] = format::Shared<Sequence>(*file.sequences[0]);
    File native = file;
    native.native.Edit([](format::NativeArchive&) {});
    File volume = file;
    volume.aux_volumes[1] = 0.5f;
    File effect = file;
    effect.aux_effects[1] = file.aux_effects[2];
    File engine = file;
    engine.engine = format::Engine::kWiiU;
    File mixing = file;
    mixing.mixing = file.mixing == Mixing::kStrict ? Mixing::kEnhanced : Mixing::kStrict;
    File coloured = file;
    coloured.track_colours[{.sound = 0, .track = 1}] = {.red = 1};
    File scened = file;
    scened.scenes.pop_back();
    XSFLIB_CHECK(kept(format::WithTag(file, 0, "title", "B")) && kept(coloured) && kept(scened));
    XSFLIB_CHECK(kept(mixing) && kept(format::WithMixing(file, 0, Mixing::kStrict)));
    XSFLIB_CHECK(kept(format::WithEngine(file, 0, format::Engine::kWiiU)));
    XSFLIB_CHECK(dropped(sequence) && dropped(native) && dropped(volume) && dropped(effect) && dropped(engine));
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("file_chunks",
                                    []
                                    {
                                        TestViewRoundTrip();
                                        TestViewChunk();
                                        TestViewLines();
                                        TestViewRefusedLines();
                                        TestViewDamagedChunk();
                                        TestViewNotFinite();
                                        TestTagsRoundTrip();
                                        TestTagsChunk();
                                        TestTagsLines();
                                        TestTagsRefusedLines();
                                        TestTagsDamagedChunk();
                                        TestTagsNotWritten();
                                        TestTagsSet();
                                        TestLengthsRoundTrip();
                                        TestLengthsChunk();
                                        TestLengthsLines();
                                        TestLengthsRefused();
                                        TestLengthsNotWritten();
                                        TestLengthsStale();
                                    });
}
