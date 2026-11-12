// SPDX-License-Identifier: MIT

// A sequence sound of a sound archive as the arrangement and the mixer show it: its tempo, its loop, and each track's
// notes, instrument, volume, pan and sends. A sequence with a loop shows its intro and one pass of the loop. Each note
// leads to the command that started it, for an edit. The sequence file's labels name the subroutines that calls play. A
// count of the file's calls shows which subroutines play from more than one place.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sound_build.h"

namespace hokuto::song
{

class DriverRom;
class DriverRules;
struct DsArchive;
struct DseSet;
struct GbaRom;
struct BrownieRom;
struct KonamiRom;
struct RareRom;
struct QuintetRom;
struct KrawallRom;
struct Rd2Rom;
struct UbiMilanRom;

// A stretch of a track's commands that ran one after another: from the track's start, a jump, a call, a return or a
// loop's end to the next.
using Stretch = ntdlib::sequence_export::Stretch;

// The `command` of a note that no command started.
inline constexpr uint32_t kNoCommand = 0xFFFFFFFF;

// Times count ticks from the sequence's start. `command` is the offset of the note command that started the note, in
// the bytecode of the sound's sequence file. `stretch` is the stretch of the track's commands that ran it, in
// Track::stretches. `bank_slot` and `program` give the instrument that the note plays: a slot of the sound's banks, and
// a program of the bank in that slot. On Konami's driver, `command` can be the offset of a volume command: the command
// starts a PSG note after a note command at volume 0 (ReadSequence).
struct Note
{
    friend bool operator==(const Note&, const Note&) = default;

    uint32_t start = 0;
    uint32_t length = 0;
    uint8_t key = 60;
    uint8_t velocity = 127;
    uint32_t command = 0;
    uint32_t stretch = 0;
    uint8_t bank_slot = 0;
    uint32_t program = 0;

    // On the GBA: the PSG channel that the note's voice plays on, 1 to 4, or 0 for a sample. On the DS: the channel
    // that plays the note's PSG voice, less 7: 1 to 6 for the square waves of channels 8 to 13, and 7 and 8 for the
    // noise of channels 14 and 15. Another note has 0. On the GBA, the DS and DSE: whether a later note took the note's
    // channel, or on DSE its last voice, while the driver still held the note. The note then ends there.
    uint8_t psg = 0;
    bool cut = false;

    // On the DS: the driver's channel that plays the note, or -1.
    int16_t channel = -1;

    // On Konami's driver: the track whose commands hold the note's command, or -1 on the note's track. `stretch` counts
    // in that track's stretches. A PSG track off the centre, and some notes of the revision without a mixer, play on
    // the next track.
    int8_t command_track = -1;

    // On Konami's driver: the semitones that the command of a sample's note adds to the sample's pitch. Nothing on a
    // PSG note. `program` is the sample.
    std::optional<int8_t> sample_semitones = std::nullopt;

    // On Konami's driver: the note's key in the song's MIDI file. The note of a sample is at the pitch of the sample in
    // `key`, plus the semitones of the command. The MIDI file lays the notes of a sample out by the song's other notes,
    // and a move of a note there can move the sample's other notes too.
    uint8_t midi_key = 60;

    // Whether the note starts in the tick where `command` ran. On Konami's driver, it needn't: another command can
    // start the note, and a note starts at the end of the frame, after the track's other commands of that frame. On
    // Krawall, a note delay starts the note later in its row. The waits give each command's tick there
    // (edit::CommandRows).
    bool start_gives_tick = true;
};

// A setting of a track's mix: its volume, its pan, or its send to one of the aux buses A to C.
using Mix = ntdlib::sequence_export::Mix;

// The number of aux buses that a track sends to.
inline constexpr int kSendCount = 3;

// The value that a track's commands give a setting from `tick` on.
struct Setting
{
    friend bool operator==(const Setting&, const Setting&) = default;

    uint32_t tick = 0;
    int value = 0;
};

struct Track
{
    friend bool operator==(const Track&, const Track&) = default;

