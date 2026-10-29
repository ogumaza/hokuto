// SPDX-License-Identifier: MIT

// Tests Hokuto's files (src/format/): the numbers of the kinds, engines, effects, archives and sounds, the CRC-32, the
// binary form, the text form, and the Switch's sound archives into files and back. An archive made for the tests holds
// two sequence files and a file of other bytes. A file made by hand holds every unusual part that a command can have,
// labels whose names take quotes, an empty sequence, effects with unusual floats, tables with unusual values and chunks
// that Hokuto doesn't read. Each comes back unchanged through both forms. Damaged files and unknown parts throw with a
// message. An archive with a group whose file holds a copy of a sequence shows how an export measures growth. A 3DS
// archive comes back the same way, with the 3DS's engine. A Wii U file holds the console's four effects. Both forms
// refuse a reverb's mode outside its choices. The aux buses' volumes come back through both forms, and both refuse a
// volume outside 0 to 2. A sequence file that only the group files beside a 3DS archive hold becomes one of the file's
// sequences. An archive of the Wii comes back too, with the values of its tables and the game's build of NW4R SND. A
// sound's bank slots export in a list of another length. An export measures its growth from the game's sizes. The text
// form reads a command's value as the nearest 2^-16, and an effect's parameter as the nearest float. Both forms keep
// the Wii's and the 3DS's effects, the scenes, the tracks' colours, the program's view, the tags, the lengths, the
// sounds' mixing modes and engines, the timelines, the streams beside the archive, and the models of edited banks and
// waves. The group files beside an archive come back cut, on the Switch and on the 3DS. Copies of a file share its
// parts. A read of the text refuses a value that the console's commands can't hold. The messages name the kinds of the
// files that Hokuto doesn't take.

#include <algorithm>
#include <array>
#include <bit>
#include <clocale>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/rsar/sequence_file.h"
#include "ntdlib/sar/bank_file.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/groups.h"
#include "ntdlib/sar/layout.h"
#include "ntdlib/sar/sarc.h"
#include "ntdlib/sound_build.h"
#include "ntdlib/testing/rsar_tools.h"
#include "ntdlib/testing/sar_files.h"
#include "ntdlib/testing/sar_tools.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/sha1.h"
#include "xsflib/testing/check.h"

#include "format/archive.h"
#include "format/codes.h"
#include "format/file.h"
#include "format/kinds.h"
#include "format/model.h"
#include "format/text.h"

namespace
{

namespace sar_files = ntdlib::testing::sar_files;
namespace sar_tools = ntdlib::testing::sar_tools;
using hokuto::format::EditorView;
using hokuto::format::Effect;
using hokuto::format::EffectKind;
using hokuto::format::Engine;
using hokuto::format::File;
using hokuto::format::FileBeside;
using hokuto::format::GameSizes;
using hokuto::format::Mixing;
using hokuto::format::MixingRefused;
using hokuto::format::NativeArchive;
using hokuto::format::NativeGroupFile;
using hokuto::format::NativeSound;
using hokuto::format::NativeStream;
using hokuto::format::NativeTables;
using hokuto::format::TrackHints;
using hokuto::format::ViewFront;
using hokuto::format::WaveModel;
using hokuto::format::WavePlace;
using ntdlib::commands::Argument;
using ntdlib::commands::Command;
using ntdlib::commands::Fixed;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Sequence;
using ntdlib::commands::Source;
using ntdlib::sar::Flavor;
using ntdlib::sar::SoundType;

// A volume, a wait and an end, then a note and an end that only a sound's start reaches, at offset 5.
const std::vector<uint8_t> kSong = {0xC1, 0x64, 0x80, 0x30, 0xFF, 0x3C, 0x64, 0x30, 0xFF};

// A Switch archive with a player and three sequence sounds. Song plays kSong from its start, and Entry from offset 5.
// File 1 holds five other bytes, and file 2 the sequence that Other plays.
std::vector<uint8_t> Archive()
{
    sar_files::ArchiveSpec spec;
    spec.sounds = {
        {.name = "Song", .file = 0}, {.name = "Entry", .file = 0, .start_offset = 5}, {.name = "Other", .file = 2}};
    spec.players = {{"PLAYER", 1}};
    spec.files = {{.bytes = sar_files::Sequence(Flavor::kNx, kSong, {{"Song", 0}})},
                  {.bytes = {1, 2, 3, 4, 5}},
                  {.bytes = sar_files::Sequence(Flavor::kNx, {0xC1, 0x50, 0xFF}, {{"Other", 0}})}};

    return sar_files::Archive(Flavor::kNx, spec);
}

// The sequence file of kSong, with `volumes` more volume commands at its start.
std::vector<uint8_t> LongerSong(std::size_t volumes)
{
    std::vector<uint8_t> song = kSong;
    for (std::size_t i = 0; i < volumes; i++)
    {
        song.insert(song.begin(), {0xC1, 0x64});
    }

    return sar_files::Sequence(Flavor::kNx, song, {{"Song", 0}});
}

// An archive of `song`, a sequence file, with a group whose file holds a copy of it.
std::vector<uint8_t> GroupArchive(const std::vector<uint8_t>& song)
{
    sar_files::ArchiveSpec spec;
    spec.sounds = {{.name = "Song", .file = 0}};
    spec.players = {{"PLAYER", 1}};
    spec.groups = {{"GROUP_SONG", 1}};
    spec.files = {{.bytes = song}, {.bytes = sar_files::Group(Flavor::kNx, {{0, song}})}};

    return sar_files::Archive(Flavor::kNx, spec);
}

// A command of `kind` with the fixed value `value`.
Command Item(Kind kind, Fixed value = 0)
{
    Command item;
    item.kind = kind;
    item.value.value = value;

    return item;
}

// A SHA-1 of 20 bytes of `byte`, for the game's group files.
std::array<uint8_t, 20> GameHash(uint8_t byte)
{
    std::array<uint8_t, 20> hash{};
    hash.fill(byte);

    return hash;
}

// The SHA-1 of no bytes.
constexpr std::array<uint8_t, 20> kEmptyHash = {0xDA, 0x39, 0xA3, 0xEE, 0x5E, 0x6B, 0x4B, 0x0D, 0x32, 0x55,
                                                0xBF, 0xEF, 0x95, 0x60, 0x18, 0x90, 0xAF, 0xD8, 0x07, 0x09};

// The GRPF chunk of the unusual file, worked out from docs/format.md: the count, then for each group file its group,
// the game's size and SHA-1, the count of its pieces, each piece's type, three zeros, its size and its body, the count
// of its fields and each field's three places.
const std::vector<uint8_t> kGroupFilesChunk = {
    2,    0,    0,    0,    0,    0,    0,    0,    16,   0,    0,    0,    0,    0,    0,    0,    0xAB, 0xAB,
    0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB,
    2,    0,    0,    0,    0,    0,    0,    0,    4,    0,    0,    0,    0,    0,    0,    0,    0x46, 0x47,
    0x52, 0x50, 1,    0,    0,    0,    12,   0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    2,    0,    0,    0,    0,    0,    1,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    2,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0xFF, 0xFF, 0xFF, 0xFF, 0,    0,
    0,    0,    0,    0,    0,    0,    0xDA, 0x39, 0xA3, 0xEE, 0x5E, 0x6B, 0x4B, 0x0D, 0x32, 0x55, 0xBF, 0xEF,
    0x95, 0x60, 0x18, 0x90, 0xAF, 0xD8, 0x07, 0x09, 0,    0,    0,    0,    0,    0,    0,    0};

// The same chunk in the text form, 57 bytes to a line of base64.
constexpr const char* kGroupFilesText =
    "    AgAAAAAAAAAQAAAAAAAAAKurq6urq6urq6urq6urq6urq6urAgAAAAAAAAAEAAAAAAAAAEZHUlAB\n"
    "    AAAADAAAAAAAAAAAAAAAAAACAAAAAAABAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAgAAAAAA\n"
    "    AAAAAAAA/////wAAAAAAAAAA2jmj7l5rSw0yVb/vlWAYkK/YBwkAAAAAAAAAAA==\n";

// Every unusual part that a command can hold, in a file with an empty sequence and two chunks that Hokuto doesn't read.
File Unusual()
{
    File file;
    file.mixing = hokuto::format::Mixing::kEnhanced;

    // A reverb on aux bus A with a negative zero, the largest float, the smallest and a third. None on bus B. Two on
    // bus C, the second with values at the edges of the text without an exponent.
    constexpr float kMax = std::numeric_limits<float>::max();
    constexpr float kTiny = std::numeric_limits<float>::denorm_min();
    file.aux_effects[0] = {
        {EffectKind::kI3dl2Reverb,
         {-0.0f, kMax, kTiny, 0.1f, -1234.5f, 1.0f / 3.0f, -kMax, 2.5f, 100.0f, 0.00001f, 5000.0f, 1.0f}}};
    file.aux_effects[2] = {
        {EffectKind::kI3dl2Reverb, std::vector<float>(12, 0.0f)},
        {EffectKind::kI3dl2Reverb,
         {1e15f, 1e14f, 1e-6f, 123456789.0f, -0.5f, 7.0f, 65536.0f, 0.0009765625f, 3.0f, 1e-38f, 20000.0f, 0.75f}}};

    Sequence sequence;
    std::vector<Command>& items = sequence.items;

    Command note = Item(Kind::kNote, 48 * kOne);
    note.key = 60;
    note.velocity = 100 * kOne;
    items.push_back(note);

    Command volume = Item(Kind::kVolume);
    volume.value = {Source::kRandom, 64 * kOne, 127 * kOne};
    volume.time = Argument{Source::kVariable, 3, 0};
    items.push_back(volume);

    Command pan = Item(Kind::kPan, -64 * kOne);
    pan.conditional = true;
    items.push_back(pan);

    Command jump = Item(Kind::kJump);
    jump.target = 6;
    items.push_back(jump);

    Command track = Item(Kind::kOpenTrack);
    track.index = 1;
    track.target = 2;
    items.push_back(track);

    items.push_back(Item(Kind::kPitchBend, kOne / 128));

    Command other = Item(Kind::kOther, 5 * kOne);
    other.encoding.opcode = 0xA7;
    items.push_back(other);

    Command data = Item(Kind::kData);
    data.bytes = {0x00, 0xFF, 0x12};
    items.push_back(data);

    // An end with every part that an end doesn't have.
    Command end = Item(Kind::kEnd);
    end.index = 3;
    end.key = 9;
    end.velocity = kOne;
    end.value = {Source::kVariable, 2, 0};
    end.target = 1;
    end.encoding.opcode = 0x12;
    end.bytes = {1, 2};
    items.push_back(end);

    Command call = Item(Kind::kCall);
    call.target = 100;
    call.target_outside = true;
    items.push_back(call);

    Command sweep = Item(Kind::kSweepPitch, 25 * kOne / 16);
    sweep.encoding = {0, 3, 0xA3, true, true};
    items.push_back(sweep);

    items.push_back(Item(Kind::kTranspose, -12800 * kOne));
    items.push_back(Item(Kind::kTempo, 32767 * kOne));

    Command depth = Item(Kind::kLfoDepth);
    depth.index = 2;
    depth.value = {Source::kValue, 43 * kOne / 128, 0};
    items.push_back(depth);

    // Labels whose names take quotes, out of the order of their names, and one at the end.
    sequence.labels = {{"Song", 0},
                       {"\"odd\" name", 3},
                       {"\xE3\x83\x9B\xE3\x82\xAF\xE3\x83\x88", 5},
                       {"\xFF bad", 6},
                       {"over", 7},
                       {".dot", 8},
                       {"end_label", 14},
                       {"12", 9}};

    // Tables with a sound that sets every value, sounds with names that take quotes or read as a word of the text, an
    // empty list, and a player without a name.
    NativeTables tables;
    tables.sounds = {{.name = "Song",
                      .kind = SoundType::kSequence,
                      .player = 0x04000001,
                      .volume = 0,
                      .pan_mode = 1,
                      .pan_curve = 2,
                      .player_priority = 127,
                      .actor_player = 3,
                      .banks = {0x03000000, 0xFFFFFFFF, 0x03000001},
                      .tracks = 0xFFFF,
                      .channel_priority = 0,
                      .release_priority_fix = true},
                     {.name = "", .kind = SoundType::kUnknown},
                     {.name = "two words", .kind = SoundType::kWaveSound, .volume = 255},
                     {.name = "sound", .kind = SoundType::kStream}};
    tables.banks = {"BANK", "\"quoted\""};
    tables.players = {{"PLAYER", 4}, {"", 0}};
    tables.groups = {"GROUP"};

    // Two copies of the first sequence's file, and fields that end at items, at the archive's end and past it. Two
    // group files: one with a copy of the first sequence's file and a field, and an empty one whose group Hokuto
    // couldn't find.
    file.sequences = {std::move(sequence), Sequence{}};
    file.native =
        NativeArchive{Flavor::kNx,
                      std::vector<ntdlib::sar::Piece>{
                          std::vector<uint8_t>{1, 2, 3, 4, 5, 6, 7, 8}, ntdlib::sar::SequencePiece{0, 0x00020000},
                          ntdlib::sar::Padding{64}, ntdlib::sar::SequencePiece{0, 0x00020000}, std::vector<uint8_t>{}},
                      {{{0, 0, false}, {0, 0, false}, {5, 0, false}},
                       {{0, 4, false}, {1, 0, true}, {1, 14, true}},
                       {{0, 1, false}, {2, 0, false}, {5, 0x10, false}},
                       {{0, 2, false}, {1, 0, true}, {1, 4, true}},
                       {{0, 3, false}, {3, 0, true}, {3, 9, true}}},
                      std::vector<NativeGroupFile>{
                          {.group = 0,
                           .game_size = 16,
                           .game_hash = GameHash(0xAB),
                           .pieces = std::vector<ntdlib::sar::Piece>{std::vector<uint8_t>{0x46, 0x47, 0x52, 0x50},
                                                                     ntdlib::sar::SequencePiece{0, 0x00020000}},
                           .fields = {{{0, 0, false}, {0, 0, false}, {2, 0, false}}}},
                          {.game_hash = kEmptyHash}},
                      tables,
                      std::nullopt,
                      {}};
    file.other_chunks = {{"XTRA", {1, 2, 3}}, {"x y!", {}}};

    return file;
}

// The unusual file with scenes: one with values at the edges and changes at the first and the last millisecond, two of
// them at the same time, one with no name and no values, and one of a sound that the tables lack, named like a word of
// the text.
File WithScenes()
{
    File file = Unusual();
    file.scenes = {
        {.sound = 0,
         .name = "Last lap",
         .values = {{0, -32768}, {15, 32767}, {16, -1}, {31, 0}},
         .changes = {{0, 3, 1}, {90000, 3, 2}, {90000, 31, -5}, {UINT32_MAX, 0, 7}}},
        {.sound = 0, .name = "", .values = {}, .changes = {}},
        {.sound = 70000, .name = "scene", .values = {{1, 1}}, .changes = {{1500, 1, 0}}},
    };

    return file;
}

// Whether `action` throws an `Error`.
template <typename Error, typename Action>
bool Throws(const Action& action)
{
    try
    {
        action();
    }
    catch (const Error&)
    {
        return true;
    }

    return false;
}

// The message that `action` throws as an Error, or nothing.
template <typename Error, typename Action>
std::string Message(const Action& action)
{
    try
    {
        action();
    }
    catch (const Error& error)
    {
        return error.what();
    }

    return "";
}

// The message that ReadText throws for `text`, or nothing.
std::string TextError(std::string_view text)
{
    return Message<xsflib::FormatError>([&] { hokuto::format::ReadText(text); });
}

// The IDs in the table of chunks of `binary`, a file in the binary form.
std::vector<std::string> ChunkIds(std::span<const uint8_t> binary)
{
    std::vector<std::string> ids;
    const uint32_t count = xsflib::LoadLe32(&binary[16]);
    for (std::size_t c = 0; c < count; ++c)
    {
        const uint8_t* entry = &binary[24 + 24 * c];
        ids.emplace_back(entry, entry + 4);
    }

    return ids;
}

// The bytes of chunk `chunk` of `binary`, by its place in the table of chunks.
std::span<const uint8_t> ChunkBytes(std::span<const uint8_t> binary, std::size_t chunk)
{
    const uint8_t* entry = &binary[24 + 24 * chunk];
    return binary.subspan(static_cast<std::size_t>(xsflib::LoadLe64(entry + 8)),
                          static_cast<std::size_t>(xsflib::LoadLe64(entry + 16)));
}

// The message that Read throws for `binary` with byte `at` of chunk `chunk` set to `value`, or nothing. The chunk's
// CRC-32 matches its new bytes.
std::string Patched(std::vector<uint8_t> binary, std::size_t chunk, std::size_t at, uint8_t value)
{
    uint8_t* entry = &binary[24 + 24 * chunk];
    const auto start = static_cast<std::size_t>(xsflib::LoadLe64(entry + 8));
    const auto size = static_cast<std::size_t>(xsflib::LoadLe64(entry + 16));
    binary[start + at] = value;
    xsflib::StoreLe32(entry + 4, hokuto::format::Crc32(std::span(binary).subspan(start, size)));

    return Message<xsflib::FormatError>([&] { hokuto::format::Read(binary); });
}

// The number of the line of `text` that holds `part`.
std::size_t LineOf(std::string_view text, std::string_view part)
{
    const auto before = text.substr(0, text.find(part));
    return static_cast<std::size_t>(std::count(before.begin(), before.end(), '\n')) + 1;
}

// The start of the message of a text form's line `line`.
std::string LineStart(std::size_t line)
{
    return "Line " + std::to_string(line) + ": ";
}

// `text` with its first `old` replaced by `replacement`.
std::string Replaced(std::string text, std::string_view old, std::string_view replacement)
{
    text.replace(text.find(old), old.size(), replacement);

    return text;
}

// The CRC-32 of "123456789" is the standard check value, 0xCBF43926. The CRC-32 of no bytes is 0.
void TestCrc()
{
    const std::string_view kCheck = "123456789";

    XSFLIB_CHECK(hokuto::format::Crc32(std::span(reinterpret_cast<const uint8_t*>(kCheck.data()), kCheck.size())) ==
                 0xCBF43926u);
    XSFLIB_CHECK(hokuto::format::Crc32({}) == 0);
}

// A kind of effect with its code, its name in the text form and the number of its parameters.
struct EffectKindCode
{
    EffectKind kind;
    uint8_t code;
    std::string_view name;
    std::size_t parameters;
};

// The numbers and names that never change: those of the commands' kinds, the engines, the mixing modes, the consoles,
// the kinds of sound and the effects. Each code reads back as its kind. A code past the last reads as nothing. So does
// 0 where it names no kind: mixing mode 0 is strict, and sound kind 0 is unknown. Code 4 of the archives is the Wii's.
// It reads as no flavor: ntdlib gives the Wii's archives none. The effects take their parameters' names, and the
// reverbs their modes.
void TestCodes()
{
    std::vector<uint16_t> codes;
    for (int k = 0; k < ntdlib::commands::kKindCount; ++k)
    {
        const auto kind = static_cast<Kind>(k);
        const uint16_t code = hokuto::format::CodeOf(kind);
        XSFLIB_CHECK(code != 0 && hokuto::format::KindOfCode(code) == kind);
        XSFLIB_CHECK(std::find(codes.begin(), codes.end(), code) == codes.end());
        codes.push_back(code);
    }
    XSFLIB_CHECK(hokuto::format::CodeOf(Kind::kNote) == 1 && hokuto::format::CodeOf(Kind::kData) == 76);
    XSFLIB_CHECK(!hokuto::format::KindOfCode(0) && !hokuto::format::KindOfCode(77));

    // The engines and the mixing modes.
    XSFLIB_CHECK(hokuto::format::CodeOf(Engine::kSwitch) == 1 && hokuto::format::NameOf(Engine::kSwitch) == "switch");
    XSFLIB_CHECK(hokuto::format::EngineNamed("switch") == Engine::kSwitch && !hokuto::format::EngineOfCode(0));
    XSFLIB_CHECK(hokuto::format::CodeOf(Engine::kWiiU) == 2 && hokuto::format::EngineNamed("wii_u") == Engine::kWiiU);
    XSFLIB_CHECK(hokuto::format::CodeOf(Engine::k3ds) == 3 && hokuto::format::NameOf(Engine::k3ds) == "3ds");
    XSFLIB_CHECK(hokuto::format::EngineOfCode(3) == Engine::k3ds && !hokuto::format::EngineOfCode(5));
    XSFLIB_CHECK(hokuto::format::EngineOf(Flavor::kNx) == Engine::kSwitch &&
                 hokuto::format::EngineOf(Flavor::kCafe) == Engine::kWiiU &&
                 hokuto::format::EngineOf(Flavor::kCtr) == Engine::k3ds);
    XSFLIB_CHECK(hokuto::format::MixingNamed("enhanced") == Mixing::kEnhanced &&
                 hokuto::format::MixingOfCode(0) == Mixing::kStrict);
    XSFLIB_CHECK(hokuto::format::MixingOfCode(1) == Mixing::kEnhanced && !hokuto::format::MixingOfCode(2));

    // The consoles and the kinds of sound.
    for (const Flavor flavor : {Flavor::kNx, Flavor::kCafe, Flavor::kCtr})
    {
        XSFLIB_CHECK(hokuto::format::FlavorOfCode(hokuto::format::CodeOf(flavor)) == flavor);
        XSFLIB_CHECK(hokuto::format::FlavorNamed(hokuto::format::NameOf(flavor)) == flavor);
    }

    XSFLIB_CHECK(!hokuto::format::FlavorOfCode(0) && !hokuto::format::FlavorOfCode(hokuto::format::kWiiArchiveCode));

    for (const SoundType type : {SoundType::kUnknown, SoundType::kSequence, SoundType::kWaveSound, SoundType::kStream})
    {
        XSFLIB_CHECK(hokuto::format::SoundTypeOfCode(hokuto::format::CodeOf(type)) == type);
        XSFLIB_CHECK(hokuto::format::SoundTypeNamed(hokuto::format::NameOf(type)) == type);
    }

    XSFLIB_CHECK(hokuto::format::CodeOf(SoundType::kStream) == 3 && !hokuto::format::SoundTypeOfCode(4));

    // An I3DL2 reverb is effect 1, with nn::audio's 12 parameters.
    XSFLIB_CHECK(hokuto::format::CodeOf(EffectKind::kI3dl2Reverb) == 1 &&
                 hokuto::format::EffectKindOfCode(1) == EffectKind::kI3dl2Reverb);
    XSFLIB_CHECK(hokuto::format::EffectKindNamed("i3dl2_reverb") == EffectKind::kI3dl2Reverb &&
                 hokuto::format::NameOf(EffectKind::kI3dl2Reverb) == "i3dl2_reverb");
    XSFLIB_CHECK(!hokuto::format::EffectKindOfCode(0) && !hokuto::format::EffectKindOfCode(17));
    const std::span<const std::string_view> names = hokuto::format::ParameterNamesOf(EffectKind::kI3dl2Reverb);
    XSFLIB_CHECK(names.size() == 12 && names[0] == "room_gain" && names[11] == "dry_gain");

    // The Wii U's effects are 2 to 5, with the fields of their nw::snd parameter structures. Only the reverbs have
    // modes: 8 early modes and 6 fused modes.
    for (const auto& [kind, code, name, parameters] :
         {EffectKindCode{EffectKind::kReverbHi, 2, "reverb_hi", 11},
          EffectKindCode{EffectKind::kReverbStd, 3, "reverb_std", 10},
          EffectKindCode{EffectKind::kChorus, 4, "chorus", 5}, EffectKindCode{EffectKind::kDelay, 5, "delay", 5}})
    {
        XSFLIB_CHECK(hokuto::format::CodeOf(kind) == code && hokuto::format::EffectKindOfCode(code) == kind);
        XSFLIB_CHECK(hokuto::format::NameOf(kind) == name && hokuto::format::EffectKindNamed(name) == kind);
        XSFLIB_CHECK(hokuto::format::ParameterNamesOf(kind).size() == parameters);
    }

    // The Wii's effects are 6 to 11. The reverbs of 2008 have the Wii U's parameters and modes. The older reverbs, the
    // chorus and the delay have the parameters of NW4R SND's classes, and no modes.
    for (const auto& [kind, code, name, parameters] :
         {EffectKindCode{EffectKind::kWiiReverbHi, 6, "wii_reverb_hi", 6},
          EffectKindCode{EffectKind::kWiiReverbHi2008, 7, "wii_reverb_hi_2008", 11},
          EffectKindCode{EffectKind::kWiiChorus, 8, "wii_chorus", 3},
          EffectKindCode{EffectKind::kWiiDelay, 9, "wii_delay", 3},
          EffectKindCode{EffectKind::kWiiReverbStd, 10, "wii_reverb_std", 5},
          EffectKindCode{EffectKind::kWiiReverbStd2008, 11, "wii_reverb_std_2008", 10}})
    {
        XSFLIB_CHECK(hokuto::format::CodeOf(kind) == code && hokuto::format::EffectKindOfCode(code) == kind);
        XSFLIB_CHECK(hokuto::format::NameOf(kind) == name && hokuto::format::EffectKindNamed(name) == kind);
        XSFLIB_CHECK(hokuto::format::ParameterNamesOf(kind).size() == parameters);
    }

    XSFLIB_CHECK(hokuto::format::ParameterNamesOf(EffectKind::kWiiReverbHi)[4] == "crosstalk" &&
                 hokuto::format::ParameterNamesOf(EffectKind::kWiiReverbStd)[4] == "out_gain");
    XSFLIB_CHECK(hokuto::format::ParameterNamesOf(EffectKind::kWiiChorus)[2] == "rate" &&
                 hokuto::format::ParameterNamesOf(EffectKind::kWiiDelay)[2] == "out_gain");
    XSFLIB_CHECK(hokuto::format::ChoicesOf(EffectKind::kWiiReverbHi2008, 6) == 8 &&
                 hokuto::format::ChoicesOf(EffectKind::kWiiReverbStd2008, 7) == 6);
    for (std::size_t p = 0; p < 6; ++p)
    {
        XSFLIB_CHECK(hokuto::format::ChoicesOf(EffectKind::kWiiReverbHi, p) == 0);
    }

    // The names of the Wii U's parameters, and their modes.
    const std::span<const std::string_view> reverb = hokuto::format::ParameterNamesOf(EffectKind::kReverbHi);
    XSFLIB_CHECK(reverb[0] == "pre_delay_time" && reverb[4] == "crosstalk" && reverb[10] == "fused_gain");
    XSFLIB_CHECK(hokuto::format::ParameterNamesOf(EffectKind::kReverbStd)[4] == "out_gain");
    XSFLIB_CHECK(hokuto::format::ParameterNamesOf(EffectKind::kChorus)[0] == "delay_time");
    XSFLIB_CHECK(hokuto::format::ParameterNamesOf(EffectKind::kDelay)[4] == "lpf");
    XSFLIB_CHECK(hokuto::format::ChoicesOf(EffectKind::kReverbHi, 6) == 8 &&
                 hokuto::format::ChoicesOf(EffectKind::kReverbHi, 8) == 6);
    XSFLIB_CHECK(hokuto::format::ChoicesOf(EffectKind::kReverbStd, 5) == 8 &&
                 hokuto::format::ChoicesOf(EffectKind::kReverbStd, 7) == 6);
    XSFLIB_CHECK(hokuto::format::ChoicesOf(EffectKind::kReverbHi, 7) == 0 &&
                 hokuto::format::ChoicesOf(EffectKind::kReverbHi, 11) == 0);

    // The I3DL2 reverb, the chorus and the delay have no modes.
    for (std::size_t p = 0; p < 12; ++p)
    {
        XSFLIB_CHECK(hokuto::format::ChoicesOf(EffectKind::kI3dl2Reverb, p) == 0 &&
                     hokuto::format::ChoicesOf(EffectKind::kChorus, p) == 0 &&
                     hokuto::format::ChoicesOf(EffectKind::kDelay, p) == 0);
    }
}

// A Switch archive imports into a file and exports back byte for byte, with the group files that the import is given.
// Both forms give the file back, and the text form may start with a byte-order mark. Edits of the sequences and the
// tables export. An export refuses a pan and bank slots that the archive has no place for, a new name and a missing
// player. An archive whose string table is damaged imports without tables and exports unchanged.
void TestArchive()
{
    const std::vector<uint8_t> archive = Archive();

    const File file = hokuto::format::ImportSwitchArchive(archive);

    // The sequences in units, no effects, the pieces, the tables, the game's sizes, and the archive back.
    SEQLIB_REQUIRE(file.engine == hokuto::format::Engine::kSwitch && file.sequences.size() == 2 && file.native);
    XSFLIB_CHECK(file.aux_effects == decltype(file.aux_effects){});
    XSFLIB_CHECK(file.sequences[0]->items.size() == 5 && file.sequences[0]->items[0].value.value == 100 * kOne);
    NativeTables tables;
    for (const char* name : {"Song", "Entry", "Other"})
    {
        tables.sounds.push_back({.name = name, .kind = SoundType::kSequence, .player = 0x04000000, .volume = 100});
    }
    tables.players = {{"PLAYER", 1}};
    XSFLIB_CHECK(file.native->tables == tables && file.native->group_files.empty());
    XSFLIB_CHECK((file.native->game_sizes == GameSizes{archive.size(), {}}));
    XSFLIB_CHECK(hokuto::format::ExportArchive(file) == archive);

    // The file keeps the group files that the import is given. A group file that isn't one of the console's stays one
    // run, and an empty one has no pieces. The export gives them back.
    const std::vector<std::vector<uint8_t>> kGroupFiles = {{1, 2, 3}, {}};
    const File grouped = hokuto::format::ImportSwitchArchive(archive, kGroupFiles);
    XSFLIB_CHECK(hokuto::format::ExportGroupFiles(grouped) == kGroupFiles && grouped.native->group_files.size() == 2);
    if (grouped.native->group_files.size() == 2)
    {
        const NativeGroupFile& first = grouped.native->group_files[0];
        XSFLIB_CHECK(!first.group && first.game_size == 3 && first.fields.empty());
        XSFLIB_CHECK((*first.pieces == std::vector<ntdlib::sar::Piece>{std::vector<uint8_t>{1, 2, 3}}));
        XSFLIB_CHECK(grouped.native->group_files[1].pieces->empty() &&
                     grouped.native->group_files[1].game_hash == kEmptyHash);
    }

    // Both forms give the file back.
    const std::vector<uint8_t> binary = hokuto::format::Write(file);
    const std::string text = hokuto::format::WriteText(file);
    XSFLIB_CHECK(hokuto::format::IsBinary(binary) && !hokuto::format::IsText(binary));
    const std::vector<std::string> kIds = {"ENGN", "SEQS", "NATV", "TABL", "SIZE"};
    XSFLIB_CHECK(ChunkIds(binary) == kIds);
    XSFLIB_CHECK(hokuto::format::Read(binary) == file && hokuto::format::ReadText(text) == file);
    XSFLIB_CHECK(hokuto::format::ReadEitherForm(binary) == file);
    XSFLIB_CHECK(
        hokuto::format::ReadEitherForm(std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size())) == file);

