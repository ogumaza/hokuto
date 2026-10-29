// SPDX-License-Identifier: MIT

// Tests the edits of a sound's values in the archive's tables (src/edit/tables.h). A small archive of each console
// holds two sequence sounds of one file, a wave sound, two banks and two players. The first sequence sound plays the
// first bank, and the second the second. Laid out as NintendoWare's tools lay it out, the archive gives every sound's
// entry every option. Without some of them, the entries take them for new values. Laid out otherwise, the archive gives
// the first sequence sound every option, the second only the option of the priorities, and the wave sound only the
// option of the pan. Its entries can't take new options. Each change exports, and ntdlib reads the new value from the
// archive. An archive with eight sequence sounds checks the files' sequences and the sounds' start fields after the
// tables grow. A 3DS archive takes the edits too. So does an archive of the Wii, in place, with one bank slot and the
// values that its version holds.

#include "edit/tables.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/sequence_file.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/layout.h"
#include "ntdlib/testing/rsar_files.h"
#include "ntdlib/testing/sar_files.h"
#include "ntdlib/testing/sar_tools.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "format/archive.h"
#include "format/file.h"

using hokuto::edit::ChangeSound;
using hokuto::edit::HeldValuesOf;
using hokuto::edit::SoundChange;
using hokuto::edit::SoundValue;
using hokuto::edit::ValueOf;
using hokuto::format::File;
using hokuto::format::NativeSound;
using ntdlib::sar::Flavor;