    int number = 0;   // the sequence's track, from 0 to 15
    std::string name; // "Track" and the number, as in the converters' MIDI files
    std::string bank; // the bank of the track's first note, or empty
    int program = -1; // the program of the track's first note, or -1
    std::vector<Note> notes;
    std::vector<Setting> volume;                        // the track's volume, from 0 to 255, or to 127 on the GBA
    std::vector<Setting> pan;                           // the track's pan, from -64 (left) to 63 (right)
    std::array<std::vector<Setting>, kSendCount> sends; // the track's sends to the aux buses, from 0 to 255
    std::vector<Stretch> stretches;                     // in the order that they ran
    int channel = -1; // on DSE, the driver's channel that plays the track: tracks of one channel share their settings

    // On the GBA, the DS and DSE, the notes that the driver never started, in the order of their ticks. On the GBA, no
    // channel took them, their voice plays nothing, or a later note took their channel before they sounded. On the DS,
    // no channel took them, or their program plays nothing at their key. On DSE, no voice took them, or their program
    // plays nothing at their key and velocity. Each lasts the length of its command, and a tie lasts 0 ticks. On the
    // DS, so does a note of length 0. Such a note lasts until its track releases it.
    std::vector<Note> unstarted;
};

struct Tempo
{
    friend bool operator==(const Tempo&, const Tempo&) = default;

    uint32_t tick = 0;
    double bpm = 120.0; // quarter notes a minute
};

// The range that the sequence plays over and over once it reaches `end`.
struct Loop
{
    friend bool operator==(const Loop&, const Loop&) = default;

    uint32_t start = 0;
    uint32_t end = 0;
};

// The variables that a game can set through a sound's handle, by their numbers in the commands: 0 to 15 are the
// player's variables, and 16 to 31 the global ones. Each starts at -1 until the game or a command sets it. On the Wii,
// a global variable starts at 0 (SeqPlayer::SetGlobalVariable).
inline constexpr int kGameVariables = 32;
inline constexpr int kFirstGlobal = 16;

// The values that a sound's variables take before it starts, by their numbers. A game sets them through the sound's
// handle. The variables that aren't given start at -1.
using StartValues = std::map<int, int16_t>;

// A game's change of a variable while the sound plays, through the sound's handle: variable `variable` takes `value` at
// `time`, in milliseconds from the sound's start.
struct VariableChange
{
    friend bool operator==(const VariableChange&, const VariableChange&) = default;