    // A UTF-8 byte-order mark may start the text form. Some editors on Windows add one.
    const std::string marked = "\xEF\xBB\xBF" + text;
    const std::span<const uint8_t> marked_bytes(reinterpret_cast<const uint8_t*>(marked.data()), marked.size());
    XSFLIB_CHECK(hokuto::format::IsText(marked_bytes) && hokuto::format::ReadEitherForm(marked_bytes) == file);

    // Edits export. A longer wait moves the second sound's start, and twenty more volumes make the second sequence file
    // 32 bytes larger.
    File longer = file;
    longer.sequences[0].Edit([](Sequence& sequence) { sequence.items[1].value.value = 200 * kOne; });
    const std::vector<uint8_t> moved = hokuto::format::ExportArchive(longer);
    ntdlib::sar::Options options;
    options.flavor = Flavor::kNx;
    XSFLIB_CHECK(moved.size() == archive.size());
    XSFLIB_CHECK(ntdlib::sar::SoundArchive::Load(moved, options).Sounds()[1].start_offset == 6);
    XSFLIB_CHECK(hokuto::format::ImportSwitchArchive(moved).sequences == longer.sequences);
    File more = file;
    const auto add_volumes = [](Sequence& sequence)
    {
        sequence.items.insert(sequence.items.begin() + 1, 20, sequence.items[0]);
    };
    more.sequences[1].Edit(add_volumes);
    const std::vector<uint8_t> grown = hokuto::format::ExportArchive(more);
    XSFLIB_CHECK(grown.size() == archive.size() + 32);
    XSFLIB_CHECK(hokuto::format::ImportSwitchArchive(grown).sequences == more.sequences);
    XSFLIB_CHECK(Throws<std::invalid_argument>([] { hokuto::format::ExportArchive(File{}); }));

    // A sound's volume and a player's sound limit export.
    File louder = file;
    const auto raise = [](NativeArchive& native)
    {
        native.tables->sounds[2].volume = 64;
        native.tables->players[0].sound_limit = 3;
    };
    louder.native.Edit(raise);
    const std::vector<uint8_t> loud = hokuto::format::ExportArchive(louder);
    XSFLIB_CHECK(loud.size() == archive.size());
    XSFLIB_CHECK(hokuto::format::ImportSwitchArchive(loud).native->tables == louder.native->tables);
    XSFLIB_CHECK(hokuto::format::ExportArchive(hokuto::format::ImportSwitchArchive(loud)) == loud);

    // A place that the archive can't take, a new name and a missing player don't. NintendoWare's tools didn't lay the
    // archive out.
    const auto refused = [](const File& edited)
    {
        return Message<std::invalid_argument>([&] { hokuto::format::ExportArchive(edited); });
    };

    File panned = file;
    panned.native.Edit([](NativeArchive& native) { native.tables->sounds[0].pan_mode = 1; });
    XSFLIB_CHECK(refused(panned) == "Hokuto can't give sound Song a place for its pan in the archive's tables.");
    File unheld = file;
    unheld.native.Edit([](NativeArchive& native) { native.tables->sounds[0].banks = {0x03000000}; });
    XSFLIB_CHECK(refused(unheld) == "Hokuto can't write the bank slots of sound Song into the archive's tables.");
    File renamed = file;
    renamed.native.Edit([](NativeArchive& native) { native.tables->sounds[1].name = "Start"; });
    XSFLIB_CHECK(refused(renamed).starts_with(
        "The archive's tables hold sound Entry (sequence) where the file's tables hold sound Start (sequence)."));
    File fewer = file;
    fewer.native.Edit([](NativeArchive& native) { native.tables->players.clear(); });
    XSFLIB_CHECK(refused(fewer).starts_with(
        "The archive's tables hold player PLAYER where the file's tables hold nothing more."));

    // An archive whose string table counts more names than it holds imports without tables, and exports unchanged. The
    // game's sizes hold its size alone.
    std::vector<uint8_t> damaged = archive;
    const std::string_view kStrg = "STRG";
    const auto block = static_cast<std::size_t>(
        std::search(damaged.begin(), damaged.end(), kStrg.begin(), kStrg.end()) - damaged.begin());
    xsflib::StoreLe32(&damaged[block + 8 + xsflib::LoadLe32(&damaged[block + 12])], 0x7FFFFFFF);
    const File without = hokuto::format::ImportSwitchArchive(damaged);
    XSFLIB_CHECK(!without.native->tables && hokuto::format::ExportArchive(without) == damaged);
    XSFLIB_CHECK((without.native->game_sizes == GameSizes{damaged.size(), {}}));
}

// The song of the Wii's test archive: a volume of 100, key 60 for 48 ticks, a wait of 48 ticks, and the end.
const std::vector<uint8_t> kWiiSong = {0xC1, 0x64, 0x3C, 0x64, 0x30, 0x80, 0x30, 0xFF};

// An RSEQ file with `bytecode` and a label at its start.
std::vector<uint8_t> WiiSequence(const std::vector<uint8_t>& bytecode)
{
    ntdlib::rsar::SequenceFile file;
    file.data = bytecode;
    file.labels = {{"SEQ_SONG", 0}};

    return file.Write();
}

// An archive of the Wii with `song`, a bank and its wave data in one group. The layout follows NW4R's tools.
std::vector<uint8_t> WiiArchive(const std::vector<uint8_t>& song, uint16_t version = 0x0104)
{
    const std::vector<uint8_t> kBankBytes(0x40, 0x5A);
    const std::vector<uint8_t> kWaveBytes(0x20, 0xA5);
    return ntdlib::testing::rsar_tools::MakeArchive(version, {{song}, {kBankBytes, kWaveBytes}}, {{"SEQ_SONG", 0, 0}},
                                                    {{0, 1}});
}

// An archive of the Wii, in both forms. The file takes the Wii's engine, the archive's size as the game's size and the
// values of the archive's tables. Edits export, and the files after a longer song move. The functions for the later
// consoles' archives refuse it. An import refuses three bytes that aren't an archive.
void TestWiiArchive()
{
    const std::vector<uint8_t> archive = WiiArchive(WiiSequence(kWiiSong));

    const File file = hokuto::format::ImportWiiArchive(archive);

    SEQLIB_REQUIRE(file.engine == hokuto::format::Engine::kWii && file.native && file.native->wii);
    XSFLIB_CHECK(file.sequences.size() == 1 && file.sequences[0]->items.size() == 4);
    XSFLIB_CHECK((file.native->game_sizes == GameSizes{archive.size(), {}}));

    // The sound's player and bank take the item IDs of the later consoles. The archive's tables hold no banks and no
    // players, and the Wii's tables hold no wave archives.
    const NativeSound kSound{
        .name = "SEQ_SONG", .kind = SoundType::kSequence, .player = 0x04000000, .banks = {0x03000000}};
    SEQLIB_REQUIRE(file.native->tables && (file.native->tables->sounds == std::vector<NativeSound>{kSound}));
    XSFLIB_CHECK(file.native->tables->banks.empty() && file.native->tables->wave_archives.empty() &&
                 file.native->tables->players.empty() && file.native->tables->groups.empty());
    XSFLIB_CHECK(hokuto::format::ExportArchive(file) == archive && hokuto::format::ExportGroupFiles(file).empty());
    XSFLIB_CHECK(hokuto::format::BuildOfWiiArchive(*file.native).date == 20080620);
    XSFLIB_CHECK(hokuto::format::BuildOfWiiArchive(
                     *hokuto::format::ImportWiiArchive(WiiArchive(WiiSequence(kWiiSong), 0x0101)).native)
                     .date == 0);

    // Both forms give the file back. The engine and the archive take number 4 and the name wii.
    const std::vector<uint8_t> binary = hokuto::format::Write(file);
    const std::string text = hokuto::format::WriteText(file);
    XSFLIB_CHECK(hokuto::format::Read(binary) == file && hokuto::format::ReadText(text) == file);
    XSFLIB_CHECK(hokuto::format::CodeOf(hokuto::format::Engine::kWii) == 4 &&
                 hokuto::format::EngineNamed("wii") == hokuto::format::Engine::kWii);
    XSFLIB_CHECK(text.find("\nengine wii\n") != std::string::npos && text.find("\nnative wii\n") != std::string::npos);

    // Twenty more volumes make the song's file 32 bytes larger. The tables follow, and the bank and its wave data keep
    // their bytes.
    File more = file;
    const auto lengthen = [](Sequence& sequence)
    {
        sequence.items.insert(sequence.items.begin() + 1, 20, sequence.items[0]);
    };
    more.sequences[0].Edit(lengthen);
    std::vector<uint8_t> longer = kWiiSong;
    for (int i = 0; i < 20; ++i)
    {
        longer.insert(longer.begin() + 2, {0xC1, 0x64});
    }
    const std::vector<uint8_t> grown = hokuto::format::ExportArchive(more);
    XSFLIB_CHECK(grown.size() == archive.size() + 32 && grown == WiiArchive(WiiSequence(longer)));
    XSFLIB_CHECK(hokuto::format::ImportWiiArchive(grown).sequences == more.sequences);

    // The sound's start lies in the one field that names it.
    const std::vector<std::optional<uint32_t>> starts = hokuto::format::StartFieldsOfSounds(file, archive);
    XSFLIB_CHECK(starts.size() == 1 && starts[0].has_value());

    // The functions for the later consoles' archives refuse a Wii archive.
    ntdlib::sar::Options options;
    options.flavor = Flavor::kNx;
    const ntdlib::sar::SoundArchive other = ntdlib::sar::SoundArchive::Load(Archive(), options);
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { hokuto::format::SequencesOfFiles(file, archive, other); }));
    XSFLIB_CHECK(!hokuto::format::TableValuePlacesOf(*file.native));
    XSFLIB_CHECK(Throws<xsflib::FormatError>([] { hokuto::format::ImportWiiArchive(std::vector<uint8_t>{1, 2, 3}); }));
}

// The values of a Wii archive's tables, edited in a file. An export writes each one in place: nothing moves, and the
// archive differs from the game's in the bytes of the edited values alone. The import reads them back, and both forms
// keep them. An archive before version 1.2 has no place for a pan mode, and one before version 1.3 none for the release
// priority fix. The export refuses them there, and writes the other values.
void TestWiiTables()
{
    const std::vector<uint8_t> archive = WiiArchive(WiiSequence(kWiiSong));
    File file = hokuto::format::ImportWiiArchive(archive);
    file.native.Edit(
        [](NativeArchive& native)
        {
            NativeSound& sound = native.tables->sounds[0];
            sound.volume = 100;
            sound.player_priority = 70;
            sound.pan_mode = 1;
            sound.pan_curve = 2;
            sound.banks = {0x03000003};
            sound.tracks = 0x5;
            sound.channel_priority = 90;
            sound.release_priority_fix = true;
        });

    const std::vector<uint8_t> edited = hokuto::format::ExportArchive(file);

    XSFLIB_CHECK(edited.size() == archive.size());
    XSFLIB_CHECK(std::ranges::count_if(std::views::iota(std::size_t{0}, std::min(edited.size(), archive.size())),
                                       [&](std::size_t i) { return edited[i] != archive[i]; }) == 8);
    XSFLIB_CHECK(hokuto::format::ImportWiiArchive(edited).native->tables == file.native->tables);
    XSFLIB_CHECK(hokuto::format::Read(hokuto::format::Write(file)) == file);
    XSFLIB_CHECK(hokuto::format::ReadText(hokuto::format::WriteText(file)) == file);

    // Version 1.1 reads the balance as the pan mode of each sound, without the release priority fix.
    const std::vector<uint8_t> old_archive = WiiArchive(WiiSequence(kWiiSong), 0x0101);
    const File old_file = hokuto::format::ImportWiiArchive(old_archive);
    SEQLIB_REQUIRE(old_file.native->tables && old_file.native->tables->sounds.size() == 1);
    XSFLIB_CHECK(old_file.native->tables->sounds[0].pan_mode == 1);

    const auto refused = [](const File& wii)
    {
        return Message<std::invalid_argument>([&] { hokuto::format::ExportArchive(wii); });
    };

    File panned = old_file;
    panned.native.Edit([](NativeArchive& native) { native.tables->sounds[0].pan_mode = 0; });
    XSFLIB_CHECK(refused(panned) == "The archive's tables have no place for the pan mode of sound SEQ_SONG.");
    File fixed = old_file;
    fixed.native.Edit([](NativeArchive& native) { native.tables->sounds[0].release_priority_fix = true; });
    XSFLIB_CHECK(refused(fixed) ==
                 "The archive's tables have no place for the release priority fix of sound SEQ_SONG.");
    File louder = old_file;
    louder.native.Edit([](NativeArchive& native) { native.tables->sounds[0].volume = 120; });
    const std::vector<uint8_t> loud = hokuto::format::ExportArchive(louder);
    XSFLIB_CHECK(loud.size() == old_archive.size() &&
                 hokuto::format::ImportWiiArchive(loud).native->tables == louder.native->tables);
}

