// SPDX-License-Identifier: MIT

// This header contains the archives, files and helpers that the tests of the format share.

#pragma once

#include <algorithm>
#include <array>
#include <bit>
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
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/dse/layout.h"
#include "ntdlib/mp2k/layout.h"
#include "ntdlib/ntr/rom.h"
#include "ntdlib/rsar/sequence_file.h"
#include "ntdlib/sar/bank_file.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/groups.h"
#include "ntdlib/sar/layout.h"
#include "ntdlib/sar/sarc.h"
#include "ntdlib/sound_build.h"
#include "ntdlib/testing/dse_song_files.h"
#include "ntdlib/testing/mp2k_cart.h"
#include "ntdlib/testing/nds_files.h"
#include "ntdlib/testing/rsar_tools.h"
#include "ntdlib/testing/sar_files.h"
#include "ntdlib/testing/sar_tools.h"
#include "ntdlib/testing/sdat_files.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/sha1.h"
#include "xsflib/testing/check.h"

#include "binary_form_testing.h"
#include "format/archive.h"
#include "format/codes.h"
#include "format/file.h"
#include "format/kinds.h"
#include "format/model.h"
#include "format/text.h"

namespace
{

namespace format = hokuto::format;
namespace sar_files = ntdlib::testing::sar_files;
namespace sar_tools = ntdlib::testing::sar_tools;
using format::EditorView;
using format::Effect;
using format::EffectKind;
using format::Engine;
using format::File;
using format::FileBeside;
using format::GameSizes;
using format::Mixing;
using format::MixingRefused;
using format::NativeArchive;
using format::NativeGroupFile;
using format::NativeSound;
using format::NativeStream;
using format::NativeTables;
using format::TrackHints;
using format::ViewFront;
using format::WaveModel;
using format::WavePlace;
using hokuto::testing::ChunkBytes;
using hokuto::testing::ChunkIds;
using hokuto::testing::Patched;
using ntdlib::commands::Argument;
using ntdlib::commands::Command;
using ntdlib::commands::Fixed;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Sequence;
using ntdlib::commands::Source;
using ntdlib::sar::Flavor;
using ntdlib::sar::SoundType;
using xsflib::testing::ErrorOf;

// A volume, a wait and an end, then a note and an end that only a sound's start reaches, at offset 5.
const std::vector<uint8_t> kSong = {0xC1, 0x64, 0x80, 0x30, 0xFF, 0x3C, 0x64, 0x30, 0xFF};

// A Switch archive with a player and three sequence sounds. Song plays kSong from its start, and Entry from offset 5.
// File 1 holds five other bytes, and file 2 the sequence that Other plays.
inline std::vector<uint8_t> Archive()
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
inline std::vector<uint8_t> LongerSong(std::size_t volumes)
{
    std::vector<uint8_t> song = kSong;
    for (std::size_t i = 0; i < volumes; i++)
    {
        song.insert(song.begin(), {0xC1, 0x64});
    }

    return sar_files::Sequence(Flavor::kNx, song, {{"Song", 0}});
}

// An archive of `song`, a sequence file, with a group whose file holds a copy of it.
inline std::vector<uint8_t> GroupArchive(const std::vector<uint8_t>& song)
{
    sar_files::ArchiveSpec spec;
    spec.sounds = {{.name = "Song", .file = 0}};
    spec.players = {{"PLAYER", 1}};
    spec.groups = {{"GROUP_SONG", 1}};
    spec.files = {{.bytes = song}, {.bytes = sar_files::Group(Flavor::kNx, {{0, song}})}};

    return sar_files::Archive(Flavor::kNx, spec);
}

// A command of `kind` with the fixed value `value`.
inline Command Item(Kind kind, Fixed value = 0)
{
    Command item;
    item.kind = kind;
    item.value.value = value;

    return item;
}

// A SHA-1 of 20 bytes of `byte`, for the game's group files.
inline std::array<uint8_t, 20> GameHash(uint8_t byte)
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
inline File Unusual()
{
    File file;
    file.mixing = format::Mixing::kEnhanced;

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
    sweep.encoding = {.value_bytes = 3, .unused_time = 0xA3, .unused_random = true, .unused_variable = true};
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
        NativeArchive{hokuto::format::Engine::kSwitch,
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
inline File WithScenes()
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
    return !ErrorOf<Error>(action).empty();
}

// The message that ReadText throws for `text`, or nothing.
inline std::string TextError(std::string_view text)
{
    return ErrorOf<xsflib::FormatError>([&] { format::ReadText(text); });
}

// The number of the line of `text` that holds `part`.
inline std::size_t LineOf(std::string_view text, std::string_view part)
{
    const auto before = text.substr(0, text.find(part));
    return static_cast<std::size_t>(std::count(before.begin(), before.end(), '\n')) + 1;
}

// The start of the message of a text form's line `line`.
inline std::string LineStart(std::size_t line)
{
    return "Line " + std::to_string(line) + ": ";
}

// `text` with its first `old` replaced by `replacement`.
inline std::string Replaced(std::string text, std::string_view old, std::string_view replacement)
{
    text.replace(text.find(old), old.size(), replacement);

    return text;
}

// A kind of effect with its code, its name in the text form and the number of its parameters.
struct EffectKindCode
{
    EffectKind kind;
    uint8_t code;
    std::string_view name;
    std::size_t parameters;
};

// The text form of a file of the Switch whose native archive holds 3 bytes, without chunks. The tests of the chunks put
// chunks after it.
const std::string kNativeOnly = "hok 1.1\nengine switch\nnative switch\n    bytes 3\n        AQID\n";

} // namespace