    uint32_t time = 0;
    int variable = 0;
    int16_t value = -1;
};

// A sound's changes, in the order of their times. Changes at the same time come in their order here.
using Changes = std::vector<VariableChange>;

// The length of a console's sound frames in milliseconds: the fraction `numerator` / `denominator`.
struct FrameMs
{
    uint64_t numerator = 5;
    uint64_t denominator = 1;
};

// The length of the sound frames of the console of `flavor`: 5 ms on the Switch, 240 samples at 48 kHz, and 3 ms on the
// Wii U, 144 samples at 48 kHz. On the 3DS, a frame lasts 160 samples at the DSP's rate of 268111856 / 8192 Hz:
// 81920000 / 16756991 ms, about 4.889 ms. ntdlib's converter gives the 3DS's frames the same length.
constexpr FrameMs FrameMsOf(ntdlib::sar::Flavor flavor)
{
    switch (flavor)
    {
    case ntdlib::sar::Flavor::kCafe:
        return {3, 1};
    case ntdlib::sar::Flavor::kCtr:
        return {81920000, 16756991};
    default:
        return {5, 1};
    }
}

// The length of the GBA's frames: 280,896 cycles at 16,777,216 Hz, or 548,625 / 32,768 ms.
inline constexpr FrameMs kGbaFrameMs{548625, 32768};

// The length of the Wii's sound frames: 3 ms, 96 samples at 32 kHz. NW4R SND runs a sound frame after each of AX's
// frames.
inline constexpr FrameMs kWiiFrameMs{3, 1};

// The length of the model's frames on the DS: two of the driver's frames, 349,184 of the system's clocks, in both
// modes.
inline constexpr FrameMs kDsFrameMs{174592000, 16756991};

// The frame that a change at `time` comes in, on a console whose frames last `frame_ms`, counted from 0 at the sound's
// start: the first frame that starts at that time or later. The change comes before the frame's ticks.
uint64_t ChangeFrame(uint32_t time, FrameMs frame_ms);

// The whole frames in `ms` milliseconds, on a console whose frames last `frame_ms`.
uint64_t FramesIn(uint64_t ms, FrameMs frame_ms);

// The options of ntdlib's performer for a game that starts the variables with `values` and makes `changes` while the
// sound plays, on the console of `flavor`. Values and changes for variables past the game's are left out. The changes
// come in the order of their times, each in its frame.
ntdlib::sequence_export::PerformOptions PerformOptionsOf(const StartValues& values, const Changes& changes,
                                                         ntdlib::sar::Flavor flavor);

// The same options on a console whose frames last `frame_ms`. The sound's first tick plays in frame `first_tick` there.
ntdlib::sequence_export::PerformOptions PerformOptionsOf(const StartValues& values, const Changes& changes,
                                                         FrameMs frame_ms, uint64_t first_tick);

// A variable whose starting value can change what a sound plays. The sound's commands read it, in a comparison or in
// another command's argument, before they set it. `written` says whether they change it at all.
//
// `choices` holds a value for each set of outcomes that the comparisons of the variable with fixed values can have, in
// order. Two values with the same outcomes take the same branches. Of those, the choice is the starting value, or else
// the value nearest 0, positive before negative. A variable whose exact value counts has no choices: a command takes
// its starting value as an argument, compares it with another variable or a random value, or works out a new value from
// it. Neither does a variable that no comparison tests against a fixed value.
struct Variable
{
    friend bool operator==(const Variable&, const Variable&) = default;

    int number = 0;
    std::vector<int> choices;
    bool written = false;
};

struct Sequence;

// A map of the ticks of a sound's arrangement to the ticks of its conversion for another console's engine. The
// conversion plays in place of the sound. Where the quarter notes of the conversion play at the times of those of the
// arrangement, within an eighth of a quarter note at each change of either tempo and at each end, the map scales the
// ticks by the two timebases. A song of MP2K counts 24 ticks to a quarter note, and its conversion 48. Elsewhere the
// conversion's quarter note has another length. On Konami's driver, a tick of the arrangement is one of the driver's
// frames, and the conversion's quarter note follows the beat of the song's MIDI file. The map then matches the ticks by
// time. It passes through the start of each note of the arrangement and the start of the conversion's note with the
// same key nearest the same time, within an eighth of a quarter note. It also passes through the starts and the ends of
// the two loops. Between two such points, a tick of one maps to the tick of the other at the same share of the time
// between them. Past the last point, a tick maps to the same time after that point. The default map scales by timebases
// of 1: each tick is itself.
class TickMap
{
public:
    friend bool operator==(const TickMap&, const TickMap&) = default;

    TickMap() = default;

    // The map of `shown`, a sound's arrangement, to `played`, the sound's conversion for another engine.
    TickMap(const Sequence& shown, const Sequence& played);

    // The tick of the conversion at tick `tick` of the arrangement, and the tick of the arrangement at tick `tick` of
    // the conversion. A tick before 0 scales by the timebases.
    double Played(double tick) const;
    double Shown(double tick) const;

    // Whether the map matches the ticks by time.
    bool ByTime() const;

private:
    // A timebase in ticks a quarter note, and the changes of the tempo.
    struct Timing
    {
        friend bool operator==(const Timing&, const Timing&) = default;

        int timebase = 1;
        std::vector<Tempo> tempo;
    };

    // A tick of the arrangement and the tick of the conversion that it maps to.
    struct Point
    {
        friend bool operator==(const Point&, const Point&) = default;

        double shown = 0.0;
        double played = 0.0;
    };

    // The tick of `to` at tick `tick` of `from`, through `points`, in the order of `from`'s ticks.
    static double Through(const Timing& from, const Timing& to, const std::vector<Point>& points, bool shown,
                          double tick);