// A Wii file keeps the game's build of NW4R SND in both forms, where its main.dol gave one: a date, and the flag of the
// build that comes with AX of October 2006. The build then takes the place of the archive's version's, and the archive
// comes back byte for byte. A build belongs to an archive of the Wii alone, and a text form refuses one elsewhere.
void TestWiiBuild()
{
    const std::vector<uint8_t> archive = WiiArchive(WiiSequence(kWiiSong));
    const ntdlib::SndBuild kWiiSports{.date = 20070608};
    const ntdlib::SndBuild kSmoothMoves{.date = 0, .october_2006 = true};
    const File sports = hokuto::format::ImportWiiArchive(archive, kWiiSports);
    const File moves = hokuto::format::ImportWiiArchive(archive, kSmoothMoves);
    const std::string sports_text = hokuto::format::WriteText(sports);
    const std::string moves_text = hokuto::format::WriteText(moves);

    XSFLIB_CHECK(sports.native && sports.native->wii_build == kWiiSports);
    XSFLIB_CHECK(hokuto::format::BuildOfWiiArchive(*sports.native) == kWiiSports);
    XSFLIB_CHECK(hokuto::format::BuildOfWiiArchive(*moves.native) == kSmoothMoves);
    XSFLIB_CHECK(hokuto::format::ExportArchive(sports) == archive && hokuto::format::ExportArchive(moves) == archive);
    XSFLIB_CHECK(hokuto::format::Read(hokuto::format::Write(sports)) == sports);
    XSFLIB_CHECK(hokuto::format::Read(hokuto::format::Write(moves)) == moves);
    XSFLIB_CHECK(hokuto::format::ReadText(sports_text) == sports && hokuto::format::ReadText(moves_text) == moves);
    XSFLIB_CHECK(sports_text.find("\n    build 20070608\n") != std::string::npos);
    XSFLIB_CHECK(moves_text.find("\n    build 0 october_2006\n") != std::string::npos);
    XSFLIB_CHECK(hokuto::format::WriteText(hokuto::format::ImportWiiArchive(archive)).find(" build ") ==
                 std::string::npos);

    File other = hokuto::format::ImportArchive(Archive(), Flavor::kNx);
    other.native.Edit([&](NativeArchive& native) { native.wii_build = kWiiSports; });
    std::string switch_text = hokuto::format::WriteText(hokuto::format::ImportArchive(Archive(), Flavor::kNx));
    const std::size_t native = switch_text.find("\nnative switch\n");
    XSFLIB_CHECK(native != std::string::npos);
    if (native != std::string::npos)
    {
        switch_text.insert(native + std::string("\nnative switch\n").size(), "    build 20070608\n");
    }
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { hokuto::format::Write(other); }));
    XSFLIB_CHECK(Throws<xsflib::FormatError>([&] { hokuto::format::ReadText(switch_text); }));
}

// A 3DS archive imports with the 3DS's engine, and comes back byte for byte through both forms. sar_files.h doesn't lay
// it out as NintendoWare's tools do. The model form therefore refuses it.
void Test3dsArchive()
{
    sar_files::ArchiveSpec spec;
    spec.sounds = {{.name = "Song", .file = 0}, {.name = "Entry", .file = 0, .start_offset = 5}};
    spec.files = {{.bytes = sar_files::Sequence(Flavor::kCtr, kSong, {{"Song", 0}})}, {.bytes = {1, 2, 3, 4, 5}}};
    const std::vector<uint8_t> archive = sar_files::Archive(Flavor::kCtr, spec);

    const File file = hokuto::format::ImportArchive(archive, Flavor::kCtr);
    SEQLIB_REQUIRE(file.engine == hokuto::format::Engine::k3ds && file.native && file.native->flavor == Flavor::kCtr);
    SEQLIB_REQUIRE(file.sequences.size() == 1 && file.sequences[0]->items.size() == 5);
    XSFLIB_CHECK(file.sequences[0]->items[0].kind == Kind::kVolume && file.sequences[0]->items[3].kind == Kind::kNote);
    XSFLIB_CHECK(file.native->tables && file.native->tables->sounds.size() == 2 &&
                 file.native->tables->players.empty());
    XSFLIB_CHECK(hokuto::format::ExportArchive(file) == archive);

    const std::vector<uint8_t> binary = hokuto::format::Write(file);
    const std::string text = hokuto::format::WriteText(file);
    XSFLIB_CHECK(text.find("\nengine 3ds\n") != std::string::npos);
    XSFLIB_CHECK(hokuto::format::Read(binary) == file && hokuto::format::ReadText(text) == file);
    XSFLIB_CHECK(hokuto::format::ExportArchive(hokuto::format::ReadText(text)) == archive);

    XSFLIB_CHECK(Throws<xsflib::FormatError>([&] { hokuto::format::InModelForm(file); }));
}

// A sound's bank slots export in a list of another length. The list ends the sound's entry, and the entry grows or
// shrinks with it. That requires an archive laid out as NintendoWare's tools lay one out.
void TestBankSlots()
{
    constexpr uint32_t kBank = 0x03000000;
    constexpr uint32_t kEmpty = 0xFFFFFFFF;
    sar_files::ArchiveSpec spec;
    spec.sounds = {{.name = "Song", .file = 0, .banks = {0}}, {.name = "Entry", .file = 0, .start_offset = 5}};
    spec.banks = {{"BANK", 1}};
    spec.players = {{"PLAYER", 1}};
    spec.files = {{.bytes = sar_files::Sequence(Flavor::kNx, kSong, {{"Song", 0}})}, {.bytes = {1, 2, 3, 4, 5}}};
    const std::vector<uint8_t> archive = sar_tools::Archive(Flavor::kNx, spec);
    const File file = hokuto::format::ImportSwitchArchive(archive);
    const auto longer = [](NativeArchive& native)
    {
        native.tables->sounds[0].banks = {kEmpty, kBank};
        native.tables->sounds[1].banks = {kBank};
    };
    const auto shorter = [](NativeArchive& native)
    {
        native.tables->sounds[0].banks = {kBank};
        native.tables->sounds[1].banks.clear();
    };

    File grown = file;
    grown.native.Edit(longer);
    const std::vector<uint8_t> grown_archive = hokuto::format::ExportArchive(grown);
    File back = hokuto::format::ImportSwitchArchive(grown_archive);
    back.native.Edit(shorter);

    const File read = hokuto::format::ImportSwitchArchive(grown_archive);
    XSFLIB_CHECK(read.native->tables == grown.native->tables && read.sequences == file.sequences);
    XSFLIB_CHECK(hokuto::format::ExportArchive(back) == archive);
}

// An export measures how much the archive grew from the sizes of another, and the files of each group that grew. An
// archive whose groups ntdlib can't read gives only its size.
void TestGrowth()
{
    const std::vector<uint8_t> song = LongerSong(0);
    const std::vector<uint8_t> longer = LongerSong(20);
    const std::vector<uint8_t> before = GroupArchive(song);
    const std::vector<uint8_t> after = GroupArchive(longer);
    const std::vector<uint8_t> kBroken = {1, 2, 3};
    const GameSizes sizes = hokuto::format::SizesOf(before, Flavor::kNx);
    const GameSizes broken_sizes = hokuto::format::SizesOf(kBroken, Flavor::kNx);

    const hokuto::format::ArchiveGrowth grown = hokuto::format::MeasureGrowth(sizes, after, Flavor::kNx);
    const hokuto::format::ArchiveGrowth same = hokuto::format::MeasureGrowth(sizes, before, Flavor::kNx);
    const hokuto::format::ArchiveGrowth unread = hokuto::format::MeasureGrowth(sizes, kBroken, Flavor::kNx);
    const hokuto::format::ArchiveGrowth from_unread = hokuto::format::MeasureGrowth(broken_sizes, after, Flavor::kNx);

    XSFLIB_CHECK((sizes == GameSizes{before.size(), {song.size()}}));
    XSFLIB_CHECK((broken_sizes == GameSizes{kBroken.size(), {}}));
    XSFLIB_CHECK(longer.size() > song.size() && after.size() > before.size());
    XSFLIB_CHECK(grown.before == before.size() && grown.after == after.size());
    XSFLIB_CHECK(grown.groups ==
                 std::vector<hokuto::format::GroupGrowth>({{"GROUP_SONG", song.size(), longer.size()}}));
    XSFLIB_CHECK(same.before == same.after && same.groups.empty());
    XSFLIB_CHECK(unread.before == before.size() && unread.after == kBroken.size() && unread.groups.empty());
    XSFLIB_CHECK(from_unread.before == kBroken.size() && from_unread.after == after.size() &&
                 from_unread.groups.empty());
}

// The game's sizes, in both forms. The import measures the archive and the files that its group loads: a copy of the
// song. SIZE follows TABL. The game_size line follows the native line, and the group's line gives the group's size. The
// sizes stay after a save, and an export measures its growth from them.
void TestGameSizes()
{
    const std::vector<uint8_t> song = LongerSong(0);
    const std::vector<uint8_t> longer = LongerSong(20);
    const std::vector<uint8_t> archive = GroupArchive(song);
    const std::vector<uint8_t> grown = GroupArchive(longer);
    const File file = hokuto::format::ImportSwitchArchive(archive);
    File largest = file;
    largest.native.Edit([](NativeArchive& native) { native.game_sizes = GameSizes{UINT64_MAX, {0}}; });
    File without = file;
    without.native.Edit([](NativeArchive& native) { native.game_sizes.reset(); });

    const std::vector<uint8_t> binary = hokuto::format::Write(file);
    const std::string text = hokuto::format::WriteText(file);
    const File saved = hokuto::format::Read(binary);
    const hokuto::format::ArchiveGrowth growth =
        hokuto::format::MeasureGrowth(*saved.native->game_sizes, grown, Flavor::kNx);

    XSFLIB_CHECK((file.native->game_sizes == GameSizes{archive.size(), {song.size()}}));
    XSFLIB_CHECK(saved == file && hokuto::format::ReadText(text) == file);
    XSFLIB_CHECK(hokuto::format::WriteText(hokuto::format::ReadText(text)) == text);
    for (const File& other : {largest, without})
    {
        XSFLIB_CHECK(hokuto::format::Read(hokuto::format::Write(other)) == other);
        XSFLIB_CHECK(hokuto::format::ReadText(hokuto::format::WriteText(other)) == other);
    }
    XSFLIB_CHECK(growth.before == archive.size() && growth.after == grown.size());
    XSFLIB_CHECK(growth.groups ==
                 std::vector<hokuto::format::GroupGrowth>({{"GROUP_SONG", song.size(), longer.size()}}));

    // The archive's size in 8 bytes, the number of groups in 4, and each group's size in 8. A file without the game's
    // sizes has no SIZE chunk.
    std::vector<uint8_t> expected(20);
    xsflib::StoreLe64(&expected[0], archive.size());
    xsflib::StoreLe32(&expected[8], 1);
    xsflib::StoreLe64(&expected[12], song.size());
    const std::vector<std::string> kIds = {"ENGN", "SEQS", "NATV", "TABL", "SIZE"};
    XSFLIB_CHECK(ChunkIds(binary) == kIds);
    XSFLIB_CHECK(std::ranges::equal(ChunkBytes(binary, 4), expected));
    XSFLIB_CHECK(ChunkIds(hokuto::format::Write(without)).size() == kIds.size() - 1);

    // The lines, then lines that a reader refuses: a second game_size line, one after the tables, a group's size
    // without the game_size line, a group's line without its size, and a size below 0. Each message names the changed
    // line.
    const std::string archive_line = "game_size " + std::to_string(archive.size());
    const std::string group_line = "group GROUP_SONG game_size " + std::to_string(song.size()) + " # ID 0x6000000";
    const std::string bare = hokuto::format::WriteText(without);
    XSFLIB_CHECK(text.find("\nnative switch\n    " + archive_line + "\n") != std::string::npos);
    XSFLIB_CHECK(text.find("\n        " + group_line + "\n") != std::string::npos);
    const std::size_t archive_at = LineOf(text, archive_line);
    const std::size_t tables_at = LineOf(bare, "    tables\n");
    const std::size_t group_at = LineOf(text, group_line);
    XSFLIB_CHECK(TextError(Replaced(text, archive_line, archive_line + "\n    game_size 1"))
                     .starts_with(LineStart(archive_at + 1) + "the native archive has one game_size line"));
    XSFLIB_CHECK(TextError(Replaced(bare, "    tables\n", "    tables\n    game_size 1\n"))
                     .starts_with(LineStart(tables_at + 1) + "the native archive's game_size comes before the tables"));
    XSFLIB_CHECK(TextError(Replaced(bare, "group GROUP_SONG #", "group GROUP_SONG game_size 1 #"))
                     .starts_with(LineStart(group_at - 1) +
                                  "a group's game_size requires the native archive's game_size line, before the "
                                  "tables"));
    XSFLIB_CHECK(TextError(Replaced(text, group_line, "group GROUP_SONG"))
                     .starts_with(LineStart(group_at) +
                                  "group GROUP_SONG gives no game_size, and the native archive gives its game_size"));
    XSFLIB_CHECK(TextError(Replaced(text, archive_line, "game_size -1"))
                     .starts_with(LineStart(archive_at) +
                                  "a size has to be a whole number from 0 to 18446744073709551615, not -1"));

    // The binary form refuses a SIZE chunk that covers no group, one that ends early, two SIZE chunks, and the game's
    // sizes without a native archive. Byte 8 of SIZE holds the number of groups. The table of chunks holds ENGN, SEQS
    // and NATV first, and each chunk's ID starts its entry.
    XSFLIB_CHECK(Patched(binary, 4, 8, 0) ==
                 "The file is damaged: the game's sizes cover 0 groups, and the tables list 1 group.");
    XSFLIB_CHECK(Patched(binary, 4, 8, 2) == "The file is damaged: the SIZE chunk ends early.");

    const auto renamed = [](std::vector<uint8_t> bytes, std::size_t chunk, std::string_view id)
    {
        std::copy(id.begin(), id.end(), bytes.begin() + 24 + 24 * static_cast<std::ptrdiff_t>(chunk));

        return Message<xsflib::FormatError>([&] { hokuto::format::Read(bytes); });
    };

    File loose;
    loose.native = NativeArchive{.pieces = std::vector<ntdlib::sar::Piece>{std::vector<uint8_t>{1, 2, 3}},
                                 .game_sizes = GameSizes{3, {}}};
    XSFLIB_CHECK(renamed(binary, 3, "SIZE") == "The file is damaged: it holds two SIZE chunks.");
    XSFLIB_CHECK(renamed(hokuto::format::Write(loose), 2, "XTRA") ==
                 "The file is damaged: it holds the game's sizes without a native archive.");
    XSFLIB_CHECK(hokuto::format::Read(hokuto::format::Write(loose)) == loose);
    XSFLIB_CHECK(hokuto::format::ReadText(hokuto::format::WriteText(loose)) == loose);

    // Neither form writes sizes that don't cover the tables' groups.
    File more = file;
    more.native.Edit([](NativeArchive& native) { native.game_sizes->groups.push_back(1); });
    File untabled = loose;
    untabled.native.Edit([](NativeArchive& native) { native.game_sizes->groups.push_back(1); });
    XSFLIB_CHECK(Message<std::invalid_argument>([&] { hokuto::format::Write(more); }) ==
                 "the game's sizes cover 2 groups, and the tables list 1 group");
    XSFLIB_CHECK(Message<std::invalid_argument>([&] { hokuto::format::WriteText(more); }) ==
                 "the game's sizes cover 2 groups, and the tables list 1 group");
    XSFLIB_CHECK(Message<std::invalid_argument>([&] { hokuto::format::Write(untabled); }) ==
                 "the game's sizes cover 1 group, and the native archive has no tables");
}

// The file that Unusual makes comes back through both forms. The binary form holds version 1.0, the group files in GRPF
// and the effects in MIXR. The text form writes each float with the fewest digits that read back as the float. It gives
// the same text where the decimal point is a comma. The text holds the tables' lines, the labels, the bracketed parts,
// and the native archive's pieces and fields.
void TestUnusual()
{
    const File file = Unusual();

    const std::vector<uint8_t> binary = hokuto::format::Write(file);
    const std::string text = hokuto::format::WriteText(file);

    XSFLIB_CHECK(hokuto::format::Read(binary) == file);
    XSFLIB_CHECK(hokuto::format::ReadText(text) == file);
    XSFLIB_CHECK(hokuto::format::WriteText(hokuto::format::ReadText(text)) == text);

    // Version 1.0, and the group files in the GRPF chunk before the other chunks: their count, then each one's group,
    // the game's size and SHA-1, its pieces and its fields, as in NATV. NATV holds the sequence's encodings.
    XSFLIB_CHECK(binary[8] == 1 && binary[9] == 0 && binary[10] == 0 && binary[11] == 0);
    XSFLIB_CHECK(text.starts_with("hok 1.0\n") &&
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
            const std::string written = hokuto::format::WriteText(file);
            const File read = hokuto::format::ReadText(text);
            std::setlocale(LC_NUMERIC, "C");
            XSFLIB_CHECK(written == text && read == file);
        }
        std::setlocale(LC_NUMERIC, "C");
    }

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
        return hokuto::format::ReadText(text).sequences[0]->items[0].value.value;
    };

    // The nearest 2^-16. Halfway goes away from zero.
    XSFLIB_CHECK(value("0.1") == 6554 && value("-0.1") == -6554 && value("100") == 100 * kOne);
    XSFLIB_CHECK(value("0.00000762939453125") == 1 && value("-0.00000762939453125") == -1);
    XSFLIB_CHECK(value("0.0000076293945312") == 0 && value(".5") == kOne / 2 && value("7.") == 7 * kOne);
    XSFLIB_CHECK(!TextError("hok 1.0\nengine switch\nsequence 0\n    volume 1e5\n").empty());
    XSFLIB_CHECK(!TextError("hok 1.0\nengine switch\nsequence 0\n    volume 140737488355328\n").empty());
    XSFLIB_CHECK(value("-140737488355328") == std::numeric_limits<Fixed>::min());
}

// The effects' lines of the text form take their parameters in any order, with 0 for each one that a line leaves out. A
// value rounds to the nearest float. A reader refuses lines with mistakes, and an effect after the sequences. The
// binary form refuses an effect of a later version, a parameter short and a parameter that isn't a number. Neither form
// writes an effect with a parameter short or an infinite one.
void TestEffects()
{
    const std::string kHeader = "hok 1.1\nengine switch\n";
    const auto effects = [&](std::string_view lines)
    {
        return hokuto::format::ReadText(kHeader + std::string(lines)).aux_effects;
    };

    // Parameters in any order, with 0 for each one that a line leaves out. A bus keeps the order of its lines.
    const auto read = effects(
        "effect C i3dl2_reverb dry_gain 0.5 room_gain -1000\neffect A i3dl2_reverb\n"
        "effect C i3dl2_reverb late_reverb_delay 1e-3\n");
    std::vector<float> first(12, 0.0f);
    first[0] = -1000.0f;
    first[11] = 0.5f;
    std::vector<float> second(12, 0.0f);
    second[7] = 0.001f;
    const std::vector<Effect> kBusA = {{EffectKind::kI3dl2Reverb, std::vector<float>(12, 0.0f)}};
    const std::vector<Effect> kBusC = {{EffectKind::kI3dl2Reverb, first}, {EffectKind::kI3dl2Reverb, second}};
    XSFLIB_CHECK(read[0] == kBusA && read[1].empty() && read[2] == kBusC);

    // A value rounds to the nearest float, up to the largest.
    const auto value = [&](std::string_view number)
    {
        return effects("effect A i3dl2_reverb dry_gain " + std::string(number) + "\n")[0][0].parameters[11];
    };
    XSFLIB_CHECK(value("0.1") == 0.1f && value("1E3") == 1000.0f && value(".5") == 0.5f && value("7.") == 7.0f);
    XSFLIB_CHECK(value("1e-46") == 0.0f && value("8e-46") == std::numeric_limits<float>::denorm_min());
    XSFLIB_CHECK(value("3.4028235677e38") == std::numeric_limits<float>::max());
    XSFLIB_CHECK(value("-0") == 0.0f && std::signbit(value("-0")));

    // Lines that a reader refuses.
    const auto error = [&](std::string_view line)
    {
        return TextError(kHeader + std::string(line) + "\n");
    };
    XSFLIB_CHECK(error("effect D i3dl2_reverb").starts_with("Line 3: an effect's line is effect, a bus from A to C"));
    XSFLIB_CHECK(error("effect A").starts_with("Line 3: an effect's line"));
    XSFLIB_CHECK(error("effect A echo").starts_with("Line 3: Hokuto has no effect named echo"));
    XSFLIB_CHECK(
        error("effect A i3dl2_reverb loudness 3").starts_with("Line 3: the effect has no parameter named loudness"));
    XSFLIB_CHECK(
        error("effect A i3dl2_reverb dry_gain 1 dry_gain 2").starts_with("Line 3: the effect gives dry_gain twice"));
    XSFLIB_CHECK(error("effect A i3dl2_reverb dry_gain").starts_with("Line 3: dry_gain takes a number"));
    for (const std::string_view number :
         {"3.4028235678e38", "1e39", "+1", "0,5", "1x", "\"1\"", "inf", "nan", "0x10", "-"})
    {
        XSFLIB_CHECK(error("effect A i3dl2_reverb dry_gain " + std::string(number))
                         .starts_with("Line 3: a parameter's value is a decimal number within a float's range"));
    }
    XSFLIB_CHECK(TextError(kHeader + "sequence 0\n    end\neffect A i3dl2_reverb\n")
                     .starts_with("Line 5: the effects come before the sequences"));

    // Effects that the binary form refuses: a kind of a later version, a parameter short, and a parameter that isn't a
    // number. MIXR's byte 4 holds the first effect's kind and byte 8 the number of its parameters. A third byte of 0xFF
    // makes its second parameter, the largest float, a NaN.
    const std::vector<uint8_t> binary = hokuto::format::Write(Unusual());
    XSFLIB_CHECK(Patched(binary, 1, 4, 17).starts_with("The file holds an effect (17) that this version"));
    XSFLIB_CHECK(Patched(binary, 1, 8, 11) ==
                 "The file is damaged: an effect on aux bus A has 11 parameters, and its kind has 12.");
    XSFLIB_CHECK(Patched(binary, 1, 18, 0xFF) ==
                 "The file is damaged: an effect on aux bus A has a parameter that isn't a finite number.");
    XSFLIB_CHECK(Patched(binary, 1, 4, 1).empty());

    // Effects that neither form can hold.
    File short_effect = Unusual();
    short_effect.aux_effects[1] = {{EffectKind::kI3dl2Reverb, {1.0f}}};
    XSFLIB_CHECK(Message<std::invalid_argument>([&] { hokuto::format::Write(short_effect); }) ==
                 "an effect on aux bus B has 1 parameter, and its kind has 12");
    XSFLIB_CHECK(Message<std::invalid_argument>([&] { hokuto::format::WriteText(short_effect); }) ==
                 "an effect on aux bus B has 1 parameter, and its kind has 12");
    File infinite = Unusual();
    infinite.aux_effects[2][1].parameters[3] = std::numeric_limits<float>::infinity();
    XSFLIB_CHECK(Message<std::invalid_argument>([&] { hokuto::format::Write(infinite); }) ==
                 "an effect on aux bus C has a parameter that isn't a finite number");
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { hokuto::format::WriteText(infinite); }));
}

