// SPDX-License-Identifier: MIT

// Hokuto's files (.hok) in memory, and their binary form. A file is a sound archive in Hokuto's form. Its sequences are
// commands in ntdlib's common form, with values in ntdlib's units, the same for every console. The archive that the
// file came from stays as the file's native layout: ntdlib's pieces of it. The pieces name the sequences and keep the
// bytes that Hokuto doesn't model yet. The sequences' commands keep the encoder's choices that the console's bytes held
// (ntdlib::commands::Encoding). The binary form stores those choices in the native layout. docs/format.md describes the
// binary form and the text form.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/sar/bank_file.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/layout.h"
#include "ntdlib/sound_build.h"

#include "format/shared.h"

namespace hokuto::format
{

// The rules that play a file's sounds: one console's, the Switch's, the Wii U's, the 3DS's or the Wii's. A sound can
// play on another engine than its file's (File::sound_engines).
enum class Engine : uint8_t
{
    kSwitch,
    kWiiU,
    k3ds,
    kWii,
};

// The engine of the archives of `flavor`: the Switch's for kNx, the Wii U's for kCafe and the 3DS's for kCtr.
Engine EngineOf(ntdlib::sar::Flavor flavor);

// The flavor of the archives of `engine`'s console, or nothing for the Wii. Its archives are of another family.
std::optional<ntdlib::sar::Flavor> FlavorOf(Engine engine);

// How the engine mixes: as the console does, or with Hokuto's enhancements.
enum class Mixing : uint8_t
{
    kStrict,
    kEnhanced,
};

// The effects that an aux bus can hold. kI3dl2Reverb is the Switch's I3DL2 reverb. The game runs it on what an aux
// effect reads from the bus. The next four are effects of the Wii U: nw::snd's FxReverbHi, FxReverbStd, FxChorus and
// FxDelay. These classes run the system library's effects on AX's aux buses. The next six are effects of the Wii, from
// the SDK's AXFX library as NW4R SND's classes set them up: FxReverbHi of the builds before 2008 and of 2008, FxChorus,
// FxDelay, and FxReverbStd before 2008 and of 2008. The last five are the effects that the 3DS's sound libraries run on
// the ARM11 over an aux bus: nw::snd's FxReverb and FxDelay, nn::snd's FxReverb and FxDelay, and the version of
// nn::snd's FxReverb that games of 2011 link. A file may hold an effect of any console. A player plays only the effects
// of the file's engine.
enum class EffectKind : uint8_t
{
    kI3dl2Reverb,
    kReverbHi,
    kReverbStd,
    kChorus,
    kDelay,
    kWiiReverbHi,
    kWiiReverbHi2008,
    kWiiChorus,
    kWiiDelay,
    kWiiReverbStd,
    kWiiReverbStd2008,
    kCtrReverb,
    kCtrDelay,
    kCtrNnReverb,
    kCtrNnDelay,
    kCtrNnReverb2011,
};

// An effect on an aux bus: its kind, and its parameters in the order of the console's set of them. An I3DL2 reverb has
// the 12 parameters of nn::audio's I3dl2ReverbParameterSet. An effect of the Wii U or the Wii has the fields of its
// class's parameter structure. A reverb's modes are whole numbers. An effect of the 3DS has the fields of its class's
// Param, with a reverb's filter sizes in place of the pointer to them. Its times and sizes are whole numbers, and its
// switches 0 or 1.
struct Effect
{
    friend bool operator==(const Effect&, const Effect&) = default;

    EffectKind kind = EffectKind::kI3dl2Reverb;
    std::vector<float> parameters;
};

// The aux buses: A, B and C.
inline constexpr std::size_t kAuxBuses = 3;

// The highest volume of an aux bus, as a gain: about +6 dB. AX's return volume holds a gain just below 2.
inline constexpr float kMaxAuxVolume = 2.0f;

// A sound of a native archive's tables, with the values that ntdlib reads, in the console's units. A 3DS sound's
// player, player priority and actor player stay at their defaults. The pieces and the fields hold the sound's file and
// where it starts.
struct NativeSound
{
    friend bool operator==(const NativeSound&, const NativeSound&) = default;