    // Whether every change of either tempo and each end keeps its quarter note within `reach` ticks of the conversion.
    bool KeepsQuarters(const Sequence& shown, const Sequence& played, double reach) const;

    // The points that the map passes through for certain, and the points of the notes that start within `reach` of
    // each other's times (TickMap's constructor).
    static std::vector<Point> FixedPoints(const Sequence& shown, const Sequence& played);
    std::vector<Point> NotePoints(const Sequence& shown, const Sequence& played, double reach) const;

    Timing shown_;
    Timing played_;
    std::vector<Point> points_; // empty where the map scales by the timebases
};

struct Sequence
{
    friend bool operator==(const Sequence&, const Sequence&) = default;

    std::string name;
    uint32_t sound = 0; // the sound's index in the archive's sound table
    int timebase = 48;  // ticks a quarter note
    std::vector<Tempo> tempo;
    uint32_t length = 0;    // the ticks to the end of the sequence or of its loop, and of its last note
    uint32_t notes_end = 0; // the tick at which the last note ends, before its release
    std::optional<Loop> loop;
    bool ds = false;    // a sequence of the DS
    bool holds = false; // the sound holds until the game stops it: the performer ended it 10 s into the hold
    bool gba = false;   // a song of a GBA ROM that MP2K's performer read
    const DriverRules* driver = nullptr; // a song of a GBA ROM that the performer of this driver read (song/driver.h)
    std::vector<Track> tracks;           // the tracks that ran commands, with notes or without, by their numbers
    std::map<uint32_t, std::string> labels; // the sequence file's labels, by their offsets in its bytecode

    // The number of call commands in the sequence file that go to each offset. Calls from the file's other sounds count
    // too. Calls that no sound can reach don't.
    std::map<uint32_t, int> calls;

    // The variables whose starting values can change what the sound plays, in the order of their numbers. The sequence
    // plays with `start_values`, and with `changes` while it plays. The tracks' notes and settings follow them, and so
    // does a model of the sequence. The arrangement's loop is then a pass that starts after the last change.
    std::vector<Variable> variables;
    StartValues start_values;
    Changes changes;

    // Every variable that the sound's commands read, in the order of their numbers. A change while the sound plays can
    // change what it plays through any of them. The choices take every comparison into account.
    std::vector<Variable> read_variables;

    // On the Wii, the build of NW4R SND whose rules the performer read the sequence by. A model of the sequence plays
    // by the same build.
    std::optional<ntdlib::SndBuild> wii_build;