// The Wii U's four effects in both forms. MIXR gives each effect's kind and the number of its parameters before them.
// Neither form holds a reverb's mode that isn't a whole number among its choices. A Switch file holds the Wii U's
// effects too.
void TestWiiUEffects()
{
    File file;
    file.engine = hokuto::format::Engine::kWiiU;
    file.aux_effects[0] = {
        {EffectKind::kReverbHi, {0.02f, 3.0f, 0.6f, 0.4f, 0.1f, 1.0f, 7.0f, 0.02f, 5.0f, 0.0f, 1.0f}},
        {EffectKind::kChorus, {10.0f, 0.5f, 1.0f, 0.0f, 1.0f}}};
    file.aux_effects[1] = {{EffectKind::kReverbStd, {0.05f, 1.5f, 0.5f, 0.5f, 0.8f, 0.0f, 0.05f, 2.0f, 0.25f, 0.75f}}};
    file.aux_effects[2] = {{EffectKind::kDelay, {160.0f, 0.4f, 1.0f, 500.0f, 1.0f}}};

    const std::vector<uint8_t> binary = hokuto::format::Write(file);
    const std::string text = hokuto::format::WriteText(file);

    // Both forms hold the four kinds. MIXR gives each effect's kind and the number of its parameters before them: the
    // high reverb's 11 at 4, the chorus's 5 at 56, the standard reverb's 10 at 88 and the delay's 5 at 140.
    XSFLIB_CHECK(hokuto::format::Read(binary) == file && hokuto::format::ReadText(text) == file);
    const std::span<const uint8_t> mixer = ChunkBytes(binary, 1);
    SEQLIB_REQUIRE(mixer.size() == 168);
    XSFLIB_CHECK(xsflib::LoadLe32(&mixer[0]) == 2);
    XSFLIB_CHECK(mixer[4] == 2 && xsflib::LoadLe32(&mixer[8]) == 11 && mixer[56] == 4 &&
                 xsflib::LoadLe32(&mixer[60]) == 5);
    XSFLIB_CHECK(xsflib::LoadLe32(&mixer[84]) == 1 && mixer[88] == 3 && xsflib::LoadLe32(&mixer[92]) == 10);
    XSFLIB_CHECK(xsflib::LoadLe32(&mixer[136]) == 1 && mixer[140] == 5 && xsflib::LoadLe32(&mixer[144]) == 5);
    XSFLIB_CHECK(text.find("\neffect A reverb_hi pre_delay_time 0.02 fused_time 3 coloration 0.6 damping 0.4 "
                           "crosstalk 0.1 out_gain 1 early_mode 7 pre_delay_time_max 0.02 fused_mode 5 early_gain 0 "
                           "fused_gain 1\neffect A chorus delay_time 10 depth 0.5 rate 1 feedback 0 out_gain 1\n"
                           "effect B reverb_std pre_delay_time 0.05 fused_time 1.5 coloration 0.5 damping 0.5 "
                           "out_gain 0.8 early_mode 0 pre_delay_time_max 0.05 fused_mode 2 early_gain 0.25 "
                           "fused_gain 0.75\neffect C delay delay 160 feedback 0.4 out_gain 1 max_delay 500 lpf 1\n") !=
                 std::string::npos);

    // Modes that neither form holds: a fraction, one past the last choice, and a negative one. The binary form's early
    // mode is the high reverb's parameter 6, at 36 in MIXR. A third byte of 0xF0 makes its 7 a 7.5.
    File fraction = file;
    fraction.aux_effects[0][0].parameters[6] = 2.5f;
    File past = file;
    past.aux_effects[1][0].parameters[7] = 6.0f;
    File negative = file;
    negative.aux_effects[0][0].parameters[8] = -1.0f;
    XSFLIB_CHECK(Message<std::invalid_argument>([&] { hokuto::format::Write(fraction); }) ==
                 "an effect on aux bus A has a value of early_mode that isn't a whole number from 0 to 7");
    XSFLIB_CHECK(Message<std::invalid_argument>([&] { hokuto::format::WriteText(past); }) ==
                 "an effect on aux bus B has a value of fused_mode that isn't a whole number from 0 to 5");
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { hokuto::format::Write(negative); }));
    XSFLIB_CHECK(Patched(binary, 1, 38, 0xF0) ==
                 "The file is damaged: an effect on aux bus A has a value of early_mode that isn't a whole number "
                 "from 0 to 7.");
    const std::string kHeader = "hok 1.0\nengine wii_u\n";
    XSFLIB_CHECK(TextError(kHeader + "effect A reverb_std fused_mode 6\n") ==
                 "Line 3: the effect has a value of fused_mode that isn't a whole number from 0 to 5.");
    XSFLIB_CHECK(
        TextError(kHeader + "effect A reverb_hi early_mode 0.5\n").starts_with("Line 3: the effect has a value"));
    XSFLIB_CHECK(TextError(kHeader + "effect A reverb_hi early_mode -0\n").empty());

    // A file holds an effect of the other console too.
    File switch_file;
    switch_file.aux_effects = file.aux_effects;
    XSFLIB_CHECK(hokuto::format::Read(hokuto::format::Write(switch_file)) == switch_file);
}

// The Wii's effects in both forms: each kind's code, then its parameters. A reverb of 2008 holds whole modes, and the
// other kinds hold none.
void TestWiiEffects()
{
    File file;
    file.aux_effects[0] = {{EffectKind::kWiiReverbHi, {0.02f, 3.0f, 0.6f, 0.4f, 0.1f, 1.0f}},
                           {EffectKind::kWiiChorus, {10.0f, 0.5f, 1.0f}}};
    file.aux_effects[1] = {
        {EffectKind::kWiiReverbStd2008, {0.05f, 1.5f, 0.5f, 0.5f, 0.8f, 0.0f, 0.05f, 2.0f, 0.25f, 0.75f}},
        {EffectKind::kWiiReverbStd, {0.02f, 3.0f, 0.6f, 0.4f, 1.0f}}};
    file.aux_effects[2] = {
        {EffectKind::kWiiDelay, {160.0f, 0.4f, 1.0f}},
        {EffectKind::kWiiReverbHi2008, {0.02f, 3.0f, 0.6f, 0.4f, 0.1f, 1.0f, 7.0f, 0.02f, 5.0f, 0.0f, 1.0f}}};

    const std::vector<uint8_t> binary = hokuto::format::Write(file);
    const std::string text = hokuto::format::WriteText(file);

    // MIXR gives bus A's count, then the high reverb's code 6 and its 6 parameters at 4, and the chorus's code 8 and
    // its 3 parameters at 36.
    XSFLIB_CHECK(hokuto::format::Read(binary) == file && hokuto::format::ReadText(text) == file);
    const std::span<const uint8_t> mixer = ChunkBytes(binary, 1);
    SEQLIB_REQUIRE(mixer.size() >= 44);
    XSFLIB_CHECK(xsflib::LoadLe32(&mixer[0]) == 2 && mixer[4] == 6 && xsflib::LoadLe32(&mixer[8]) == 6);
    XSFLIB_CHECK(mixer[36] == 8 && xsflib::LoadLe32(&mixer[40]) == 3);
    XSFLIB_CHECK(text.find("\neffect A wii_reverb_hi pre_delay_time 0.02 fused_time 3 coloration 0.6 damping 0.4 "
                           "crosstalk 0.1 out_gain 1\neffect A wii_chorus delay_time 10 depth 0.5 rate 1\n") !=
                 std::string::npos);
    XSFLIB_CHECK(text.find("\neffect C wii_delay delay 160 feedback 0.4 out_gain 1\neffect C wii_reverb_hi_2008 ") !=
                 std::string::npos);

    // A mode of a reverb of 2008 is a whole number from 0, as on the Wii U.
    File fraction = file;
    fraction.aux_effects[2][1].parameters[8] = 1.5f;
    XSFLIB_CHECK(Message<std::invalid_argument>([&] { hokuto::format::Write(fraction); }) ==
                 "an effect on aux bus C has a value of fused_mode that isn't a whole number from 0 to 5");
    XSFLIB_CHECK(TextError("hok 1.0\nengine wii\neffect A wii_chorus delay_time 10 depth 0.5 rate 1 feedback 0\n")
                     .starts_with("Line 3: "));
}

// The 3DS's effects are 12 to 16, with the fields of their classes' Param, and a reverb's filter sizes in place of the
// pointer to them. Their times, sizes and gains have limits, and their switches two choices.
void TestCtrEffects()
{
    for (const auto& [kind, code, name, parameters] :
         {EffectKindCode{EffectKind::kCtrReverb, 12, "3ds_reverb", 12},
          EffectKindCode{EffectKind::kCtrDelay, 13, "3ds_delay", 4},
          EffectKindCode{EffectKind::kCtrNnReverb, 14, "3ds_nn_reverb", 11},
          EffectKindCode{EffectKind::kCtrNnDelay, 15, "3ds_nn_delay", 4},
          EffectKindCode{EffectKind::kCtrNnReverb2011, 16, "3ds_nn_reverb_2011", 10}})
    {
        XSFLIB_CHECK(hokuto::format::CodeOf(kind) == code && hokuto::format::EffectKindOfCode(code) == kind);
        XSFLIB_CHECK(hokuto::format::NameOf(kind) == name && hokuto::format::EffectKindNamed(name) == kind);
        XSFLIB_CHECK(hokuto::format::ParameterNamesOf(kind).size() == parameters && hokuto::format::IsCtrKind(kind));
    }
    XSFLIB_CHECK(!hokuto::format::IsCtrKind(EffectKind::kI3dl2Reverb) &&
                 !hokuto::format::IsCtrKind(EffectKind::kDelay) && !hokuto::format::IsCtrKind(EffectKind::kWiiDelay));
    const std::span<const std::string_view> reverb = hokuto::format::ParameterNamesOf(EffectKind::kCtrReverb);
    XSFLIB_CHECK(reverb[0] == "early_reflection_time" && reverb[1] == "fused_time" && reverb[5] == "comb_size_0" &&
                 reverb[7] == "all_pass_size" && reverb[10] == "surround" && reverb[11] == "high_pass_damping");
    XSFLIB_CHECK(hokuto::format::ParameterNamesOf(EffectKind::kCtrNnReverb)[10] == "high_pass_damping");
    XSFLIB_CHECK(hokuto::format::ParameterNamesOf(EffectKind::kCtrNnReverb2011)[9] == "fused_gain");
    const std::span<const std::string_view> delay = hokuto::format::ParameterNamesOf(EffectKind::kCtrNnDelay);
    XSFLIB_CHECK(delay[0] == "delay_time" && delay[1] == "feedback" && delay[2] == "damping" && delay[3] == "surround");

    // The switches have two choices. The times are whole milliseconds and the sizes whole frames of 160 samples.
    XSFLIB_CHECK(hokuto::format::ChoicesOf(EffectKind::kCtrReverb, 10) == 2 &&
                 hokuto::format::ChoicesOf(EffectKind::kCtrReverb, 11) == 2 &&
                 hokuto::format::ChoicesOf(EffectKind::kCtrNnReverb, 10) == 2 &&
                 hokuto::format::ChoicesOf(EffectKind::kCtrDelay, 3) == 2 &&
                 hokuto::format::ChoicesOf(EffectKind::kCtrReverb, 9) == 0);

    const auto limits = [](EffectKind kind, std::size_t parameter)
    {
        const std::optional<hokuto::format::ParameterLimits> found = hokuto::format::LimitsOf(kind, parameter);
        return found ? std::array{found->low, found->high, found->step} : std::array{-1.0f, -1.0f, -1.0f};
    };
    XSFLIB_CHECK((limits(EffectKind::kCtrReverb, 0) == std::array{0.0f, 1000.0f, 1.0f}));
    XSFLIB_CHECK((limits(EffectKind::kCtrReverb, 1) == std::array{0.0f, 20000.0f, 1.0f}));
    XSFLIB_CHECK((limits(EffectKind::kCtrNnReverb2011, 7) == std::array{160.0f, 32000.0f, 160.0f}));
    XSFLIB_CHECK((limits(EffectKind::kCtrNnReverb, 9) == std::array{0.0f, 1.0f, 0.0f}));
    XSFLIB_CHECK((limits(EffectKind::kCtrDelay, 0) == std::array{1.0f, 5000.0f, 1.0f}));
    XSFLIB_CHECK(!hokuto::format::LimitsOf(EffectKind::kCtrReverb, 10) &&
                 !hokuto::format::LimitsOf(EffectKind::kCtrReverb, 12) &&
                 !hokuto::format::LimitsOf(EffectKind::kReverbHi, 0));

    File file;
    file.engine = hokuto::format::Engine::k3ds;
    file.aux_effects[0] = {{EffectKind::kCtrReverb,
                            {0.0f, 4600.0f, 1.0f, 0.5f, 0.91f, 1120.0f, 1440.0f, 4960.0f, 0.3f, 0.73f, 0.0f, 1.0f}},
                           {EffectKind::kCtrDelay, {250.0f, 0.4f, 0.5f, 1.0f}}};
    file.aux_effects[1] = {
        {EffectKind::kCtrNnReverb, {60.0f, 4000.0f, 100.0f, 0.5f, 0.4f, 3040.0f, 3680.0f, 2080.0f, 0.6f, 0.4f, 1.0f}},
        {EffectKind::kCtrNnDelay, {1.0f, 1.0f, -0.0f, 0.0f}},
        {EffectKind::kCtrNnReverb2011,
         {1000.0f, 20000.0f, 1000.0f, 1.0f, 1.0f, 160.0f, 32000.0f, 32000.0f, 1.0f, 1.0f}}};
    const std::vector<uint8_t> binary = hokuto::format::Write(file);
    const std::string text = hokuto::format::WriteText(file);

    // Both forms hold the five kinds. MIXR gives each effect's kind and the number of its parameters before them: the
    // reverb's 12 at 4, the delay's 4 at 60, then on bus B the reverbs' 11 at 88 and 10 at 164, around the delay's 4 at
    // 140.
    XSFLIB_CHECK(hokuto::format::Read(binary) == file && hokuto::format::ReadText(text) == file);
    const std::span<const uint8_t> mixer = ChunkBytes(binary, 1);
    SEQLIB_REQUIRE(mixer.size() == 216);
    XSFLIB_CHECK(xsflib::LoadLe32(&mixer[0]) == 2);
    XSFLIB_CHECK(mixer[4] == 12 && xsflib::LoadLe32(&mixer[8]) == 12 && mixer[60] == 13 &&
                 xsflib::LoadLe32(&mixer[64]) == 4);
    XSFLIB_CHECK(xsflib::LoadLe32(&mixer[84]) == 3 && mixer[88] == 14 && xsflib::LoadLe32(&mixer[92]) == 11);
    XSFLIB_CHECK(mixer[140] == 15 && xsflib::LoadLe32(&mixer[144]) == 4 && mixer[164] == 16 &&
                 xsflib::LoadLe32(&mixer[168]) == 10 && xsflib::LoadLe32(&mixer[212]) == 0);
    XSFLIB_CHECK(text.find("\neffect A 3ds_reverb early_reflection_time 0 fused_time 4600 pre_delay_time 1 "
                           "coloration 0.5 damping 0.91 comb_size_0 1120 comb_size_1 1440 all_pass_size 4960 "
                           "early_gain 0.3 fused_gain 0.73 surround 0 high_pass_damping 1\neffect A 3ds_delay "
                           "delay_time 250 feedback 0.4 damping 0.5 surround 1\neffect B 3ds_nn_reverb "
                           "early_reflection_time 60 fused_time 4000 pre_delay_time 100 coloration 0.5 damping 0.4 "
                           "comb_size_0 3040 comb_size_1 3680 all_pass_size 2080 early_gain 0.6 fused_gain 0.4 "
                           "high_pass_damping 1\neffect B 3ds_nn_delay delay_time 1 feedback 1 damping -0 surround 0\n"
                           "effect B 3ds_nn_reverb_2011 early_reflection_time 1000 fused_time 20000 pre_delay_time "
                           "1000 coloration 1 damping 1 comb_size_0 160 comb_size_1 32000 all_pass_size 32000 "
                           "early_gain 1 fused_gain 1\n") != std::string::npos);

    // Values outside the limits. Neither form holds them.
    struct Outside
    {
        std::size_t bus;
        std::size_t effect;
        std::size_t parameter;
        float value;
        std::string_view problem;
    };
    for (const Outside& outside : {
             Outside{0, 0, 0, 1000.5f, "a value of early_reflection_time that isn't a whole number from 0 to 1000"},
             Outside{0, 0, 1, 20001.0f, "a value of fused_time that isn't a whole number from 0 to 20000"},
             Outside{0, 0, 2, -1.0f, "a value of pre_delay_time that isn't a whole number from 0 to 1000"},
             Outside{0, 0, 3, 1.01f, "a value of coloration that lies outside 0 to 1"},
             Outside{0, 0, 5, 0.0f, "a value of comb_size_0 that isn't a multiple of 160 from 160 to 32000"},
             Outside{0, 0, 6, 1441.0f, "a value of comb_size_1 that isn't a multiple of 160 from 160 to 32000"},
             Outside{0, 0, 7, 32160.0f, "a value of all_pass_size that isn't a multiple of 160 from 160 to 32000"},
             Outside{0, 0, 8, -0.01f, "a value of early_gain that lies outside 0 to 1"},
             Outside{0, 0, 10, 2.0f, "a value of surround that isn't a whole number from 0 to 1"},
             Outside{0, 1, 0, 0.0f, "a value of delay_time that isn't a whole number from 1 to 5000"},
             Outside{0, 1, 0, 5001.0f, "a value of delay_time that isn't a whole number from 1 to 5000"},
             Outside{1, 1, 1, 1.5f, "a value of feedback that lies outside 0 to 1"},
             Outside{1, 2, 4, 2.0f, "a value of damping that lies outside 0 to 1"},
             Outside{1, 0, 10, 0.5f, "a value of high_pass_damping that isn't a whole number from 0 to 1"},
         })
    {
        File outside_file = file;
        outside_file.aux_effects[outside.bus][outside.effect].parameters[outside.parameter] = outside.value;
        const std::string problem = "an effect on aux bus " + std::string(1, static_cast<char>('A' + outside.bus)) +
                                    " has " + std::string(outside.problem);
        XSFLIB_CHECK(Message<std::invalid_argument>([&] { hokuto::format::Write(outside_file); }) == problem);
        XSFLIB_CHECK(Message<std::invalid_argument>([&] { hokuto::format::WriteText(outside_file); }) == problem);
    }

    // A reader refuses them too. The first effect's comb_size_0 lies at 32 in MIXR. A first byte of 1 makes its 1120 a
    // little more. A line that leaves a filter's size out gives it 0.
    XSFLIB_CHECK(Patched(binary, 1, 32, 1) ==
                 "The file is damaged: an effect on aux bus A has a value of comb_size_0 that isn't a multiple of 160 "
                 "from 160 to 32000.");
    const std::string kHeader = "hok 1.0\nengine 3ds\n";
    XSFLIB_CHECK(TextError(kHeader + "effect A 3ds_delay delay_time 250\n").empty());
    XSFLIB_CHECK(TextError(kHeader + "effect A 3ds_nn_reverb_2011 fused_time 4000\n") ==
                 "Line 3: the effect has a value of comb_size_0 that isn't a multiple of 160 from 160 to 32000.");
    XSFLIB_CHECK(TextError(kHeader + "effect A 3ds_delay delay_time 250.5\n") ==
                 "Line 3: the effect has a value of delay_time that isn't a whole number from 1 to 5000.");
}