    std::string name;
    ntdlib::sar::SoundType kind = ntdlib::sar::SoundType::kUnknown;
    uint32_t player = 0; // the player's item ID
    uint8_t volume = 127;
    uint8_t pan_mode = 0;
    uint8_t pan_curve = 0;
    uint8_t player_priority = 64;
    uint8_t actor_player = 0;

    // A sequence sound's values. Another sound keeps the defaults.
    std::vector<uint32_t> banks; // the item IDs in the bank slots, or 0xFFFFFFFF for an empty slot
    uint32_t tracks = 1;         // the tracks that the sound allocates, a bit each
    uint8_t channel_priority = 64;
    bool release_priority_fix = false;
};

// A player of a native archive's tables: its name, and how many sounds it plays at once.
struct NativePlayer
{
    friend bool operator==(const NativePlayer&, const NativePlayer&) = default;

    std::string name;
    uint32_t sound_limit = 0;
};

// What ntdlib reads of a native archive's tables when it stops at the first problem. An item's place in its list gives
// its item ID.
struct NativeTables
{
    friend bool operator==(const NativeTables&, const NativeTables&) = default;

    std::vector<NativeSound> sounds;
    std::vector<std::string> banks;
    std::vector<std::string> wave_archives;
    std::vector<NativePlayer> players;
    std::vector<std::string> groups;
};

// The sizes of the game's archive when Hokuto imported it: the archive's size in bytes, and the size of the files that
// each group of its tables loads, in the order of the groups. A game loads each group into its sound heap, and the
// game's archive fits the heap. An export measures how much the archive and its groups grow from these sizes. An
// archive without tables has no groups' sizes. A group that an edit adds has a size of 0: the game has no such group.
struct GameSizes
{
    friend bool operator==(const GameSizes&, const GameSizes&) = default;

    uint64_t archive = 0;
    std::vector<uint64_t> groups;
};

// A stream of a native archive: a stream file that the archive's file table names outside the archive, such as
// stream/BGM_TITLE.bfstm. A game keeps its streams in files beside its archive. A Hokuto file keeps each one whole, and
// whether the game's file holds the same bytes. An export writes only the streams that the game lacks.
struct NativeStream
{
    friend bool operator==(const NativeStream&, const NativeStream&) = default;

    Shared<std::vector<uint8_t>> bytes = std::vector<uint8_t>();
    bool from_game = true;
};

// A group file that the game keeps beside the archive, cut into ntdlib's pieces as the archive is
// (ntdlib::sar::GroupFileLayout). Its sequence pieces name the file's sequences. A copy there of one of the archive's
// sequence files therefore follows the edits of its sequence. The group's index in the archive's group table names the
// group file. The game's file stays known by its size and its SHA-1. An export writes a group file whose bytes differ
// from the game's.
struct NativeGroupFile
{
    friend bool operator==(const NativeGroupFile&, const NativeGroupFile&) = default;

    std::optional<uint32_t> group; // none when Hokuto couldn't find the group
    uint64_t game_size = 0;
    std::array<uint8_t, 20> game_hash{};
    Shared<std::vector<ntdlib::sar::Piece>> pieces = std::vector<ntdlib::sar::Piece>();
    std::vector<ntdlib::sar::Field> fields;
};

// The archive that a file came from, cut into ntdlib's pieces, with the fields of its tables that hold offsets and
// sizes. A sequence piece names one of the file's sequences. The group files that the game keeps beside the archive
// come in the order that ntdlib found them. The tables hold the values that an export writes into the archive. An
// archive whose tables ntdlib can't read without a problem has none. The game's sizes stay as the import measured them,
// whatever the edits change. A file saved before Hokuto kept them has none. The pieces hold most of the archive's
// bytes, and the group files' pieces most of theirs. The copies of an archive share them until an edit changes them.
struct NativeArchive
{
    friend bool operator==(const NativeArchive&, const NativeArchive&) = default;

    ntdlib::sar::Flavor flavor = ntdlib::sar::Flavor::kNx;
    Shared<std::vector<ntdlib::sar::Piece>> pieces = std::vector<ntdlib::sar::Piece>();
    std::vector<ntdlib::sar::Field> fields;
    std::vector<NativeGroupFile> group_files;
    std::optional<NativeTables> tables;
    std::optional<GameSizes> game_sizes;
    std::map<uint32_t, NativeStream> streams; // by the stream file's ID in the archive's file table
    bool wii = false; // an archive of the Wii (RSAR). ntdlib's rsar module cuts it. `flavor` means nothing then.