    // For a conversion of a sound for another console's engine: the map of the ticks of the sound's arrangement to the
    // conversion's ticks. The program's playhead, its cycle and its seeks go through it (PlayedTickOf).
    std::optional<TickMap> arrangement_ticks;
};

// A run of a track's notes that the arrangement shows as one part: notes `first` to `first + count - 1`, from tick
// `start` to tick `end`. A call's part holds the notes of one call of a subroutine. `call` is then the subroutine's
// offset in the bytecode. The part also contains notes `unstarted_first` to `unstarted_first + unstarted_count - 1` of
// those that the driver never started (Track::unstarted). A part can hold only such notes.
struct Part
{
    uint32_t start = 0;
    uint32_t end = 0;
    std::size_t first = 0;
    std::size_t count = 0;
    std::optional<uint32_t> call;
    std::size_t unstarted_first = 0;
    std::size_t unstarted_count = 0;
};

// The parts of a track, on a grid of lines `grid` quarter notes apart. A part starts at the line before its first note
// and ends at the line after its last note. A gap of `gap` quarter notes or more between notes starts a new part. So do
// the loop's start and its end. A part never crosses either one. A call of a subroutine from outside any call makes a
// separate part, from the call to the return, with the notes of the calls inside it. A call that never returns ends at
// the line after its last note. Parts never overlap. The part before a call's part ends where the call starts. The
// notes that the driver never started (Track::unstarted) count among the notes. Such a note starts no part of a call.
// It joins the part of a call that it starts in, before the call returns. Otherwise it stays outside the calls.
std::vector<Part> SplitParts(const Sequence& sequence, const Track& track, int grid, int gap);

// The stretch of `track` that entered the call from outside any call that stretch `stretch` ran in, or nothing outside
// calls. A call's part (SplitParts) starts at that stretch.
std::optional<std::size_t> OutermostCall(const Track& track, std::size_t stretch);

// The value that a track's settings have at `tick`, or `fallback` before the first one.
int SettingAt(const std::vector<Setting>& settings, uint32_t tick, int fallback);

// The settings of `track` for `setting`, in the order of their ticks.
const std::vector<Setting>& MixSettings(const Track& track, Mix setting);

// The value of `setting` before a command sets it: a volume of 127, or 0 where tracks start silent (`starts_silent`),
// a pan of 0, and sends of 0. MP2K's tracks start silent, and so do those of the GBA's drivers that play a track at
// volume 0 until a command sets it.
int MixDefault(Mix setting, bool starts_silent = false);

// The volume of a DSE channel before a command sets it. ReadSequence shows it at tick 0.
inline constexpr int kDseDefaultVolume = 100;

// The stretches that `track` of `sequence` shows its mix from. They are the track's stretches, followed on DSE by those
// of the other tracks of its channel. Those tracks share the track's settings (Track::channel).
std::vector<Stretch> MixStretches(const Sequence& sequence, const Track& track);

// The stretches that `note` of `track` counts in (Note::stretch): those of the track that ran its command. A note of
// Konami's driver can play on the track after the track of its command.
const std::vector<Stretch>& StretchesOfNote(const Sequence& sequence, const Track& track, const Note& note);

// The value that `setting` of `track` has at `tick`.
int MixAt(const Track& track, Mix setting, uint32_t tick);

// The tempo at `tick`.
double TempoAt(const Sequence& sequence, uint32_t tick);

// The time of tick `tick` in milliseconds from the sequence's start, from the tempo of each stretch of ticks. Past the
// last change of the tempo, the ticks go on at the last tempo. TickOfMs is the inverse.
double MsOfTick(const Sequence& sequence, double tick);
double TickOfMs(const Sequence& sequence, double ms);

// The position of the sequence after the player has counted `ticks` ticks: `ticks` up to the loop's end, and then round
// the loop. The player counts every tick, and doesn't go back when the sequence loops.
uint32_t PositionOfTicks(const Sequence& sequence, uint64_t ticks);

// The tick of `played`, the sequence that a model plays for the sound that `shown` arranges, at tick `tick` of `shown`,
// and the tick of `shown` at tick `tick` of `played`. A conversion for another engine maps the ticks
// (Sequence::arrangement_ticks). Another version of the sound scales them by the two timebases.
double PlayedTickOf(const Sequence& shown, const Sequence& played, double tick);
double ShownTickOf(const Sequence& shown, const Sequence& played, double tick);

// The ticks that a model of `to` has counted where a model of `from` has counted `counted` ticks, both of them models
// that play the sound that `shown` arranges. The position in each pass of `from`'s loop maps to `to` through the ticks
// of `shown` (ShownTickOf and PlayedTickOf), and each pass adds the length of `to`'s loop. A catch-up to a model of
// another engine runs up to that count.
double CountedTicksOn(const Sequence& shown, const Sequence& from, const Sequence& to, double counted);

// `sequence` up to the end of the first pass of `loop`, with `loop` as its loop. The notes and the settings from the
// loop's end on go, and a note that still sounds there lasts to the loop's end. For a loop that the performer found a
// whole number of passes after `loop`. Each pass of such a loop runs the same commands.
Sequence FirstPass(Sequence sequence, Loop loop);

// The loop of `played`, a conversion for another engine of the sound that `shown` arranges, in its first pass. The
// performer can find a conversion's loop a whole number of passes late. Returns nothing where the performer found the
// loop at the time of the loop of `shown`, or where the times don't show a whole number of passes within an eighth of a
// quarter note.
std::optional<Loop> FirstLoopOfConversion(const Sequence& shown, const Sequence& played);

// Whether `a` and `b` play each tick at the same time: the same timebase, the same changes of the tempo and the same
// loop. A model of one then plays the same ticks in each frame as a model of the other. An edit of a tempo, or of a
// wait before a change of the tempo or the loop's end, moves the time of the ticks after it.
bool SameTiming(const Sequence& a, const Sequence& b);

// For each number of `shown`, a mask of the numbers of the tracks of `played` that play that track of `shown`. `played`
// is another console's conversion of a song of the GBA or a sequence of the DS or of DSE. Its tracks don't keep the
// numbers of the source: a track of the source without notes gets no number, and a track that plays several voices can
// get one for each. Each track of `played` is matched with the track of `shown` that plays most of its notes with the
// same key, within an eighth of a quarter note of the same place. A conversion can move a note that far: a conversion
// of DSE starts a tick's notes at the start of the driver's tick that runs it. The place comes from the map of the
// ticks (Sequence::arrangement_ticks, or else TickMap). Of two tracks that play as many, the one nearer the notes wins.
// A track that plays them as another plays them too is matched with the one that has no track yet, and then with the
// lower number. A track of `played` that shares no note matches none. A song of DSE can number its tracks up to 31.
std::array<uint32_t, 32> TracksOfConversion(const Sequence& shown, const Sequence& played);

// Runs sequence sound `sound` of `archive` through ntdlib's performer, with the settings of the archive's console
// (SettingsOf), the variables' starting values `values`, and the game's `changes`. Values and changes for variables
// past the game's are left out. Throws std::runtime_error with a message for the user if it isn't a sequence sound or
// its data is damaged.
Sequence ReadSequence(const ntdlib::sar::SoundArchive& archive, uint32_t sound, const StartValues& values = {},
                      const Changes& changes = {});

// The same for sequence sound `sound` of the Wii's `archive`, with the settings of `build` of NW4R SND, the game's
// build, or else of the build that the archive's version gives (SettingsOf). The labels, the counts of calls and the
// list of variables come from the sequence file, decoded by the rules of that build.
Sequence ReadSequence(const ntdlib::rsar::SoundArchive& archive, uint32_t sound, const StartValues& values = {},
                      const Changes& changes = {}, const std::optional<ntdlib::SndBuild>& build = {});

// Runs song `song` of the GBA's `rom` through ntdlib's performer, with one pass of its loop (song/gba.h). MP2K counts
// 24 ticks to a quarter note. Each note leads to its command, at its offset from the song's first byte
// (ntdlib::mp2k::DecodeSong). Every note plays the song's voice group: a note's program is its voice there. A song has
// no labels and no variables. Throws std::runtime_error with a message for the user if the song can't be read.
Sequence ReadSequence(const GbaRom& rom, int song);

// Runs sound `sound` of `rom` through ntdlib's performer of Konami's driver, with one pass of its loop (song/konami.h).
// One tick is one of the driver's frames. The performer sets the timebase to the number of frames nearest a quarter
// note, with the one tempo at which those frames last their real time. Each note leads to its command, at its offset
// from the song's first byte (ntdlib::konami::DecodeSong). A note's velocity comes from the command's volume
// (KonamiVelocityOf). A sample note's program is its sample. Another note's program is the duty or the wave of the last
// such command that its track ran. A track's volume is the commands' volume. The sequence contains no labels and no
// variables. Throws std::runtime_error with a message for the user if the song can't be read.
Sequence ReadSequence(const KonamiRom& rom, uint32_t sound);

// Runs sound `sound` of `rom` through ntdlib's performer of Nintendo R&D2's driver, with one pass of its loop
// (song/rd2.h). The driver counts 24 ticks to a quarter note, and its tempo commands give the tempo. Each note leads to
// its command, at its offset from the first byte of its sequence (ntdlib::rd2::DecodeRomSequence). A note's velocity
// comes from the command's velocity (Rd2VelocityOf). A note's program is 128 times its bank, among the driver's banks,
// plus its instrument. A track's volume is the commands' volume, and 128 before a command sets it. The sequence
// contains no labels and no variables. Throws std::runtime_error with a message for the user if the song can't be read.
Sequence ReadSequence(const Rd2Rom& rom, uint32_t sound);

// Runs sound `sound` of `rom` through ntdlib's performer of Brownie Brown's driver, with one pass of its loop
// (song/brownie.h). One tick is one of the driver's frames. The performer sets the timebase to the number of frames
// nearest a quarter note, with the one tempo at which those frames last their real time. Each note leads to its
// command, at its offset from the first byte of the region that contains the list of the sound
// (ntdlib::brownie::FindRegions). A note that no command started in its tick sounds again after the channel fell
// silent, and doesn't start in the tick where its command ran (Note::start_gives_tick). Each note shows at velocity
// 127: the driver's notes have none. A note's program is the last sample set, sample, duty or wave that its channel
// selected (BrownieProgramIndex), and a noise note shows at the byte of its command. A track's volume is the value of
// command F2, and 0 before a command sets it. The sequence contains no labels and no variables. Throws
// std::runtime_error with a message for the user if the sound can't be read.
Sequence ReadSequence(const BrownieRom& rom, uint32_t sound);

// Runs sound `sound` of `rom`, a piece of music, through ntdlib's performer of Ubisoft Milan's driver, with one pass of
// its loop (song/ubimilan.h). One tick is one of the console's frames. The performer sets the timebase to the number of
// frames nearest a quarter note, with the one tempo at which those frames last their real time. A track is one of the
// sequence's 16 channels, and contains every stretch of the piece's one track of commands. A note leads to its command,
// at its offset from the first byte of the piece's first sequence (ntdlib::ubimilan::DecodeSequence). A note of a later
// sequence of the piece's list leads to no command. A note has the velocity that the converter sets. A note's program
// is the kit's offset on channel 9, and a PSG channel's instrument on channels 0 to 2. The driver has no volume, pan or
// sends of a track. The sequence contains no labels and no variables. Throws std::runtime_error with a message for the
// user if the piece can't be read.
Sequence ReadSequence(const UbiMilanRom& rom, uint32_t sound);

// Runs sound `sound` of `rom` through ntdlib's performer of Rare's driver, with one pass of its loop (song/rare.h). The
// tune header sets the ticks of a quarter note, and the tempo commands of the tune set the tempo. Each note leads to
// its command, at its offset from the first byte of its sequence (ntdlib::rare::DecodeRomSequence). A note has the
// velocity of its command. A note's program is the program of the tune's program map. A track's volume is the volume of
// the channels that its commands set (controller 7), and 127 before a command sets it. The sequence contains no labels
// and no variables. Throws std::runtime_error with a message for the user if the song can't be read.
Sequence ReadSequence(const RareRom& rom, uint32_t sound);

// Runs sound `sound` of `rom` through ntdlib's performer of Quintet's driver, with one pass of its loop
// (song/quintet.h). The performer counts 37 ticks to one tick of the driver and 3552 to a quarter note, with the tempo
// that the song's commands set. A note of the performer is a run of frames in which its channel sounds. A command can
// therefore start several notes. Each note leads to its command, at its offset from the byte after the song's header
// (ntdlib::quintet::DecodeRomSequence). Every note has the velocity 127: the driver's notes have none. A note's program
// is the instrument that the commands of its channel selected last: the duty on a square, and the wave, the noise macro
// or the sample on the other channels. A track's volume is the channel's volume, and 0 before a command sets it. The
// sequence contains no labels and no variables. Throws std::runtime_error with a message for the user if the song can't
// be read.
Sequence ReadSequence(const QuintetRom& rom, uint32_t sound);

// Runs sound `sound` of `rom` through ntdlib's performer of Krawall, with one pass of its loop (song/krawall.h). The
// performer counts the ticks of the MIDI file of ntdlib's conversion. A tick of the player lasts the MIDI ticks that
// the timing of the conversion sets for it. Each of the module's first 16 channels is a track. Each note leads to the
// entry of its row, at its offset from the module's first pattern (ntdlib::krawall::DecodeRomSequence). Every note has
// the velocity 127: the player's notes have none. A note's program is the sample of the game's table that its
// instrument plays at its note. A track's volume runs from 0 to 64. The player keeps the volume of a channel in that
// range. The sequence contains no labels and no variables. Throws std::runtime_error with a message for the user if the
// piece can't be read.
Sequence ReadSequence(const KrawallRom& rom, uint32_t sound);

// Runs sound `sound` of the DS's `archive` through ntdlib's performer, with one pass of its loop, the build of the
// archive's driver, the sound's profile and the variables' starting values `values` (DsPerformOptions in song/ds.h).
// NITRO-Composer counts 48 ticks to a quarter note. Each note leads to its command, at its offset from the start of the
// bytecode of the sequence's file: its SSEQ, or the SSAR of a sequence archive's sequence. A track's volume runs from 0
// to 127, and the DS has no sends. A note's program is a program of the sequence's bank, in bank slot 0. An SSEQ has no
// labels. The calls and the variables come from the file's commands (ntdlib::ntr::snd::DecodeSequence). The read
// applies `changes` as the reads of the other consoles apply them. Throws std::runtime_error with a message for the
// user if the sound contains no sequence or can't be read.
Sequence ReadSequence(const DsArchive& archive, uint32_t sound, const StartValues& values = {},
                      const Changes& changes = {});

// Runs sound `sound` of the DSE sounds `set` through ntdlib's performer, with one pass of its loop and the build of the
// image's driver (song/dse.h). A song's settings give its ticks to a quarter note. The number of each track is its slot
// in the sequence. Each note leads to its event, at its offset from the start of the file of the song or of the bank of
// sound effects. The note belongs to the track whose events played it. A track's volume and pan are those of its
// channel, from 0 to 127 and from -64 to 63: tracks that share a channel show the same settings. DSE has no sends. A
// note's bank slot is the place among the sound's wave banks of the bank that its channel selects, and its program is a
// program of that bank. DSE has no labels, no calls and no variables. Throws std::runtime_error with a message for the
// user if the sound can't be read, or if it has more tracks than a model mutes and meters (SequenceModel::kTracks).
Sequence ReadSequence(const DseSet& set, uint32_t sound);

// The notes of a GBA song, a DS sound or a sound of DSE that collide on the console's channels: those that a later
// note cuts short (Note::cut), and those that the driver never starts (Track::unstarted).
struct Collisions
{
    friend bool operator==(const Collisions&, const Collisions&) = default;