// The buses' volumes, in a file without effects and in one with them. MIXR holds them after bus C's effects, and the
// text form a line for each bus whose volume isn't 1. A file whose buses are all at full volume keeps the chunk as it
// was, and one without effects has no MIXR chunk.
void TestBusVolumes()
{
    File file;
    file.aux_volumes = {0.5f, 1.0f, 0.0f};
    File with_effects = Unusual();
    with_effects.aux_volumes = {1.0f, 2.0f, 1.0f};

    const std::vector<uint8_t> binary = hokuto::format::Write(file);
    const std::string text = hokuto::format::WriteText(file);
    const std::vector<uint8_t> unusual = hokuto::format::Write(with_effects);
    const std::string unusual_text = hokuto::format::WriteText(with_effects);

    // Without effects: three counts, then 0.5, 1 and 0 as floats. The first and third counts are 0. With them:
    // Unusual's 180 bytes, then 1, 2 and 1.
    XSFLIB_CHECK(hokuto::format::Read(binary) == file && hokuto::format::ReadText(text) == file);
    XSFLIB_CHECK(ChunkIds(binary) == std::vector<std::string>({"ENGN", "MIXR", "SEQS"}));
    const std::span<const uint8_t> mixer = ChunkBytes(binary, 1);
    SEQLIB_REQUIRE(mixer.size() == 24);
    XSFLIB_CHECK(xsflib::LoadLe32(&mixer[0]) == 0 && xsflib::LoadLe32(&mixer[8]) == 0);
    XSFLIB_CHECK(xsflib::LoadLe32(&mixer[12]) == 0x3F000000u && xsflib::LoadLe32(&mixer[16]) == 0x3F800000u &&
                 xsflib::LoadLe32(&mixer[20]) == 0);
    XSFLIB_CHECK(text.find("\nmixing strict\nbus_volume A 0.5\nbus_volume C 0\n") != std::string::npos);
    XSFLIB_CHECK(hokuto::format::Read(unusual) == with_effects &&
                 hokuto::format::ReadText(unusual_text) == with_effects);
    const std::span<const uint8_t> unusual_mixer = ChunkBytes(unusual, 1);
    XSFLIB_CHECK(unusual_mixer.size() == 180 + 12 && xsflib::LoadLe32(&unusual_mixer[180]) == 0x3F800000u &&
                 xsflib::LoadLe32(&unusual_mixer[184]) == 0x40000000u);
    XSFLIB_CHECK(unusual_text.find("dry_gain 0.75\nbus_volume B 2\n\nsequence 0\n") != std::string::npos);

    // Full volume.
    XSFLIB_CHECK(ChunkBytes(hokuto::format::Write(Unusual()), 1).size() == 180);
    XSFLIB_CHECK(ChunkIds(hokuto::format::Write(File())) == std::vector<std::string>({"ENGN", "SEQS"}));
    const File unity = hokuto::format::ReadText("hok 1.0\nengine switch\nbus_volume A 1\n");
    XSFLIB_CHECK(unity == File() && hokuto::format::WriteText(unity).find("bus_volume") == std::string::npos);

    // Volumes that the binary form refuses: A's 0.5 becomes 8, and C's 0 becomes -0.5. -0 reads as a volume of 0.
    XSFLIB_CHECK(Patched(binary, 1, 15, 0x41) == "The file is damaged: aux bus A's volume lies outside 0 to 2.");
    XSFLIB_CHECK(Patched(binary, 1, 23, 0xBF) == "The file is damaged: aux bus C's volume lies outside 0 to 2.");
    XSFLIB_CHECK(Patched(binary, 1, 23, 0x80).empty());

    // Volumes that neither form can hold.
    File not_a_number;
    not_a_number.aux_volumes[1] = std::numeric_limits<float>::quiet_NaN();
    File loud;
    loud.aux_volumes[2] = 2.5f;
    XSFLIB_CHECK(Message<std::invalid_argument>([&] { hokuto::format::Write(not_a_number); }) ==
                 "aux bus B's volume isn't a finite number");
    XSFLIB_CHECK(Message<std::invalid_argument>([&] { hokuto::format::WriteText(not_a_number); }) ==
                 "aux bus B's volume isn't a finite number");
    XSFLIB_CHECK(Message<std::invalid_argument>([&] { hokuto::format::Write(loud); }) ==
                 "aux bus C's volume lies outside 0 to 2");
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { hokuto::format::WriteText(loud); }));

    // Lines that a reader refuses.
    const std::string kHeader = "hok 1.0\nengine switch\n";
    const auto error = [&](std::string_view lines)
    {
        return TextError(kHeader + std::string(lines));
    };
    XSFLIB_CHECK(error("bus_volume D 1\n") ==
                 "Line 3: a bus's volume's line is bus_volume, a bus from A to C and the volume.");
    XSFLIB_CHECK(error("bus_volume A\n").starts_with("Line 3: a bus's volume's line"));
    XSFLIB_CHECK(error("bus_volume A 1 2\n").starts_with("Line 3: a bus's volume's line"));
    XSFLIB_CHECK(error("bus_volume A 3\n") == "Line 3: the volume lies outside 0 to 2.");
    XSFLIB_CHECK(error("bus_volume B -1\n") == "Line 3: the volume lies outside 0 to 2.");
    XSFLIB_CHECK(error("bus_volume A nan\n").starts_with("Line 3: a parameter's value is a decimal number"));
    XSFLIB_CHECK(error("bus_volume A 1\nbus_volume A 0.5\n") == "Line 4: the file gives bus A's volume twice.");
    XSFLIB_CHECK(error("sequence 0\n    end\nbus_volume A 0.5\n") ==
                 "Line 5: the buses' volumes come before the sequences.");
}

// The scenes in both forms. SCEN follows GRPF, and the text form gives the scenes after the effects. A reader refuses
// times and values out of range, and lines out of place. Neither form holds a variable past 31 or changes out of the
// order of their times. The binary form refuses a kind of variable of a later version. A file without scenes has no
// SCEN chunk.
void TestScenes()
{
    const File file = WithScenes();

    const std::vector<uint8_t> binary = hokuto::format::Write(file);
    const std::string text = hokuto::format::WriteText(file);

    XSFLIB_CHECK(hokuto::format::Read(binary) == file);
    XSFLIB_CHECK(hokuto::format::ReadText(text) == file);
    XSFLIB_CHECK(hokuto::format::WriteText(hokuto::format::ReadText(text)) == text);

    // The scenes in the SCEN chunk after GRPF: their number, then each scene's sound, name, starting values and
    // changes. The first scene's name takes 8 bytes after its length. Its first starting value follows the count at 20:
    // variable 0, a zero and -32768. Its first change follows the count at 40, and its second starts at 52.
    const std::vector<std::string> kIds = {"ENGN", "MIXR", "SEQS", "NATV", "TABL", "GRPF", "SCEN", "XTRA", "x y!"};
    const std::span<const uint8_t> scenes = ChunkBytes(binary, 6);
    XSFLIB_CHECK(ChunkIds(binary) == kIds);
    SEQLIB_REQUIRE(scenes.size() == 4 + 72 + 16 + 33);
    XSFLIB_CHECK(xsflib::LoadLe32(&scenes[0]) == 3 && xsflib::LoadLe32(&scenes[4]) == 0 &&
                 xsflib::LoadLe32(&scenes[8]) == 8 && xsflib::LoadLe32(&scenes[20]) == 4);
    XSFLIB_CHECK(scenes[24] == 0 && scenes[25] == 0 && xsflib::LoadLe16(&scenes[26]) == 0x8000);
    XSFLIB_CHECK(xsflib::LoadLe32(&scenes[40]) == 4 && xsflib::LoadLe32(&scenes[44]) == 0 && scenes[48] == 3 &&
                 scenes[49] == 0 && xsflib::LoadLe16(&scenes[50]) == 1 && xsflib::LoadLe32(&scenes[52]) == 90000);

    // The scenes after the effects, each after a blank line. A comment gives the sound's name from the tables. A time
    // is in seconds, with a fraction only where it has one.
    XSFLIB_CHECK(
        text.find("dry_gain 0.75\n\nscene 0 \"Last lap\" # Song\n    variable 0 -32768\n    variable 15 32767\n"
                  "    variable 16 -1\n    variable 31 0\n    at 0 variable 3 1\n    at 90 variable 3 2\n"
                  "    at 90 variable 31 -5\n    at 4294967.295 variable 0 7\n\nscene 0 \"\" # Song\n\n"
                  "scene 70000 scene\n    variable 1 1\n    at 1.5 variable 1 0\n\nsequence 0\n") != std::string::npos);

    // Times and values that a reader takes, in hex too for a value.
    const std::string kHeader = "hok 1.2\nengine switch\nscene 3 Lap\n";
    const auto scene = [&](std::string_view lines)
    {
        return hokuto::format::ReadText(kHeader + std::string(lines)).scenes.at(0);
    };
    const hokuto::format::Scene read =
        scene("    variable 7 0x7FFF\n    variable 2 -0\n    at 0.001 variable 2 5\n    at 12.34 variable 7 -1\n");
    XSFLIB_CHECK(read.sound == 3 && read.name == "Lap");
    XSFLIB_CHECK(read.values == (std::map<uint8_t, int16_t>{{2, 0}, {7, 32767}}));
    XSFLIB_CHECK(read.changes == (std::vector<hokuto::format::VariableChange>{{1, 2, 5}, {12340, 7, -1}}));

    // Lines that a reader refuses.
    const auto error = [&](std::string_view lines)
    {
        return TextError(kHeader + std::string(lines));
    };
    for (const std::string_view time : {"4294967.296", "1.0005", "-1", "1.", ".5", "0x10", "1e3", "\"1\"", "1,5"})
    {
        XSFLIB_CHECK(error("    at " + std::string(time) + " variable 1 1\n")
                         .starts_with("Line 4: a time is a number of seconds from 0 to 4294967.295"));
    }
    for (const std::string_view value : {"32768", "-32769", "1.5", "\"1\"", "--1", "x"})
    {
        XSFLIB_CHECK(error("    variable 1 " + std::string(value) + "\n")
                         .starts_with("Line 4: a variable's value is a whole number from -32768 to 32767"));
    }
    XSFLIB_CHECK(error("    variable 32 1\n")
                     .starts_with("Line 4: a variable's number has to be a whole number from 0 to 31, not 32"));
    XSFLIB_CHECK(
        error("    variable 3 1\n    variable 3 2\n").starts_with("Line 5: the scene starts variable 3 twice"));
    XSFLIB_CHECK(error("    at 2 variable 3 1\n    at 1 variable 3 2\n")
                     .starts_with("Line 5: a scene's changes come in the order of their times"));
    XSFLIB_CHECK(error("    at 2 3 1\n").starts_with("Line 4: a scene's line is variable"));
    XSFLIB_CHECK(error("    variable 3\n").starts_with("Line 4: a scene's line is variable"));
    XSFLIB_CHECK(error("scene 4\n").starts_with("Line 4: a scene's line is scene, its sound's number and its name"));
    XSFLIB_CHECK(error("effect A i3dl2_reverb\n    variable 1 1\n")
                     .starts_with("Line 5: variable belongs to a scene, after the scene's line"));
    XSFLIB_CHECK(TextError("hok 1.2\nengine switch\n    at 1 variable 1 1\n")
                     .starts_with("Line 3: at belongs to a scene, after the scene's line"));
    XSFLIB_CHECK(
        error("sequence 0\n    end\nscene 0 Late\n").starts_with("Line 6: the scenes come before the sequences"));
    XSFLIB_CHECK(error("sequence 0\n    variable 1 1\n").starts_with("Line 5: no command"));

    // Scenes that the binary form refuses: a zero of a later version, a variable past 31, a variable started twice and
    // changes out of order. A top byte of 0xFF puts the second change's time after the third's.
    XSFLIB_CHECK(Patched(binary, 6, 25, 1).starts_with("The file holds a kind of variable (1) that this version"));
    XSFLIB_CHECK(Patched(binary, 6, 49, 1).starts_with("The file holds a kind of variable (1) that this version"));
    XSFLIB_CHECK(Patched(binary, 6, 24, 40) ==
                 "The file is damaged: scene 0 sets variable 40, and a scene sets variables 0 to 31 only.");
    XSFLIB_CHECK(Patched(binary, 6, 28, 0) == "The file is damaged: scene 0 starts variable 0 twice.");
    XSFLIB_CHECK(Patched(binary, 6, 55, 0xFF) ==
                 "The file is damaged: scene 0 changes its variables out of the order of their times.");
    XSFLIB_CHECK(Patched(binary, 6, 48, 31).empty());

    // Scenes that neither form can hold.
    File past = WithScenes();
    past.scenes[1].changes.push_back({5, 32, 0});
    XSFLIB_CHECK(Message<std::invalid_argument>([&] { hokuto::format::Write(past); }) ==
                 "scene 1 sets variable 32, and a scene sets variables 0 to 31 only");
    XSFLIB_CHECK(Message<std::invalid_argument>([&] { hokuto::format::WriteText(past); }) ==
                 "scene 1 sets variable 32, and a scene sets variables 0 to 31 only");
    File backwards = WithScenes();
    std::swap(backwards.scenes[0].changes[0], backwards.scenes[0].changes[1]);
    XSFLIB_CHECK(Message<std::invalid_argument>([&] { hokuto::format::Write(backwards); }) ==
                 "scene 0 changes its variables out of the order of their times");
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { hokuto::format::WriteText(backwards); }));

    // A file without scenes has no SCEN chunk.
    XSFLIB_CHECK(ChunkIds(hokuto::format::Write(Unusual())).size() == kIds.size() - 1);
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

    const std::vector<uint8_t> binary = hokuto::format::Write(file);
    const std::string text = hokuto::format::WriteText(file);

    XSFLIB_CHECK(hokuto::format::Read(binary) == file);
    XSFLIB_CHECK(hokuto::format::ReadText(text) == file);
    XSFLIB_CHECK(hokuto::format::WriteText(hokuto::format::ReadText(text)) == text);

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
    XSFLIB_CHECK(Patched(binary, 7, 16, 0) == "The file is damaged: it gives track 0 of sound 0 two colours.");

    // A file without colours has no TCOL chunk.
    XSFLIB_CHECK(ChunkIds(hokuto::format::Write(WithScenes())).size() == kIds.size() - 1);
}

// The program's view, in both forms. EDTR follows TCOL.
void TestView()
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

    const std::vector<uint8_t> binary = hokuto::format::Write(file);
    const std::string text = hokuto::format::WriteText(file);
    const std::string closed_text = hokuto::format::WriteText(closed);

    XSFLIB_CHECK(hokuto::format::Read(binary) == file);
    XSFLIB_CHECK(hokuto::format::ReadText(text) == file);
    XSFLIB_CHECK(hokuto::format::WriteText(hokuto::format::ReadText(text)) == text);
    XSFLIB_CHECK(hokuto::format::Read(hokuto::format::Write(closed)) == closed);
    XSFLIB_CHECK(hokuto::format::ReadText(closed_text) == closed);

    // The sound, the position and the cycle's locators, then a byte for the cycle and one for each track: the selected
    // one, the Key Editor's and the List Editor's, or 0xFF for none. The arrangement's sizes, scroll and sets of
    // tracks, the Key Editor's sizes and scroll, and the lanes: the number of each one's kind in 2 bytes, its bus and a
    // 0. A byte for the window in front ends the chunk: 1 for the Key Editor.
    const std::vector<std::string> kIds = {"ENGN", "MIXR", "SEQS", "NATV", "TABL", "GRPF",
                                           "SCEN", "TCOL", "EDTR", "XTRA", "x y!"};
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
    XSFLIB_CHECK(xsflib::LoadLe16(&view[64]) == hokuto::format::CodeOf(Kind::kVolume) && view[66] == 0 &&
                 view[67] == 0);
    XSFLIB_CHECK(xsflib::LoadLe16(&view[68]) == hokuto::format::CodeOf(Kind::kSend) && view[70] == 2 && view[71] == 0);
    XSFLIB_CHECK(view[72] == 1);
    const std::vector<uint8_t> closed_binary = hokuto::format::Write(closed);
    const std::span<const uint8_t> closed_view = ChunkBytes(closed_binary, 8);
    XSFLIB_CHECK(closed_view.size() == 65 && closed_view[17] == 0xFF && closed_view[18] == 0xFF &&
                 closed_view[19] == 0xFF && closed_view[64] == 1);

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
    XSFLIB_CHECK(hokuto::format::WriteText(named).find("\n\nview sound 0 # Song\nview position") != std::string::npos);

    // Lines that a reader refuses. A view gives each part that the writers always write.
    const auto error = [](std::string_view lines)
    {
        return TextError("hok 1.0\nengine switch\n" + std::string(lines));
    };

    const std::string kParts = "view position 0\nview cycle 0 1 off\nview scroll 0 0\nview key_scroll 0 127\n";
    const std::string kSized = "view sound 0\nview zoom 1 1\nview key_zoom 1 1\n" + kParts;
    XSFLIB_CHECK(error(kSized).empty());
    XSFLIB_CHECK(error("view sound 0\nview sound 1\n").starts_with("Line 4: the view gives its sound twice"));
    XSFLIB_CHECK(error("view position 1\n").starts_with("Line 3: the view gives no sound"));
    XSFLIB_CHECK(error("view sound 0\n").starts_with("Line 3: the view gives no position"));
    XSFLIB_CHECK(error("view sound 0\nview zoom 1 1\nview key_zoom 1 1\nview position 0\nview cycle 0 1 off\n"
                       "view scroll 0 0\n")
                     .starts_with("Line 3: the view gives no key_scroll"));
    XSFLIB_CHECK(error("view sound 0\nview zoom 1 0\nview key_zoom 1 1\n" + kParts)
                     .starts_with("Line 3: the view holds a size of 0 or less"));
    XSFLIB_CHECK(error(kSized + "view track 32\n")
                     .starts_with("Line 10: a track's number has to be a whole number from 0 to 31, not 32"));
    XSFLIB_CHECK(error(kSized + "view lane loud 0\n").starts_with("Line 10: Hokuto has no kind of command named loud"));
    XSFLIB_CHECK(error("view cycle 0 1 maybe\n").starts_with("Line 3: the cycle is on or off, not maybe"));
    XSFLIB_CHECK(error(kSized + "view colour 1\n").starts_with("Line 10: the view has no part named colour"));
    XSFLIB_CHECK(error(kSized + "view front piano\n")
                     .starts_with("Line 10: the window in front is mixer, key_editor or list_editor, not piano"));
    XSFLIB_CHECK(
        error(kSized + "view front\n").starts_with("Line 10: the view's front takes mixer, key_editor or list_editor"));
    XSFLIB_CHECK(error(kSized + "view mute\n").starts_with("Line 10: the view's mute takes the numbers of tracks"));
    XSFLIB_CHECK(error(kSized + "view zoom 1\n").starts_with("Line 10: the view gives its zoom twice"));
    XSFLIB_CHECK(error("view sound 0\nview zoom 1\n")
                     .starts_with("Line 4: the view's zoom takes the width of a beat and the height of a track"));
    XSFLIB_CHECK(error("view\n").starts_with("Line 3: a line of the view is view"));
    XSFLIB_CHECK(
        error("sequence 0\n    end\nview sound 0\n").starts_with("Line 5: the view comes before the sequences"));

    // A view without a window in front puts the List Editor in front. Hokuto opened such a file that way. Each window
    // reads by its name.
    const auto front = [&kSized](std::string_view line)
    {
        return hokuto::format::ReadText("hok 1.0\nengine switch\n" + kSized + std::string(line)).view->front;
    };
    XSFLIB_CHECK(front("") == ViewFront::kListEditor && front("view front mixer\n") == ViewFront::kMixer &&
                 front("view front key_editor\n") == ViewFront::kKeyEditor &&
                 front("view front list_editor\n") == ViewFront::kListEditor);

    // The binary form refuses a cycle that's neither on nor off, a track past 31, a size below 0, and a kind of command
    // or a window in front that this version doesn't define. A track's height of -66 has the sign bit in its last byte.
    // The form skips the bytes after the window in front: a count of one lane leaves the second lane's bytes after it,
    // and their first byte gives the window. A chunk without the window puts the List Editor in front.
    XSFLIB_CHECK(Patched(binary, 8, 16, 2) == "The file is damaged: the view's cycle is neither on nor off.");
    XSFLIB_CHECK(Patched(binary, 8, 17, 32) ==
                 "The file is damaged: the view names track 32, and tracks run from 0 to 31.");
    XSFLIB_CHECK(Patched(binary, 8, 27, 0xC2) == "The file is damaged: the view holds a size of 0 or less.");
    XSFLIB_CHECK(Patched(binary, 8, 65, 0xFF).starts_with("The file holds a lane of a command ("));
    XSFLIB_CHECK(Patched(binary, 8, 72, 3) ==
                 "The file holds a window in front of the editors (3) that this version of "
                 "Hokuto doesn't know. A later version made it.");
    std::vector<uint8_t> one_lane = binary;
    uint8_t* entry = &one_lane[24 + 24 * 8];
    const auto start = static_cast<std::size_t>(xsflib::LoadLe64(entry + 8));
    one_lane[start + 60] = 1;
    one_lane[start + 68] = 2;
    xsflib::StoreLe32(entry + 4, hokuto::format::Crc32(std::span(one_lane).subspan(start, view.size())));
    File first_lane = file;
    first_lane.view->lanes.pop_back();
    first_lane.view->front = ViewFront::kListEditor;
    XSFLIB_CHECK(hokuto::format::Read(one_lane) == first_lane);
    std::vector<uint8_t> without_front = binary;
    entry = &without_front[24 + 24 * 8];
    xsflib::StoreLe64(entry + 16, view.size() - 1);
    xsflib::StoreLe32(entry + 4, hokuto::format::Crc32(std::span(without_front).subspan(start, view.size() - 1)));
    File listed = file;
    listed.view->front = ViewFront::kListEditor;
    XSFLIB_CHECK(hokuto::format::Read(without_front) == listed);

    // Neither form writes a view with a number that isn't finite. A file without a view has no EDTR chunk.
    File infinite = file;
    infinite.view->left = std::numeric_limits<float>::infinity();
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { hokuto::format::Write(infinite); }));
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { hokuto::format::WriteText(infinite); }));
    File none = file;
    none.view.reset();
    XSFLIB_CHECK(ChunkIds(hokuto::format::Write(none)).size() == kIds.size() - 1);
}