namespace
{

namespace sar = ntdlib::sar;
namespace sar_files = ntdlib::testing::sar_files;
namespace sar_tools = ntdlib::testing::sar_tools;

// The archive's sounds.
constexpr uint32_t kSong = 0;
constexpr uint32_t kPlain = 1;
constexpr uint32_t kWaveSound = 2;

// The archive's banks and players, and an empty bank slot.
const uint32_t kBankA = sar::ItemId(sar::kBankItem, 0);
const uint32_t kBankB = sar::ItemId(sar::kBankItem, 1);
const uint32_t kPlayerB = sar::ItemId(sar::kPlayerItem, 1);
constexpr uint32_t kEmpty = sar::kNoItem;

// The sequence file: a timebase, a note and the end.
const std::vector<uint8_t> kBytecode = {0xB0, 48, 60, 100, 24, 0xFF};

// The optional values that the edits give places. The places of every option of a sequence sound, and of a wave sound.
constexpr std::array<SoundValue, 6> kOptional = {SoundValue::kPanMode,         SoundValue::kPanCurve,
                                                 SoundValue::kPlayerPriority,  SoundValue::kActorPlayer,
                                                 SoundValue::kChannelPriority, SoundValue::kReleasePriorityFix};
constexpr ntdlib::sar::ValuePlaces kEveryPlace{true, true, true};
constexpr ntdlib::sar::ValuePlaces kWaveSoundPlaces{true, true, false};

// The archive, laid out as the tools lay it out with `tools`.
std::vector<uint8_t> TestArchive(Flavor flavor, bool tools)
{
    sar_files::SoundSpec song;
    song.name = "SEQ_SONG";
    song.pan = 0x0101;
    song.priority = 0x0230;
    song.banks = {0};
    song.start_offset = 0;
    song.channel_priority = 0x0150;
    sar_files::SoundSpec plain;
    plain.name = "SEQ_PLAIN";
    plain.priority = 0x0140;
    plain.banks = {1};
    sar_files::SoundSpec wave_sound;
    wave_sound.name = "WSD_SOUND";
    wave_sound.kind = sar_files::SoundSpec::kWaveSound;
    wave_sound.file = 1;
    wave_sound.pan = 0;

    sar_files::ArchiveSpec spec;
    spec.sounds = {song, plain, wave_sound};
    spec.banks = {{"BANK_A", 2}, {"BANK_B", 3}};
    spec.players = {{"PLAYER_A", 1}, {"PLAYER_B", 2}};
    spec.files = {{sar_files::Sequence(flavor, kBytecode, {{"Song", 0}}), "", false},
                  {sar_files::WaveSoundFile(flavor, {}), "", false},
                  {sar_files::Bank(flavor, {}), "", false},
                  {sar_files::Bank(flavor, {}), "", false}};

    return tools ? sar_tools::Archive(flavor, spec) : sar_files::Archive(flavor, spec);
}

// Eight sequence sounds of one file, each with a start, every option and one bank, laid out as the tools lay them out.
std::vector<uint8_t> EightSounds(Flavor flavor)
{
    sar_files::ArchiveSpec spec;
    for (int i = 0; i < 8; ++i)
    {
        sar_files::SoundSpec sound;
        sound.name = "SEQ_" + std::to_string(i);
        sound.pan = 0x0101;
        sound.priority = 0x0230;
        sound.banks = {0};
        sound.start_offset = 0;
        sound.channel_priority = 0x0150;
        spec.sounds.push_back(sound);
    }
    spec.banks = {{"BANK", 1}};
    spec.players = {{"PLAYER", 1}};
    spec.files = {{sar_files::Sequence(flavor, kBytecode, {{"Song", 0}}), "", false},
                  {sar_files::Bank(flavor, {}), "", false}};

    return sar_tools::Archive(flavor, spec);
}

// What ntdlib reads of `archive`, an archive of `flavor`, by the strict rules.
sar::SoundArchive Strictly(const std::vector<uint8_t>& archive, Flavor flavor)
{
    sar::Options options;
    options.flavor = flavor;
    options.rules = sar::Rules::kStopAtFirstProblem;

    return sar::SoundArchive::Load(archive, options);
}

// The values of sound `sound` that ntdlib reads from the archive that `file` exports, by the strict rules.
sar::SoundInfo Exported(const File& file, uint32_t sound)
{
    return Strictly(hokuto::format::ExportArchive(file), file.native->flavor).Sounds().at(sound);
}

// `file` after its sounds `sounds` take each optional value of `values` (kOptional). A wave sound takes only the values
// that it has.
File WithOptionalValues(File file, const std::vector<uint32_t>& sounds, const std::vector<NativeSound>& values)
{
    for (const uint32_t sound : sounds)
    {
        for (const SoundValue value : kOptional)
        {
            const bool sequence = value == SoundValue::kChannelPriority || value == SoundValue::kReleasePriorityFix;
            if (!sequence || values[sound].kind == sar::SoundType::kSequence)
            {
                file = ChangeSound(file, sound, {value, 0, ValueOf(values[sound], value, 0)});
            }
        }
    }

    return file;
}

// The message of the std::invalid_argument that a change of sound `sound` of `file` throws, or nothing.
std::string Refusal(const File& file, uint32_t sound, const SoundChange& change)
{
    try
    {
        ChangeSound(file, sound, change);
    }
    catch (const std::invalid_argument& error)
    {
        return error.what();
    }

    return {};
}

// Every value of the first sound changes, and ntdlib reads each new value from the export. The other sounds keep
// theirs. The old values then give the archive back.
void TestValues(Flavor flavor)
{
    const std::vector<uint8_t> archive = TestArchive(flavor, true);
    const File file = hokuto::format::ImportArchive(archive, flavor);
    const std::vector<SoundChange> kChanges = {
        {SoundValue::kPlayer, 0, kPlayerB},    {SoundValue::kVolume, 0, 200},
        {SoundValue::kPanMode, 0, 0},          {SoundValue::kPanCurve, 0, 8},
        {SoundValue::kPlayerPriority, 0, 127}, {SoundValue::kActorPlayer, 0, 3},
        {SoundValue::kBank, 0, kBankB},        {SoundValue::kTracks, 0, 0x8001},
        {SoundValue::kChannelPriority, 0, 0},  {SoundValue::kReleasePriorityFix, 0, 0},
    };

    File changed = file;
    for (const SoundChange& change : kChanges)
    {
        changed = ChangeSound(changed, kSong, change);
    }
    File back = changed;
    for (const SoundChange& change : kChanges)
    {
        const uint32_t old = ValueOf(file.native->tables->sounds[kSong], change.value, change.slot);
        back = ChangeSound(back, kSong, {change.value, change.slot, old});
    }
    const sar::SoundInfo song = Exported(changed, kSong);

    XSFLIB_CHECK(song.player_id == kPlayerB && song.volume == 200 && song.pan_mode == 0 && song.pan_curve == 8);
    XSFLIB_CHECK(song.player_priority == 127 && song.actor_player_id == 3);
    XSFLIB_CHECK((song.banks == std::vector<uint32_t>{kBankB}) && song.allocate_track_flags == 0x8001);
    XSFLIB_CHECK(song.channel_priority == 0 && !song.release_priority_fix);

    for (const SoundChange& change : kChanges)
    {
        XSFLIB_CHECK(ValueOf(changed.native->tables->sounds[kSong], change.value, change.slot) == change.to);
    }

    XSFLIB_CHECK(changed.native->tables->sounds[kPlain] == file.native->tables->sounds[kPlain]);
    XSFLIB_CHECK(changed.native->tables->sounds[kWaveSound] == file.native->tables->sounds[kWaveSound]);
    XSFLIB_CHECK(hokuto::format::ExportArchive(back) == archive);
}

// A bank in a slot past a sound's list makes the list longer, with empty slots before the bank. Empty slots at the end
// leave the list, but its first slot stays. The list that the archive held gives the archive back.
void TestBankSlots(Flavor flavor)
{
    const std::vector<uint8_t> archive = TestArchive(flavor, true);
    const File file = hokuto::format::ImportArchive(archive, flavor);

    const File longer = ChangeSound(file, kSong, {SoundValue::kBank, 2, kBankB});
    const File shorter = ChangeSound(longer, kSong, {SoundValue::kBank, 2, kEmpty});
    const File emptied = ChangeSound(file, kSong, {SoundValue::kBank, 0, kEmpty});
    const File last = ChangeSound(file, kPlain, {SoundValue::kBank, 3, kBankA});
    const File past = ChangeSound(file, kPlain, {SoundValue::kBank, 3, kEmpty});

    XSFLIB_CHECK((Exported(longer, kSong).banks == std::vector<uint32_t>{kBankA, kEmpty, kBankB}));
    XSFLIB_CHECK(shorter.native->tables == file.native->tables && hokuto::format::ExportArchive(shorter) == archive);
    XSFLIB_CHECK((Exported(emptied, kSong).banks == std::vector<uint32_t>{kEmpty}));
    XSFLIB_CHECK((Exported(last, kPlain).banks == std::vector<uint32_t>{kBankB, kEmpty, kEmpty, kBankA}));
    XSFLIB_CHECK(past.native->tables == file.native->tables);
    XSFLIB_CHECK(ValueOf(file.native->tables->sounds[kSong], SoundValue::kBank, 2) == kEmpty);
}

// The places of the optional values follow the options of each sound's entry. Without the places, an edit that gives a
// value a new place exports to check the place.
void TestPlaces(Flavor flavor)
{
    const File tools = hokuto::format::ImportArchive(TestArchive(flavor, true), flavor);
    const File file = hokuto::format::ImportArchive(TestArchive(flavor, false), flavor);
    File unknown = file;
    const auto pad = [](hokuto::format::NativeArchive& native)
    {
        native.pieces.Edit([](std::vector<sar::Piece>& pieces) { pieces.insert(pieces.begin(), sar::Padding{}); });
    };
    unknown.native.Edit(pad);

    const std::optional<std::vector<sar::ValuePlaces>> every = hokuto::format::TableValuePlacesOf(*tools.native);
    const std::optional<std::vector<sar::ValuePlaces>> places = hokuto::format::TableValuePlacesOf(*file.native);
    const std::optional<std::vector<sar::ValuePlaces>> none = hokuto::format::TableValuePlacesOf(*unknown.native);

    const std::vector<sar::ValuePlaces> kAll = {{true, true, true}, {true, true, true}, {true, true, false}};
    const std::vector<sar::ValuePlaces> kExpected = {{true, true, true}, {false, true, false}, {true, false, false}};
    XSFLIB_CHECK(every == kAll && places == kExpected && !none);
    XSFLIB_CHECK(Refusal(file, kWaveSound, {SoundValue::kPanCurve, 0, 3}).empty());
    XSFLIB_CHECK(Refusal(file, kPlain, {SoundValue::kActorPlayer, 0, 2}).empty());
    XSFLIB_CHECK(Refusal(file, kPlain, {SoundValue::kVolume, 0, 3}).empty());
    XSFLIB_CHECK(Refusal(unknown, kSong, {SoundValue::kPlayer, 0, kPlayerB}).empty());

    // Without the places of the first piece, an edit that gives a value a new place exports. The padding in front of
    // this archive makes the export fail. A value whose place the song's entry holds changes without an export.
    XSFLIB_CHECK(!Refusal(unknown, kWaveSound, {SoundValue::kPanCurve, 0, 3}).empty());
    XSFLIB_CHECK(Refusal(unknown, kSong, {SoundValue::kPanMode, 0, 0}).empty());
}

// An entry without an option takes it for a value other than the one that ntdlib's reader gives without the option. The
// new option lies where the tools lay it out. The values that the reader gives without the options give the archive
// without them back.
void TestNewPlaces(Flavor flavor)
{
    const std::vector<uint8_t> archive = TestArchive(flavor, true);
    const std::map<uint32_t, sar::ValuePlaces> kStripped = {
        {kSong, kEveryPlace}, {kPlain, kEveryPlace}, {kWaveSound, kWaveSoundPlaces}};
    const std::vector<uint8_t> bare = sar_tools::WithoutValuePlaces(archive, flavor, kStripped);
    const File tools = hokuto::format::ImportArchive(archive, flavor);
    const File file = hokuto::format::ImportArchive(bare, flavor);

    // The reader's values without the options.
    const NativeSound& song = file.native->tables->sounds[kSong];
    XSFLIB_CHECK(hokuto::format::TableValuePlacesOf(*file.native) == std::vector<sar::ValuePlaces>(3));
    XSFLIB_CHECK(song.pan_mode == 0 && song.pan_curve == 0 && song.player_priority == 64 && song.actor_player == 0);
    XSFLIB_CHECK(song.channel_priority == 64 && !song.release_priority_fix);

    // The values of the archive with every option. The plain sound's pan and channel priority and the wave sound's
    // values are those that the reader gives without the options. Their entries stay without them.
    const std::vector<uint32_t> kSounds = {kSong, kPlain, kWaveSound};
    const File placed = WithOptionalValues(file, kSounds, tools.native->tables->sounds);
    const std::vector<uint8_t> unplaced =
        sar_tools::WithoutValuePlaces(archive, flavor, {{kPlain, {true, false, true}}, {kWaveSound, kWaveSoundPlaces}});
    XSFLIB_CHECK(placed.native->tables == tools.native->tables);
    XSFLIB_CHECK(hokuto::format::ExportArchive(placed) == unplaced);
    XSFLIB_CHECK(hokuto::format::ExportArchive(WithOptionalValues(placed, kSounds, file.native->tables->sounds)) ==
                 bare);

    // Other values read back.
    const File curved = ChangeSound(file, kWaveSound, {SoundValue::kPanCurve, 0, 5});
    const File fixed = ChangeSound(file, kPlain, {SoundValue::kReleasePriorityFix, 0, 1});
    const File actor = ChangeSound(file, kSong, {SoundValue::kActorPlayer, 0, 3});
    XSFLIB_CHECK(Exported(curved, kWaveSound).pan_curve == 5 && Exported(curved, kWaveSound).pan_mode == 0);
    XSFLIB_CHECK(Exported(fixed, kPlain).release_priority_fix && Exported(fixed, kPlain).channel_priority == 64);
    XSFLIB_CHECK(Exported(actor, kSong).actor_player_id == 3 && Exported(actor, kSong).player_priority == 64);
    XSFLIB_CHECK(Exported(actor, kPlain).actor_player_id == 0 && Exported(actor, kPlain).volume == 100);

    // ntdlib's reader reads no channel priority of a wave sound. A text file can still give one.
    File unread = file;
    const auto set_priority = [](hokuto::format::NativeArchive& native)
    {
        native.tables->sounds[kWaveSound].channel_priority = 70;
    };
    unread.native.Edit(set_priority);
    std::string message;
    try
    {
        hokuto::format::ExportArchive(unread);
    }
    catch (const std::invalid_argument& error)
    {
        message = error.what();
    }
    XSFLIB_CHECK(message == "The archive's tables have no place for the channel priority of sound WSD_SOUND.");
}

// After an export's tables grow, the files after them move. The sequences of the archive's files and the fields of the
// sounds' starts stay those of the file. The places of eight sounds' options grow the tables, and so do eight longer
// lists of banks.
void TestMovedFiles(Flavor flavor)
{
    const std::vector<uint8_t> archive = EightSounds(flavor);
    std::map<uint32_t, sar::ValuePlaces> every;
    std::vector<uint32_t> sounds;
    for (uint32_t sound = 0; sound < 8; ++sound)
    {
        every[sound] = kEveryPlace;
        sounds.push_back(sound);
    }

    const std::vector<uint8_t> bare = sar_tools::WithoutValuePlaces(archive, flavor, every);
    const File tools = hokuto::format::ImportArchive(archive, flavor);
    const File file = hokuto::format::ImportArchive(bare, flavor);

    File listed = tools;
    for (const uint32_t sound : sounds)
    {
        listed = ChangeSound(listed, sound, {SoundValue::kBank, 3, kBankA});
    }
    const File placed = WithOptionalValues(file, sounds, tools.native->tables->sounds);

    const std::array<std::pair<const File*, const File*>, 2> edits = {{{&file, &placed}, {&tools, &listed}}};
    for (const auto& [before, after] : edits)
    {
        const std::vector<uint8_t> was = hokuto::format::ExportArchive(*before);
        const std::vector<uint8_t> is = hokuto::format::ExportArchive(*after);
        const sar::SoundArchive old = Strictly(was, flavor);
        const sar::SoundArchive now = Strictly(is, flavor);
        const std::vector<std::optional<uint32_t>> sequences = hokuto::format::SequencesOfFiles(*before, was, old);
        const std::vector<std::optional<uint32_t>> fields = hokuto::format::StartFieldsOfSounds(*before, was);

        XSFLIB_CHECK(old.FileOffset(0) != now.FileOffset(0) && old.FileOffset(1) != now.FileOffset(1));
        XSFLIB_CHECK(sequences.size() == 2 && sequences[0] == 0 && !sequences[1]);
        XSFLIB_CHECK(fields.size() == 8 &&
                     std::ranges::all_of(fields, [](const auto& field) { return field.has_value(); }));
        XSFLIB_CHECK(hokuto::format::SequencesOfFiles(*after, is, now) == sequences);
        XSFLIB_CHECK(hokuto::format::StartFieldsOfSounds(*after, is) == fields);
    }

    XSFLIB_CHECK(hokuto::format::ExportArchive(placed) == archive);
}

// The places that an archive laid out otherwise can't take, the values that the sound doesn't have, and the values past
// their ranges.
void TestRefusals()
{
    const File file = hokuto::format::ImportArchive(TestArchive(Flavor::kNx, false), Flavor::kNx);
    const std::string kNotSequence =
        "Sound WSD_SOUND isn't a sequence sound. Only a sequence sound has bank slots, "
        "tracks, a channel priority and a release priority fix.";
    const std::string kSlotsMessage = "A sound has four bank slots. Each holds one of the archive's banks, or none.";
    const std::string kTracksMessage = "A sound allocates at least one track, of tracks 0 to 15.";

    XSFLIB_CHECK(Refusal(file, kPlain, {SoundValue::kPanMode, 0, 1}) ==
                 "Hokuto can't give sound SEQ_PLAIN a place for its pan in the archive's tables.");
    XSFLIB_CHECK(Refusal(file, kWaveSound, {SoundValue::kActorPlayer, 0, 1}) ==
                 "Hokuto can't give sound WSD_SOUND a place for its priorities in the archive's tables.");
    XSFLIB_CHECK(Refusal(file, kPlain, {SoundValue::kReleasePriorityFix, 0, 1}) ==
                 "Hokuto can't give sound SEQ_PLAIN a place for its channel priority in the archive's tables.");
    XSFLIB_CHECK(Refusal(file, kPlain, {SoundValue::kPanCurve, 0, 0}).empty());
    XSFLIB_CHECK(Refusal(file, kWaveSound, {SoundValue::kPlayerPriority, 0, 64}).empty());
    XSFLIB_CHECK(Refusal(file, kWaveSound, {SoundValue::kBank, 0, kBankA}) == kNotSequence);
    XSFLIB_CHECK(Refusal(file, kWaveSound, {SoundValue::kChannelPriority, 0, 3}) == kNotSequence);
    XSFLIB_CHECK(Refusal(file, kWaveSound, {SoundValue::kTracks, 0, 1}) == kNotSequence);
    XSFLIB_CHECK(Refusal(file, kWaveSound, {SoundValue::kReleasePriorityFix, 0, 1}) == kNotSequence);
    XSFLIB_CHECK(Refusal(file, kSong, {SoundValue::kPanMode, 0, 2}) == "The pan mode goes from 0 to 1.");
    XSFLIB_CHECK(Refusal(file, kSong, {SoundValue::kPanCurve, 0, 9}) == "The pan curve goes from 0 to 8.");
    XSFLIB_CHECK(Refusal(file, kSong, {SoundValue::kActorPlayer, 0, 4}) == "The actor player goes from 0 to 3.");
    XSFLIB_CHECK(Refusal(file, kSong, {SoundValue::kVolume, 0, 256}) == "The volume goes from 0 to 255.");
    XSFLIB_CHECK(Refusal(file, kSong, {SoundValue::kChannelPriority, 0, 128}) ==
                 "The channel priority goes from 0 to 127.");
    XSFLIB_CHECK(Refusal(file, kSong, {SoundValue::kPlayer, 0, sar::ItemId(sar::kPlayerItem, 2)}) ==
                 "The archive's tables hold no such player.");
    XSFLIB_CHECK(Refusal(file, kSong, {SoundValue::kPlayer, 0, kBankA}) == "The archive's tables hold no such player.");
    XSFLIB_CHECK(Refusal(file, kSong, {SoundValue::kBank, 0, sar::ItemId(sar::kBankItem, 2)}) == kSlotsMessage);
    XSFLIB_CHECK(Refusal(file, kSong, {SoundValue::kBank, 4, kBankA}) == kSlotsMessage);
    XSFLIB_CHECK(Refusal(file, kSong, {SoundValue::kBank, -1, kBankA}) == kSlotsMessage);
    XSFLIB_CHECK(Refusal(file, kSong, {SoundValue::kTracks, 0, 0}) == kTracksMessage);
    XSFLIB_CHECK(Refusal(file, kSong, {SoundValue::kTracks, 0, 0x10000}) == kTracksMessage);
    XSFLIB_CHECK(Refusal(file, kSong, {SoundValue::kTracks, 0, 0xFFFF}).empty());
    XSFLIB_CHECK(Refusal(file, 3, {SoundValue::kVolume, 0, 1}) == "The archive's tables hold no such sound.");
    XSFLIB_CHECK(Refusal(File{}, 0, {SoundValue::kVolume, 0, 1}) == "This file holds no tables of an archive.");
}

// A 3DS sound takes a new player, volume, tracks, player priority and actor player, and its old values give the archive
// back.
void Test3ds()
{
    sar_files::SoundSpec song;
    song.name = "SEQ_CTR";
    song.banks = {0};
    song.priority = 0x0040;
    sar_files::ArchiveSpec spec;
    spec.sounds = {song};
    spec.banks = {{"BANK_CTR", 1}};
    spec.players = {{"PLAYER_CTR", 1}, {"OTHER_CTR", 2}};
    spec.files = {{sar_files::Sequence(Flavor::kCtr, kBytecode, {{"Ctr", 0}}), "", false},
                  {sar_files::Bank(Flavor::kCtr, {}), "", false}};
    const std::vector<uint8_t> archive = sar_files::Archive(Flavor::kCtr, spec);
    const File file = hokuto::format::ImportArchive(archive, Flavor::kCtr);
    const NativeSound& old = file.native->tables->sounds[kSong];
    const std::vector<SoundChange> kChanges = {{SoundValue::kPlayer, 0, sar::ItemId(sar::kPlayerItem, 1)},
                                               {SoundValue::kVolume, 0, 100},
                                               {SoundValue::kTracks, 0, 0x8001},
                                               {SoundValue::kPlayerPriority, 0, 10},
                                               {SoundValue::kActorPlayer, 0, 3}};
    const std::vector<SoundChange> undone = {{SoundValue::kPlayer, 0, old.player},
                                             {SoundValue::kVolume, 0, old.volume},
                                             {SoundValue::kTracks, 0, old.tracks},
                                             {SoundValue::kPlayerPriority, 0, old.player_priority},
                                             {SoundValue::kActorPlayer, 0, old.actor_player}};
    File changed = file;
    for (const SoundChange& change : kChanges)
    {
        changed = ChangeSound(changed, kSong, change);
    }
    File back = changed;
    for (const SoundChange& change : undone)
    {
        back = ChangeSound(back, kSong, change);
    }
    const sar::SoundInfo song_now = Exported(changed, kSong);

    XSFLIB_CHECK(old.player == sar::ItemId(sar::kPlayerItem, 0) && old.player_priority == 64 && old.actor_player == 0);
    XSFLIB_CHECK(song_now.player_id == sar::ItemId(sar::kPlayerItem, 1) && song_now.volume == 100);
    XSFLIB_CHECK(song_now.allocate_track_flags == 0x8001 && song_now.player_priority == 10);
    XSFLIB_CHECK(song_now.actor_player_id == 3);
    XSFLIB_CHECK(hokuto::format::ExportArchive(back) == archive);
}

// An archive of the Wii of version `version` with one sequence sound and its bank. ntdlib's tests lay one out the same
// way. The sound plays the bank in slot 0, with pan mode 0, pan curve 3 and the release priority fix on.
std::vector<uint8_t> WiiArchive(uint16_t version)
{
    ntdlib::rsar::SequenceFile sequence;
    sequence.data = {0xC1, 0x64, 0x3C, 0x64, 0x30, 0x80, 0x30, 0xFF};
    sequence.labels = {{"SEQ_SONG", 0}};
    std::vector<uint8_t> archive = ntdlib::testing::rsar_files::MakeArchive(sequence.Write(), 0, 1);
    xsflib::StoreBe16(&archive[6], version);

    return archive;
}

// A Wii sound takes a new volume, pan, player priority, bank, tracks, channel priority and release priority fix. ntdlib
// reads each one from the export, and the old values give the archive back. The archive holds one bank slot and no
// actor player that ntdlib reads, the pan from version 1.2 on and the release priority fix from version 1.3 on. The
// edits refuse the others.
void TestWii()
{
    const std::vector<uint8_t> archive = WiiArchive(0x0104);
    const File file = hokuto::format::ImportWiiArchive(archive);
    const NativeSound& old = file.native->tables->sounds[kSong];
    const std::vector<SoundChange> kChanges = {
        {SoundValue::kVolume, 0, 200},        {SoundValue::kPanMode, 0, 1},
        {SoundValue::kPanCurve, 0, 8},        {SoundValue::kPlayerPriority, 0, 127},
        {SoundValue::kBank, 0, kEmpty},       {SoundValue::kTracks, 0, 0x8001},
        {SoundValue::kChannelPriority, 0, 0}, {SoundValue::kReleasePriorityFix, 0, 0},
    };
    File changed = file;
    for (const SoundChange& change : kChanges)
    {
        changed = ChangeSound(changed, kSong, change);
    }
    File back = changed;
    for (const SoundChange& change : kChanges)
    {
        back = ChangeSound(back, kSong, {change.value, change.slot, ValueOf(old, change.value, change.slot)});
    }
    const std::vector<uint8_t> exported = hokuto::format::ExportArchive(changed);
    const ntdlib::rsar::SoundInfo song =
        ntdlib::rsar::SoundArchive::Load(exported, ntdlib::rvl::snd::kRsarOptions).Sounds().at(kSong);

    XSFLIB_CHECK(old.pan_mode == 0 && old.pan_curve == 3 && old.release_priority_fix && old.banks.size() == 1);
    XSFLIB_CHECK(song.volume == 200 && song.pan_mode == 1 && song.pan_curve == 8 && song.player_priority == 127);
    XSFLIB_CHECK(song.bank == kEmpty && song.allocate_track_flags == 0x8001 && song.channel_priority == 0);
    XSFLIB_CHECK(!song.release_priority_fix && exported.size() == archive.size());
    XSFLIB_CHECK(hokuto::format::ExportArchive(back) == archive);

    // One bank slot, and no actor player.
    const std::string kSlot = "A sound of the Wii plays one bank, in slot 0.";
    XSFLIB_CHECK(Refusal(file, kSong, {SoundValue::kBank, 1, kEmpty}) == kSlot);
    XSFLIB_CHECK(Refusal(file, kSong, {SoundValue::kBank, -1, kEmpty}) == kSlot);
    XSFLIB_CHECK(Refusal(file, kSong, {SoundValue::kActorPlayer, 0, 1}) ==
                 "Hokuto reads no actor player in an archive of the Wii.");

    // The versions before 1.2 and 1.3.
    const File old_file = hokuto::format::ImportWiiArchive(WiiArchive(0x0101));
    const File middle = hokuto::format::ImportWiiArchive(WiiArchive(0x0102));
    const std::string kPan =
        "An archive of the Wii holds the pan mode and the pan curve from version 1.2 on. This archive holds version "
        "1.1.";
    XSFLIB_CHECK(Refusal(old_file, kSong, {SoundValue::kPanMode, 0, 0}) == kPan);
    XSFLIB_CHECK(Refusal(old_file, kSong, {SoundValue::kPanCurve, 0, 1}) == kPan);
    XSFLIB_CHECK(Refusal(old_file, kSong, {SoundValue::kReleasePriorityFix, 0, 1}) ==
                 "An archive of the Wii holds the release priority fix from version 1.3 on. This archive holds "
                 "version 1.1.");
    XSFLIB_CHECK(Refusal(middle, kSong, {SoundValue::kReleasePriorityFix, 0, 1}) ==
                 "An archive of the Wii holds the release priority fix from version 1.3 on. This archive holds "
                 "version 1.2.");
    XSFLIB_CHECK(Refusal(middle, kSong, {SoundValue::kPanCurve, 0, 4}).empty());
    XSFLIB_CHECK(Refusal(old_file, kSong, {SoundValue::kVolume, 0, 10}).empty());

    // The values that each version holds. The other consoles' archives hold every value.
    const hokuto::edit::HeldValues every =
        HeldValuesOf(hokuto::format::ImportArchive(TestArchive(Flavor::kNx, true), Flavor::kNx));
    XSFLIB_CHECK(every.bank_slots == 4 && every.actor_player && every.pan && every.release_priority_fix);
    XSFLIB_CHECK(HeldValuesOf(file).bank_slots == 1 && !HeldValuesOf(file).actor_player);
    XSFLIB_CHECK(HeldValuesOf(file).pan && HeldValuesOf(file).release_priority_fix);
    XSFLIB_CHECK(HeldValuesOf(middle).pan && !HeldValuesOf(middle).release_priority_fix);
    XSFLIB_CHECK(!HeldValuesOf(old_file).pan && !HeldValuesOf(old_file).release_priority_fix);
}

} // namespace

int main()
{
    TestValues(Flavor::kNx);
    TestValues(Flavor::kCafe);
    TestBankSlots(Flavor::kNx);
    TestBankSlots(Flavor::kCafe);
    TestPlaces(Flavor::kNx);
    TestPlaces(Flavor::kCafe);
    TestNewPlaces(Flavor::kNx);
    TestNewPlaces(Flavor::kCafe);
    TestMovedFiles(Flavor::kNx);
    TestMovedFiles(Flavor::kCafe);
    TestRefusals();
    Test3ds();
    TestWii();

    return xsflib::testing::Summary("tables");
}