    // On the Wii, the game's build of NW4R SND, from the game's main.dol. Without it, the archive's version gives the
    // build (BuildOfWiiArchive).
    std::optional<ntdlib::SndBuild> wii_build = std::nullopt;
};

// A wave of a wave archive: the wave archive's file ID in the archive's file table, and the wave's index among the wave
// archive's waves.
struct WavePlace
{
    friend auto operator<=>(const WavePlace&, const WavePlace&) = default;

    uint32_t file = 0;
    uint32_t index = 0;
};

// A wave that the user edited: its encoding, the rate that the console plays it at, its loop and the samples that it
// holds. The loop's start and end count samples. A wave plays up to its loop's end, with a loop or without one. The
// samples after it stay in the wave where the user moved the end earlier. A wave from a WAV file, or one that took
// another encoding, also keeps its samples as PCM16: each channel's, as many as the wave holds. Its encoded samples
// then come from them. A wave without them keeps the samples that its wave archive held, in their encoding. The wave
// archive in the archive holds the wave that the model gives (edit/waves.h).
struct WaveModel
{
    friend bool operator==(const WaveModel&, const WaveModel&) = default;

    ntdlib::sar::WaveEncoding encoding = ntdlib::sar::WaveEncoding::kDspAdpcm;
    uint32_t rate = 0;
    bool loop = false;
    uint32_t loop_start = 0;
    uint32_t loop_end = 0;
    uint32_t length = 0; // the samples in each channel
    std::vector<std::vector<int16_t>> samples;
};

// The variables that a game sets through a sound's handle, by their numbers in the commands: nn::atk's player variables
// 0 to 15 and its global variables 16 to 31. Each starts at -1 until the game or a command sets it.
inline constexpr int kSceneVariables = 32;

// A change of a variable at a set time while a sound plays, such as the one that a game makes when a race's last lap
// starts. The time counts milliseconds from the sound's start. The engine makes the change at the start of its first
// frame that starts at that time or later.
struct VariableChange
{
    friend bool operator==(const VariableChange&, const VariableChange&) = default;

    uint32_t time = 0;
    uint8_t variable = 0;
    int16_t value = -1;
};

// A scene of a sound: a name, the values that the sound's variables start with, and their changes at set times. A game
// sets them the same way, through the sound's handle. A variable that the scene doesn't start starts at -1. The changes
// come in the order of their times. Two changes at the same time take effect in their order.
struct Scene
{
    friend bool operator==(const Scene&, const Scene&) = default;

    uint32_t sound = 0; // the sound's index in the archive's sound table
    std::string name;
    std::map<uint8_t, int16_t> values; // by the variables' numbers
    std::vector<VariableChange> changes;
};

// A track of a sound: the sound's index in the archive's sound table, and the track's number in its sequence.
struct TrackPlace
{
    friend auto operator<=>(const TrackPlace&, const TrackPlace&) = default;

    uint32_t sound = 0;
    uint8_t track = 0;
};

// A colour that the user gave a track, in 8-bit sRGB.
struct Colour
{
    friend bool operator==(const Colour&, const Colour&) = default;

    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;
};

// The tags of a file or of a sound, such as its title: each key with its value. A key holds lower-case ASCII letters,
// digits and underscores, and starts with a letter. A value isn't empty.
using Tags = std::map<std::string, std::string>;

// The keys that the program offers, in its order. They mean what the same keys of PSF files' tags mean. A file can hold
// any other key too.
inline constexpr std::array<std::string_view, 8> kTagKeys = {"title",   "artist",    "game",   "year",
                                                             "comment", "copyright", "length", "fade"};

// The longest key of a tag, in bytes.
inline constexpr std::size_t kLongestTagKey = 64;

// The ticks where a sound's loop starts and ends.
struct LoopTicks
{
    friend bool operator==(const LoopTicks&, const LoopTicks&) = default;

    uint32_t start = 0;
    uint32_t end = 0;
};

// The lengths of a sound for players, in milliseconds: how long a player plays the sound before the fade, and how long
// the fade lasts. A sound that loops plays its loop `loops` times before the fade. `loop` then gives the ticks where
// the loop that the arrangement shows starts and ends, if the arrangement shows one. A sound that plays on past the
// measure's limit fades without whole loops, and `loops` is 0. A sound that ends has neither loops nor a fade. Its
// length runs to its end. A silent sound never makes a sound. Hokuto measures the lengths (song/lengths.h). The tags
// length and fade go before them.
struct SoundLength
{
    friend bool operator==(const SoundLength&, const SoundLength&) = default;