// The tags of the file and of its sounds, in both forms. TAGS follows SCEN. Sound 7 lies past the tables.
void TestTags()
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

    const std::vector<uint8_t> binary = hokuto::format::Write(file);
    const std::string text = hokuto::format::WriteText(file);

    XSFLIB_CHECK(hokuto::format::Read(binary) == file);
    XSFLIB_CHECK(hokuto::format::ReadText(text) == file);
    XSFLIB_CHECK(hokuto::format::WriteText(hokuto::format::ReadText(text)) == text);
    XSFLIB_CHECK(hokuto::format::Read(hokuto::format::Write(empty)) == file);
    XSFLIB_CHECK(hokuto::format::WriteText(empty) == text);

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
    XSFLIB_CHECK(ChunkIds(binary) == kIds);
    XSFLIB_CHECK(std::ranges::equal(ChunkBytes(binary, 7), expected));

    // The lines follow the mixing line: the file's tags, then each sound's, with the sound's name where the tables give
    // one.
    const std::size_t mixing = text.find("\nmixing ");
    XSFLIB_CHECK(text.find("\ntag title") == text.find('\n', mixing + 1));
    XSFLIB_CHECK(text.find("\ntag title \"A \\\"quoted\\\" title\"\ntag year 2026\ntag zz_8 y\ntag zz_9 x\n"
                           "sound_tag 0 artist \"\xC3\x9Cn\xC3\xAF"
                           "code\" # Song\nsound_tag 0 title \"Lap\\x0ATwo\" # Song\n"
                           "sound_tag 7 comment on\neffect A") != std::string::npos);

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

    // The binary form refuses a key twice, a key with a capital, a value of no bytes and a sound's tags twice.
    XSFLIB_CHECK(Patched(binary, 7, last_key + 3, '8') == "The file is damaged: tag zz_8 comes twice.");
    XSFLIB_CHECK(Patched(binary, 7, last_key + 3, 'Z') ==
                 "The file is damaged: tag zz_Z has a key other than 1 to 64 lower-case letters, digits and "
                 "underscores from a letter on.");
    XSFLIB_CHECK(Patched(binary, 7, last_value, 0) == "The file is damaged: tag zz_9 has no value.");
    XSFLIB_CHECK(Patched(binary, 7, second_sound, 0) == "The file is damaged: sound 0's tags come twice.");

    // Neither form writes a tag without a value, or with a key that a reader refuses. A file without tags has no TAGS
    // chunk.
    File valueless = file;
    valueless.sound_tags[0]["title"].clear();
    File capital = file;
    capital.tags.emplace("Title", "x");
    XSFLIB_CHECK(Message<std::invalid_argument>([&] { hokuto::format::Write(valueless); }) ==
                 "sound 0's tag title has no value");
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { hokuto::format::WriteText(valueless); }));
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { hokuto::format::Write(capital); }));
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { hokuto::format::WriteText(capital); }));
    File none = file;
    none.tags.clear();
    none.sound_tags = {{2, {}}};
    XSFLIB_CHECK(ChunkIds(hokuto::format::Write(none)).size() == kIds.size() - 1);

    // A tag that the program sets: a new value, a new key, and an empty value that removes the tag. A sound whose last
    // tag goes has no tags left.
    const File retitled = hokuto::format::WithTag(file, std::nullopt, "title", "B");
    const File dated = hokuto::format::WithTag(file, 3, "year", "1999");
    const File untitled = hokuto::format::WithTag(file, 7, "comment", "");
    const File missing = hokuto::format::WithTag(file, 5, "comment", "");
    XSFLIB_CHECK(retitled.tags.at("title") == "B" && retitled.tags.size() == 4 &&
                 retitled.sound_tags == file.sound_tags);
    XSFLIB_CHECK(dated.sound_tags.size() == 3 && (dated.sound_tags.at(3) == hokuto::format::Tags{{"year", "1999"}}));
    XSFLIB_CHECK(untitled.sound_tags.size() == 1 && untitled.sound_tags.contains(0) && untitled.tags == file.tags);
    XSFLIB_CHECK(missing.sound_tags == file.sound_tags);
}

// The sounds' lengths, in both forms. LNTH follows TAGS. Sound 70000 lies past the tables.
void TestLengths()
{
    File file = WithScenes();
    file.lengths = {
        {0, {.length = 151234, .fade = 10000, .loops = 2, .loop = hokuto::format::LoopTicks{96, 1632}}},
        {3, {.length = 4500}},
        {9, {.length = 600000, .fade = 10000}},
        {70000, {.length = 500, .silent = true}},
    };

    const std::vector<uint8_t> binary = hokuto::format::Write(file);
    const std::string text = hokuto::format::WriteText(file);

    XSFLIB_CHECK(hokuto::format::Read(binary) == file);
    XSFLIB_CHECK(hokuto::format::ReadText(text) == file);
    XSFLIB_CHECK(hokuto::format::WriteText(hokuto::format::ReadText(text)) == text);

    // The number of sounds, then each one: the sound in 4 bytes, the flags, the loops and two zeros, the length and the
    // fade in 4 bytes each, and the loop's ticks with flag 2. Flag 1 marks a silent sound.
    const std::vector<std::string> kIds = {"ENGN", "MIXR", "SEQS", "NATV", "TABL",
                                           "GRPF", "SCEN", "LNTH", "XTRA", "x y!"};
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

    // A line for each sound after the tags, with the sound's name where the tables give it.
    XSFLIB_CHECK(text.find("\nsound_length 0 151234 10000 2 96 1632 # Song\nsound_length 3 4500 0 0 # sound\n"
                           "sound_length 9 600000 10000 0\nsound_length 70000 500 0 0 silent\neffect A") !=
                 std::string::npos);

    // Lines that a reader refuses.
    const auto error = [](std::string_view lines)
    {
        return TextError("hok 1.0\nengine switch\n" + std::string(lines));
    };
    XSFLIB_CHECK(error("sound_length 1 2 3 0 silent\nsound_length 2 2 3 2 4 5\n").empty());
    XSFLIB_CHECK(error("sound_length 1 2 3\n").starts_with("Line 3: a sound's lengths are sound_length"));
    XSFLIB_CHECK(error("sound_length 1 2 3 2 4\n").starts_with("Line 3: a sound's lengths are sound_length"));
    XSFLIB_CHECK(
        error("sound_length 1 2 3 0 4 5\n").starts_with("Line 3: sound 1's lengths give a loop's ticks without loops"));
    XSFLIB_CHECK(error("sound_length 1 2 3 2 5 5\n")
                     .starts_with("Line 3: sound 1's lengths give a loop whose end at tick 5 doesn't come after its "
                                  "start at tick 5"));
    XSFLIB_CHECK(error("sound_length 1 2 3 256\n")
                     .starts_with("Line 3: a sound's number of loops has to be a whole number from 0 to 255, not 256"));
    XSFLIB_CHECK(
        error("sound_length 1 2 3 0\nsound_length 1 4 5 0\n").starts_with("Line 4: sound 1's lengths come twice"));
    XSFLIB_CHECK(error("sequence 0\n    end\nsound_length 0 1 0 0\n")
                     .starts_with("Line 5: the sounds' lengths come before the sequences"));

    // The binary form refuses a flag that this version doesn't define, a loop's ticks without loops, and a sound's
    // lengths twice.
    XSFLIB_CHECK(Patched(binary, 7, 8, 6) ==
                 "The file holds a sound's lengths with a flag that this version of Hokuto "
                 "doesn't know. A later version made it.");
    XSFLIB_CHECK(Patched(binary, 7, 9, 0) ==
                 "The file is damaged: sound 0's lengths give a loop's ticks without loops.");
    XSFLIB_CHECK(Patched(binary, 7, 28, 0) == "The file is damaged: sound 0's lengths come twice.");

    // Neither form writes a loop that ends before it starts. A file without lengths has no LNTH chunk.
    File backward = file;
    backward.lengths[0].loop = hokuto::format::LoopTicks{1632, 96};
    XSFLIB_CHECK(Message<std::invalid_argument>([&] { hokuto::format::Write(backward); }) ==
                 "sound 0's lengths give a loop whose end at tick 96 doesn't come after its start at tick 1632");
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { hokuto::format::WriteText(backward); }));
    XSFLIB_CHECK(ChunkIds(hokuto::format::Write(WithScenes())).size() == kIds.size() - 1);

    // An edit that can change what a sound plays drops the lengths: a new copy of a sequence or of the native archive,
    // a bus's volume, an effect or the file's engine. Hokuto measures the lengths in strict mixing. A tag, a scene, a
    // track's colour, a mixing mode or a sound's engine keeps them.
    const auto kept = [&file](const File& after)
    {
        return hokuto::format::WithoutStaleLengths(file, after).lengths == file.lengths;
    };

    const auto dropped = [&file](const File& after)
    {
        return hokuto::format::WithoutStaleLengths(file, after).lengths.empty();
    };

    File sequence = file;
    sequence.sequences[0] = hokuto::format::Shared<Sequence>(*file.sequences[0]);
    File native = file;
    native.native.Edit([](hokuto::format::NativeArchive&) {});
    File volume = file;
    volume.aux_volumes[1] = 0.5f;
    File effect = file;
    effect.aux_effects[1] = file.aux_effects[2];
    File engine = file;
    engine.engine = hokuto::format::Engine::kWiiU;
    File mixing = file;
    mixing.mixing = file.mixing == Mixing::kStrict ? Mixing::kEnhanced : Mixing::kStrict;
    File coloured = file;
    coloured.track_colours[{.sound = 0, .track = 1}] = {.red = 1};
    File scened = file;
    scened.scenes.pop_back();
    XSFLIB_CHECK(kept(hokuto::format::WithTag(file, 0, "title", "B")) && kept(coloured) && kept(scened));
    XSFLIB_CHECK(kept(mixing) && kept(hokuto::format::WithMixing(file, 0, Mixing::kStrict)));
    XSFLIB_CHECK(kept(hokuto::format::WithEngine(file, 0, hokuto::format::Engine::kWiiU)));
    XSFLIB_CHECK(dropped(sequence) && dropped(native) && dropped(volume) && dropped(effect) && dropped(engine));
}

// The sounds in another mixing mode than the file's, in both forms. ENGN lists them after the file's mode. Hokuto plays
// the Wii's sounds only in strict mixing.
void TestMixing()
{
    File file = WithScenes();
    file.sound_mixing = {{2, Mixing::kStrict}, {3, Mixing::kStrict}};

    const std::vector<uint8_t> binary = hokuto::format::Write(file);
    const std::string text = hokuto::format::WriteText(file);

    XSFLIB_CHECK(hokuto::format::Read(binary) == file);
    XSFLIB_CHECK(hokuto::format::ReadText(text) == file);
    XSFLIB_CHECK(hokuto::format::WriteText(hokuto::format::ReadText(text)) == text);

    // The engine, the file's mode and two zeros, then the number of sounds, and each sound in 4 bytes, its mode and
    // three zeros. A file whose sounds all take its mode keeps the chunk at 4 bytes.
    const std::span<const uint8_t> engine = ChunkBytes(binary, 0);
    const std::vector<uint8_t> kExpected = {1, 1, 0, 0, 2, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0};
    XSFLIB_CHECK(std::equal(engine.begin(), engine.end(), kExpected.begin(), kExpected.end()));
    XSFLIB_CHECK(ChunkBytes(hokuto::format::Write(WithScenes()), 0).size() == 4);

    // A line for each sound after the mixing line, with the sound's name where the tables give it.
    XSFLIB_CHECK(
        text.find("\nmixing enhanced\nsound_mixing 2 strict # \"two words\"\nsound_mixing 3 strict # sound\n") !=
        std::string::npos);

    // Lines that a reader refuses.
    const auto error = [](std::string_view lines)
    {
        return TextError("hok 1.0\nengine switch\n" + std::string(lines));
    };
    XSFLIB_CHECK(error("sound_mixing 1 enhanced\nsound_mixing 2 strict\n").empty());
    XSFLIB_CHECK(error("sound_mixing 1\n").starts_with("Line 3: a sound's mixing mode is sound_mixing"));
    XSFLIB_CHECK(error("sound_mixing 1 loud\n").starts_with("Line 3: Hokuto has no mixing mode named loud"));
    XSFLIB_CHECK(error("sound_mixing 1 enhanced\nsound_mixing 1 strict\n")
                     .starts_with("Line 4: sound 1's mixing mode comes twice"));
    XSFLIB_CHECK(error("sequence 0\n    end\nsound_mixing 0 enhanced\n")
                     .starts_with("Line 5: a sound's mixing mode is sound_mixing"));

    // The binary form refuses a mode that this version doesn't define, and a sound twice.
    XSFLIB_CHECK(Patched(binary, 0, 12, 7) ==
                 "The file holds a mixing mode (7) that this version of Hokuto doesn't know. A later version made it.");
    XSFLIB_CHECK(Patched(binary, 0, 16, 2) == "The file is damaged: sound 2's mixing mode comes twice.");

    // A sound's mode is its mode in the list, or else the file's. A sound that takes the file's mode leaves the list,
    // and a mode for every sound empties it.
    XSFLIB_CHECK(hokuto::format::MixingOf(file, 2) == Mixing::kStrict &&
                 hokuto::format::MixingOf(file, 0) == Mixing::kEnhanced);
    XSFLIB_CHECK((hokuto::format::WithMixing(file, 2, Mixing::kEnhanced).sound_mixing ==
                  std::map<uint32_t, Mixing>{{3, Mixing::kStrict}}));
    XSFLIB_CHECK(hokuto::format::WithMixing(file, 70000, Mixing::kStrict).sound_mixing.size() == 3);
    const File strict = hokuto::format::WithMixing(file, Mixing::kStrict);
    XSFLIB_CHECK(strict.mixing == Mixing::kStrict && strict.sound_mixing.empty());

    // MixingRefused refuses enhanced mixing on the Wii, and no mode on the Switch, the Wii U and the 3DS.
    XSFLIB_CHECK(MixingRefused(Engine::kWii, Mixing::kEnhanced) ==
                 "AX's microcode mixes the Wii's sounds. Hokuto has no enhanced mixing for the Wii yet.");
    XSFLIB_CHECK(!MixingRefused(Engine::kWii, Mixing::kStrict));
    for (const Engine engine : {Engine::kSwitch, Engine::kWiiU, Engine::k3ds})
    {
        XSFLIB_CHECK(!MixingRefused(engine, Mixing::kStrict) && !MixingRefused(engine, Mixing::kEnhanced));
    }
}

// The sounds on another engine than the file's, in both forms. ENGN lists them after the sounds in another mixing mode,
// and gives the number of those even when it's 0.
void TestSoundEngines()
{
    File file = WithScenes();
    file.sound_engines = {{2, Engine::kWiiU}, {3, Engine::k3ds}};

    const std::vector<uint8_t> binary = hokuto::format::Write(file);
    const std::string text = hokuto::format::WriteText(file);

    XSFLIB_CHECK(hokuto::format::Read(binary) == file);
    XSFLIB_CHECK(hokuto::format::ReadText(text) == file);
    XSFLIB_CHECK(hokuto::format::WriteText(hokuto::format::ReadText(text)) == text);

    // The engine, the file's mode and two zeros, no sounds in another mode, then the number of sounds on another
    // engine, and each sound in 4 bytes, its engine and three zeros.
    const std::span<const uint8_t> engine = ChunkBytes(binary, 0);
    const std::vector<uint8_t> kExpected = {1, 1, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 2, 0,
                                            0, 0, 2, 0, 0, 0, 3, 0, 0, 0, 3, 0, 0, 0};
    XSFLIB_CHECK(std::equal(engine.begin(), engine.end(), kExpected.begin(), kExpected.end()));

    // A line for each sound after the lines of the mixing modes, with the sound's name where the tables give it.
    File both = file;
    both.sound_mixing = {{1, Mixing::kStrict}};
    const std::string both_text = hokuto::format::WriteText(both);
    XSFLIB_CHECK(text.find("\nmixing enhanced\nsound_engine 2 wii_u # \"two words\"\nsound_engine 3 3ds # sound\n") !=
                 std::string::npos);
    const std::size_t mixing_line = both_text.find("\nsound_mixing 1 strict");
    const std::size_t engine_line = both_text.find("\nsound_engine 2 wii_u");
    XSFLIB_CHECK(mixing_line != std::string::npos && engine_line != std::string::npos && mixing_line < engine_line);
    XSFLIB_CHECK(hokuto::format::Read(hokuto::format::Write(both)) == both);
    XSFLIB_CHECK(hokuto::format::ReadText(both_text) == both);

    // Lines that a reader refuses.
    const auto error = [](std::string_view lines)
    {
        return TextError("hok 1.0\nengine switch\n" + std::string(lines));
    };
    XSFLIB_CHECK(error("sound_engine 1 wii_u\nsound_engine 2 wii\n").empty());
    XSFLIB_CHECK(error("sound_engine 1\n").starts_with("Line 3: a sound's engine is sound_engine"));
    XSFLIB_CHECK(error("sound_engine 1 gba\n").starts_with("Line 3: Hokuto has no engine named gba"));
    XSFLIB_CHECK(
        error("sound_engine 1 3ds\nsound_engine 1 wii_u\n").starts_with("Line 4: sound 1's engine comes twice"));
    XSFLIB_CHECK(
        error("sequence 0\n    end\nsound_engine 0 3ds\n").starts_with("Line 5: a sound's engine is sound_engine"));

    // The binary form refuses an engine that this version doesn't define, and a sound twice.
    XSFLIB_CHECK(Patched(binary, 0, 16, 9) ==
                 "The file holds an engine (9) that this version of Hokuto doesn't know. A later version made it.");
    XSFLIB_CHECK(Patched(binary, 0, 20, 2) == "The file is damaged: sound 2's engine comes twice.");

    // A sound's engine is its engine in the list, or else the file's. A sound on the file's engine leaves the list.
    XSFLIB_CHECK(hokuto::format::EngineOf(file, 2) == Engine::kWiiU &&
                 hokuto::format::EngineOf(file, 0) == Engine::kSwitch);
    XSFLIB_CHECK((hokuto::format::WithEngine(file, 2, Engine::kSwitch).sound_engines ==
                  std::map<uint32_t, Engine>{{3, Engine::k3ds}}));
    XSFLIB_CHECK(hokuto::format::WithEngine(file, 70000, Engine::kWii).sound_engines.size() == 3);
}

// The timelines, in both forms. TMLN follows TAGS.
void TestTimelines()
{
    File file = WithScenes();
    file.timelines.emplace();
    (*file.timelines)[0] = {{{192, 12}, {144, 1}, {192, 1}},
                            {TrackHints{0, {8, 8}, {1, 4}, {}}, TrackHints{2, {}, {}, {7}}}};
    (*file.timelines)[1] = {{{96, 1}}, {}};
    File none = WithScenes();
    none.timelines.emplace();

    const std::vector<uint8_t> binary = hokuto::format::Write(file);
    const std::string text = hokuto::format::WriteText(file);

    XSFLIB_CHECK(hokuto::format::Read(binary) == file);
    XSFLIB_CHECK(hokuto::format::ReadText(text) == file);
    XSFLIB_CHECK(hokuto::format::WriteText(hokuto::format::ReadText(text)) == text);
    XSFLIB_CHECK(hokuto::format::Read(hokuto::format::Write(none)) == none);
    XSFLIB_CHECK(hokuto::format::ReadText(hokuto::format::WriteText(none)) == none);
    XSFLIB_CHECK(!hokuto::format::Read(hokuto::format::Write(WithScenes())).timelines);
    XSFLIB_CHECK(!hokuto::format::ReadText(hokuto::format::WriteText(WithScenes())).timelines);

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
    XSFLIB_CHECK(ChunkBytes(hokuto::format::Write(none), 7).size() == 4);

    // A line of each timeline's bars and of each track's hints, after the scenes. A file without timelines that says so
    // has a line of none.
    XSFLIB_CHECK(text.find("\n\ntimeline 0 bars 192 12 144 1 192 1\ntimeline 0 track 0 opening 8 8 kept 1 4\n"
                           "timeline 0 track 2 called 7\ntimeline 1 bars 96 1\n\nsequence 0\n") != std::string::npos);
    XSFLIB_CHECK(hokuto::format::WriteText(none).find("\n\ntimeline none\n\nsequence 0\n") != std::string::npos);

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
                     .starts_with("Line 4: a track's hints give kept once"));
    XSFLIB_CHECK(error("timeline 5 bars 192 1\n").starts_with("Line 3: the file has no sequence 5"));
    XSFLIB_CHECK(error("timeline 0 bars 192 1\ntimeline 0 track 0 kept 2 1\n")
                     .starts_with("Line 3: the timelines give the bars of track 0 of sequence 0 out of order"));
    XSFLIB_CHECK(error("timeline 0 bars 0 1\n")
                     .starts_with("Line 3: the timelines hold a run of bars of sequence 0 with no ticks or no bars"));
    XSFLIB_CHECK(TextError("hok 1.0\nengine switch\nsequence 0\n    end\ntimeline 0 bars 192 1\n")
                     .starts_with("Line 5: the timelines come before the sequences"));

    // The binary form refuses a run of no ticks, and a timeline of a sequence that the file lacks.
    XSFLIB_CHECK(Patched(binary, 7, 12, 0) ==
                 "The file is damaged: the timelines hold a run of bars of sequence 0 with no ticks or no bars.");
    XSFLIB_CHECK(Patched(binary, 7, 4, 9) == "The file is damaged: the timelines name sequence 9, and the file has 2.");

    // Neither form writes a timeline of a sequence that the file lacks.
    File past = file;
    (*past.timelines)[2] = {};
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { hokuto::format::Write(past); }));
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { hokuto::format::WriteText(past); }));
}