    std::size_t cut = 0;
    std::size_t unstarted = 0;
};

// The collisions of `sequence`, a GBA song, a DS sound or a sound of DSE that ReadSequence returns.
Collisions CollisionsOf(const Sequence& sequence);

// A song whose notes collide more often in one ROM than in another, and the collisions that it adds.
struct AddedCollisions
{
    friend bool operator==(const AddedCollisions&, const AddedCollisions&) = default;

    int song = 0;
    Collisions added;
};

// The songs among `songs` that collide more often in `after` than in `before`: those that add notes that later notes
// cut short, or notes that the driver never starts. A song that either ROM can't read stays out.
std::vector<AddedCollisions> AddedGbaCollisions(const GbaRom& before, const GbaRom& after,
                                                const std::vector<int>& songs);

// The same for the DS's sounds. AddedCollisions::song contains the number of a sound. A sound that either archive can't
// read stays out.
std::vector<AddedCollisions> AddedDsCollisions(const DsArchive& before, const DsArchive& after,
                                               const std::vector<uint32_t>& sounds);

// The variables that a game can set and that the commands that the player reaches from item `start` of `sequence` read.
// `starting` holds those whose starting values can change what the sound plays (Sequence::variables), and `all` every
// variable that the commands read (Sequence::read_variables). The paths follow the tracks that the commands open. The
// global variables start at `global_start`: 0 on the Wii.
struct SoundVariables
{
    std::vector<Variable> starting;
    std::vector<Variable> all;
};
SoundVariables VariablesOf(const ntdlib::commands::Sequence& sequence, uint32_t start, int global_start = -1);

// The value that variable `number` of `sequence` starts with when the game sets none: -1, or 0 for a global variable on
// the Wii.
int StartingValue(const Sequence& sequence, int number);

} // namespace hokuto::song