    uint32_t length = 0;
    uint32_t fade = 0;
    uint8_t loops = 0;
    bool silent = false;
    std::optional<LoopTicks> loop;
};

// A chunk that this version of Hokuto doesn't read. The file keeps it as it was.
struct OtherChunk
{
    friend bool operator==(const OtherChunk&, const OtherChunk&) = default;

    std::string id; // four ASCII characters
    std::vector<uint8_t> bytes;
};

// A controller lane of the Key Editor in a file's view: the kind of command that it shows, and the bus or the LFO that
// the command addresses.
struct ViewLane
{
    friend bool operator==(const ViewLane&, const ViewLane&) = default;

    ntdlib::commands::Kind kind = ntdlib::commands::Kind::kVolume;
    uint8_t index = 0;
};

// The highest number of a track in a file's view, and the tracks that a set of them can hold: a bit for each number.
inline constexpr uint8_t kLastViewTrack = 31;

// The window in front of the zone of a file's view that the mixer, the Key Editor and the List Editor share. One of
// them shows there at a time.
enum class ViewFront : uint8_t
{
    kMixer,
    kKeyEditor,
    kListEditor,
};

// The program's view of a sound of a file when the file was saved: the sound, the transport, the arrangement and the
// editors. The file opens as it was saved. Players and exports ignore the view. Positions count quarter notes from the
// sound's start. Sizes count points at a display scale of 1. Tracks go by their numbers in the sound's sequence.
struct EditorView
{
    friend bool operator==(const EditorView&, const EditorView&) = default;

    uint32_t sound = 0; // the sound's index in the archive's sound table
    float position = 0.0f;
    float cycle_start = 0.0f;
    float cycle_end = 0.0f;
    bool cycle = false;

    // The arrangement: the width of a beat, the height of a track, the quarter note at the left edge, the track at the
    // top, the selected track, and the tracks that are muted and soloed, a bit for each track's number.
    float beat_width = 0.0f;
    float track_height = 0.0f;
    float left = 0.0f;
    float top = 0.0f;
    std::optional<uint8_t> selected_track;
    uint32_t muted = 0;
    uint32_t soloed = 0;

    // The Key Editor: the width of a beat, the height of a key, the quarter note at the left edge, the key at the top,
    // the controller lanes from the top, and the track while the editor is open. The List Editor's track while it's
    // open. The window in front: an editor there that the view leaves closed gives way to the List Editor, then the Key
    // Editor, then the mixer.
    float key_beat_width = 0.0f;
    float key_height = 0.0f;
    float key_left = 0.0f;
    float key_top = 0.0f;
    std::vector<ViewLane> lanes;
    std::optional<uint8_t> key_editor_track;
    std::optional<uint8_t> list_editor_track;
    ViewFront front = ViewFront::kMixer;
};

// A run of bars of one length, in ticks.
struct BarRun
{
    friend bool operator==(const BarRun&, const BarRun&) = default;

    uint32_t length = 0;
    uint32_t count = 0;
};

// What the commands of a track of a timeline don't show (see ConverterHints). NintendoWare's MIDI converter wrote each
// bar of the MIDI file before the song's start as a block of commands at tick 0, without its rests. `opening` gives the
// number of commands in each such block, in order. A bar counts from 0 at the song's start. `kept_inline` lists the
// bars that the converter's rules would call but the converter wrote in the track, and `called` those that the rules
// would leave in the track but the converter called. Both lists rise.
struct TrackHints
{
    friend bool operator==(const TrackHints&, const TrackHints&) = default;

    uint8_t track = 0;
    std::vector<uint32_t> opening;
    std::vector<uint32_t> kept_inline;
    std::vector<uint32_t> called;
};

// What a song of NintendoWare's MIDI converter doesn't show in its commands: its bar lines, as runs of bars from the
// song's start, and its tracks' hints, in the order of their numbers. Bars of the last run's length go on past the runs
// to the end of every track. Hokuto's converter lays the song out again from its commands and these hints
// (edit/converter.h).
struct ConverterHints
{
    friend bool operator==(const ConverterHints&, const ConverterHints&) = default;