// The group files that a game keeps beside an archive. An import cuts them: the copy of the archive's sequence file
// becomes a piece of its sequence, and the other file a run. An edit of the sequence reaches the copy, and the group
// file's tables follow. Both forms keep the pieces, the group, and the game's size and SHA-1. An export writes a group
// file that changed, into a SARC archive that holds a file of its group if there is one. A GRPF chunk in the form of an
// earlier Hokuto gives each group file as one run of bytes.
void TestGroupFiles()
{
    const std::vector<uint8_t> song = LongerSong(0);
    sar_files::ArchiveSpec spec;
    spec.sounds = {{.name = "Song", .file = 0}};
    spec.players = {{"PLAYER", 1}};
    spec.groups = {{"GROUP_SONG", ntdlib::sar::kExternalGroupFile}};
    spec.files = {{.bytes = song, .groups = {0}}, {.group_only = true, .groups = {0}}};
    const std::vector<uint8_t> archive = sar_tools::Archive(Flavor::kNx, spec);
    const std::vector<uint8_t> group = sar_files::Group(Flavor::kNx, {{0, song}, {1, {1, 2, 3, 4, 5}}});

    const File file = hokuto::format::ImportSwitchArchive(archive, {group});
    XSFLIB_CHECK(file.sequences.size() == 1 && file.native->group_files.size() == 1);
    if (file.sequences.size() != 1 || file.native->group_files.size() != 1)
    {
        return;
    }
    const NativeGroupFile& imported = file.native->group_files[0];
    const auto is_sequence = [](const ntdlib::sar::Piece& piece)
    {
        return std::holds_alternative<ntdlib::sar::SequencePiece>(piece);
    };
    const auto sequence_pieces = std::ranges::count_if(*imported.pieces, is_sequence);
    XSFLIB_CHECK(imported.group == 0u && imported.game_size == group.size() && sequence_pieces == 1);
    XSFLIB_CHECK(imported.game_hash == xsflib::Sha1::Of(group) && !imported.fields.empty());
    XSFLIB_CHECK(hokuto::format::ExportGroupFiles(file) == std::vector<std::vector<uint8_t>>{group});
    XSFLIB_CHECK(hokuto::format::ChangedGroupFiles(file).empty());
    XSFLIB_CHECK(hokuto::format::Read(hokuto::format::Write(file)) == file);
    XSFLIB_CHECK(hokuto::format::ReadText(hokuto::format::WriteText(file)) == file);

    // 20 more volumes at the sequence's start. The group file's copy takes the archive copy's new bytes.
    File longer = file;
    const auto lengthen = [](Sequence& sequence)
    {
        sequence.items.insert(sequence.items.begin(), 20, sequence.items[0]);
    };
    longer.sequences[0].Edit(lengthen);
    const std::vector<std::vector<uint8_t>> groups = hokuto::format::ExportGroupFiles(longer);
    const std::vector<hokuto::format::ChangedGroupFile> changed = hokuto::format::ChangedGroupFiles(longer);
    ntdlib::sar::Options options;
    options.flavor = Flavor::kNx;
    const ntdlib::sar::SoundArchive read =
        ntdlib::sar::SoundArchive::Load(hokuto::format::ExportArchive(longer), options);
    XSFLIB_CHECK(groups.size() == 1 && changed.size() == 1);
    if (groups.size() == 1 && changed.size() == 1)
    {
        const ntdlib::sar::GroupContents contents = ntdlib::sar::ReadGroupFile(groups[0], Flavor::kNx);
        const std::span<const uint8_t> copy = read.FileData(0);
        XSFLIB_CHECK(contents.files.size() == 2 && copy.size() > song.size());
        if (contents.files.size() == 2)
        {
            XSFLIB_CHECK(contents.files[0].copy && std::ranges::equal(*contents.files[0].copy, copy));
            XSFLIB_CHECK(contents.files[1].copy == std::vector<uint8_t>({1, 2, 3, 4, 5}));
        }
        XSFLIB_CHECK(changed[0].index == 0 && changed[0].name == "GROUP_SONG" && changed[0].bytes == groups[0]);

        // An export writes the group file under its group's name, or into the first SARC archive that holds a file of
        // its group, in that file's place. The new group file outgrows its place there, and goes after the next file.
        // The SARC archive goes out under its name. A SARC archive that holds an earlier export's file of the group
        // takes the group file too. A group file whose group Hokuto couldn't find stays out.
        const std::vector<uint8_t> exported = hokuto::format::ExportArchive(longer);
        const std::vector<FileBeside> alone = hokuto::format::GroupFileOutputs(changed, exported, Flavor::kNx, {});
        XSFLIB_CHECK(alone.size() == 1 && alone[0].first == "GROUP_SONG.bfgrp" && alone[0].second == groups[0]);
        const std::vector<FileBeside> sarcs = {
            {"Other.sarc", sar_files::Sarc(false, {{"Other.bfgrp", {1, 2, 3}}})},
            {"Song.sarc", sar_files::Sarc(false, {{"Song.bfgrp", group}, {"After.bin", {4, 5, 6}}})},
            {"Again.sarc", sar_files::Sarc(false, {{"Song.bfgrp", group}})}};
        const std::vector<FileBeside> into = hokuto::format::GroupFileOutputs(changed, exported, Flavor::kNx, sarcs);
        XSFLIB_CHECK(into.size() == 1 && into[0].first == "Song.sarc");
        if (into.size() == 1)
        {
            const std::vector<ntdlib::sar::SarcFile> files = ntdlib::sar::SarcFiles(into[0].second);
            XSFLIB_CHECK(files.size() == 2 && files[0].name == "Song.bfgrp" &&
                         std::ranges::equal(files[0].data, groups[0]) && files[1].name == "After.bin" &&
                         std::ranges::equal(files[1].data, std::vector<uint8_t>{4, 5, 6}));
        }
        const std::vector<FileBeside> earlier = {{"Song.sarc", sar_files::Sarc(false, {{"Song.bfgrp", groups[0]}})}};
        const std::vector<FileBeside> again = hokuto::format::GroupFileOutputs(changed, exported, Flavor::kNx, earlier);
        XSFLIB_CHECK(again.size() == 1 && again[0].first == "Song.sarc" &&
                     ntdlib::sar::SarcFiles(again[0].second).size() == 1 &&
                     std::ranges::equal(ntdlib::sar::SarcFiles(again[0].second)[0].data, groups[0]));
        std::vector<hokuto::format::ChangedGroupFile> unknown = changed;
        unknown[0].group.reset();
        XSFLIB_CHECK(hokuto::format::GroupFileOutputs(unknown, exported, Flavor::kNx, sarcs).empty());
    }

    // The earlier form: the count, then each group file's size in 8 bytes and its bytes.
    std::vector<uint8_t> earlier = {2, 0, 0, 0};
    for (const std::vector<uint8_t>& bytes : {group, std::vector<uint8_t>()})
    {
        for (int i = 0; i < 8; ++i)
        {
            earlier.push_back(static_cast<uint8_t>(static_cast<uint64_t>(bytes.size()) >> (8 * i)));
        }
        earlier.insert(earlier.end(), bytes.begin(), bytes.end());
    }

    std::vector<Sequence> sequences;
    const std::vector<NativeGroupFile> whole = hokuto::format::ReadGroupFiles(earlier, sequences);
    XSFLIB_CHECK(whole.size() == 2);
    if (whole.size() == 2)
    {
        XSFLIB_CHECK(!whole[0].group && whole[0].game_size == group.size() && whole[0].fields.empty());
        XSFLIB_CHECK((*whole[0].pieces == std::vector<ntdlib::sar::Piece>{group}));
        XSFLIB_CHECK(whole[0].game_hash == xsflib::Sha1::Of(group) && whole[1].pieces->empty());
    }

    // A field of a group file that lies in a sequence piece.
    std::vector<uint8_t> damaged = kGroupFilesChunk;
    damaged[84] = 1;
    std::vector<Sequence> two(2);
    XSFLIB_CHECK(Message<xsflib::FormatError>([&] { hokuto::format::ReadGroupFiles(damaged, two); }) ==
                 "The file is damaged: field 0 of group file 0 lies outside a run of bytes.");
}

// A group that a 3DS game keeps outside its archive. The 3DS's file table holds no lists of groups. Only the label of
// the group's file gives the group file its group. The program finds the file under the group's name. An edit of the
// sequence reaches the group file's copy, and an export writes the new group file under the group's name and .bcgrp.
// Without the label, the group file has no group, and the export leaves it out. A file in the model form keeps the
// group through both forms.
void TestGroupFilesOfThe3ds()
{
    const std::vector<uint8_t> song = sar_files::Sequence(Flavor::kCtr, kSong, {{"Song", 0}});
    sar_files::ArchiveSpec spec;
    spec.sounds = {{.name = "Song", .file = 0}};
    spec.players = {{"PLAYER", 1}};
    spec.groups = {{"GROUP_SONG", ntdlib::sar::kExternalGroupFile}};
    spec.files = {{.bytes = song}};
    const std::vector<uint8_t> archive = sar_tools::Archive(Flavor::kCtr, spec);
    const std::vector<uint8_t> group = sar_files::Group(Flavor::kCtr, {{0, song}});
    const std::vector<std::string> kLabels = {"GROUP_SONG"};

    const auto longer = [](File file)
    {
        const auto lengthen = [](Sequence& sequence)
        {
            sequence.items.insert(sequence.items.begin(), 20, sequence.items[0]);
        };
        file.sequences[0].Edit(lengthen);

        return file;
    };

    const auto outputs = [](const File& file)
    {
        return hokuto::format::GroupFileOutputs(hokuto::format::ChangedGroupFiles(file),
                                                hokuto::format::ExportArchive(file), Flavor::kCtr, {});
    };

    const File labelled = hokuto::format::ImportArchive(archive, Flavor::kCtr, {group}, {}, kLabels);
    const File unlabelled = hokuto::format::ImportArchive(archive, Flavor::kCtr, {group});
    const File model = hokuto::format::InModelForm(labelled);

    XSFLIB_CHECK(labelled.native->group_files.size() == 1 && unlabelled.native->group_files.size() == 1);
    if (labelled.native->group_files.size() != 1 || unlabelled.native->group_files.size() != 1)
    {
        return;
    }
    XSFLIB_CHECK(labelled.native->group_files[0].group == 0u && !unlabelled.native->group_files[0].group);
    XSFLIB_CHECK(hokuto::format::ExportGroupFiles(labelled) == std::vector<std::vector<uint8_t>>{group});
    const std::vector<hokuto::format::FileBeside> written = outputs(longer(labelled));
    XSFLIB_CHECK(written.size() == 1 && written[0].first == "GROUP_SONG.bcgrp" &&
                 written[0].second == hokuto::format::ExportGroupFiles(longer(labelled)).at(0) &&
                 written[0].second != group);
    XSFLIB_CHECK(outputs(longer(unlabelled)).empty());
    XSFLIB_CHECK(hokuto::format::ModelOf(model).group_file_groups == std::vector<std::optional<uint32_t>>{0u});
    const File binary = hokuto::format::Read(hokuto::format::Write(model));
    const File text = hokuto::format::ReadText(hokuto::format::WriteText(model));
    XSFLIB_CHECK(binary.native->group_files.at(0).group == 0u && text.native->group_files.at(0).group == 0u);
    const std::vector<hokuto::format::FileBeside> from_model = outputs(longer(binary));
    XSFLIB_CHECK(from_model.size() == 1 && from_model[0].first == "GROUP_SONG.bcgrp");
}

// A sequence file that only the group files beside a 3DS archive hold, as in a game that keeps its music in its group
// files. The import makes it one of the file's sequences, from the first group file's copy. The archive's fields of the
// sounds' starts name items of the sequence, and both forms keep them. SequencesOfFiles finds the file's sequence in
// the group files. Ten volumes before a start move it by 20 bytes, and both group files' copies take the new bytes. On
// the Switch, a file in the model form keeps such a sequence. When the sequence changes outside the program, as in the
// text form, the group file's copy takes the new bytes too.
void TestGroupSequences()
{
    const std::vector<uint8_t> song = sar_files::Sequence(Flavor::kCtr, kSong, {{"Song", 0}});
    sar_files::ArchiveSpec spec;
    spec.sounds = {{.name = "Song", .file = 0}, {.name = "Entry", .file = 0, .start_offset = 5}};
    spec.players = {{"PLAYER", 1}};
    spec.groups = {{"GROUP_A", ntdlib::sar::kExternalGroupFile}, {"GROUP_B", ntdlib::sar::kExternalGroupFile}};
    spec.files = {{.group_only = true}};
    const std::vector<uint8_t> archive = sar_tools::Archive(Flavor::kCtr, spec);
    const std::vector<std::vector<uint8_t>> groups(2, sar_tools::Group(Flavor::kCtr, {.files = {{0, song}}}));
    const std::vector<std::string> kLabels = {"GROUP_A", "GROUP_B"};
    const File file = hokuto::format::ImportArchive(archive, Flavor::kCtr, groups, {}, kLabels);
    XSFLIB_CHECK(file.sequences.size() == 1 && file.native->group_files.size() == 2);
    if (file.sequences.size() != 1 || file.native->group_files.size() != 2)
    {
        return;
    }

    const auto is_sequence = [](const ntdlib::sar::Piece& piece)
    {
        return std::holds_alternative<ntdlib::sar::SequencePiece>(piece);
    };
    XSFLIB_CHECK(std::ranges::none_of(*file.native->pieces, is_sequence));
    for (const NativeGroupFile& group_file : file.native->group_files)
    {
        XSFLIB_CHECK(std::ranges::count_if(*group_file.pieces, is_sequence) == 1);
    }

    // The fields of Song's start and Entry's.
    std::vector<ntdlib::sar::Field> starts;
    std::ranges::copy_if(file.native->fields, std::back_inserter(starts),
                         [](const ntdlib::sar::Field& field) { return field.from.sequence || field.to.sequence; });
    const ntdlib::sar::Place kFirst{0, 0, true, true};
    XSFLIB_CHECK(starts.size() == 2);
    XSFLIB_CHECK((starts.size() == 2 && starts[0].from == kFirst && starts[0].to == kFirst &&
                  starts[1].from == kFirst && starts[1].to == ntdlib::sar::Place{0, 3, true, true}));
    XSFLIB_CHECK(hokuto::format::ExportArchive(file) == archive && hokuto::format::ExportGroupFiles(file) == groups);
    const std::string text = hokuto::format::WriteText(file);
    XSFLIB_CHECK(text.find(" from sequence 0 item 0 to sequence 0 item ") != std::string::npos);
    XSFLIB_CHECK(hokuto::format::Read(hokuto::format::Write(file)) == file);
    XSFLIB_CHECK(hokuto::format::ReadText(text) == file);

    ntdlib::sar::Options options;
    options.flavor = Flavor::kCtr;
    const ntdlib::sar::SoundArchive read = ntdlib::sar::SoundArchive::Load(archive, options);
    XSFLIB_CHECK(hokuto::format::SequencesOfFiles(file, archive, read) == std::vector<std::optional<uint32_t>>{0u});

    // Ten volumes after the first.
    File longer = file;
    const auto lengthen = [](Sequence& sequence)
    {
        sequence.items.insert(sequence.items.begin() + 1, 10, sequence.items[0]);
    };
    longer.sequences[0].Edit(lengthen);
    longer.native.Edit(
        [](NativeArchive& native)
        {
            for (ntdlib::sar::Field& field : native.fields)
            {
                field.to.offset += field.to.sequence && field.to.offset > 0 ? 10 : 0;
            }
        });

    std::vector<uint8_t> longer_song;
    for (int i = 0; i < 11; ++i)
    {
        longer_song.insert(longer_song.end(), kSong.begin(), kSong.begin() + 2);
    }
    longer_song.insert(longer_song.end(), kSong.begin() + 2, kSong.end());

    const std::vector<uint8_t> moved = hokuto::format::ExportArchive(longer);
    const ntdlib::sar::SoundArchive moved_read = ntdlib::sar::SoundArchive::Load(moved, options);
    const std::vector<uint8_t> longer_group =
        sar_tools::Group(Flavor::kCtr, {.files = {{0, sar_files::Sequence(Flavor::kCtr, longer_song, {{"Song", 0}})}}});
    XSFLIB_CHECK(moved.size() == archive.size() && moved_read.Sounds().size() == 2);
    XSFLIB_CHECK(moved_read.Sounds().size() == 2 && moved_read.Sounds()[0].start_offset == 0 &&
                 moved_read.Sounds()[1].start_offset == 25);
    XSFLIB_CHECK(hokuto::format::ExportGroupFiles(longer) == std::vector<std::vector<uint8_t>>(2, longer_group));
    XSFLIB_CHECK(hokuto::format::ChangedGroupFiles(longer).size() == 2);

    // The model form, on the Switch. A volume before the last end.
    sar_files::ArchiveSpec switch_spec = spec;
    switch_spec.groups.pop_back();
    switch_spec.files = {{.group_only = true, .groups = {0}}};
    const std::vector<uint8_t> switch_archive = sar_tools::Archive(Flavor::kNx, switch_spec);
    const std::vector<uint8_t> switch_group =
        sar_tools::Group(Flavor::kNx, {.files = {{0, sar_files::Sequence(Flavor::kNx, kSong, {{"Song", 0}})}}});
    const File model =
        hokuto::format::InModelForm(hokuto::format::ImportArchive(switch_archive, Flavor::kNx, {switch_group}));
    XSFLIB_CHECK(model.sequences.size() == 1 && hokuto::format::ExportArchive(model) == switch_archive);
    XSFLIB_CHECK(hokuto::format::ExportGroupFiles(model) == std::vector<std::vector<uint8_t>>{switch_group});
    XSFLIB_CHECK(hokuto::format::ReadText(hokuto::format::WriteText(model)) == model);
    File edited = model;
    const auto add_command = [](Sequence& sequence)
    {
        sequence.items.insert(sequence.items.end() - 1, sequence.items[0]);
    };
    edited.sequences[0].Edit(add_command);
    std::vector<uint8_t> edited_song(kSong.begin(), kSong.end() - 1);
    edited_song.insert(edited_song.end(), {0xC1, 0x64, 0xFF});
    const File reread = hokuto::format::WithModel(edited, hokuto::format::ModelOf(model));
    XSFLIB_CHECK(hokuto::format::ExportArchive(reread) == switch_archive);
    XSFLIB_CHECK(hokuto::format::ExportGroupFiles(reread) ==
                 std::vector<std::vector<uint8_t>>{sar_tools::Group(
                     Flavor::kNx, {.files = {{0, sar_files::Sequence(Flavor::kNx, edited_song, {{"Song", 0}})}}})});
}

// The text form of a file of the Switch whose native archive holds 3 bytes, without chunks. The tests of the chunks put
// chunks after it.
const std::string kNativeOnly = "hok 1.1\nengine switch\nnative switch\n    bytes 3\n        AQID\n";

// The streams beside the archive: one from the game, and an empty one that the game lacks. Both forms keep them in the
// STRM chunk, after GRPF.
void TestStreams()
{
    File file = Unusual();
    file.native.Edit(
        [](NativeArchive& native)
        {
            native.streams = {{2, NativeStream{std::vector<uint8_t>{0x46, 0x53, 0x54, 0x4D}, true}},
                              {7, NativeStream{std::vector<uint8_t>(), false}}};
        });

    const std::vector<uint8_t> binary = hokuto::format::Write(file);
    const std::string text = hokuto::format::WriteText(file);
    XSFLIB_CHECK(hokuto::format::Read(binary) == file && hokuto::format::ReadText(text) == file);
    XSFLIB_CHECK(hokuto::format::WriteText(hokuto::format::ReadText(text)) == text);

    // The number of streams, then each one's file ID, its flags, its size in 8 bytes and its bytes. Flag 1 marks a
    // stream that the game lacks.
    const std::vector<uint8_t> kExpected = {2,    0,    0,    0,    2, 0, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0,
                                            0x46, 0x53, 0x54, 0x4D, 7, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    const std::vector<std::string> kIds = {"ENGN", "MIXR", "SEQS", "NATV", "TABL", "GRPF", "STRM", "XTRA", "x y!"};
    XSFLIB_CHECK(ChunkIds(binary) == kIds);
    XSFLIB_CHECK(std::ranges::equal(ChunkBytes(binary, 6), kExpected));
    XSFLIB_CHECK(text.find(std::string("\nchunk GRPF 160\n") + kGroupFilesText +
                           "\nchunk STRM 40\n"
                           "    AgAAAAIAAAAAAAAABAAAAAAAAABGU1RNBwAAAAEAAAAAAAAAAAAAAA==\n\nchunk XTRA") !=
                 std::string::npos);

    // The binary form refuses file IDs that don't rise, a flag of a later version, a stream that runs past the chunk's
    // end, and streams without a native archive. The second stream's ID lies at byte 24 of STRM, and its flags at 28.
    XSFLIB_CHECK(Patched(binary, 6, 24, 2) == "The file is damaged: the file IDs of its streams don't rise.");
    XSFLIB_CHECK(Patched(binary, 6, 28, 3) ==
                 "The file holds a stream with a flag that this version of Hokuto doesn't "
                 "know. A later version made it.");
    XSFLIB_CHECK(Patched(binary, 6, 12, 5) == "The file is damaged: the STRM chunk ends early.");
    File loose;
    loose.native = NativeArchive{.pieces = std::vector<ntdlib::sar::Piece>{std::vector<uint8_t>{1, 2, 3}},
                                 .streams = {{0, NativeStream{std::vector<uint8_t>{4}, true}}}};
    std::vector<uint8_t> unnative = hokuto::format::Write(loose);
    XSFLIB_CHECK(ChunkIds(unnative) == std::vector<std::string>({"ENGN", "SEQS", "NATV", "STRM"}));
    std::ranges::copy(std::string_view("XTRA"), unnative.begin() + 24 + 24 * 2);
    XSFLIB_CHECK(Message<xsflib::FormatError>([&] { hokuto::format::Read(unnative); }) ==
                 "The file is damaged: it holds streams without a native archive.");
    XSFLIB_CHECK(hokuto::format::ReadText(hokuto::format::WriteText(loose)) == loose);

    // The text form refuses streams before the native archive, a second STRM chunk, group files after the streams, and
    // a chunk that ends early. Each message names the chunk's line.
    const std::string kEmptyChunk = "chunk STRM 4\n    AAAAAA==\n";
    XSFLIB_CHECK(TextError("hok 1.1\nengine switch\n" + kEmptyChunk)
                     .starts_with("Line 3: the STRM chunk comes once, after the native archive"));
    XSFLIB_CHECK(TextError(kNativeOnly + kEmptyChunk + kEmptyChunk).starts_with("Line 8: the STRM chunk comes once"));
    XSFLIB_CHECK(
        TextError(kNativeOnly + kEmptyChunk + "chunk GRPF 4\n    AAAAAA==\n")
            .starts_with("Line 8: the GRPF chunk comes once, after the native archive and before its streams"));
    XSFLIB_CHECK(TextError(kNativeOnly + "chunk STRM 4\n    AQAAAA==\n") ==
                 "Line 6: The file is damaged: the STRM chunk ends early.");
    XSFLIB_CHECK(TextError(kNativeOnly + kEmptyChunk).empty());
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

// The models of edited banks and waves in both forms. BANK and WAVE follow GRPF. The binary form refuses banks and
// waves out of order, parts of a later version, more than the chunk holds, a bank that can't be laid out and a loop
// past the samples. A file without an archive can't hold them, and a wave at a rate of 0 doesn't save. The text form
// takes each chunk once, after the archive.
void TestBanksAndWaves()
{
    // A wave that keeps its wave archive's samples, and one with PCM16 samples in two channels.
    const WaveModel kLooped{ntdlib::sar::WaveEncoding::kDspAdpcm, 32000, true, 14, 28, 42, {}};
    const WaveModel kPcm{ntdlib::sar::WaveEncoding::kPcm16, 48000, false, 0, 2, 3, {{1, -2, 3}, {-4, 5, -6}}};
    File file = Unusual();
    file.banks = {{9, SmallBank()}, {12, WideBank()}};
    file.waves = {{WavePlace{4, 1}, kLooped}, {WavePlace{4, 2}, kPcm}};

    const std::vector<uint8_t> binary = hokuto::format::Write(file);
    const std::string text = hokuto::format::WriteText(file);

    XSFLIB_CHECK(hokuto::format::Read(binary) == file && hokuto::format::ReadText(text) == file);
    XSFLIB_CHECK(hokuto::format::WriteText(hokuto::format::ReadText(text)) == text);
    const std::vector<std::string> kIds = {"ENGN", "MIXR", "SEQS", "NATV", "TABL",
                                           "GRPF", "BANK", "WAVE", "XTRA", "x y!"};
    XSFLIB_CHECK(ChunkIds(binary) == kIds);
    XSFLIB_CHECK(text.find("\nchunk BANK ") != std::string::npos &&
                 text.find("\nchunk WAVE 76\n") != std::string::npos);

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
    XSFLIB_CHECK(std::ranges::equal(ChunkBytes(hokuto::format::Write(small), 6), kBankBytes));

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

    // The binary form refuses banks whose IDs don't rise, a console, a flag of a later version, a bank that the tools
    // can't lay out and more programs than the chunk holds. The second bank's ID lies at byte 84 of BANK.
    XSFLIB_CHECK(Patched(binary, 6, 84, 9) == "The file is damaged: the file IDs of its banks don't rise.");
    XSFLIB_CHECK(Patched(binary, 6, 8, 9).starts_with("The file holds a bank of a console that this version"));
    XSFLIB_CHECK(Patched(binary, 6, 40, 3).starts_with("The file holds an instrument with a flag that this version"));
    XSFLIB_CHECK(Patched(binary, 6, 67, 2).starts_with("The file holds a region with a flag that this version"));
    XSFLIB_CHECK(Patched(binary, 6, 56, 0x80).starts_with("The file is damaged: bank 9 can't be laid out: "));
    XSFLIB_CHECK(Patched(binary, 6, 31, 0x40) == "The file is damaged: bank 9 holds more programs than its chunk.");

    // It refuses waves whose places don't rise, an encoding or a flag of a later version, more samples than the chunk
    // holds, and a loop past the samples. The second wave starts at byte 32 of WAVE.
    XSFLIB_CHECK(Patched(binary, 7, 36, 1) == "The file is damaged: the places of its waves don't rise.");
    XSFLIB_CHECK(Patched(binary, 7, 12, 3).starts_with("The file holds a wave of an encoding that this version"));
    XSFLIB_CHECK(Patched(binary, 7, 13, 4).starts_with("The file holds a wave with a flag that this version"));
    XSFLIB_CHECK(Patched(binary, 7, 56, 9) ==
                 "The file is damaged: wave 2 of file 4 holds more samples than its chunk.");
    XSFLIB_CHECK(Patched(binary, 7, 52, 4) ==
                 "The file is damaged: wave 2 of file 4 ends its loop after its last sample.");
    XSFLIB_CHECK(Patched(binary, 7, 20, 30).starts_with("The file is damaged: wave 1 of file 4 has a loop that"));

    // Banks and waves name an archive's files. A file without an archive can't hold them, and a model that the chunk
    // can't hold doesn't save.
    File loose;
    loose.banks = {{9, SmallBank()}};
    XSFLIB_CHECK(Message<xsflib::FormatError>([&] { hokuto::format::Read(hokuto::format::Write(loose)); }) ==
                 "The file is damaged: it holds banks or waves without an archive.");
    File silent = Unusual();
    silent.waves = {{WavePlace{4, 1}, WaveModel{ntdlib::sar::WaveEncoding::kPcm16, 0, false, 0, 3, 3, {}}}};
    XSFLIB_CHECK(Message<std::invalid_argument>([&] { hokuto::format::Write(silent); }) ==
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
    const hokuto::format::Shared<Sequence> first = *file.sequences[0];
    const hokuto::format::Shared<Sequence> none;
    XSFLIB_CHECK(!first.SharesWith(file.sequences[0]) && first == file.sequences[0]);
    XSFLIB_CHECK(none == hokuto::format::Shared<Sequence>() && !(none == first) && !none);

    // Edit starts from a value-initialised value when there's none.
    hokuto::format::Shared<Sequence> empty;
    empty.Edit([](Sequence& sequence) { sequence.labels.push_back({"x", 0}); });
    XSFLIB_CHECK(empty && empty->labels.size() == 1 && empty->items.empty());
}

// Files that a read refuses. The binary form refuses a changed byte, a short file, a later major version, chunks
// without their archive, twice or cut short, parts of a later version and fields that name the wrong items. A file that
// a form can't hold throws, such as one whose encoder's choices lack their sequence file. A text with a mistake gives
// the mistake's line, in the sequences, the native archive and its tables.
void TestDamage()
{
    const std::vector<uint8_t> binary = hokuto::format::Write(Unusual());

    const auto refuses = [](std::vector<uint8_t> bytes)
    {
        return Throws<xsflib::FormatError>([&] { hokuto::format::Read(bytes); });
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
    XSFLIB_CHECK(Throws<xsflib::FormatError>([] { hokuto::format::ReadEitherForm(std::vector<uint8_t>{1, 2}); }));

    // A file that the binary form can't hold.
    File missing = Unusual();
    missing.sequences.clear();
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { hokuto::format::Write(missing); }));
    File clash = Unusual();
    clash.other_chunks.push_back({"SEQS", {}});
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { hokuto::format::Write(clash); }));
    File twice = Unusual();
    twice.sequences[0].Edit([](Sequence& sequence) { sequence.labels.push_back({"Song", 1}); });
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { hokuto::format::WriteText(twice); }));

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
    XSFLIB_CHECK(Patched(binary, 4, 12, 9).starts_with("The file holds a kind of sound (9) that this version"));
    XSFLIB_CHECK(Patched(binary, 4, 19, 3).starts_with("The file holds a flag of a sound that this version"));
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
    XSFLIB_CHECK(refuses(hokuto::format::Write(in_file)));
    File item = Unusual();
    item.native.Edit([](NativeArchive& native) { native.fields[1].to.piece = 2; });
    XSFLIB_CHECK(refuses(hokuto::format::Write(item)));
    XSFLIB_CHECK(TextError(hokuto::format::WriteText(item)).find("names an item of piece 2") != std::string::npos);

    // The encoder's choices without the sequence file that holds them.
    File unfiled = Unusual();
    unfiled.native = {};
    XSFLIB_CHECK(hokuto::format::UnfiledChoices(unfiled) == 0u);
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { hokuto::format::Write(unfiled); }));
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { hokuto::format::WriteText(unfiled); }));
    XSFLIB_CHECK(TextError("hok 1.0\nengine switch\nsequence 0\n    wait 5\n    wait 6 [value_bytes 2]\n")
                     .starts_with("Line 5: [value_bytes]"));

    // Texts with mistakes give the line.
    XSFLIB_CHECK(TextError("hok 1.0\nengine switch\nsequence 0\n    louder 5\n").starts_with("Line 4: no command"));
    XSFLIB_CHECK(TextError("hok 1.0\nengine switch\nsequence 0\n    jump nowhere\n").starts_with("Line 4: no label"));
    XSFLIB_CHECK(TextError("hok 2.0\nengine switch\n").starts_with("Line 1: a later version"));
    XSFLIB_CHECK(TextError("hok 1.0\nsequence 0\n").find("names no engine") != std::string::npos);
    XSFLIB_CHECK(TextError("hok 1.0\nengine switch\nnative switch\n    bytes 4\n        AAAA\n").starts_with("Line 4"));
    XSFLIB_CHECK(TextError("hok 1.0\nengine switch\nsequence 1\n").starts_with("Line 3"));
    const std::string kNative = "hok 1.0\nengine switch\nnative switch\n    bytes 3\n        AQID\n";
    XSFLIB_CHECK(
        TextError(kNative + "    field 0 from 0 to end\n").starts_with("Line 6: the field lies outside a run"));
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
    XSFLIB_CHECK(
        TextError(kTables + "    sound A song\n").starts_with("Line 7: Hokuto has no kind of sound named song"));
    XSFLIB_CHECK(TextError(kTables + "    sound A sequence loudness 3\n")
                     .starts_with("Line 7: a sound has no value named loudness"));
    XSFLIB_CHECK(TextError(kTables + "    sound A sequence volume 256\n")
                     .starts_with("Line 7: volume has to be a whole number from 0 to 255, not 256"));
    XSFLIB_CHECK(TextError(kTables + "    sound A sequence volume 1 volume 2\n")
                     .starts_with("Line 7: the sound gives volume twice"));
    XSFLIB_CHECK(TextError(kTables + "    sound A sequence volume\n").starts_with("Line 7: volume takes a number"));
    XSFLIB_CHECK(TextError(kTables + "    player A 4\n").starts_with("Line 7: a player's line"));
    XSFLIB_CHECK(TextError(kTables + "    bank A B\n").starts_with("Line 7: a line of bank holds its name"));
    XSFLIB_CHECK(TextError(kTables + "    sound A sequence banks 1 none\n    group G\n").empty());
    XSFLIB_CHECK(TextError("").find("empty") != std::string::npos);
}

// The values of commands that a read of the text refuses: those that the sequences of the archive's console can't hold,
// with the line and the values that they hold there. The ranges come from the Switch's commands: a wait in up to 4
// bytes of 7 bits and a byte for the rest, a volume in a byte, a transpose in a signed byte of semitones, a low-pass in
// 64ths around 64, and a time in two bytes. A note's key takes 7 bits. The depth of an LFO past the first takes no
// random value. A file without an archive names no console, and its values aren't checked.
void TestCommandValues()
{
    const auto error = [](std::string_view command)
    {
        return TextError("hok 1.0\nengine switch\nsequence 0\n    " + std::string(command) +
                         "\nnative switch\n    bytes 3\n        AQID\n");
    };
    XSFLIB_CHECK(error("wait -5") == "Line 4: wait has to be a whole number from 0 to 4294967295, not -5.");
    XSFLIB_CHECK(error("wait 99999999999") ==
                 "Line 4: wait has to be a whole number from 0 to 4294967295, not 99999999999.");
    XSFLIB_CHECK(error("wait 1.5") == "Line 4: wait has to be a whole number from 0 to 4294967295, not 1.5.");
    XSFLIB_CHECK(error("volume 300") == "Line 4: volume has to be a whole number from 0 to 255, not 300.");
    XSFLIB_CHECK(error("volume random(0,0.5)") ==
                 "Line 4: volume has to be a whole number from -32768 to 32767, not random(0,0.5).");
    XSFLIB_CHECK(error("transpose 150") ==
                 "Line 4: transpose has to be a number from -12800 to 12700 in steps of 100, not 150.");
    XSFLIB_CHECK(error("low_pass 0.01") ==
                 "Line 4: low_pass has to be a number from -1 to 2.984375 in steps of 0.015625, not 0.01.");
    XSFLIB_CHECK(error("pan 3 over 1.5") == "Line 4: pan's time has to be a whole number from 0 to 65535, not 1.5.");
    XSFLIB_CHECK(error("pan var(300)") == "Line 4: pan has to read a variable from 0 to 255, not var(300).");
    XSFLIB_CHECK(error("lfo_depth 2 random(0,1)") == "Line 4: lfo_depth takes no random value, not random(0,1).");
    XSFLIB_CHECK(error("note 60 1.5 48") ==
                 "Line 4: a note's velocity has to be a whole number from 0 to 255, not 1.5.");
    XSFLIB_CHECK(error("note 128 100 48") == "Line 4: a note's key has to be a whole number from 0 to 127, not 128.");
    XSFLIB_CHECK(error("note 60 100 -1") ==
                 "Line 4: a note's length has to be a whole number from 0 to 4294967295, not -1.");
    for (const std::string_view held :
         {"wait 268435455", "volume 255", "transpose 12700", "low_pass 0.015625", "pan 3 over 65535", "pan var(255)",
          "note 127 255 48", "end [index 3] [value var(2)]"})
    {
        XSFLIB_CHECK(error(held).empty());
    }
    XSFLIB_CHECK(TextError("hok 1.0\nengine switch\nsequence 0\n    wait 1.5\n").empty());

    // A file in the model form takes its console from the model. A new file of the Wii holds RSAR 1.4. Its sequences
    // come before its chunks.
    const std::string wii = hokuto::format::WriteText(hokuto::format::NewFile(hokuto::format::Engine::kWii));
    const std::size_t chunks = wii.find("\nchunk ") + 1;
    const auto line = std::ranges::count(wii.begin(), wii.begin() + static_cast<std::ptrdiff_t>(chunks), '\n') + 2;
    XSFLIB_CHECK(TextError(wii.substr(0, chunks) + "sequence 0\n    wait 1.5\n" + wii.substr(chunks)) ==
                 "Line " + std::to_string(line) + ": wait has to be a whole number from 0 to 4294967295, not 1.5.");

    // An empty file names no other kind. A file of another kind gives its kind.
    const auto either = [](std::vector<uint8_t> bytes)
    {
        return Message<xsflib::FormatError>([&] { hokuto::format::ReadEitherForm(bytes); });
    };
    XSFLIB_CHECK(either({}) == "It's empty.");
    XSFLIB_CHECK(either({'x', 'y', 'z'}) == "It isn't a Hokuto file.");
}

// The kinds that the messages name. The first bytes give a file's kind: the console's letter, three letters of the kind
// and a byte order mark for the consoles' files. The extension gives the kind that a file's name indicates. File > Open
// and File > Import > Sound Archive… name the kind of a file that they don't take, or the kind that its name indicates,
// and the command that takes the file's kind (src/app/menus.cpp): an item of File > Import, or Replace… for an AAC
// file. Hokuto takes no MIDI file.
void TestKinds()
{
    const auto bytes = [](std::string_view text)
    {
        return std::vector<uint8_t>(text.begin(), text.end());
    };

    const std::vector<uint8_t> kNxWave = {'F', 'W', 'A', 'V', 0xFF, 0xFE, 0x40, 0};
    const std::vector<uint8_t> kCafeBank = {'F', 'B', 'N', 'K', 0xFE, 0xFF, 0, 0x40};
    const std::vector<uint8_t> kWiiArchive = {'R', 'S', 'A', 'R', 0xFE, 0xFF, 1, 4};
    const std::vector<uint8_t> kCtrSequence = {'C', 'S', 'E', 'Q', 0xFF, 0xFE, 0x40, 0};
    const std::vector<uint8_t> kUnmarked = {'F', 'S', 'E', 'Q', 0, 0, 0, 0};
    const std::vector<uint8_t> kWav = {'R', 'I', 'F', 'F', 0x10, 0, 0, 0, 'W', 'A', 'V', 'E'};
    const std::vector<uint8_t> kAac = {0xFF, 0xF1, 0x50, 0x80};
    const std::vector<uint8_t> hok = hokuto::format::Write(hokuto::format::ImportArchive(Archive(), Flavor::kNx));
    const std::vector<uint8_t> notes = bytes("Notes for the next session.\n");
    const std::vector<uint8_t> kMidi = {'M', 'T', 'h', 'd', 0, 0, 0, 6};
    const std::vector<uint8_t> kDsArchive = {'S', 'D', 'A', 'T', 0xFF, 0xFE, 0, 1};

    XSFLIB_CHECK(hokuto::format::KindOfBytes({}) == "an empty file" &&
                 hokuto::format::KindOfBytes(hok) == "a Hokuto file" &&
                 hokuto::format::KindOfBytes(bytes("hok 1.0\n")) == "a Hokuto file");
    XSFLIB_CHECK(hokuto::format::KindOfBytes(kWav) == "a WAV file" &&
                 hokuto::format::KindOfBytes(kAac) == "an AAC file" &&
                 hokuto::format::KindOfBytes(bytes("SARC\xFF\xFE")) == "a SARC archive");
    XSFLIB_CHECK(hokuto::format::KindOfBytes(kMidi) == "a MIDI file" &&
                 hokuto::format::KindOfBytes(kDsArchive) == "a sound archive of the DS");
    XSFLIB_CHECK(hokuto::format::KindOfBytes(kNxWave) == "a wave file of the Switch" &&
                 hokuto::format::KindOfBytes(kCafeBank) == "a bank file of the Wii U" &&
                 hokuto::format::KindOfBytes(kWiiArchive) == "a sound archive of the Wii" &&
                 hokuto::format::KindOfBytes(kCtrSequence) == "a sequence file of the 3DS");
    XSFLIB_CHECK(hokuto::format::KindOfBytes(kUnmarked).empty() && hokuto::format::KindOfBytes(notes).empty());
    XSFLIB_CHECK(hokuto::format::KindOfName("Song.hok") == "a Hokuto file" &&
                 hokuto::format::KindOfName("Take.WAV") == "a WAV file" &&
                 hokuto::format::KindOfName("BGM.bfseq") == "a sequence file of the Wii U or the Switch" &&
                 hokuto::format::KindOfName("Drums.bcbnk") == "a bank file of the 3DS" &&
                 hokuto::format::KindOfName("Voice.brwsd") == "a wave sound file of the Wii");
    XSFLIB_CHECK(hokuto::format::KindOfName("Notes.txt").empty() &&
                 hokuto::format::KindOfName("Archive.bxsar").empty());
    XSFLIB_CHECK(hokuto::format::NotKindText(kNxWave, "a sequence file") ==
                 "It's a wave file of the Switch, not a sequence file.");
    XSFLIB_CHECK(hokuto::format::NotKindText({}, "a sequence file") == "It's empty." &&
                 hokuto::format::NotKindText(notes, "a sequence file") == "It isn't a sequence file." &&
                 hokuto::format::NotKindText(kCtrSequence, "a sequence file") == "It isn't a sequence file.");
    XSFLIB_CHECK(hokuto::format::NotKindText(kCafeBank, "a bank file of the Wii") ==
                 "It's a bank file of the Wii U, not a bank file of the Wii.");

    // File > Open, and File > Import > Sound Archive….
    const std::string kTakes =
        "File > Open opens Hokuto files and the sound archives of the Wii, the 3DS, the Wii U and the Switch.";
    const std::string kImports =
        "File > Import > Sound Archive… takes the sound archives of the Wii, the 3DS, the Wii U and the Switch.";
    XSFLIB_CHECK(hokuto::format::NotOpenedText("Empty.hok", {}, false) ==
                 "Hokuto can't open Empty.hok. It's empty. " + kTakes);
    XSFLIB_CHECK(hokuto::format::NotOpenedText("Notes.wav", notes, false) ==
                 "Hokuto can't open Notes.wav. Its name ends in .wav, but it doesn't hold a WAV file. " + kTakes);
    XSFLIB_CHECK(hokuto::format::NotOpenedText("Note.bfwav", kNxWave, false) ==
                 "Hokuto can't open Note.bfwav. It's a wave file of the Switch. " + kTakes +
                     " File > Import > Wave… takes it.");
    XSFLIB_CHECK(hokuto::format::NotOpenedText("Notes.txt", notes, false) == "Hokuto can't open Notes.txt. " + kTakes);
    XSFLIB_CHECK(hokuto::format::NotOpenedText("Take.wav", kWav, true) ==
                 "Hokuto can't import Take.wav. It's a WAV file. " + kImports +
                     " File > Import > Audio File… or Stream… takes it.");
    XSFLIB_CHECK(hokuto::format::NotOpenedText("Song.aac", kAac, true) ==
                 "Hokuto can't import Song.aac. It's an AAC file. " + kImports +
                     " Replace… of a stream sound of the 3DS that plays an AAC stream takes it.");
    XSFLIB_CHECK(hokuto::format::NotOpenedText("Song.mid", kMidi, false) ==
                 "Hokuto can't open Song.mid. It's a MIDI file. " + kTakes +
                     " Hokuto writes MIDI files, and reads none.");
    XSFLIB_CHECK(hokuto::format::NotOpenedText("sound_data.sdat", kDsArchive, true) ==
                 "Hokuto can't import sound_data.sdat. It's a sound archive of the DS. " + kImports);
    XSFLIB_CHECK(hokuto::format::AsSentence("too short to be an FSAR file") == "Too short to be an FSAR file." &&
                 hokuto::format::AsSentence("It's done.") == "It's done." &&
                 hokuto::format::AsSentence("sound_main can't be read.") == "sound_main can't be read." &&
                 hokuto::format::AsSentence("") == "");
}

} // namespace

int main()
{
    TestCrc();
    TestCodes();
    TestArchive();
    Test3dsArchive();
    TestWiiArchive();
    TestWiiTables();
    TestWiiBuild();
    TestBankSlots();
    TestGrowth();
    TestGameSizes();
    TestUnusual();
    TestNumbers();
    TestEffects();
    TestWiiUEffects();
    TestWiiEffects();
    TestCtrEffects();
    TestBusVolumes();
    TestScenes();
    TestTrackColours();
    TestView();
    TestTags();
    TestLengths();
    TestMixing();
    TestSoundEngines();
    TestTimelines();
    TestGroupFiles();
    TestGroupFilesOfThe3ds();
    TestGroupSequences();
    TestStreams();
    TestBanksAndWaves();
    TestSharing();
    TestDamage();
    TestCommandValues();
    TestKinds();

    return xsflib::testing::Summary("format");
}