    std::vector<BarRun> bars;
    std::vector<TrackHints> tracks;
};

// How a file keeps its archive on disk. kNative keeps the game's archive as the native layout: the pieces of the
// archive's bytes. An export joins them again byte for byte. kModel keeps Hokuto's model of the archive's items, and an
// export lays the archive out as NintendoWare's tools do (format/model.h). In memory, a file of either form holds its
// native archive.
enum class ArchiveForm : uint8_t
{
    kNative,
    kModel,
};

// A Hokuto file. Copies of a file share its sequences and its native archive (see Shared). An edit of a sequence puts a
// new sequence in its place.
struct File
{
    friend bool operator==(const File&, const File&) = default;

    Engine engine = Engine::kSwitch;
    std::map<uint32_t, Engine> sound_engines; // the sounds on another engine, by index in the archive's sound table
    Mixing mixing = Mixing::kStrict;          // the mode of each sound that sound_mixing leaves out
    std::map<uint32_t, Mixing> sound_mixing;  // the sounds in another mode, by index in the archive's sound table
    std::array<std::vector<Effect>, kAuxBuses> aux_effects; // each bus's effects, in the order that they process it
    std::array<float, kAuxBuses> aux_volumes = {1.0f, 1.0f, 1.0f}; // each bus's gain as it joins the output
    std::vector<Scene> scenes;                                     // in the order that they were stored
    std::map<TrackPlace, Colour> track_colours;                    // the tracks that the user gave a colour
    Tags tags;                                                     // the file's tags
    std::map<uint32_t, Tags> sound_tags;     // each sound's tags, by the sound's index in the archive's sound table
    std::map<uint32_t, SoundLength> lengths; // the sounds' lengths, by their indexes in the archive's sound table
    std::vector<Shared<ntdlib::commands::Sequence>> sequences;
    Shared<NativeArchive> native;            // none for a file without a native archive
    ArchiveForm form = ArchiveForm::kNative; // how the file keeps `native` on disk
    std::vector<OtherChunk> other_chunks;    // in the order of the file that held them
    std::optional<EditorView> view;          // the program's view when the file was saved

    // The banks and the waves that the user edited, in Hokuto's model of them: a bank by its file's ID in the archive's
    // file table, and a wave by its place. Each copy of a bank's file in the archive holds the bank that the model lays
    // out, and each wave archive the waves that the models give (edit/banks.h and edit/waves.h). The other banks and
    // waves stay as the game's bytes.
    std::map<uint32_t, Shared<ntdlib::sar::BankFile>> banks;
    std::map<WavePlace, Shared<WaveModel>> waves;

    // The sequences that the program edits as timelines, by their indexes, with their converter's hints. The program
    // edits each other sequence as a program. Nothing when the file doesn't say: the program then sets each sequence's
    // mode from its commands (edit/timelines.h). A file on disk holds each sequence as the console's bytes hold it.
    // While the program edits a timeline, the sequence holds every bar in its track.
    std::optional<std::map<uint32_t, ConverterHints>> timelines;
};

// `sequences` as a file's sequences, each one shared.
std::vector<Shared<ntdlib::commands::Sequence>> ShareSequences(std::vector<ntdlib::commands::Sequence> sequences);

// Copies of the values of `file`'s sequences, for ntdlib's functions that take the sequences in one array.
std::vector<ntdlib::commands::Sequence> CopySequences(const File& file);

// The version that Write gives a file. Read takes every file of the major version. A minor version adds what older
// readers can skip. The version stays 1.0 until the public release.
inline constexpr uint16_t kMajorVersion = 1;
inline constexpr uint16_t kMinorVersion = 0;

// The first bytes of a file in the binary form.
inline constexpr std::array<uint8_t, 8> kSignature = {0x89, 'H', 'O', 'K', 0x0D, 0x0A, 0x1A, 0x0A};

// The chunk that holds the native archive's group files, in both forms.
inline constexpr std::string_view kGroupFilesChunk = "GRPF";

// The bytes of the GRPF chunk of `file`'s native archive: the number of group files, then each one's group, the game's
// size and SHA-1, its pieces and its fields. A sequence's encodings go with its first piece, and NATV comes first.
std::vector<uint8_t> GroupFilesChunk(const File& file);

// The group files that the GRPF chunk `chunk` holds. Their sequence pieces name `sequences`, and take any encodings
// that the chunk holds. A chunk in the form of an earlier Hokuto, the group files whole, gives each group file as one
// run of bytes. Throws xsflib::FormatError with a message for the user if the group files run past the chunk's end, or
// if a piece or a field is damaged.
std::vector<NativeGroupFile> ReadGroupFiles(std::span<const uint8_t> chunk,
                                            std::vector<ntdlib::commands::Sequence>& sequences);

// The chunk that holds the native archive's streams, in both forms.
inline constexpr std::string_view kStreamsChunk = "STRM";

// The bytes of the STRM chunk of `native`: the number of streams, then each one's file ID, its flags, its size and its
// bytes, in the order of the file IDs. Flag 1 marks a stream that the game lacks.
std::vector<uint8_t> StreamsChunk(const NativeArchive& native);

// The chunks that hold the models of the banks and the waves that the user edited, in both forms.
inline constexpr std::string_view kBanksChunk = "BANK";
inline constexpr std::string_view kWavesChunk = "WAVE";

// The bytes of the BANK chunk of `file`: the number of banks, then each one's file ID and its model, in the order of
// the file IDs. Throws std::invalid_argument for a bank that the chunk can't hold.
std::vector<uint8_t> BanksChunk(const File& file);

// The banks that the BANK chunk `chunk` holds. Throws xsflib::FormatError with a message for the user if they run past
// the chunk's end, if their file IDs don't rise, if a value is out of its range, or if a bank can't be laid out as
// NintendoWare's tools lay one out.
std::map<uint32_t, Shared<ntdlib::sar::BankFile>> ReadBanks(std::span<const uint8_t> chunk);

// The bytes of the WAVE chunk of `file`: the number of waves, then each one's place and its model, in the order of the
// places. Throws std::invalid_argument for a wave that the chunk can't hold.
std::vector<uint8_t> WavesChunk(const File& file);

// The waves that the WAVE chunk `chunk` holds. Throws xsflib::FormatError with a message for the user if they run past
// the chunk's end, if their places don't rise, or if a wave has an encoding or a flag that a later version of Hokuto
// set.
std::map<WavePlace, Shared<WaveModel>> ReadWaves(std::span<const uint8_t> chunk);

// What's wrong with `wave`, a wave's model, or nothing. Its encoding is DSP-ADPCM, PCM16 or PCM8, and its rate is above
// 0. It holds a sample at least. Its loop ends after its start, at its last sample or before it. A wave without a loop
// starts its loop at 0. Its PCM16 samples, where it holds them, take one or two channels, each of the wave's length.
std::optional<std::string> WaveModelProblem(const WaveModel& wave);

// The streams that the STRM chunk `chunk` holds. Throws xsflib::FormatError with a message for the user if they run
// past the chunk's end, if their file IDs don't rise, or if a stream has a flag that a later version of Hokuto set.
std::map<uint32_t, NativeStream> ReadStreams(std::span<const uint8_t> chunk);

// The binary form of `file`. Throws std::invalid_argument for a file that the form can't hold, such as one whose pieces
// name a sequence that it lacks.
std::vector<uint8_t> Write(const File& file);

// Reads a file in the binary form. Throws xsflib::FormatError with a message for the user for bytes that aren't such a
// file, a damaged file, and a file of a later major version.
File Read(std::span<const uint8_t> bytes);

// Whether `bytes` start as a file in the binary form.
bool IsBinary(std::span<const uint8_t> bytes);

// Whether `item` holds the encoder's choices: its Encoding, but for a kOther command's opcode. The opcode is the
// command. The choices belong to the native archive's sequence file of the item's sequence.
bool HasChoices(const ntdlib::commands::Command& item);

// The first of `file`'s sequences that holds the encoder's choices and that no sequence file of the native archive or
// its group files names, or nothing. Neither form can hold such a sequence.
std::optional<std::size_t> UnfiledChoices(const File& file);

// What's wrong with `effect`, or nothing. An effect has as many parameters as its kind has, and each one is a finite
// number. A mode is one of its choices (see ChoicesOf).
std::optional<std::string> EffectProblem(const Effect& effect);

// What's wrong with `volume`, an aux bus's volume, or nothing. A volume is a finite number from 0 to kMaxAuxVolume.
std::optional<std::string> AuxVolumeProblem(float volume);

// What's wrong with `scene`, or nothing. A scene sets variables 0 to 31 only, and its changes come in the order of
// their times.
std::optional<std::string> SceneProblem(const Scene& scene);

// What's wrong with the game's sizes of `native`, or nothing. They give a size for each group of the tables, and none
// without tables.
std::optional<std::string> GameSizesProblem(const NativeArchive& native);

// `file` with tag `key` set to `value`: a tag of sound `sound`, or of the file without a sound. An empty value removes
// the tag. A sound whose last tag goes leaves sound_tags.
File WithTag(File file, std::optional<uint32_t> sound, const std::string& key, const std::string& value);

// The mixing mode of sound `sound` of `file`: the sound's mode in sound_mixing, or else the file's.
Mixing MixingOf(const File& file, uint32_t sound);

// `file` with sound `sound` in mixing mode `mixing`. sound_mixing holds the sound only when its mode differs from the
// file's.
File WithMixing(File file, uint32_t sound, Mixing mixing);

// `file` with every sound in mixing mode `mixing`.
File WithMixing(File file, Mixing mixing);

// Why Hokuto can't play the sounds of a file of `engine` in mixing mode `mixing`, or nothing when it can. AX's
// microcode mixes the Wii's sounds. Hokuto has no enhanced mixing for the Wii yet. A file can still hold a mode that
// Hokuto refuses.
std::optional<std::string> MixingRefused(Engine engine, Mixing mixing);

// The engine that plays sound `sound` of `file`: the sound's engine in sound_engines, or else the file's.
Engine EngineOf(const File& file, uint32_t sound);

// `file` with sound `sound` on `engine`. sound_engines holds the sound only when its engine differs from the file's.
// Hokuto plays a sound on another engine as that console plays the sound's conversion for it (edit/other_console.h).
// The archive keeps the sound for the file's engine.
File WithEngine(File file, uint32_t sound, Engine engine);

// What's wrong with `length`, a sound's lengths, or nothing. Only a sound with loops has a loop's ticks, and its loop
// ends after its start.
std::optional<std::string> LengthProblem(const SoundLength& length);

// `after`, the file after an edit of `before`, without its lengths when the edit can change what a sound plays: when it
// changes the engine, the mixer, a sequence or the native archive. Hokuto then measures the lengths again
// (song/lengths.h). The lengths come from strict mixing, and a change of a mixing mode keeps them.
File WithoutStaleLengths(const File& before, File after);

// What's wrong with a tag of key `key` and value `value`, or nothing. A key holds 1 to kLongestTagKey lower-case ASCII
// letters, digits and underscores, and starts with a letter. A value isn't empty.
std::optional<std::string> TagProblem(std::string_view key, std::string_view value);

// What's wrong with the timelines of `file`, or nothing. Each names a sequence of the file. Its runs of bars have a
// length and a number of bars above 0. Its tracks' hints come in the order of their tracks' numbers, a track once at
// most, and each list of bars rises.
std::optional<std::string> TimelinesProblem(const File& file);

// What's wrong with `view`, or nothing. Its numbers are finite, and its sizes above 0. Its tracks' numbers run from 0
// to kLastViewTrack.
std::optional<std::string> ViewProblem(const EditorView& view);

// What's wrong with `field` of `native` with the sequences `sequences`, or nothing. A field lies inside a run of bytes.
// Its places name a byte of a run, the start of any piece, an item of a sequence file, or the archive's end. A place at
// the end can count bytes past it. A field can also name two items of one sequence, without a piece
// (ntdlib::sar::Place::sequence).
std::optional<std::string> FieldProblem(const NativeArchive& native,
                                        std::span<const ntdlib::commands::Sequence> sequences,
                                        const ntdlib::sar::Field& field);

// What's wrong with `field` among `pieces`, the pieces of an archive or of a group file, or nothing, by the rules of
// the native archive's fields.
std::optional<std::string> FieldProblem(std::span<const ntdlib::sar::Piece> pieces,
                                        std::span<const ntdlib::commands::Sequence> sequences,
                                        const ntdlib::sar::Field& field);

// The CRC-32 of `bytes`: the CRC that zlib and PNG use.
uint32_t Crc32(std::span<const uint8_t> bytes);

} // namespace hokuto::format
