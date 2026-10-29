// SPDX-License-Identifier: MIT

// Tests the songs (src/song/): where a tick count lies in a looped sequence, the settings and the tempo at a tick, how
// a track's notes split into parts, the playback clock, reading a sequence on the reader's threads, playing it, the
// browser's previews of a wave and of an instrument's note, the group files beside an archive, and the variables that a
// game can set. A small archive made for the tests holds four sequences. One has an intro and a loop, one plays a note
// and ends, one plays a note with a slow release, and one plays a note panned to the left. Each plays on track 0 alone.
// The player's output has to match ntdlib's model run directly, frame for frame, from the start and from a seek. Track
// 0's meter has to match the output. With the cycle off, the output ends with the pass. Past the end of a sequence
// without a loop, the position goes on at the tempo at the end. A copy of the archive adds a sequence that sends its
// note to aux bus A. A reverb there plays it as in ntdlib's model. A copy for the Wii U plays it with each of the Wii
// U's four effects. On both consoles, a bus's volume scales what the bus adds to the output, and the bus's meter
// measures it. A Wii archive's sequence plays as ntdlib's model of the Wii run directly, with the model of AX's
// microcode in place of the DSP, and with each of the Wii's effects. A model that the program offers during playback
// takes over at the player's next frame. That frame crossfades from the old model's output to the new one's. While the
// render worker plays, a catch-up runs the new model up to the player's frame on a thread of its own. A model at
// another tempo takes over at the player's tick. A sequence plays with the values that its variables start with. Its
// output matches ntdlib's model with the same values. The starting values of the variables that a sound lists change
// what ntdlib's model plays, and those of the others don't. An export to MIDI writes the files of ntdlib's converter
// frame in the same places, with the changes at set times. It never writes over another export's samples. Its message
// about a folder that it can't make ends with one full stop. The dry render for the Export window's warnings matches
// ntdlib's model with the aux buses silent. A 3DS archive plays on ntdlib's model of the DSP's audio firmware.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "ntdlib/cafe/snd/engine.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/nx/atk/engine.h"
#include "ntdlib/nx/audio/i3dl2.h"
#include "ntdlib/rvl/snd/engine.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/decode.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/stream_file.h"
#include "ntdlib/sequence_export/consoles.h"
#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/sequence_export/frame.h"
#include "ntdlib/sequence_export/sfz.h"
#include "ntdlib/sound_build.h"
#include "ntdlib/testing/dol_files.h"
#include "ntdlib/testing/rsar_files.h"
#include "ntdlib/testing/sar_files.h"
#include "seqlib/midi/reader.h"
#include "seqlib/pcm.h"
#include "seqlib/sf2/reader.h"
#include "seqlib/wav.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/files.h"
#include "xsflib/rip/length.h"
#include "xsflib/testing/check.h"

#include "audio/frame_ring.h"
#include "audio/render_worker.h"
#include "song/archive.h"
#include "song/catch_up.h"
#include "song/dry_render.h"
#include "song/items.h"
#include "song/midi_export.h"
#include "song/player.h"
#include "song/preview.h"
#include "song/sequence.h"
#include "song/sequence_preview.h"
#include "song/sequence_reader.h"

using hokuto::song::AuxEffects;
using hokuto::song::AuxVolumes;
using hokuto::song::ChangeFrame;
using hokuto::song::Changes;
using hokuto::song::ConvertBank;
using hokuto::song::ConvertItemFile;
using hokuto::song::ConvertSequenceFile;
using hokuto::song::ConvertToMidi;
using hokuto::song::DecodeWaveSound;
using hokuto::song::DryRender;
using hokuto::song::ExportVersions;
using hokuto::song::FileVersions;
using hokuto::song::FolderFailure;
using hokuto::song::I3dl2Parameters;
using hokuto::song::I3dl2Reverb;
using hokuto::song::Item;
using hokuto::song::ItemFile;
using hokuto::song::ItemFileName;
using hokuto::song::ItemKind;
using hokuto::song::ItemName;
using hokuto::song::kAuxBuses;
using hokuto::song::kFullVolumes;
using hokuto::song::LoadArchive;
using hokuto::song::LoadWiiArchive;
using hokuto::song::Loop;
using hokuto::song::Mix;
using hokuto::song::MixAt;
using hokuto::song::Mixing;
using hokuto::song::ModelCatchUp;
using hokuto::song::MsOfTick;
using hokuto::song::Note;
using hokuto::song::NotePreview;
using hokuto::song::OpenArchive;
using hokuto::song::OtherConsole;
using hokuto::song::OtherConsoleFile;
using hokuto::song::OtherConsoleSequenceFile;
using hokuto::song::Part;
using hokuto::song::PlaceGroupFiles;
using hokuto::song::PlaybackClock;
using hokuto::song::PositionOfTicks;
using hokuto::song::ProgramsOf;
using hokuto::song::ReadSequence;
using hokuto::song::RenderDry;
using hokuto::song::ReportFor;
using hokuto::song::SameTiming;
using hokuto::song::Sequence;
using hokuto::song::SequenceModel;
using hokuto::song::SequencePlayer;
using hokuto::song::SequencePreview;
using hokuto::song::SequenceReader;
using hokuto::song::Setting;
using hokuto::song::SettingAt;
using hokuto::song::SoundFontNames;
using hokuto::song::SoundPreview;
using hokuto::song::SoundsSharingFile;
using hokuto::song::SplitParts;
using hokuto::song::StartValues;
using hokuto::song::TempoAt;
using hokuto::song::TickOfMs;
using hokuto::song::Track;
using hokuto::song::Variable;
using hokuto::song::VariableChange;
using hokuto::song::WaveArchivesOf;
using hokuto::song::WaveAsWav;
using hokuto::song::WaveChannels;
using hokuto::song::WaveCountOf;
using hokuto::song::WavePreview;
using hokuto::song::WaveSoundAsWav;
using hokuto::song::WaveSoundNote;
using hokuto::song::WaveSoundNoteOf;
using hokuto::song::WaveSoundSet;
using hokuto::song::WaveSoundSets;
using hokuto::song::WaveSoundWavFileName;
using hokuto::song::WiiWaveArchive;
using hokuto::song::WithMidiTags;
using hokuto::song::WriteBankSfz;
using hokuto::song::WriteMidiAndSfz;
using hokuto::song::WriteMidiAndSoundFont;

namespace
{

namespace atk = ntdlib::nx::atk;
namespace cafe = ntdlib::cafe::snd;
namespace export_ = ntdlib::sequence_export;
namespace sar = ntdlib::sar;
namespace sar_files = ntdlib::testing::sar_files;

// The test archive's sounds.
constexpr uint32_t kLoopSound = 0;
constexpr uint32_t kNoteSound = 1;
constexpr uint32_t kReleaseSound = 2;
constexpr uint32_t kPanSound = 3;

// The first sound that the test archive lacks. The archives of the effects' test, the variables' test, the changes'
// test, the instruments' test, the held note's test and the test of a new tempo add a sound there, and the archive of
// the calls' test adds two. The archive of MadeUpWaveSoundSpec adds a wave sound there.
constexpr uint32_t kMissingSound = 4;
constexpr uint32_t kWaveSound = 4;
constexpr uint32_t kSendSound = 4;
constexpr uint32_t kSlowSound = 4;
constexpr uint32_t kCallsSound = 4;
constexpr uint32_t kCallsAgainSound = 5;
constexpr uint32_t kVariablesSound = 4;
constexpr uint32_t kChangesSound = 4;
constexpr uint32_t kInstrumentsSound = 4;
constexpr uint32_t kHeldSound = 4;

// The sounds of the archive of the variables' paths (kVariablePathsBytecode).
constexpr uint32_t kSetInCallSound = 4;
constexpr uint32_t kSetBeforeWaitSound = 5;
constexpr uint32_t kWaitInCallSound = 6;
constexpr uint32_t kIdleLoopSound = 7;
constexpr uint32_t kCompareThenTransposeSound = 8;
constexpr uint32_t kRetryInCallSound = 9;
constexpr uint32_t kBranchesSound = 10;

// The stereo frames of a frame of the Switch's model, and the frame's length: 5 ms.
constexpr std::size_t kFrameSamples = ntdlib::nx::atk::ArchiveModel::kFrameSamples;
constexpr hokuto::song::FrameMs kFrameMs = hokuto::song::FrameMsOf(ntdlib::sar::Flavor::kNx);

// The cycle on the loop sequence's loop, from tick 96 to 192.
constexpr PlaybackClock::CycleRange kGameLoop{true, 96, 192};

// A sequence at timebase 48 and tempo 120 plays 96 ticks a second, 0.48 ticks a frame. A note waits for its length. The
// intro plays key 60 for 48 ticks and rests for 48. The loop, from tick 96 to 192, plays key 64 for 48 ticks, rests for
// 48 and jumps back to byte 10. The Switch's sequences give arguments in little-endian order.
const std::vector<uint8_t> kLoopBytecode = {0xB0, 48,  0xE1, 120,  0x00, 60,   100, 48,   0x80, 48,
                                            64,   100, 48,   0x80, 48,   0x89, 10,  0x00, 0x00, 0xFF};

// The loop's sequence as the Wii U holds it: the arguments of the tempo and the jump are big-endian.
const std::vector<uint8_t> kWiiULoopBytecode = {0xB0, 48,  0xE1, 0x00, 120, 60,   100,  48,   0x80, 48,
                                                64,   100, 48,   0x80, 48,  0x89, 0x00, 0x00, 10,   0xFF};

// The loop's sequence at tempo 60: 0.24 ticks a frame.
const std::vector<uint8_t> kSlowLoopBytecode = {0xB0, 48,  0xE1, 60,   0x00, 60,   100, 48,   0x80, 48,
                                                64,   100, 48,   0x80, 48,   0x89, 10,  0x00, 0x00, 0xFF};

// Key 60 for 24 ticks, then the end.
const std::vector<uint8_t> kNoteBytecode = {0xB0, 48, 0xE1, 120, 0x00, 60, 100, 24, 0xFF};

// The note's sequence as the Wii U holds it: the tempo's argument is big-endian.
const std::vector<uint8_t> kWiiUNoteBytecode = {0xB0, 48, 0xE1, 0x00, 120, 60, 100, 24, 0xFF};

// Key 60 for 24 ticks with a slow release (0xD3), then the end. The release sounds for about 2 s after the end, to tick
// 213 at the tempo there.
const std::vector<uint8_t> kReleaseBytecode = {0xB0, 48, 0xE1, 120, 0x00, 0xD3, 100, 60, 100, 24, 0xFF};

// Key 60 for 48 ticks, panned halfway to the left (0xC0 with 32, a pan of -32), then the end.
const std::vector<uint8_t> kPanBytecode = {0xB0, 48, 0xE1, 120, 0x00, 0xC0, 32, 60, 100, 48, 0xFF};

// Key 60 for 48 ticks, sent to aux bus A at full level (0xD9 with 127), then the end.
const std::vector<uint8_t> kSendBytecode = {0xB0, 48, 0xE1, 120, 0x00, 0xD9, 127, 60, 100, 48, 0xFF};

// The same, sent to aux bus C (0xDE with 127).
const std::vector<uint8_t> kSendCBytecode = {0xB0, 48, 0xE1, 120, 0x00, 0xDE, 127, 60, 100, 48, 0xFF};

// The loop's sequence, sent to aux bus A at full level. Its jump goes back to byte 12.
const std::vector<uint8_t> kSendLoopBytecode = {0xB0, 48, 0xE1, 120, 0x00, 0xD9, 127,  60, 100,  48,   0x80,
                                                48,   64, 100,  48,  0x80, 48,   0x89, 12, 0x00, 0x00, 0xFF};

// The send sequence as the Wii U holds it.
const std::vector<uint8_t> kWiiUSendBytecode = {0xB0, 48, 0xE1, 0x00, 120, 0xD9, 127, 60, 100, 48, 0xFF};

// A note that never ends in a short loop. With the note wait off (0xC7 with 0), key 60 starts for 0x0FFFFFFF ticks, a
// length of four bytes. The intro rests for 24 ticks. The loop, from tick 24 to 72, rests for 48 ticks and jumps back
// to byte 15.
const std::vector<uint8_t> kHeldBytecode = {0xB0, 48,   0xE1, 120, 0x00, 0xC7, 0,    60, 100,  0xFF, 0xFF,
                                            0xFF, 0x7F, 0x80, 24,  0x80, 48,   0x89, 15, 0x00, 0x00, 0xFF};

// Notes that sound past the loop's end. With the note wait off, the intro rests for 24 ticks. Each pass of the loop
// rests for 24 ticks, starts key 62 for 48 ticks, rests for 24 more and jumps back to byte 9. Each note therefore ends
// in the next pass.
const std::vector<uint8_t> kAcrossBytecode = {0xB0, 48,  0xE1, 120,  0x00, 0xC7, 0, 0x80, 24,   0x80, 24,
                                              62,   100, 48,   0x80, 24,   0x89, 9, 0x00, 0x00, 0xFF};

// Key 60 on program 0 of bank slot 0, key 62 on program 3 (0x81 with 3), then key 64 on program 3 of bank slot 1 (0xB6
// with 1), each for 24 ticks.
const std::vector<uint8_t> kInstrumentsBytecode = {0xB0, 48,  0xE1, 120,  0x00, 60, 100, 24, 0x81, 3,
                                                   62,   100, 24,   0xB6, 1,    64, 100, 24, 0xFF};

// Two sounds in one sequence file. The first, from byte 0, calls (0x8A) a phrase at byte 24 and then a fill at byte 28.
// The second, from byte 14, calls the phrase alone. The phrase plays key 60 for 48 ticks and returns (0xFD). The fill
// plays key 64 for 48 ticks, calls the phrase and returns. Bytes 36 to 40 call the fill and end, but no sound reaches
// them.
const std::vector<uint8_t> kCallsBytecode = {0xB0, 48,   0xE1, 120,  0x00, 0x8A, 24,   0x00, 0x00, 0x8A, 28,
                                             0x00, 0x00, 0xFF, 0xB0, 48,   0xE1, 120,  0x00, 0x8A, 24,   0x00,
                                             0x00, 0xFF, 60,   100,  48,   0xFD, 64,   100,  48,   0x8A, 24,
                                             0x00, 0x00, 0xFD, 0x8A, 28,   0x00, 0x00, 0xFF};

// A sound that reads variables. It compares variable 0 with 1 (0xF0 0x90) and, if they're equal (the if prefix, 0xA2),
// jumps to byte 106. There it plays key 64 for 48 ticks and ends. Otherwise it compares global variable 0, numbered 16,
// with 2 (0xF0 0x92, greater), variable 2 with 0, 4 and 2 (0xF0 0x94, less, 0x91, greater or equal, and 0x95, not
// equal) and variable 3 with 2 (0xF0 0x93, less or equal). It sets variable 1 to 5 (0xF0 0x80) and waits for the value
// of variable 1 (the variable prefix, 0xA1, before a wait). If the last comparison held, it sets variable 6 to 3. It
// then waits for the value of variable 6 and adds 1 to variable 4 (0xF0 0x81). Unless variable 3 equals 5, it sets
// variable 9 to 0. Either way, it compares variable 9 with 2 and plays key 60 for 48 ticks. Last, it sets variable 7 to
// a random value (0xF0 0x86) and variable 8 to the inverted bits of 0 (0xF0 0x8A). It compares each with a value and
// ends.
const std::vector<uint8_t> kVariablesBytecode = {
    0xB0, 48,   0xE1, 120,  0x00, 0xF0, 0x90, 0,    1,    0x00, 0xA2, 0x89, 106,  0x00, 0x00, 0xF0, 0x92, 16,   2,
    0x00, 0xF0, 0x94, 2,    0,    0x00, 0xF0, 0x91, 2,    4,    0x00, 0xF0, 0x95, 2,    2,    0x00, 0xF0, 0x93, 3,
    2,    0x00, 0xF0, 0x80, 1,    5,    0x00, 0xA1, 0x80, 1,    0xA2, 0xF0, 0x80, 6,    3,    0x00, 0xA1, 0x80, 6,
    0xF0, 0x81, 4,    1,    0x00, 0xF0, 0x90, 3,    5,    0x00, 0xA2, 0x89, 77,   0x00, 0x00, 0xF0, 0x80, 9,    0,
    0x00, 0xF0, 0x90, 9,    2,    0x00, 60,   100,  48,   0xF0, 0x86, 7,    3,    0x00, 0xF0, 0x90, 7,    1,    0x00,
    0xF0, 0x8A, 8,    0,    0x00, 0xF0, 0x90, 8,    0,    0x00, 0xFF, 64,   100,  48,   0xFF};

// A sound whose loop plays key 72 for 48 ticks while variable 0 is 1, and key 60 otherwise. It rests for a tick first.
// At byte 7, each pass compares variable 0 with 1 and, if they're equal, jumps to byte 24. Each note then jumps back to
// byte 7. The passes start at ticks 1, 49, 97 and so on, 10.4 ms after a frame's start.
const std::vector<uint8_t> kChangesBytecode = {0xB0, 48,   0xE1, 120, 0x00, 0x80, 1,  0xF0, 0x90, 0,    1,
                                               0x00, 0xA2, 0x89, 24,  0x00, 0x00, 60, 100,  48,   0x89, 7,
                                               0x00, 0x00, 72,   100, 48,   0x89, 7,  0x00, 0x00};

// Seven sounds whose variables pass through branches, calls and tracks, or whose exact values count. Each plays key 60
// for 24 ticks after the commands that read its variables. Three subroutines set a variable to 0 and return: SetOne
// (byte 134) variable 1, SetTwo (byte 140) variable 2, and WaitThenSetFour (byte 146) variable 4 after a wait of 1
// tick.
//
// SetInCall (byte 0) calls SetOne and transposes its note by variable 1 (the variable prefix, 0xA1, before 0xC3). It
// then compares variable 3 with 1. If they're equal, it calls SetTwo. It transposes a second note by variable 2.
//
// SetBeforeWait (byte 32) opens track 1 at byte 55 (0x88) and calls SetOne. After its note, it sets variable 5. Track 1
// transposes its note by variable 1 and pans it by variable 5 (0xC0).
//
// WaitInCall (byte 65) opens track 1 at byte 86, calls WaitThenSetFour and transposes its note by variable 4. So does
// track 1.
//
// IdleLoop (byte 93) opens track 1 at byte 122. It jumps back to its comparison of variable 6 with 1 until the two are
// equal (0xF0 0x95, not equal, and an if prefix before the jump). It then sets variable 7. Track 1 sets variable 6 to 1
// and transposes its note by variable 7.
//
// CompareThenTranspose (byte 154) compares variable 3 with 1 and transposes its note by variable 3.
//
// RetryInCall (byte 171) opens track 1 at byte 189 and calls DrawTwo (byte 196). DrawTwo sets variable 2 to a random
// value from 0 to 3 (0xF0 0x86) until the value isn't 0, and returns. Track 1 transposes its note by variable 2.
//
// Branches (byte 212) compares variable 0 with 1. If they're equal, it jumps to byte 236 and sets variable 2. Otherwise
// it sets variable 1 and jumps (0x89) past that. Both paths go on at byte 241: the note's transpose by variable 1 and
// its pan by variable 2.
const std::vector<uint8_t> kVariablePathsBytecode = {
    // SetInCall
    0xB0, 48, 0xE1, 120, 0x00, 0x8A, 134, 0x00, 0x00, 0xA1, 0xC3, 1, 60, 100, 24, 0xF0, 0x90, 3, 1, 0x00, 0xA2, 0x8A,
    140, 0x00, 0x00, 0xA1, 0xC3, 2, 60, 100, 24, 0xFF,
    // SetBeforeWait, and its track 1 from byte 55
    0xB0, 48, 0xE1, 120, 0x00, 0x88, 1, 55, 0x00, 0x00, 0x8A, 134, 0x00, 0x00, 60, 100, 24, 0xF0, 0x80, 5, 0, 0x00,
    0xFF, 0xA1, 0xC3, 1, 0xA1, 0xC0, 5, 60, 100, 24, 0xFF,
    // WaitInCall, and its track 1 from byte 86
    0xB0, 48, 0xE1, 120, 0x00, 0x88, 1, 86, 0x00, 0x00, 0x8A, 146, 0x00, 0x00, 0xA1, 0xC3, 4, 60, 100, 24, 0xFF, 0xA1,
    0xC3, 4, 60, 100, 24, 0xFF,
    // IdleLoop, and its track 1 from byte 122
    0xB0, 48, 0xE1, 120, 0x00, 0x88, 1, 122, 0x00, 0x00, 0xF0, 0x95, 6, 1, 0x00, 0xA2, 0x89, 103, 0x00, 0x00, 0xF0,
    0x80, 7, 0, 0x00, 60, 100, 24, 0xFF, 0xF0, 0x80, 6, 1, 0x00, 0xA1, 0xC3, 7, 60, 100, 24, 0xFF,
    // SetOne, SetTwo and WaitThenSetFour
    0xF0, 0x80, 1, 0, 0x00, 0xFD, 0xF0, 0x80, 2, 0, 0x00, 0xFD, 0x80, 1, 0xF0, 0x80, 4, 0, 0x00, 0xFD,
    // CompareThenTranspose
    0xB0, 48, 0xE1, 120, 0x00, 0xF0, 0x90, 3, 1, 0x00, 0xA1, 0xC3, 3, 60, 100, 24, 0xFF,
    // RetryInCall, its track 1 from byte 189, and DrawTwo
    0xB0, 48, 0xE1, 120, 0x00, 0x88, 1, 189, 0x00, 0x00, 0x8A, 196, 0x00, 0x00, 60, 100, 24, 0xFF, 0xA1, 0xC3, 2, 60,
    100, 24, 0xFF, 0xF0, 0x86, 2, 3, 0x00, 0xF0, 0x90, 2, 0, 0x00, 0xA2, 0x89, 196, 0x00, 0x00, 0xFD,
    // Branches
    0xB0, 48, 0xE1, 120, 0x00, 0xF0, 0x90, 0, 1, 0x00, 0xA2, 0x89, 236, 0x00, 0x00, 0xF0, 0x80, 1, 0, 0x00, 0x89, 241,
    0x00, 0x00, 0xF0, 0x80, 2, 0, 0x00, 0xA1, 0xC3, 1, 0xA1, 0xC0, 2, 60, 100, 24, 0xFF};

// Sample `i` of the saw, rising or falling.
int SawSample(int i, bool falling)
{
    return (falling ? 479 - i : i) * 40 - 9600;
}

// The instrument's wave: a looped saw of 480 samples at 48 kHz, as PCM16 in the Switch's byte order or, with
// `big_endian`, the Wii U's. Its samples differ from one another. A frame out of place then changes the output. With
// `falling`, the saw falls instead of rising.
std::vector<uint8_t> SawWave(bool falling, bool big_endian)
{
    std::vector<uint8_t> bytes;
    for (int i = 0; i < 480; i++)
    {
        const auto sample = static_cast<uint16_t>(static_cast<int16_t>(SawSample(i, falling)));
        const auto high = static_cast<uint8_t>(sample >> 8);
        const auto low = static_cast<uint8_t>(sample);
        bytes.push_back(big_endian ? high : low);
        bytes.push_back(big_endian ? low : high);
    }

    return bytes;
}

// The test archive's waves as wave files: the saw, and a stereo wave with the saw on the left and the falling saw on
// the right. Both are PCM16 and loop over their 480 samples at 48 kHz.
std::vector<std::vector<uint8_t>> TestWaveFiles(bool big_endian)
{
    sar_files::WaveSpec wave;
    wave.encoding = 1;
    wave.loop = true;
    wave.sample_rate = 48000;
    wave.loop_end = 480;
    wave.channels = {SawWave(false, big_endian)};
    sar_files::WaveSpec stereo = wave;
    stereo.channels = {SawWave(false, big_endian), SawWave(true, big_endian)};

    return {sar_files::Wave(big_endian, wave), sar_files::Wave(big_endian, stereo)};
}

// The test archive: the four sequences on a bank of three instruments, programs 0, 1 and 3. Each plays the saw for
// every key, at original key 60. Program 1 ignores note-off. Its notes sound until something stops them. The wave
// archive holds the two test waves. It's file 3. The test archive's tables and files, in the Switch's byte order or,
// with `big_endian`, the Wii U's. The sequences' bytecode stays as it is.
sar_files::ArchiveSpec TestArchiveSpec(bool big_endian = false)
{
    sar_files::Region region;
    region.original_key = 60;
    sar_files::VelocityTable velocities;
    velocities.entries = {{0, region}};
    sar_files::KeyTable keys;
    keys.entries = {{0, velocities}};
    sar_files::Region held = region;
    held.flags = 1;
    sar_files::VelocityTable held_velocities;
    held_velocities.entries = {{0, held}};
    sar_files::KeyTable held_keys;
    held_keys.entries = {{0, held_velocities}};
    sar_files::BankSpec bank;
    bank.waves = {{0x05000000, 0}};
    bank.instruments = {keys, held_keys, std::nullopt, keys};

    sar_files::SoundSpec loop_sound;
    loop_sound.name = "SEQ_LOOP";
    loop_sound.volume = 127;
    loop_sound.banks = {0};
    sar_files::SoundSpec note_sound = loop_sound;
    note_sound.name = "SEQ_NOTE";
    note_sound.file = 1;
    sar_files::SoundSpec release_sound = loop_sound;
    release_sound.name = "SEQ_RELEASE";
    release_sound.file = 4;
    sar_files::SoundSpec pan_sound = loop_sound;
    pan_sound.name = "SEQ_PAN";
    pan_sound.file = 5;

    sar_files::ArchiveSpec spec;
    spec.sounds = {loop_sound, note_sound, release_sound, pan_sound};
    spec.banks = {{"BANK_TEST", 2}};
    spec.wave_archives = {{"WARC_TEST", 3}};
    spec.players = {{"PLAYER_TEST", 1}};
    spec.files = {{sar_files::Sequence(big_endian, kLoopBytecode, {{"Loop", 0}}), "", false},
                  {sar_files::Sequence(big_endian, kNoteBytecode, {{"Note", 0}}), "", false},
                  {sar_files::Bank(big_endian, bank), "", false},
                  {sar_files::WaveArchive(big_endian, TestWaveFiles(big_endian)), "", false},
                  {sar_files::Sequence(big_endian, kReleaseBytecode, {{"Release", 0}}), "", false},
                  {sar_files::Sequence(big_endian, kPanBytecode, {{"Pan", 0}}), "", false}};

    return spec;
}

// The test archive, in the Switch's byte order.
std::vector<uint8_t> TestArchiveBytes()
{
    return sar_files::Archive(false, TestArchiveSpec());
}

// The test archive with a wave sound after its sequence sounds: sound kWaveSound. The sound's file holds 8 made-up
// bytes.
sar_files::ArchiveSpec MadeUpWaveSoundSpec()
{
    sar_files::ArchiveSpec spec = TestArchiveSpec();
    spec.sounds.push_back({.name = "WSD_TEST", .kind = sar_files::SoundSpec::kWaveSound, .file = 6});
    spec.files.push_back({{'F', 'W', 'S', 'D', 1, 2, 3, 4}, "", false});

    return spec;
}

// A file in the system's temporary folder with `bytes` in it, deleted again with the object.
class TemporaryFile
{
public:
    explicit TemporaryFile(std::span<const uint8_t> bytes)
        : path_(std::filesystem::temp_directory_path() /
                ("hokuto_song_test_" + std::to_string(std::random_device{}()) + ".bfsar"))
    {
        xsflib::WriteFile(path_, bytes);
    }

    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;

    ~TemporaryFile()
    {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    const std::filesystem::path& Path() const
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

// A folder in the system's temporary folder, deleted again with the object and the files in it.
class TemporaryFolder
{
public:
    TemporaryFolder()
        : path_(std::filesystem::temp_directory_path() / ("hokuto_song_test_" + std::to_string(std::random_device{}())))
    {
        std::filesystem::create_directory(path_);
    }

    TemporaryFolder(const TemporaryFolder&) = delete;
    TemporaryFolder& operator=(const TemporaryFolder&) = delete;

    ~TemporaryFolder()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& Path() const
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

// An error category that gives `reason` for every error. A system's category gives its reasons the same way.
class ReasonCategory : public std::error_category
{
public:
    explicit ReasonCategory(std::string reason) : reason_(std::move(reason))
    {
    }

    const char* name() const noexcept override
    {
        return "reason";
    }

    std::string message(int) const override
    {
        return reason_;
    }

private:
    std::string reason_;
};

// The test archive, opened from a file the way the program opens one.
std::shared_ptr<const sar::SoundArchive> OpenTestArchive()
{
    const TemporaryFile file(TestArchiveBytes());
    return OpenArchive(file.Path());
}

// The test archive as the Wii U holds it, opened the way the program opens one. The note's sequence holds its tempo in
// the Wii U's byte order. The test plays no other sequence of it.
std::shared_ptr<const sar::SoundArchive> OpenWiiUArchive()
{
    sar_files::ArchiveSpec spec = TestArchiveSpec(true);
    spec.files[1].bytes = sar_files::Sequence(true, kWiiUNoteBytecode, {{"Note", 0}});
    const TemporaryFile file(sar_files::Archive(true, spec));
    return OpenArchive(file.Path());
}

// The Wii U's test archive with the send sequence in place of the note's.
std::shared_ptr<const sar::SoundArchive> OpenWiiUSendArchive()
{
    sar_files::ArchiveSpec spec = TestArchiveSpec(true);
    spec.files[1].bytes = sar_files::Sequence(true, kWiiUSendBytecode, {{"Note", 0}});
    const TemporaryFile file(sar_files::Archive(true, spec));
    return OpenArchive(file.Path());
}

// The Wii U's test archive with the loop's sequence as the Wii U holds it. The test plays no other sequence of it.
std::shared_ptr<const sar::SoundArchive> OpenWiiULoopArchive()
{
    sar_files::ArchiveSpec spec = TestArchiveSpec(true);
    spec.files[0].bytes = sar_files::Sequence(true, kWiiULoopBytecode, {{"Loop", 0}});
    const TemporaryFile file(sar_files::Archive(true, spec));
    return OpenArchive(file.Path());
}

// The test archive with a fifth sequence, `bytecode`. The send sequence sends its note to aux bus A.
std::shared_ptr<const sar::SoundArchive> OpenSendArchive(const std::vector<uint8_t>& bytecode = kSendBytecode)
{
    sar_files::ArchiveSpec spec = TestArchiveSpec();
    sar_files::SoundSpec send = spec.sounds[kNoteSound];
    send.name = "SEQ_SEND";
    send.file = static_cast<uint32_t>(spec.files.size());
    spec.sounds.push_back(send);
    spec.files.push_back({sar_files::Sequence(false, bytecode, {{"Send", 0}}), "", false});
    const TemporaryFile file(sar_files::Archive(false, spec));
    return OpenArchive(file.Path());
}

// The test archive with a fifth sequence: the loop's sequence at half the tempo.
std::shared_ptr<const sar::SoundArchive> OpenSlowArchive()
{
    sar_files::ArchiveSpec spec = TestArchiveSpec();
    sar_files::SoundSpec slow = spec.sounds[kLoopSound];
    slow.name = "SEQ_SLOW";
    slow.file = static_cast<uint32_t>(spec.files.size());
    spec.sounds.push_back(slow);
    spec.files.push_back({sar_files::Sequence(false, kSlowLoopBytecode, {{"Slow", 0}}), "", false});
    const TemporaryFile file(sar_files::Archive(false, spec));
    return OpenArchive(file.Path());
}

// The test archive with the two sounds of the calls' sequence file.
std::shared_ptr<const sar::SoundArchive> OpenCallsArchive()
{
    sar_files::ArchiveSpec spec = TestArchiveSpec();
    sar_files::SoundSpec calls = spec.sounds[kNoteSound];
    calls.name = "SEQ_CALLS";
    calls.file = static_cast<uint32_t>(spec.files.size());
    sar_files::SoundSpec again = calls;
    again.name = "SEQ_CALLS_AGAIN";
    again.start_offset = 14;
    spec.sounds.push_back(calls);
    spec.sounds.push_back(again);
    const std::vector<std::pair<std::string, uint32_t>> kLabels = {
        {"Calls", 0}, {"CallsAgain", 14}, {"Phrase", 24}, {"Fill", 28}};
    spec.files.push_back({sar_files::Sequence(false, kCallsBytecode, kLabels), "", false});
    const TemporaryFile file(sar_files::Archive(false, spec));
    return OpenArchive(file.Path());
}

// The test archive with a sound that plays three instruments (kInstrumentsBytecode). The test bank is in both of its
// bank slots.
std::shared_ptr<const sar::SoundArchive> OpenInstrumentsArchive()
{
    sar_files::ArchiveSpec spec = TestArchiveSpec();
    sar_files::SoundSpec instruments = spec.sounds[kNoteSound];
    instruments.name = "SEQ_INSTRUMENTS";
    instruments.file = static_cast<uint32_t>(spec.files.size());
    instruments.banks = {0, 0};
    spec.sounds.push_back(instruments);
    spec.files.push_back({sar_files::Sequence(false, kInstrumentsBytecode, {{"Instruments", 0}}), "", false});
    const TemporaryFile file(sar_files::Archive(false, spec));
    return OpenArchive(file.Path());
}

// The test archive with a sound that plays `bytecode` in a short loop (kHeldBytecode or kAcrossBytecode).
std::shared_ptr<const sar::SoundArchive> OpenHeldArchive(const std::vector<uint8_t>& bytecode)
{
    sar_files::ArchiveSpec spec = TestArchiveSpec();
    sar_files::SoundSpec held = spec.sounds[kNoteSound];
    held.name = "SEQ_HELD";
    held.file = static_cast<uint32_t>(spec.files.size());
    spec.sounds.push_back(held);
    spec.files.push_back({sar_files::Sequence(false, bytecode, {{"Held", 0}}), "", false});
    const TemporaryFile file(sar_files::Archive(false, spec));
    return OpenArchive(file.Path());
}

// A copy of the test archive with a sound that reads variables (kVariablesBytecode).
std::shared_ptr<const sar::SoundArchive> OpenVariablesArchive()
{
    sar_files::ArchiveSpec spec = TestArchiveSpec();
    sar_files::SoundSpec variables = spec.sounds[kNoteSound];
    variables.name = "SEQ_VARIABLES";
    variables.file = static_cast<uint32_t>(spec.files.size());
    spec.sounds.push_back(variables);
    spec.files.push_back({sar_files::Sequence(false, kVariablesBytecode, {{"Variables", 0}}), "", false});
    const TemporaryFile file(sar_files::Archive(false, spec));
    return OpenArchive(file.Path());
}

// A copy of the test archive with a sound whose loop follows a variable (kChangesBytecode).
std::shared_ptr<const sar::SoundArchive> OpenChangesArchive()
{
    sar_files::ArchiveSpec spec = TestArchiveSpec();
    sar_files::SoundSpec changes = spec.sounds[kNoteSound];
    changes.name = "SEQ_CHANGES";
    changes.file = static_cast<uint32_t>(spec.files.size());
    spec.sounds.push_back(changes);
    spec.files.push_back({sar_files::Sequence(false, kChangesBytecode, {{"Changes", 0}}), "", false});
    const TemporaryFile file(sar_files::Archive(false, spec));
    return OpenArchive(file.Path());
}

// A copy of the test archive with the seven sounds of kVariablePathsBytecode. Each plays on tracks 0 and 1.
std::shared_ptr<const sar::SoundArchive> OpenVariablePathsArchive()
{
    sar_files::ArchiveSpec spec = TestArchiveSpec();
    const std::vector<std::pair<std::string, uint32_t>> kStarts = {
        {"SetInCall", 0}, {"SetBeforeWait", 32},         {"WaitInCall", 65},
        {"IdleLoop", 93}, {"CompareThenTranspose", 154}, {"RetryInCall", 171},
        {"Branches", 212}};
    for (const auto& [name, start] : kStarts)
    {
        sar_files::SoundSpec sound = spec.sounds[kNoteSound];
        sound.name = "SEQ_" + name;
        sound.file = static_cast<uint32_t>(spec.files.size());
        sound.start_offset = start;
        sound.allocate_track_flags = 0x3;
        spec.sounds.push_back(sound);
    }
    std::vector<std::pair<std::string, uint32_t>> labels = kStarts;
    labels.insert(labels.end(), {{"SetOne", 134}, {"SetTwo", 140}, {"WaitThenSetFour", 146}, {"DrawTwo", 196}});
    spec.files.push_back({sar_files::Sequence(false, kVariablePathsBytecode, labels), "", false});
    const TemporaryFile file(sar_files::Archive(false, spec));
    return OpenArchive(file.Path());
}

// A model for a sequence's players, with snapshots every `interval` frames.
std::shared_ptr<SequenceModel> ModelOf(std::shared_ptr<const sar::SoundArchive> archive,
                                       std::shared_ptr<const Sequence> sequence,
                                       uint64_t interval = SequenceModel::kSnapshotInterval, AuxEffects effects = {},
                                       Mixing mixing = Mixing::kStrict)
{
    return std::make_shared<SequenceModel>(std::move(archive), std::move(sequence), std::move(effects), mixing,
                                           interval);
}

// The model's output for `frames` frames of a sequence sound, run directly, with the aux buses at `volumes`. The
// enhanced mode's output in floating point too. For each frame, the sequence player's tick count before and after it,
// whether the engine was busy before it, and whether the sequence had ended before it.
struct ModelRun
{
    std::vector<int16_t> samples;
    std::vector<float> floats; // none in the console's mode
    std::vector<uint64_t> before;
    std::vector<uint64_t> after;
    std::vector<bool> busy;
    std::vector<bool> finished;
};

// The ModelRun of sound `sound` of `archive` for `frames` frames, with the engine's `options`, the starting values
// `variables`, the `changes` and the aux buses at `volumes`.
ModelRun RunModel(const sar::SoundArchive& archive, uint32_t sound, std::size_t frames,
                  const atk::EngineOptions& options = {}, const StartValues& variables = {},
                  const Changes& changes = {}, const AuxVolumes& volumes = kFullVolumes)
{
    // Sub-mix 0 holds the main bus, then the aux buses. Each bus's channels reach the final mix at the bus's volume.
    ModelRun run;
    atk::ArchiveModel model(archive, options);
    const int channels = model.sub_mix.channel_count;
    for (std::size_t bus = 0; bus < kAuxBuses; bus++)
    {
        for (int channel = 0; channel < channels; channel++)
        {
            const int buffer = (1 + static_cast<int>(bus)) * channels + channel;
            model.renderer.SetSubMixMixVolume(model.sub_mix.mix, volumes[bus], buffer, channel);
        }
    }
    atk::SequenceSoundPlayer* player = model.engine->StartSequence(sound);
    if (player == nullptr)
    {
        return run;
    }

    // A game sets the variables through the sound's handle before the sound's first frame.
    for (const auto& [number, value] : variables)
    {
        player->SetVariable(*model.engine, number, value);
    }

    // A game makes a change through the sound's handle too. The change comes before the ticks of its frame.
    for (std::size_t i = 0; i < frames; i++)
    {
        for (const VariableChange& change : changes)
        {
            if (ChangeFrame(change.time, kFrameMs) == i)
            {
                player->SetVariable(*model.engine, change.variable, change.value);
            }
        }
        run.busy.push_back(model.engine->IsBusy());
        run.finished.push_back(player->IsFinished());
        run.before.push_back(player->TickCount());
        model.engine->RunFrame();
        run.after.push_back(player->TickCount());
        const std::span<const int16_t> output = model.renderer.DeviceOutput();
        run.samples.insert(run.samples.end(), output.begin(), output.end());
        const std::span<const float> floats = model.renderer.DeviceOutputFloat();
        run.floats.insert(run.floats.end(), floats.begin(), floats.end());
    }

    return run;
}

// The first frame of `run` after which the tick count has reached `ticks`, or the number of frames.
std::size_t FirstFrameReaching(const ModelRun& run, uint64_t ticks)
{
    std::size_t frame = 0;
    while (frame < run.after.size() && run.after[frame] < ticks)
    {
        frame++;
    }

    return frame;
}

// The frames of a run of a sequence without a loop that matter at its end: the first frame that plays a tick, the first
// frame after the end of the sequence, and the first frame after the end of the last note's release.
struct EndFrames
{
    std::size_t first = 0;
    std::size_t finished = 0;
    std::size_t idle = 0;
};

// The EndFrames of `model`, a run of a sequence without a loop.
EndFrames EndFramesOf(const ModelRun& model)
{
    EndFrames ends;
    ends.first = FirstFrameReaching(model, 1);
    const auto finished = std::find(model.finished.begin(), model.finished.end(), true);
    ends.finished = static_cast<std::size_t>(finished - model.finished.begin());
    const auto idle = std::find(model.busy.begin() + static_cast<std::ptrdiff_t>(ends.first), model.busy.end(), false);
    ends.idle = static_cast<std::size_t>(idle - model.busy.begin());

    return ends;
}

// The position before frame `frame` of the run, from the first frame after the end of the sequence on. The position
// goes on from the end at the tempo there: 0.48 ticks a frame.
double PositionBefore(const ModelRun& model, const EndFrames& ends, std::size_t frame)
{
    const auto frames_past_end = static_cast<double>(frame - ends.finished);
    return static_cast<double>(model.after[ends.finished - 1]) + frames_past_end * 0.48;
}

// The first frame of the run, from the end of the sequence on, after which the position is past `ticks`. That frame
// plays tick `ticks`.
std::size_t FramePlayingPastEnd(const ModelRun& model, const EndFrames& ends, double ticks)
{
    std::size_t frame = ends.finished;
    while (PositionBefore(model, ends, frame + 1) <= ticks)
    {
        frame++;
    }

    return frame;
}

// A player's output, rendered in blocks of 256 stereo frames. The render worker renders in blocks too. The clock's
// position at the start of each frame is read after each block. The program reads it soon after the block too. `ended`
// says whether a block came back short.
struct PlayerRun
{
    std::vector<float> samples;
    std::vector<std::optional<double>> marks;
    std::vector<PlaybackClock::Peaks> peaks;
    std::vector<std::optional<PlaybackClock::Variables>> variables;
    bool ended = false;
};

// The PlayerRun of `player` for `frames` frames, with the positions, peaks and variables that `clock` gives.
PlayerRun RunPlayer(SequencePlayer& player, const PlaybackClock& clock, std::size_t frames)
{
    constexpr std::size_t kBlock = 256;
    PlayerRun run;
    const std::size_t total = frames * kFrameSamples;
    run.samples.resize(total * 2);
    std::size_t done = 0;
    while (done < total && !run.ended)
    {
        const std::size_t block = std::min(kBlock, total - done);
        const std::size_t rendered = player.Render(run.samples.data() + done * 2, block);
        done += rendered;
        run.ended = rendered < block;
        while (run.marks.size() * kFrameSamples < done)
        {
            const uint64_t start = run.marks.size() * kFrameSamples;
            run.marks.push_back(clock.PositionAt(start));
            run.peaks.push_back(clock.PeaksBetween(start, start + kFrameSamples));
            run.variables.push_back(clock.VariablesAt(start));
        }
    }
    run.samples.resize(done * 2);

    return run;
}

// `count` frames of a player's output from frame `from`.
std::vector<float> FramesOf(const PlayerRun& run, std::size_t from, std::size_t count)
{
    const auto at = [&run](std::size_t frame)
    {
        return run.samples.begin() + static_cast<std::ptrdiff_t>(frame * kFrameSamples * 2);
    };

    return std::vector<float>(at(from), at(from + count));
}

// The peaks of frame `frame` of a player's output, left and right.
std::array<float, 2> OutputPeaks(const std::vector<float>& samples, std::size_t frame)
{
    std::array<float, 2> peaks = {0.0f, 0.0f};
    for (std::size_t i = 0; i < kFrameSamples * 2; i++)
    {
        float& peak = peaks[i % 2];
        peak = std::max(peak, std::abs(samples[frame * kFrameSamples * 2 + i]));
    }

    return peaks;
}

// The frames of a run, from frame `first` on, in which track 0's meter differs from the peaks of the output.
std::size_t MeterMismatches(const PlayerRun& run, std::size_t first)
{
    std::size_t wrong = 0;
    for (std::size_t frame = first; frame < run.peaks.size(); frame++)
    {
        if (run.peaks[frame].tracks[0] != OutputPeaks(run.samples, frame))
        {
            wrong++;
        }
    }

    return wrong;
}

// Whether the meters of tracks `first` to 15 stay silent in each frame of a run.
bool TracksSilent(const PlayerRun& run, std::size_t first)
{
    constexpr std::array<float, 2> kSilent = {0.0f, 0.0f};
    for (const PlaybackClock::Peaks& peaks : run.peaks)
    {
        for (std::size_t track = first; track < peaks.tracks.size(); track++)
        {
            if (peaks.tracks[track] != kSilent)
            {
                return false;
            }
        }
    }

    return true;
}

// A sample of the model's output as the player writes it.
float SampleOf(const ModelRun& run, std::size_t frame, std::size_t index)
{
    return static_cast<float>(run.samples[frame * kFrameSamples * 2 + index]) / 32768.0f;
}

// Frame `frame` of the model's output.
std::span<const int16_t> ModelFrame(const ModelRun& run, std::size_t frame)
{
    return std::span<const int16_t>(run.samples).subspan(frame * kFrameSamples * 2, kFrameSamples * 2);
}

// Whether `samples` are the model's output from frame `first` of `run`, after the first `skip` samples. Output that
// ends within the first `skip` samples doesn't match.
bool Matches(const std::vector<float>& samples, const ModelRun& run, std::size_t first, std::size_t skip)
{
    if (samples.size() <= skip || first * kFrameSamples * 2 + samples.size() > run.samples.size())
    {
        return false;
    }

    for (std::size_t i = skip; i < samples.size(); i++)
    {
        if (samples[i] != SampleOf(run, first, i))
        {
            return false;
        }
    }

    return true;
}

// Whether `samples` hold frame `frame`, and `run` holds frame `model_frame`.
bool HoldsFrames(const std::vector<float>& samples, std::size_t frame, const ModelRun& run, std::size_t model_frame)
{
    const std::size_t kLength = kFrameSamples * 2;
    return (frame + 1) * kLength <= samples.size() && (model_frame + 1) * kLength <= run.samples.size();
}

// Whether a frame of output fades in from silence or out to it, in a straight line over the frame: `frame` of the
// samples against frame `model_frame` of the model's output. A frame that either lacks doesn't fade.
bool Fades(const std::vector<float>& samples, std::size_t frame, const ModelRun& run, std::size_t model_frame,
           bool fade_in)
{
    if (!HoldsFrames(samples, frame, run, model_frame))
    {
        return false;
    }

    for (std::size_t i = 0; i < kFrameSamples * 2; i++)
    {
        const float through = static_cast<float>(i / 2) / static_cast<float>(kFrameSamples);
        const float expected = SampleOf(run, model_frame, i) * (fade_in ? through : 1.0f - through);
        if (samples[frame * kFrameSamples * 2 + i] != expected)
        {
            return false;
        }
    }

    return true;
}

// Whether frame `frame` of `samples` crossfades from frame `model_frame` of `from` to the same frame of `to`, in a
// straight line over the frame. A frame that any of them lacks doesn't crossfade.
bool Crossfades(const std::vector<float>& samples, std::size_t frame, const ModelRun& from, const ModelRun& to,
                std::size_t model_frame)
{
    if (!HoldsFrames(samples, frame, from, model_frame) || !HoldsFrames(samples, frame, to, model_frame))
    {
        return false;
    }

    for (std::size_t i = 0; i < kFrameSamples * 2; i++)
    {
        const float through = static_cast<float>(i / 2) / static_cast<float>(kFrameSamples);
        const float out = SampleOf(from, model_frame, i) * (1.0f - through);
        const float in = SampleOf(to, model_frame, i) * through;
        if (samples[frame * kFrameSamples * 2 + i] != in + out)
        {
            return false;
        }
    }

    return true;
}

// Whether frame `frame` of `samples` ramps in from silence to frame `model_frame` of the model's output, in a straight
// line over the frame: the ramp after an unmute. The model ramps each voice before it mixes them, in fixed point. The
// samples therefore lie within 3/32768 of the line, not on it. A frame that either lacks doesn't ramp in.
bool RampsIn(const std::vector<float>& samples, std::size_t frame, const ModelRun& run, std::size_t model_frame)
{
    if (!HoldsFrames(samples, frame, run, model_frame))
    {
        return false;
    }

    for (std::size_t i = 0; i < kFrameSamples * 2; i++)
    {
        const float through = static_cast<float>(i / 2) / static_cast<float>(kFrameSamples);
        const float expected = SampleOf(run, model_frame, i) * through;
        if (std::abs(samples[frame * kFrameSamples * 2 + i] - expected) > 3.0f / 32768.0f)
        {
            return false;
        }
    }

    return true;
}

// Whether any of `samples` is other than zero.
template <typename Sample>
bool HasSound(std::span<const Sample> samples)
{
    return std::any_of(samples.begin(), samples.end(), [](Sample sample) { return sample != 0; });
}

// The largest sample of `samples`, positive or negative.
float Peak(std::span<const float> samples)
{
    float peak = 0.0f;
    for (const float sample : samples)
    {
        peak = std::max(peak, std::abs(sample));
    }

    return peak;
}

// The ratio of the power of `expected` to the power of the difference of `samples` from it, in dB. Samples that are the
// same give infinity.
double SignalToDifference(std::span<const int16_t> samples, std::span<const int16_t> expected)
{
    double signal = 0.0;
    double difference = 0.0;
    for (std::size_t i = 0; i < samples.size() && i < expected.size(); i++)
    {
        const double wanted = expected[i];
        const double error = samples[i] - wanted;
        signal += wanted * wanted;
        difference += error * error;
    }

    return difference > 0.0 ? 10.0 * std::log10(signal / difference) : std::numeric_limits<double>::infinity();
}

// Whether `part` runs from tick `start` to tick `end`, with the `count` notes from note `first`.
bool IsPart(const Part& part, uint32_t start, uint32_t end, std::size_t first, std::size_t count)
{
    return part.start == start && part.end == end && part.first == first && part.count == count;
}

// Whether `note` starts at tick `start` and lasts `length` ticks, with key `key` and velocity `velocity`.
bool IsNote(const Note& note, uint32_t start, uint32_t length, uint8_t key, uint8_t velocity)
{
    return note.start == start && note.length == length && note.key == key && note.velocity == velocity;
}

// Whether `function` throws std::runtime_error, the error that the program shows the user.
template <typename Function>
bool ThrowsRuntimeError(Function function)
{
    try
    {
        function();
    }
    catch (const std::runtime_error&)
    {
        return true;
    }

    return false;
}

// A preview's output, rendered in blocks of 256 stereo frames, up to `frames` frames of the model or to its end. A
// frame of the model holds `frame_samples` stereo frames.
std::vector<float> RunPreview(hokuto::audio::Renderer& preview, std::size_t frames,
                              std::size_t frame_samples = kFrameSamples)
{
    constexpr std::size_t kBlock = 256;
    const std::size_t total = frames * frame_samples;
    std::vector<float> samples(total * 2);
    std::size_t done = 0;
    while (done < total)
    {
        const std::size_t block = std::min(kBlock, total - done);
        const std::size_t rendered = preview.Render(samples.data() + done * 2, block);
        done += rendered;
        if (rendered < block)
        {
            break;
        }
    }
    samples.resize(done * 2);

    return samples;
}

// An instrument with a sound on each key from `lo` to `hi` of each of `ranges`.
sar::Instrument InstrumentOn(std::initializer_list<std::pair<int, int>> ranges)
{
    sar::Instrument instrument;
    for (const auto& [lo, hi] : ranges)
    {
        sar::KeyRegion key;
        key.lo = lo;
        key.hi = hi;
        key.velocities.push_back({0, 127, sar::VelocityRegion{}});
        instrument.keys.push_back(key);
    }

    return instrument;
}

// Waits up to 30 s for the result of the reader's latest request.
std::optional<SequenceReader::Result> WaitForRead(SequenceReader& reader)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (std::optional<SequenceReader::Result> result = reader.Take())
        {
            return result;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return std::nullopt;
}

// Waits up to 30 s for the catch-up's run to stop, and takes its model. Returns nullptr if the run goes on.
std::shared_ptr<SequenceModel> WaitForCatchUp(ModelCatchUp& catch_up)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (catch_up.Finished())
        {
            return catch_up.Take();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return nullptr;
}

// The reverb that the tests of effects put on aux bus A: nn::audio's preset 1, with a decay of 1.5 s. AuxReverb gives
// it to the model run directly, and AuxEffectsWithReverb to a SequenceModel.
const ntdlib::nx::audio::I3dl2ReverbParameterSet& Reverb()
{
    return ntdlib::nx::audio::kI3dl2ReverbPresets[1];
}

// The engine's options of a model run directly, with Reverb on aux bus A.
atk::EngineOptions AuxReverb()
{
    atk::EngineOptions options;
    options.aux_reverbs[0] = Reverb();

    return options;
}

// The effects of a SequenceModel, with Reverb on aux bus A.
AuxEffects AuxEffectsWithReverb()
{
    return {{{Reverb()}, {}, {}}};
}

// Before the loop's end, a tick count is its position. From there on, the count goes round the loop.
void TestPositionOfTicks()
{
    Sequence looped;
    looped.loop = Loop{96, 480};
    Sequence empty_loop;
    empty_loop.loop = Loop{96, 96};

    SEQLIB_CHECK_EQ(PositionOfTicks(looped, 0), 0);
    SEQLIB_CHECK_EQ(PositionOfTicks(looped, 479), 479);
    SEQLIB_CHECK_EQ(PositionOfTicks(looped, 480), 96);
    SEQLIB_CHECK_EQ(PositionOfTicks(looped, 863), 479);
    SEQLIB_CHECK_EQ(PositionOfTicks(looped, 864), 96);
    SEQLIB_CHECK_EQ(PositionOfTicks(Sequence{}, 1000), 1000);
    SEQLIB_CHECK_EQ(PositionOfTicks(empty_loop, 1000), 1000);
}

// A setting holds from its tick to the next one. Before the first, the default holds.
void TestSettingAt()
{
    const std::vector<Setting> kSettings = {{48, 100}, {96, 50}};

    SEQLIB_CHECK_EQ(SettingAt(kSettings, 0, 127), 127);
    SEQLIB_CHECK_EQ(SettingAt(kSettings, 48, 127), 100);
    SEQLIB_CHECK_EQ(SettingAt(kSettings, 95, 127), 100);
    SEQLIB_CHECK_EQ(SettingAt(kSettings, 96, 127), 50);
    SEQLIB_CHECK_EQ(SettingAt({}, 96, 127), 127);
}

// A tempo holds from its tick to the next one. A sequence without one plays at 120 beats a minute.
void TestTempoAt()
{
    Sequence sequence;
    sequence.tempo = {{0, 140.0}, {96, 90.0}};

    SEQLIB_CHECK(TempoAt(sequence, 95) == 140.0);
    SEQLIB_CHECK(TempoAt(sequence, 96) == 90.0);
    SEQLIB_CHECK(TempoAt(Sequence{}, 0) == 120.0);
}

// At timebase 48, a tick lasts 10.4 ms at 120 BPM and 20.8 ms at 60 BPM. From tick 96 on, the ticks go at 60 BPM.
// Before the first tempo, they go at 120 BPM.
void TestMsOfTick()
{
    Sequence sequence;
    sequence.timebase = 48;
    sequence.tempo = {{0, 120.0}, {96, 60.0}};

    const auto near = [](double a, double b)
    {
        return std::abs(a - b) < 1e-9;
    };

    SEQLIB_CHECK(near(MsOfTick(sequence, 48.0), 500.0));
    SEQLIB_CHECK(near(MsOfTick(sequence, 96.0), 1000.0));
    SEQLIB_CHECK(near(MsOfTick(sequence, 120.0), 1500.0));
    SEQLIB_CHECK(near(TickOfMs(sequence, 500.0), 48.0));
    SEQLIB_CHECK(near(TickOfMs(sequence, 1500.0), 120.0));
    SEQLIB_CHECK(near(TickOfMs(sequence, 3000.0), 192.0));
    SEQLIB_CHECK(near(MsOfTick(Sequence{}, 96.0), 1000.0));
}

// A change comes in the first frame that starts at its time or later: a frame of 5 ms on the Switch, of 3 ms on the Wii
// U, and of 81920000 / 16756991 ms on the 3DS, about 4.889 ms. The 3DS's DSP runs at 134,055,928 Hz, and makes a sample
// in 4096 of its cycles and a frame in 160 samples.
void TestChangeFrame()
{
    SEQLIB_CHECK(ChangeFrame(0, kFrameMs) == 0u);
    SEQLIB_CHECK(ChangeFrame(1, kFrameMs) == 1u);
    SEQLIB_CHECK(ChangeFrame(5, kFrameMs) == 1u);
    SEQLIB_CHECK(ChangeFrame(6, kFrameMs) == 2u);
    SEQLIB_CHECK(ChangeFrame(760, kFrameMs) == 152u);
    SEQLIB_CHECK(ChangeFrame(4'294'967'295, kFrameMs) == 858'993'459u);

    const hokuto::song::FrameMs wii_u = hokuto::song::FrameMsOf(ntdlib::sar::Flavor::kCafe);
    SEQLIB_CHECK(ChangeFrame(3, wii_u) == 1u);
    SEQLIB_CHECK(ChangeFrame(4, wii_u) == 2u);
    SEQLIB_CHECK(ChangeFrame(4'294'967'295, wii_u) == 1'431'655'765u);

    const hokuto::song::FrameMs ctr = hokuto::song::FrameMsOf(ntdlib::sar::Flavor::kCtr);
    SEQLIB_CHECK(ChangeFrame(0, ctr) == 0u);
    SEQLIB_CHECK(ChangeFrame(4, ctr) == 1u);
    SEQLIB_CHECK(ChangeFrame(5, ctr) == 2u);
    SEQLIB_CHECK(ChangeFrame(9, ctr) == 2u);
    SEQLIB_CHECK(ChangeFrame(10, ctr) == 3u);
    SEQLIB_CHECK(ChangeFrame(1000, ctr) == 205u);
    SEQLIB_CHECK(ChangeFrame(4'294'967'295, ctr) == 878'548'930u);
    SEQLIB_CHECK(hokuto::song::FramesIn(1000, ctr) == 204u);
    SEQLIB_CHECK(hokuto::song::FramesIn(10000, ctr) == 2045u);
}

// ntdlib's performer counts its frames from the frame of the first tick: frame 1 of the models of the Switch and the
// Wii U, and frame 0 of the 3DS's archive mode (ctr_test checks that). A change comes in the same frame of the model
// either way.
void TestPerformOptions()
{
    const hokuto::song::Changes kChanges = {{0, 4, 1}, {12, 3, 7}};
    for (const auto& [flavor, first_tick] :
         {std::pair{sar::Flavor::kNx, 1u}, std::pair{sar::Flavor::kCafe, 1u}, std::pair{sar::Flavor::kCtr, 0u}})
    {
        const export_::PerformOptions options = hokuto::song::PerformOptionsOf({{2, 5}}, kChanges, flavor);
        SEQLIB_CHECK(options.variables.at(2) == 5);
        SEQLIB_REQUIRE_EQ(options.changes.size(), 2);
        SEQLIB_CHECK(options.changes[0].frame == 0 && options.changes[0].index == 4);
        const uint64_t frame = ChangeFrame(12, hokuto::song::FrameMsOf(flavor));
        SEQLIB_CHECK(options.changes[1].frame == frame - first_tick && options.changes[1].index == 3 &&
                     options.changes[1].value == 7);
    }
}

// On a grid of bars of 4 beats, a bar at timebase 48 is 192 ticks. Notes with 8 beats, two bars, or more between them
// go in separate parts. A part starts at a bar and ends at one, and the loop's start and end divide parts too: here at
// ticks 768 and 1536.
void TestSplitParts()
{
    Sequence sequence;
    sequence.timebase = 48;
    sequence.loop = Loop{768, 1536};
    Track track;
    track.notes = {{0, 48}, {200, 48}, {500, 100}, {800, 48}, {1400, 48}};

    const std::vector<Part> parts = SplitParts(sequence, track, 4, 8);

    SEQLIB_REQUIRE_EQ(parts.size(), 3);
    SEQLIB_CHECK(IsPart(parts[0], 0, 768, 0, 3));
    SEQLIB_CHECK(IsPart(parts[1], 768, 960, 3, 1));
    SEQLIB_CHECK(IsPart(parts[2], 1344, 1536, 4, 1));
}

// On a grid of beats, 48 ticks apart at timebase 48, a part starts at the beat before its first note and ends at the
// beat after its last note. The notes above, with the same gap, give the same three parts on beats: the first ends at
// 624 after the note that ends at 600, and the last runs from 1392 to 1488 around its note at 1400.
void TestSplitPartsOnBeats()
{
    Sequence sequence;
    sequence.timebase = 48;
    sequence.loop = Loop{768, 1536};
    Track track;
    track.notes = {{0, 48}, {200, 48}, {500, 100}, {800, 48}, {1400, 48}};

    const std::vector<Part> parts = SplitParts(sequence, track, 1, 8);

    SEQLIB_REQUIRE_EQ(parts.size(), 3);
    SEQLIB_CHECK(IsPart(parts[0], 0, 624, 0, 3));
    SEQLIB_CHECK(IsPart(parts[1], 768, 864, 3, 1));
    SEQLIB_CHECK(IsPart(parts[2], 1392, 1488, 4, 1));
}

// A track that plays a note, calls the subroutine at offset 100 twice with a note between, and then calls the one at
// 300. That subroutine never returns. The first call makes a second call inside it, to offset 200. On a grid of beats,
// each call makes a separate part from the call to the return, with the notes of the call inside it. The note between
// the calls gets a part from the beat before it to the next call. The call that never returns ends at the beat after
// its note.
void TestSplitPartsWithCalls()
{
    using hokuto::song::Stretch;
    using Entry = ntdlib::sequence_export::Entry;
    Sequence sequence;
    sequence.timebase = 48;
    Track track;
    track.stretches = {Stretch{.tick = 0, .first = 0},
                       Stretch{.tick = 96, .first = 100, .entry = Entry::kCall, .depth = 1},
                       Stretch{.tick = 120, .first = 200, .entry = Entry::kCall, .depth = 2},
                       Stretch{.tick = 130, .first = 110, .entry = Entry::kReturn, .depth = 1},
                       Stretch{.tick = 192, .first = 10, .entry = Entry::kReturn},
                       Stretch{.tick = 288, .first = 100, .entry = Entry::kCall, .depth = 1},
                       Stretch{.tick = 384, .first = 14, .entry = Entry::kReturn},
                       Stretch{.tick = 400, .first = 300, .entry = Entry::kCall, .depth = 1}};
    track.notes = {{.start = 0, .length = 48, .stretch = 0},   {.start = 96, .length = 24, .stretch = 1},
                   {.start = 120, .length = 10, .stretch = 2}, {.start = 144, .length = 24, .stretch = 3},
                   {.start = 200, .length = 48, .stretch = 4}, {.start = 288, .length = 48, .stretch = 5},
                   {.start = 400, .length = 30, .stretch = 7}};

    const std::vector<Part> parts = SplitParts(sequence, track, 1, 8);

    SEQLIB_REQUIRE_EQ(parts.size(), 5);
    SEQLIB_CHECK(IsPart(parts[0], 0, 48, 0, 1) && !parts[0].call);
    SEQLIB_CHECK(IsPart(parts[1], 96, 192, 1, 3) && parts[1].call == 100u);
    SEQLIB_CHECK(IsPart(parts[2], 192, 288, 4, 1) && !parts[2].call);
    SEQLIB_CHECK(IsPart(parts[3], 288, 384, 5, 1) && parts[3].call == 100u);
    SEQLIB_CHECK(IsPart(parts[4], 400, 432, 6, 1) && parts[4].call == 300u);
}

// Between the starts of two frames, the position moves on in a straight line. Across the end of the loop, it stays at
// the frame's mark. A sample not yet written has no position. The cycle starts off, and keeps the locators it's given.
void TestPlaybackClock()
{
    PlaybackClock clock(kFrameSamples);
    clock.Mark(0, 10.0, {});
    clock.Mark(1, 12.0, {});
    clock.Mark(2, 2.0, {});
    clock.SetWritten(3 * kFrameSamples);

    SEQLIB_CHECK(clock.PositionAt(0) == 10.0);
    SEQLIB_CHECK(clock.PositionAt(kFrameSamples / 2) == 11.0);
    SEQLIB_CHECK(clock.PositionAt(kFrameSamples * 3 / 2) == 12.0);
    SEQLIB_CHECK(clock.PositionAt(2 * kFrameSamples + 1) == 2.0);
    SEQLIB_CHECK(!clock.PositionAt(3 * kFrameSamples).has_value());
    SEQLIB_CHECK(!clock.Cycle().on);
    SEQLIB_CHECK(!clock.Ended());

    clock.SetEnded();
    clock.SetCycle({true, 48, 4000000000u});
    const PlaybackClock::CycleRange cycle = clock.Cycle();

    SEQLIB_CHECK(clock.Ended());
    SEQLIB_CHECK(cycle.on && cycle.start == 48 && cycle.end == 4000000000u);
}

// The peaks of the tracks and the aux buses over a range of samples are the highest in the frames that hold those
// samples. An empty range, and frames not yet marked, add nothing. So do frames that later frames have replaced in the
// ring.
void TestPeaksBetween()
{
    constexpr std::array<float, 2> kBoth = {0.75f, 0.5f};
    const PlaybackClock::Peaks none;
    PlaybackClock::Peaks quiet;
    quiet.tracks[0] = {0.25f, 0.5f};
    quiet.buses[0] = {0.125f, 0.0f};
    PlaybackClock::Peaks loud;
    loud.tracks[0] = {0.75f, 0.125f};
    loud.tracks[15] = {1.5f, 1.0f};
    loud.buses[2] = {0.5f, 0.25f};
    PlaybackClock clock(kFrameSamples);
    clock.Mark(0, 0.0, quiet);
    clock.Mark(1, 1.0, loud);
    clock.Mark(2, 2.0, none);
    PlaybackClock wrapped(kFrameSamples);
    for (uint64_t frame = 0; frame < 300; frame++)
    {
        wrapped.Mark(frame, static_cast<double>(frame), frame == 299 ? loud : quiet);
    }

    const PlaybackClock::Peaks first = clock.PeaksBetween(0, kFrameSamples);
    const PlaybackClock::Peaks both = clock.PeaksBetween(kFrameSamples - 1, kFrameSamples + 1);
    const PlaybackClock::Peaks past_end = clock.PeaksBetween(2 * kFrameSamples, 10 * kFrameSamples);
    const PlaybackClock::Peaks empty = clock.PeaksBetween(kFrameSamples / 2, kFrameSamples / 2);
    const PlaybackClock::Peaks replaced = wrapped.PeaksBetween(43 * kFrameSamples, 44 * kFrameSamples);
    const PlaybackClock::Peaks kept = wrapped.PeaksBetween(299 * kFrameSamples, 302 * kFrameSamples);

    SEQLIB_CHECK(first == quiet);
    SEQLIB_CHECK(both.tracks[0] == kBoth && both.tracks[15] == loud.tracks[15]);
    SEQLIB_CHECK(both.buses[0] == quiet.buses[0] && both.buses[2] == loud.buses[2]);
    SEQLIB_CHECK(past_end == none);
    SEQLIB_CHECK(empty == none);
    SEQLIB_CHECK(replaced == none);
    SEQLIB_CHECK(kept == loud);
}

// The clock gives the variables' values after the frame that holds a sample. A frame not yet marked, and one that later
// frames have replaced in the ring, give nothing.
void TestVariablesAt()
{
    PlaybackClock::Variables set = PlaybackClock::kUnset;
    set[0] = 1;
    set[31] = -300;
    PlaybackClock clock(kFrameSamples);
    clock.Mark(0, 0.0, {});
    clock.Mark(1, 1.0, {}, set);
    PlaybackClock wrapped(kFrameSamples);
    for (uint64_t frame = 0; frame < 300; frame++)
    {
        wrapped.Mark(frame, static_cast<double>(frame), {}, frame == 299 ? set : PlaybackClock::kUnset);
    }

    const std::optional<PlaybackClock::Variables> first = clock.VariablesAt(kFrameSamples - 1);
    const std::optional<PlaybackClock::Variables> second = clock.VariablesAt(kFrameSamples);
    const std::optional<PlaybackClock::Variables> unmarked = clock.VariablesAt(2 * kFrameSamples);
    const std::optional<PlaybackClock::Variables> replaced = wrapped.VariablesAt(43 * kFrameSamples);
    const std::optional<PlaybackClock::Variables> kept = wrapped.VariablesAt(299 * kFrameSamples + 1);

    SEQLIB_CHECK(first == PlaybackClock::kUnset);
    SEQLIB_CHECK(second == set);
    SEQLIB_CHECK(!unmarked.has_value());
    SEQLIB_CHECK(!replaced.has_value());
    SEQLIB_CHECK(kept == set);
}

// The performer reads the intro and one pass of the loop. The track's instrument is the one its first note plays.
void TestReadSequence()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();

    const Sequence sequence = ReadSequence(*archive, kLoopSound);

    SEQLIB_CHECK(sequence.name == "SEQ_LOOP");
    SEQLIB_CHECK_EQ(sequence.sound, kLoopSound);
    SEQLIB_CHECK_EQ(sequence.timebase, 48);
    SEQLIB_CHECK(TempoAt(sequence, 0) == 120.0);
    SEQLIB_CHECK_EQ(sequence.length, 192);
    SEQLIB_CHECK_EQ(sequence.notes_end, 144);
    SEQLIB_REQUIRE(sequence.loop.has_value());
    SEQLIB_CHECK_EQ(sequence.loop->start, 96);
    SEQLIB_CHECK_EQ(sequence.loop->end, 192);
    SEQLIB_REQUIRE_EQ(sequence.tracks.size(), 1);
    const Track& track = sequence.tracks[0];
    SEQLIB_CHECK(track.name == "Track 0");
    SEQLIB_CHECK(track.bank == "BANK_TEST");
    SEQLIB_CHECK_EQ(track.program, 0);
    SEQLIB_REQUIRE_EQ(track.notes.size(), 2);
    SEQLIB_CHECK(IsNote(track.notes[0], 0, 48, 60, 100));
    SEQLIB_CHECK(IsNote(track.notes[1], 96, 48, 64, 100));
}

// A note that never ends lasts to the end of the loop. The performer repeats the loop of 48 ticks until the performance
// lasts 10 s, but the sequence ends with the loop.
void TestHeldNote()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenHeldArchive(kHeldBytecode);

    const Sequence sequence = ReadSequence(*archive, kHeldSound);

    SEQLIB_REQUIRE(sequence.loop.has_value());
    SEQLIB_CHECK_EQ(sequence.loop->start, 24);
    SEQLIB_CHECK_EQ(sequence.loop->end, 72);
    SEQLIB_CHECK_EQ(sequence.length, 72);
    SEQLIB_CHECK_EQ(sequence.notes_end, 72);
    SEQLIB_REQUIRE(sequence.tracks.size() == 1 && sequence.tracks[0].notes.size() == 1);
    SEQLIB_CHECK(IsNote(sequence.tracks[0].notes[0], 0, 72, 60, 100));
}

// A note that ends in a later pass lasts its length, past the loop's end. Only a note that still sounds where the
// performer stops lasts to the loop's end. The sequence then lasts to the end of its last note.
void TestNoteAcrossTheLoop()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenHeldArchive(kAcrossBytecode);

    const Sequence sequence = ReadSequence(*archive, kHeldSound);

    SEQLIB_REQUIRE(sequence.loop.has_value());
    SEQLIB_REQUIRE(sequence.tracks.size() == 1 && !sequence.tracks[0].notes.empty());
    const Note& last = sequence.tracks[0].notes.back();
    SEQLIB_CHECK(last.start + 48 > sequence.loop->end);
    SEQLIB_CHECK_EQ(sequence.notes_end, last.start + 48);
    SEQLIB_CHECK_EQ(sequence.length, last.start + 48);
    for (const Note& note : sequence.tracks[0].notes)
    {
        SEQLIB_CHECK(IsNote(note, note.start, 48, 62, 100));
    }
}

// Each note plays the instrument that the commands chose before it: its program and its slot of the sound's banks. The
// track's instrument is the first note's.
void TestNoteInstruments()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenInstrumentsArchive();

    const Sequence sequence = ReadSequence(*archive, kInstrumentsSound);

    SEQLIB_REQUIRE(sequence.tracks.size() == 1 && sequence.tracks[0].notes.size() == 3);
    const std::vector<Note>& notes = sequence.tracks[0].notes;
    SEQLIB_CHECK(IsNote(notes[0], 0, 24, 60, 100) && notes[0].bank_slot == 0 && notes[0].program == 0);
    SEQLIB_CHECK(IsNote(notes[1], 24, 24, 62, 100) && notes[1].bank_slot == 0 && notes[1].program == 3);
    SEQLIB_CHECK(IsNote(notes[2], 48, 24, 64, 100) && notes[2].bank_slot == 1 && notes[2].program == 3);
    SEQLIB_CHECK(sequence.tracks[0].bank == "BANK_TEST");
    SEQLIB_CHECK_EQ(sequence.tracks[0].program, 0);
}

// A track's mix is what its commands set: the pan sound's track pans to -32 at tick 0, and the send sound's track sends
// to aux bus A at full level. A setting that no command sets keeps its default.
void TestReadMix()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenSendArchive();

    const Sequence panned = ReadSequence(*archive, kPanSound);
    const Sequence sent = ReadSequence(*archive, kSendSound);

    SEQLIB_REQUIRE(panned.tracks.size() == 1 && sent.tracks.size() == 1);
    const Track& left = panned.tracks[0];
    const Track& sending = sent.tracks[0];
    const std::vector<Setting> kPanned = {{0, -32}};
    const std::vector<Setting> kSent = {{0, 127}};
    const auto no_sends = [](const std::vector<Setting>& sends)
    {
        return sends.empty();
    };
    SEQLIB_CHECK(left.pan == kPanned && left.volume.empty() && MixAt(left, Mix::kVolume, 24) == 127);
    SEQLIB_CHECK(std::ranges::all_of(left.sends, no_sends));
    SEQLIB_CHECK(sending.sends[0] == kSent && MixAt(sending, Mix::kSendA, 24) == 127);
    SEQLIB_CHECK(sending.pan.empty() && sending.sends[1].empty() && sending.sends[2].empty());
}

// A sequence holds its file's labels by their offsets. The pan sound's file has one label, at its start.
void TestReadLabels()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenSendArchive();

    const Sequence panned = ReadSequence(*archive, kPanSound);

    SEQLIB_CHECK(panned.labels == (std::map<uint32_t, std::string>{{0, "Pan"}}));
}

// Each call from outside any call makes a part from the call to the return, with the subroutine's offset. The fill's
// part holds the note of the phrase that the fill calls. Both sounds of the file count its calls: three of the phrase,
// one from each sound and one from the fill, and one of the fill. The call that no sound reaches doesn't count.
void TestReadCalls()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenCallsArchive();

    const Sequence calls = ReadSequence(*archive, kCallsSound);
    const Sequence again = ReadSequence(*archive, kCallsAgainSound);
    SEQLIB_REQUIRE(!calls.tracks.empty());
    const std::vector<Part> parts = SplitParts(calls, calls.tracks[0], 1, 8);

    const std::map<uint32_t, int> kExpected = {{24, 3}, {28, 1}};
    SEQLIB_CHECK(calls.calls == kExpected);
    SEQLIB_CHECK(again.calls == kExpected);
    SEQLIB_REQUIRE(calls.labels.size() == 4 && calls.labels.contains(24) && calls.labels.contains(28));
    SEQLIB_CHECK(calls.labels.at(24) == "Phrase" && calls.labels.at(28) == "Fill");
    SEQLIB_REQUIRE(parts.size() == 2);
    SEQLIB_CHECK(parts[0].start == 0 && parts[0].end == 48 && parts[0].count == 1 && parts[0].call == 24u);
    SEQLIB_CHECK(parts[1].start == 48 && parts[1].end == 144 && parts[1].count == 2 && parts[1].call == 28u);
    SEQLIB_CHECK(again.tracks.size() == 1 && again.tracks[0].notes.size() == 1);
}

// The variables that the sound reads before it sets them, by their numbers. Each choice gives the comparisons of its
// variable other outcomes than the other choices give: one value is equal to 1 and one isn't, and so on. The sound sets
// variables 1, 7 and 8 before it reads them, and they don't count. Variable 6 counts: a command behind an if prefix
// sets nothing for certain. Variable 9 counts too. One of the two paths to its comparison sets it, and the other
// doesn't. A wait takes variable 6's exact value, and the addition to variable 4 works from variable 4's value. Neither
// has choices. The sound sets variable 9. The variable has choices all the same: only a comparison with a fixed value
// reads it. The sequence takes the branch that its starting values choose. A value for a variable past the game's 32 is
// left out.
void TestReadVariables()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenVariablesArchive();

    const Sequence unset = ReadSequence(*archive, kVariablesSound);
    const Sequence chosen = ReadSequence(*archive, kVariablesSound, {{0, 1}, {40, 7}});

    const std::vector<Variable> kExpected = {
        {0, {-1, 1}, false}, {2, {-1, 0, 2, 4}, false}, {3, {-1, 3, 5}, false}, {4, {}, true},
        {6, {}, true},       {9, {-1, 2}, true},        {16, {-1, 3}, false},
    };
    SEQLIB_CHECK(unset.variables == kExpected);
    SEQLIB_CHECK(chosen.variables == kExpected);
    SEQLIB_CHECK(unset.start_values.empty());
    SEQLIB_CHECK(chosen.start_values == StartValues({{0, 1}}));
    SEQLIB_REQUIRE(unset.tracks.size() == 1 && chosen.tracks.size() == 1);
    SEQLIB_REQUIRE(unset.tracks[0].notes.size() == 1 && chosen.tracks[0].notes.size() == 1);
    SEQLIB_CHECK(unset.tracks[0].notes[0].key == 60 && unset.tracks[0].notes[0].start == 8);
    SEQLIB_CHECK(chosen.tracks[0].notes[0].key == 64 && chosen.tracks[0].notes[0].start == 0);
}

// On the Wii, a global variable starts at 0 (SeqPlayer::SetGlobalVariable), and a player variable at -1. The choices of
// each start from its starting value.
void TestWiiGlobalsStartAtZero()
{
    using ntdlib::commands::Argument;
    using ntdlib::commands::Command;
    using ntdlib::commands::Kind;
    using ntdlib::commands::Source;

    const auto compare = [](uint8_t variable, int value)
    {
        Command command;
        command.kind = Kind::kCompareEqual;
        command.index = variable;
        command.value = Argument{Source::kValue, value * ntdlib::commands::kOne, 0};

        return command;
    };

    Command end;
    end.kind = Kind::kEnd;
    const ntdlib::commands::Sequence commands{{compare(0, 3), compare(16, 3), end}, {}};

    SEQLIB_CHECK(hokuto::song::VariablesOf(commands, 0).starting ==
                 std::vector<Variable>({{0, {-1, 3}, false}, {16, {-1, 3}, false}}));
    SEQLIB_CHECK(hokuto::song::VariablesOf(commands, 0, 0).starting ==
                 std::vector<Variable>({{0, {-1, 3}, false}, {16, {0, 3}, false}}));

    Sequence song;
    SEQLIB_CHECK(hokuto::song::StartingValue(song, 16) == -1);

    song.wii_build = ntdlib::SndBuild{};
    SEQLIB_CHECK(hokuto::song::StartingValue(song, 15) == -1 && hokuto::song::StartingValue(song, 16) == 0 &&
                 hokuto::song::StartingValue(song, 31) == 0);
}

// The paths of a sound's commands decide which variables count. A variable counts where one path to a command that
// reads it sets it and another doesn't. A call sets what its subroutine sets before every return, but not behind an if
// prefix. A track that a command opens runs only after the opening track stops for the tick: at a note, at a wait, in a
// call of a subroutine that waits, or in a loop that waits for another track. What the opening track set before then
// doesn't count in the opened track. A loop that sets a variable doesn't wait, such as a loop that draws a random value
// until the value isn't 0. A variable that a command takes as an argument has no choices, even where a comparison tests
// it against a fixed value. The model check below leaves out Branches. Its variable 1 changes the sound only while
// variable 0 is 1.
void TestReadVariablePaths()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenVariablePathsArchive();

    const Sequence set_in_call = ReadSequence(*archive, kSetInCallSound);
    const Sequence set_before_wait = ReadSequence(*archive, kSetBeforeWaitSound);
    const Sequence wait_in_call = ReadSequence(*archive, kWaitInCallSound);
    const Sequence idle_loop = ReadSequence(*archive, kIdleLoopSound);
    const Sequence compare_then_transpose = ReadSequence(*archive, kCompareThenTransposeSound);
    const Sequence retry_in_call = ReadSequence(*archive, kRetryInCallSound);
    const Sequence branches = ReadSequence(*archive, kBranchesSound);

    SEQLIB_CHECK(set_in_call.variables == std::vector<Variable>({{2, {}, true}, {3, {-1, 1}, false}}));
    SEQLIB_CHECK(set_before_wait.variables == std::vector<Variable>({{5, {}, true}}));
    SEQLIB_CHECK(wait_in_call.variables == std::vector<Variable>({{4, {}, true}}));
    SEQLIB_CHECK(idle_loop.variables == std::vector<Variable>({{6, {-1, 1}, true}, {7, {}, true}}));
    SEQLIB_CHECK(compare_then_transpose.variables == std::vector<Variable>({{3, {}, false}}));
    SEQLIB_CHECK(retry_in_call.variables.empty());
    SEQLIB_CHECK(branches.variables == std::vector<Variable>({{0, {-1, 1}, false}, {1, {}, true}, {2, {}, true}}));
}

// The starting values of the variables that a sound lists change what ntdlib's model of nn::atk plays, and those of the
// variables that it leaves out don't. A variable with choices takes each choice, and any other variable 1 and 7.
void TestListedVariablesChangeTheSound()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenVariablePathsArchive();
    constexpr std::size_t kFrames = 150;

    for (const uint32_t sound : {kSetInCallSound, kSetBeforeWaitSound, kWaitInCallSound, kIdleLoopSound,
                                 kCompareThenTransposeSound, kRetryInCallSound})
    {
        const Sequence sequence = ReadSequence(*archive, sound);
        const std::vector<int16_t> unset = RunModel(*archive, sound, kFrames).samples;
        for (int number = 0; number < 8; number++)
        {
            const auto numbered = [number](const Variable& variable)
            {
                return variable.number == number;
            };
            const auto listed = std::find_if(sequence.variables.begin(), sequence.variables.end(), numbered);
            const bool counts = listed != sequence.variables.end();
            const std::vector<int> values = counts && !listed->choices.empty() ? listed->choices : std::vector{1, 7};

            bool changes = false;
            for (const int value : values)
            {
                const StartValues start = {{number, static_cast<int16_t>(value)}};
                changes = changes || RunModel(*archive, sound, kFrames, {}, start).samples != unset;
            }

            SEQLIB_CHECK(changes == counts);
        }
    }
}

// The model starts the sound in its frame 0 and plays the first tick in frame 1. Its frame F plays the ticks from 5 ms
// before the frame's start up to its start. A change at 1,012 ms comes in frame 203. That frame plays tick 97, at
// 1,010.4 ms. The pass from tick 49 has compared the variable already, and it plays key 60 to its end. The pass from
// tick 97 plays key 72, and it's the loop. The two notes before it make the intro. Without the change, the first pass
// is the loop. With a change to 2 at 760 ms and one to 1 at 2,015 ms, the loop starts at tick 193. Frame 403 plays that
// tick. The changes come in the order of their times. A change of a variable past the game's is left out. The sound
// reads variable 0 only.
void TestReadChanges()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenChangesArchive();
    const ModelRun model = RunModel(*archive, kChangesSound, 410);

    const Sequence unchanged = ReadSequence(*archive, kChangesSound);
    const Sequence changed = ReadSequence(*archive, kChangesSound, {}, {{1012, 0, 1}, {500, 40, 7}});
    const Sequence twice = ReadSequence(*archive, kChangesSound, {}, {{2015, 0, 1}, {760, 0, 2}});

    SEQLIB_REQUIRE(model.after[202] == 97 && model.after[203] == 98);
    SEQLIB_REQUIRE(model.after[402] == 193 && model.after[403] == 194);
    SEQLIB_CHECK(unchanged.loop == Loop({1, 49}));
    SEQLIB_CHECK(changed.loop == Loop({97, 145}));
    SEQLIB_CHECK(twice.loop == Loop({193, 241}));
    SEQLIB_CHECK(unchanged.changes.empty());
    SEQLIB_CHECK(changed.changes == Changes({{1012, 0, 1}}));
    SEQLIB_CHECK(twice.changes == Changes({{760, 0, 2}, {2015, 0, 1}}));
    SEQLIB_CHECK(changed.read_variables == std::vector<Variable>({{0, {-1, 1}, false}}));
    SEQLIB_CHECK(changed.variables == changed.read_variables);
    SEQLIB_REQUIRE(changed.tracks.size() == 1 && changed.tracks[0].notes.size() == 3);
    SEQLIB_CHECK(IsNote(changed.tracks[0].notes[0], 1, 48, 60, 100));
    SEQLIB_CHECK(IsNote(changed.tracks[0].notes[1], 49, 48, 60, 100));
    SEQLIB_CHECK(IsNote(changed.tracks[0].notes[2], 97, 48, 72, 100));
}

// A file that isn't an archive, a file that's missing, a sound past the archive's sounds and a sound that isn't a
// sequence give errors for the user. The wave sound plays a sequence file here: only its kind is refused.
void TestReadErrors()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    sar_files::ArchiveSpec wave_sound_spec = MadeUpWaveSoundSpec();
    wave_sound_spec.sounds.back().file = 1;
    const std::shared_ptr<const sar::SoundArchive> with_wave_sound =
        LoadArchive(sar_files::Archive(false, wave_sound_spec), "test.bfsar");
    const std::vector<uint8_t> kText = {'H', 'o', 'k', 'u', 't', 'o'};
    const TemporaryFile not_archive(kText);
    const std::filesystem::path missing = not_archive.Path().parent_path() / "hokuto_song_test_missing.bfsar";

    SEQLIB_CHECK(ThrowsRuntimeError([&] { OpenArchive(not_archive.Path()); }));
    SEQLIB_CHECK(ThrowsRuntimeError([&] { OpenArchive(missing); }));
    SEQLIB_CHECK(ThrowsRuntimeError([&] { ReadSequence(*archive, kMissingSound); }));
    SEQLIB_CHECK(ThrowsRuntimeError([&] { ReadSequence(*with_wave_sound, kWaveSound); }));
}

// The game keeps the test archive's wave archive in group GROUP_WAVES, in a file beside the archive. Opening the
// archive finds the group file and keeps it. Loading the archive with the kept group file reads the wave archive again.
// A .hok file's archive does the same. Without the group file, the wave archive is missing. A file that isn't a group
// file gives an error.
void TestGroupFiles()
{
    sar_files::ArchiveSpec spec = TestArchiveSpec();
    const std::vector<uint8_t> waves = spec.files[3].bytes;
    spec.files[3].group_only = true;
    spec.groups = {{"GROUP_WAVES", sar::kExternalGroupFile}};
    const std::vector<uint8_t> bytes = sar_files::Archive(false, spec);
    const std::vector<uint8_t> group = sar_files::Group(false, {{3, waves}});
    const TemporaryFolder folder;
    xsflib::WriteFile(folder.Path() / "groups.bfsar", bytes);
    xsflib::WriteFile(folder.Path() / "GROUP_WAVES.bfgrp", group);
    const std::vector<std::vector<uint8_t>> kNotGroup = {{1, 2, 3}};

    const std::shared_ptr<const sar::SoundArchive> opened = OpenArchive(folder.Path() / "groups.bfsar");
    const std::shared_ptr<const sar::SoundArchive> kept = LoadArchive(bytes, "groups.bfsar", opened->GroupFiles());
    const std::shared_ptr<const sar::SoundArchive> bare = LoadArchive(bytes, "groups.bfsar");

    SEQLIB_CHECK(opened->GroupFiles() == std::vector<std::vector<uint8_t>>{group});
    SEQLIB_CHECK(std::ranges::equal(opened->FileData(3), waves) && std::ranges::equal(kept->FileData(3), waves));
    SEQLIB_CHECK(bare->FileData(3).empty());
    SEQLIB_CHECK(ThrowsRuntimeError([&] { LoadArchive(bytes, "groups.bfsar", kNotGroup); }));
}

// A file that an export writes beside an archive, with its name and its bytes. PlaceGroupFiles takes and gives files in
// this form.
using Output = std::pair<std::string, std::vector<uint8_t>>;

// A game can keep a group file in the userManagementFiles folder beside its archive. Opening the archive finds
// GROUP_WAVES's file there, and an export writes the group's new file there too (PlaceGroupFiles). A group file goes
// beside the archive when the game keeps it there, when the game keeps a file of its name in both places, and when no
// folder holds one. Of two folders, the first that holds a file of the name decides. A SARC archive keeps its name.
void TestGroupFilePlaces()
{
    sar_files::ArchiveSpec spec = TestArchiveSpec();
    const std::vector<uint8_t> waves = spec.files[3].bytes;
    spec.files[3].group_only = true;
    spec.groups = {{"GROUP_WAVES", sar::kExternalGroupFile}};
    const std::vector<uint8_t> group = sar_files::Group(false, {{3, waves}});

    const TemporaryFolder game;
    const TemporaryFolder other;
    const std::filesystem::path managed = game.Path() / hokuto::song::kUserManagementFolder;
    const std::filesystem::path other_managed = other.Path() / hokuto::song::kUserManagementFolder;
    std::filesystem::create_directory(managed);
    std::filesystem::create_directory(other_managed);
    xsflib::WriteFile(game.Path() / "groups.bfsar", sar_files::Archive(false, spec));
    xsflib::WriteFile(managed / "GROUP_WAVES.bfgrp", group);
    const std::vector<uint8_t> kBytes = {1, 2, 3};
    for (const std::filesystem::path& file :
         {game.Path() / "GROUP_BGM.bfgrp", game.Path() / "GROUP_BOTH.bfgrp", managed / "GROUP_BOTH.bfgrp",
          managed / "Sound.sarc", other_managed / "GROUP_BGM.bfgrp", other_managed / "GROUP_OTHER.bcgrp"})
    {
        xsflib::WriteFile(file, kBytes);
    }

    const std::array<std::filesystem::path, 2> folders = {game.Path(), other.Path()};
    const std::vector<Output> kOutputs = {{"GROUP_WAVES.bfgrp", {1}}, {"GROUP_BGM.bfgrp", {2}},
                                          {"GROUP_BOTH.bfgrp", {3}},  {"GROUP_OTHER.bcgrp", {4}},
                                          {"GROUP_NEW.bfgrp", {5}},   {"Sound.sarc", {6}}};
    const std::vector<Output> kExpected = {
        {"userManagementFiles/GROUP_WAVES.bfgrp", {1}}, {"GROUP_BGM.bfgrp", {2}}, {"GROUP_BOTH.bfgrp", {3}},
        {"userManagementFiles/GROUP_OTHER.bcgrp", {4}}, {"GROUP_NEW.bfgrp", {5}}, {"Sound.sarc", {6}}};

    const std::shared_ptr<const sar::SoundArchive> opened = OpenArchive(game.Path() / "groups.bfsar");
    const std::vector<Output> placed = PlaceGroupFiles(kOutputs, folders);

    SEQLIB_CHECK(opened->GroupFiles() == std::vector<std::vector<uint8_t>>{group});
    SEQLIB_CHECK(std::ranges::equal(opened->FileData(3), waves));
    SEQLIB_CHECK(placed == kExpected);
}

// Each file under `folder` by its path there, with its bytes.
std::map<std::string, std::vector<uint8_t>> FilesUnder(const std::filesystem::path& folder)
{
    std::map<std::string, std::vector<uint8_t>> files;
    for (const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(folder))
    {
        if (entry.is_regular_file())
        {
            std::vector<uint8_t> bytes;
            xsflib::ReadFile(entry.path(), bytes);
            const std::u8string path = std::filesystem::relative(entry.path(), folder).generic_u8string();
            files.emplace(std::string(path.begin(), path.end()), bytes);
        }
    }

    return files;
}

// An export to MIDI with SoundFont 2 and SFZ writes the files that ntdlib's converter frame writes for the same sound,
// in the same places: the MIDI file and the SoundFont side by side, the SFZ files in a folder of the sound's name, and
// the samples in a folder that the sounds share.
void TestExportLayout()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const TemporaryFolder folder;
    const std::filesystem::path ours = folder.Path() / "ours";
    const std::filesystem::path theirs = folder.Path() / "theirs";
    std::filesystem::create_directory(ours);
    std::filesystem::create_directory(theirs);
    const auto convert = [&](std::size_t sound)
    {
        return export_::ConvertSequence(*archive, static_cast<uint32_t>(sound), export_::PerformOptions(),
                                        export_::SwitchSettings(), "hacsf2");
    };

    const export_::Conversion conversion = ConvertToMidi(*archive, kNoteSound, {}, {}, "hacsf2");
    WriteMidiAndSoundFont(conversion, ours / "SEQ_NOTE.mid");
    WriteMidiAndSfz(conversion, ours / "SEQ_NOTE.mid");
    export_::ConvertAll<export_::Conversion>({{kNoteSound, "SEQ_NOTE"}}, theirs, true, export_::kSfzSamplesFolder,
                                             convert);

    const std::map<std::string, std::vector<uint8_t>> files = FilesUnder(ours);
    SEQLIB_REQUIRE(!conversion.sfz.files.empty() && !conversion.sfz.samples.empty());
    SEQLIB_CHECK(files.contains("SEQ_NOTE.mid") && files.contains("SEQ_NOTE.sf2"));
    SEQLIB_CHECK(files.contains("sfz/SEQ_NOTE/" + conversion.sfz.files[0].name));
    SEQLIB_CHECK(files.contains("sfz/samples/" + conversion.sfz.samples[0].name));
    SEQLIB_CHECK(files == FilesUnder(theirs));
}

// A change at a set time reaches the MIDI file. The sequence's loop plays key 60 for 48 ticks, half a second, until
// variable 0 is 1, and then key 72. The converter repeats a loop shorter than 10 s until the file lasts 10 s: 20 notes.
// The change at 1,012 ms comes before tick 97, the start of the third pass. The loop's markers go round the first pass
// after the change. A MIDI quarter note holds 160 times the sequence's 48 ticks.
void TestExportChanges()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenChangesArchive();

    const auto keys = [](const export_::Conversion& conversion)
    {
        std::vector<int> played;
        for (const seqlib::midi::Note& note : seqlib::midi::Notes(seqlib::midi::Read(conversion.midi)))
        {
            played.push_back(note.key);
        }

        return played;
    };

    const auto loop = [](uint64_t start, uint64_t end)
    {
        return std::vector<seqlib::midi::Text>({{start * 160, "loopStart"}, {end * 160, "loopEnd"}});
    };

    std::vector<int> switched(20, 72);
    switched[0] = 60;
    switched[1] = 60;

    const export_::Conversion unchanged = ConvertToMidi(*archive, kChangesSound, {}, {}, "Hokuto");
    const export_::Conversion changed = ConvertToMidi(*archive, kChangesSound, {}, {{1012, 0, 1}}, "Hokuto");
    const export_::Conversion started = ConvertToMidi(*archive, kChangesSound, {{0, 1}}, {}, "Hokuto");

    SEQLIB_CHECK(keys(unchanged) == std::vector<int>(20, 60));
    SEQLIB_CHECK(keys(changed) == switched);
    SEQLIB_CHECK(keys(started) == std::vector<int>(20, 72));
    SEQLIB_CHECK(seqlib::midi::Markers(seqlib::midi::Read(unchanged.midi)) == loop(1, 49));
    SEQLIB_CHECK(seqlib::midi::Markers(seqlib::midi::Read(changed.midi)) == loop(97, 145));
}

// A sample that the folder of samples holds with other bytes stops the export before it writes anything. A sample with
// the same bytes stays.
void TestExportKeepsOtherSamples()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const export_::Conversion conversion = ConvertToMidi(*archive, kNoteSound, {}, {}, "Hokuto");
    SEQLIB_REQUIRE(!conversion.sfz.samples.empty());
    const TemporaryFolder folder;
    const std::filesystem::path midi = folder.Path() / "SEQ_NOTE.mid";
    const std::filesystem::path sample = folder.Path() / "sfz" / "samples" / conversion.sfz.samples[0].name;
    std::filesystem::create_directories(sample.parent_path());
    const std::vector<uint8_t> kOther = {1, 2, 3};
    xsflib::WriteFile(sample, kOther);

    const bool stopped = ThrowsRuntimeError([&] { WriteMidiAndSfz(conversion, midi); });
    const bool wrote = std::filesystem::exists(midi);
    std::vector<uint8_t> kept;
    xsflib::ReadFile(sample, kept);
    xsflib::WriteFile(sample, conversion.sfz.samples[0].wav);
    WriteMidiAndSfz(conversion, midi);

    SEQLIB_CHECK(stopped && !wrote && kept == kOther);
    SEQLIB_CHECK(FilesUnder(folder.Path()).size() == 1 + conversion.sfz.files.size() + conversion.sfz.samples.size());
}

// The tags go into the MIDI file as Standard MIDI Files 1.0 lays out a meta event: FF, the type, the text's length as a
// variable-length number, then the text. The copyright notice (type 2) comes first in the first track. The title takes
// the place of the sequence's name (type 3), the sound's name. A length of 200 takes two bytes: 0x81 0x48. The other
// events and tracks stay as they were. Without tags, the file stays as it is. A file whose first track doesn't start
// with the sequence's name can't take them, such as one whose first event is a text event (type 1). That event's type
// is byte 24: after the header's 14 bytes, the track's ID and size, the delta and FF.
void TestExportTags()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const export_::Conversion conversion = ConvertToMidi(*archive, kNoteSound, {}, {}, "Hokuto");
    const seqlib::midi::File before = seqlib::midi::Read(conversion.midi);
    const std::string kTitle = "ノート (take 2)";
    const std::string kCopyright(200, 'c');

    const auto meta = [](uint8_t type, std::vector<uint8_t> bytes, const std::string& text)
    {
        bytes.insert(bytes.begin(), {0xFF, type});
        bytes.insert(bytes.end(), text.begin(), text.end());

        return seqlib::midi::Event{0, bytes};
    };

    const auto rest = [](const seqlib::midi::Track& track, std::size_t from)
    {
        return std::vector(track.events.begin() + static_cast<std::ptrdiff_t>(from), track.events.end());
    };

    const std::vector<uint8_t> untagged = WithMidiTags(conversion.midi, {});
    const seqlib::midi::File titled = seqlib::midi::Read(WithMidiTags(conversion.midi, {kTitle, ""}));
    const seqlib::midi::File noticed = seqlib::midi::Read(WithMidiTags(conversion.midi, {"", kCopyright}));
    const std::vector<uint8_t> both = WithMidiTags(conversion.midi, {kTitle, kCopyright});
    const seqlib::midi::File tagged = seqlib::midi::Read(both);
    std::vector<uint8_t> text_first = conversion.midi;
    text_first.at(24) = 0x01;
    const bool refused = ThrowsRuntimeError([] { WithMidiTags(std::vector<uint8_t>{1, 2, 3}, {"x", ""}); }) &&
                         ThrowsRuntimeError([&text_first] { WithMidiTags(text_first, {"x", ""}); });

    SEQLIB_REQUIRE(before.tracks.size() > 1 && !before.tracks[0].events.empty());
    SEQLIB_CHECK(untagged == conversion.midi);
    SEQLIB_CHECK(before.tracks[0].events[0] == meta(0x03, {8}, "SEQ_NOTE"));
    SEQLIB_CHECK(titled.tracks[0].events[0] == meta(0x03, {18}, kTitle));
    SEQLIB_CHECK(rest(titled.tracks[0], 1) == rest(before.tracks[0], 1));
    SEQLIB_CHECK(noticed.tracks[0].events[0] == meta(0x02, {0x81, 0x48}, kCopyright));
    SEQLIB_CHECK(rest(noticed.tracks[0], 1) == before.tracks[0].events);
    SEQLIB_CHECK(tagged.tracks[0].events[0] == meta(0x02, {0x81, 0x48}, kCopyright));
    SEQLIB_CHECK(tagged.tracks[0].events[1] == meta(0x03, {18}, kTitle));
    SEQLIB_CHECK(rest(tagged.tracks[0], 2) == rest(before.tracks[0], 1));
    SEQLIB_CHECK(seqlib::midi::TrackNames(tagged)[0] == kTitle);
    for (std::size_t track = 1; track < before.tracks.size(); track++)
    {
        SEQLIB_CHECK(tagged.tracks.size() == before.tracks.size() &&
                     tagged.tracks[track].events == before.tracks[track].events);
    }
    SEQLIB_CHECK(seqlib::midi::Problems(both).empty());
    SEQLIB_CHECK(refused);
}

// An export of SFZ files leaves out the lines of ntdlib's report that name only the SoundFont, and an export of a
// SoundFont 2 file those that name only SFZ. A line that names both formats, or neither, stays in both. The lines are
// ntdlib's wording.
void TestReportFor()
{
    const std::vector<std::string> kReport = {
        "Approximated 4 times: low-pass filter (one-pole in the game, two-pole in the SoundFont)",
        "Approximated once: biquad type change (SFZ supports one type per track)",
        "Approximated twice: second to fourth LFOs (omitted)",
        "region volume exceeds 127 (SoundFont attenuated by 1.8 dB; SFZ keeps the original volume)"};

    SEQLIB_CHECK((ReportFor(kReport, true) == std::vector<std::string>{kReport[1], kReport[2], kReport[3]}));
    SEQLIB_CHECK((ReportFor(kReport, false) == std::vector<std::string>{kReport[0], kReport[2], kReport[3]}));
}

// A sequence of a Wii archive converts as revosf2 converts it without the game's main.dol: with the settings of the
// build of NW4R SND that the archive's version gives. Version 1.4 gives a build of June 2008, and earlier versions one
// of 2007. The sequence plays a note on the bank's first instrument.
void TestWiiExport()
{
    namespace rsar_files = ntdlib::testing::rsar_files;
    const std::vector<uint8_t> bytes =
        rsar_files::MakeArchive(rsar_files::MakeSequence({0x3C, 0x64, 0x30, 0xFF}), 0, 1);
    std::vector<uint8_t> older = bytes;
    xsflib::StoreBe16(&older[6], 0x0101);
    const std::shared_ptr<const ntdlib::rsar::SoundArchive> archive = LoadWiiArchive(bytes, "SOUND.brsar");
    const std::shared_ptr<const ntdlib::rsar::SoundArchive> older_archive = LoadWiiArchive(older, "SOUND.brsar");

    const export_::Conversion ours = ConvertToMidi(*archive, 0, {}, {}, "revosf2");
    const export_::Conversion theirs =
        export_::ConvertSequence(*archive, 0, export_::PerformOptions(),
                                 export_::RevolutionSettings(export_::BuildOfRsarVersion(0x0104)), "revosf2");

    SEQLIB_CHECK(ours.samples > 0 && ours.midi == theirs.midi && ours.sf2 == theirs.sf2);
    SEQLIB_REQUIRE(!ours.sfz.files.empty() && ours.sfz.files.size() == theirs.sfz.files.size());
    SEQLIB_CHECK(ours.sfz.files[0].text == theirs.sfz.files[0].text);
    SEQLIB_CHECK(hokuto::song::SettingsOf(*archive).wii_build.date == 20080620);
    SEQLIB_CHECK(hokuto::song::SettingsOf(*older_archive).wii_build.date == 0);

    // A sound past the archive's and bytes of another kind give messages.
    SEQLIB_CHECK(ThrowsRuntimeError([&] { ConvertToMidi(*archive, 1, {}, {}, "Hokuto"); }));
    SEQLIB_CHECK(ThrowsRuntimeError([] { LoadWiiArchive({1, 2, 3}, "SOUND.brsar"); }));
}

// The reader gives the sequence that ReadSequence gives, once. For a sound past the archive's sounds, it gives a
// message and no sequence.
void TestReader()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const Sequence direct = ReadSequence(*archive, kLoopSound);
    SequenceReader reader;

    reader.Request(archive, kLoopSound);
    const std::optional<SequenceReader::Result> read = WaitForRead(reader);
    const bool again = reader.Take().has_value();
    reader.Request(archive, kMissingSound);
    const std::optional<SequenceReader::Result> missing = WaitForRead(reader);

    SEQLIB_CHECK(read && read->sound == kLoopSound && read->sequence && *read->sequence == direct);
    SEQLIB_CHECK(read && read->error.empty() && !again && !reader.Reading());
    SEQLIB_CHECK(missing && missing->sound == kMissingSound && !missing->sequence && !missing->error.empty());
}

// A request abandons the read that the request before it started. Only the latest request's result comes. Abandon drops
// that one too.
void TestReaderAbandons()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    SequenceReader reader;

    reader.Request(archive, kLoopSound);
    reader.Request(archive, kNoteSound);
    const std::optional<SequenceReader::Result> read = WaitForRead(reader);
    reader.Request(archive, kReleaseSound);
    reader.Abandon();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    SEQLIB_CHECK(read && read->sound == kNoteSound && read->sequence && read->sequence->sound == kNoteSound);
    SEQLIB_CHECK(!reader.Reading() && !reader.Take());
}

// A read can build its archive first. The result holds the archive that the build made, and the sequence of it. A build
// that throws gives its message, with no archive and no sequence.
void TestReaderBuilds()
{
    const std::vector<uint8_t> bytes = TestArchiveBytes();
    const Sequence direct = ReadSequence(*LoadArchive(bytes, "test.bfsar"), kLoopSound);
    std::shared_ptr<const sar::SoundArchive> built;
    const auto build = [&bytes, &built]
    {
        built = LoadArchive(bytes, "test.bfsar");
        return built;
    };
    const auto fail = []() -> std::shared_ptr<const sar::SoundArchive>
    {
        throw std::invalid_argument("No archive.");
    };
    SequenceReader reader;

    reader.Request(build, kLoopSound);
    const std::optional<SequenceReader::Result> read = WaitForRead(reader);
    reader.Request(fail, kLoopSound);
    const std::optional<SequenceReader::Result> failed = WaitForRead(reader);

    SEQLIB_CHECK(read && built && read->archive == built && read->sequence && *read->sequence == direct);
    SEQLIB_CHECK(failed && !failed->archive && !failed->sequence && failed->error == "No archive.");
}

// After the read, a conversion gives the converted sound, and the read of it takes the same starting values. A
// conversion that throws gives its message and leaves the sequence. A read that fails converts nothing.
void TestReaderConverts()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const StartValues kValues = {{0, 1}};
    const Sequence direct = ReadSequence(*archive, kLoopSound, kValues);
    const Sequence converted_direct = ReadSequence(*archive, kNoteSound, kValues);
    int conversions = 0;
    const auto convert = [&archive, &conversions]
    {
        conversions++;

        return SequenceReader::EngineSound{.archive = archive, .sound = kNoteSound, .warnings = {"A warning."}};
    };
    const auto fail = []() -> SequenceReader::EngineSound
    {
        throw std::invalid_argument("No conversion.");
    };
    SequenceReader reader;

    reader.Request(archive, kLoopSound, kValues, {}, convert);
    const std::optional<SequenceReader::Result> read = WaitForRead(reader);
    reader.Request(archive, kLoopSound, kValues, {}, fail);
    const std::optional<SequenceReader::Result> failed = WaitForRead(reader);
    reader.Request(archive, kMissingSound, kValues, {}, convert);
    const std::optional<SequenceReader::Result> missing = WaitForRead(reader);
    reader.Request(archive, kLoopSound, kValues);
    const std::optional<SequenceReader::Result> alone = WaitForRead(reader);

    SEQLIB_CHECK(read && read->sequence && *read->sequence == direct && read->engine && read->engine_sequence);
    SEQLIB_CHECK(read && read->engine && read->engine->archive == archive && read->engine->sound == kNoteSound);
    SEQLIB_CHECK(read && read->engine && read->engine->warnings == std::vector<std::string>{"A warning."});
    SEQLIB_CHECK(read && read->engine_sequence && *read->engine_sequence == converted_direct);
    SEQLIB_CHECK(read && read->engine_error.empty());
    SEQLIB_CHECK(failed && failed->sequence && *failed->sequence == direct && !failed->engine);
    SEQLIB_CHECK(failed && !failed->engine_sequence && failed->engine_error == "No conversion.");
    SEQLIB_CHECK(missing && !missing->sequence && !missing->engine && missing->engine_error.empty());
    SEQLIB_CHECK(alone && alone->sequence && !alone->engine && !alone->engine_sequence);
    SEQLIB_CHECK(conversions == 1);
}

// From the start, the player writes the model's output from the model's first frame that runs a tick. The model makes
// no sound before it. Each frame's position is the tick count before it, round the loop. In 3.5 s, the output goes
// round the loop twice and goes on.
void TestPlayFromStart()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kLoopSound));
    const ModelRun model = RunModel(*archive, kLoopSound, 800);
    const std::size_t first = FirstFrameReaching(model, 1);
    const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
    clock->SetCycle(kGameLoop);
    SequencePlayer player(ModelOf(archive, sequence), 0, clock);

    const PlayerRun run = RunPlayer(player, *clock, 700);

    SEQLIB_CHECK(!run.ended);
    SEQLIB_CHECK(!clock->Ended());
    SEQLIB_CHECK(HasSound(std::span<const float>(run.samples)));
    SEQLIB_CHECK(!HasSound(std::span<const int16_t>(model.samples).first(first * kFrameSamples * 2)));
    SEQLIB_CHECK(Matches(run.samples, model, first, 0));
    SEQLIB_REQUIRE_EQ(run.marks.size(), 700);

    std::size_t wrong = 0;
    std::size_t wraps = 0;
    for (std::size_t i = 0; i < run.marks.size(); i++)
    {
        const double expected = PositionOfTicks(*sequence, model.before[first + i]);
        if (!run.marks[i] || *run.marks[i] != expected)
        {
            wrong++;
        }
        if (i > 0 && run.marks[i] && run.marks[i - 1] && *run.marks[i] < *run.marks[i - 1])
        {
            wraps++;
        }
    }
    SEQLIB_CHECK_EQ(wrong, 0);
    SEQLIB_CHECK_EQ(wraps, 2);
}

// A Wii U archive plays on ntdlib's model of nw::snd and AX, in frames of 3 ms: 144 samples at 48 kHz. The model's
// frames match the model run directly, from the start and after a seek back to the snapshot at frame 50. So does the
// player's output after its first frame, from the model's first frame that runs a tick. The player's first frame fades
// in. The note lasts 0.25 s, and the output ends once it has faded.
void TestWiiUPlays()
{
    constexpr std::size_t kWiiUFrame = cafe::ArchiveModel::kFrameSamples * 2;
    const std::shared_ptr<const sar::SoundArchive> archive = OpenWiiUArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kNoteSound));
    cafe::ArchiveModel direct(*archive);
    const cafe::SequenceSoundPlayer* sound = direct.engine->StartSequence(kNoteSound);
    SEQLIB_REQUIRE(sound != nullptr);
    std::vector<int16_t> expected;
    std::size_t first = 0;
    for (std::size_t frame = 0; frame < 600; frame++)
    {
        direct.engine->RunFrame();
        const std::span<const int16_t> output = direct.system.Output();
        expected.insert(expected.end(), output.begin(), output.end());
        first += sound->TickCount() == 0 ? 1 : 0;
    }

    const std::shared_ptr<SequenceModel> model = ModelOf(archive, sequence, 50);
    std::vector<int16_t> played;
    for (bool more = model->SeekFrame(0); more && played.size() < expected.size(); more = model->RunFrame())
    {
        const std::span<const int16_t> output = model->Output();
        played.insert(played.end(), output.begin(), output.end());
    }
    const bool sought = model->SeekFrame(70);
    const std::vector<int16_t> after_seek(model->Output().begin(), model->Output().end());
    const auto clock = std::make_shared<PlaybackClock>(model->FrameSamples());
    SequencePlayer player(model, 0, clock);
    std::vector<float> heard(expected.size());
    const std::size_t rendered = player.Render(heard.data(), heard.size() / 2);
    heard.resize(rendered * 2);

    SEQLIB_CHECK(archive->ReadOptions().flavor == sar::Flavor::kCafe);
    SEQLIB_CHECK_EQ(model->FrameSamples(), cafe::ArchiveModel::kFrameSamples);
    SEQLIB_CHECK_EQ(model->SampleRate(), 48000);
    SEQLIB_CHECK_EQ(sequence->timebase, 48);
    SEQLIB_REQUIRE(played.size() > 80 * kWiiUFrame && played.size() < expected.size());
    SEQLIB_CHECK(HasSound(std::span<const int16_t>(played)));
    SEQLIB_CHECK(std::equal(played.begin(), played.end(), expected.begin()));
    SEQLIB_CHECK(!HasSound(std::span<const int16_t>(expected).subspan(played.size())));
    SEQLIB_CHECK(sought && after_seek.size() == kWiiUFrame &&
                 std::equal(after_seek.begin(), after_seek.end(), expected.begin() + 70 * kWiiUFrame));
    SEQLIB_REQUIRE(rendered < expected.size() / 2 && heard.size() > 2 * kWiiUFrame);
    std::size_t wrong = 0;
    for (std::size_t i = kWiiUFrame; i < heard.size() - kWiiUFrame; i++)
    {
        wrong += heard[i] != static_cast<float>(expected[first * kWiiUFrame + i]) / 32768.0f ? 1 : 0;
    }
    SEQLIB_CHECK_EQ(wrong, 0);
}

// A note of 48 ticks.
const std::vector<uint8_t> kWiiNoteBytecode = {0x3C, 0x64, 0x30, 0xFF};

// kChangesBytecode as the Wii holds it, after a command that allocates track 0 (0xFE): the arguments of the allocation,
// the tempo, the comparison and the jumps are big-endian. The test archive's labels lie at bytes 0 and 8, at the starts
// of commands. The comparison starts at byte 10, and the note of key 72 at byte 27.
const std::vector<uint8_t> kWiiChangesBytecode = {0xFE, 0x00, 0x01, 0xB0, 48,   0xE1, 0x00, 120,  0x80, 1,   0xF0, 0x90,
                                                  0,    0x00, 1,    0xA2, 0x89, 0x00, 0x00, 27,   60,   100, 48,   0x89,
                                                  0x00, 0x00, 10,   72,   100,  48,   0x89, 0x00, 0x00, 10};

// The bytes of a Wii archive whose sequence plays `commands` on a looped wave: by default, a note of 48 ticks. ntdlib's
// test archive holds silent waves of DSP-ADPCM. Here each frame of the waves gives the samples of its nibbles at a
// scale of 2^11. The note's release sounds on for longer than the tests play it. Without `loop`, the wave plays its 112
// samples once. OpenWiiArchive opens the archive.
std::vector<uint8_t> WiiArchiveBytes(const std::vector<uint8_t>& commands = kWiiNoteBytecode, bool loop = true)
{
    namespace rsar_files = ntdlib::testing::rsar_files;
    std::vector<uint8_t> bytes = rsar_files::MakeArchive(rsar_files::MakeSequence(commands), 0, 1, loop);
    constexpr std::array<uint8_t, 7> kNibbles = {0x7F, 0x19, 0xE3, 0x5A, 0xC6, 0x2D, 0x91};
    for (std::size_t at = 0; at + 4 <= bytes.size(); at++)
    {
        if (std::memcmp(&bytes[at], "RWAV", 4) != 0)
        {
            continue;
        }

        // The wave's DATA block holds 0x40 bytes of samples after its header of 8 bytes.
        const uint32_t data = xsflib::LoadBe32(&bytes[at + 0x18]);
        for (std::size_t frame = 0; frame < 0x40; frame += 8)
        {
            uint8_t* samples = &bytes[at + data + 8 + frame];
            samples[0] = 0x0B;
            std::copy(kNibbles.begin(), kNibbles.end(), samples + 1);
        }
    }

    return bytes;
}

// The Wii archive of WiiArchiveBytes with `commands` and `loop`. The program opens an archive the same way.
std::shared_ptr<const ntdlib::rsar::SoundArchive> OpenWiiArchive(
    const std::vector<uint8_t>& commands = kWiiNoteBytecode, bool loop = true)
{
    return LoadWiiArchive(WiiArchiveBytes(commands, loop), "SOUND.brsar", ntdlib::rvl::snd::kRsarOptions);
}

// A Wii archive's sequence reads with revosf2's performer, and plays as ntdlib's model of NW4R SND and AX plays it run
// directly, with ntdlib's model of AX's microcode in place of the DSP: 96 stereo samples at 32 kHz in each frame, for
// 2.1 s from the start and after a seek. A muted track goes silent, also after a seek, and sounds again once the mute
// ends. The reader gives the same sequence.
void TestWiiPlays()
{
    constexpr std::size_t kWiiFrame = 96 * 2;
    const std::shared_ptr<const ntdlib::rsar::SoundArchive> archive = OpenWiiArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, 0));
    ntdlib::rvl::snd::ArchiveModel direct;
    direct.build = export_::BuildOfRsarVersion(archive->Version());
    direct.StartWithRenderer(*archive);
    SEQLIB_REQUIRE(direct.engine->StartSequence(0) != nullptr);
    std::vector<int16_t> expected;
    for (int frame = 0; frame < 700; frame++)
    {
        SEQLIB_REQUIRE(direct.RunFrame(expected));
    }

    const auto model = std::make_shared<SequenceModel>(archive, sequence, AuxEffects{}, 50);
    std::vector<int16_t> played;
    for (bool more = model->SeekFrame(0); more && played.size() < expected.size(); more = model->RunFrame())
    {
        const std::span<const int16_t> output = model->Output();
        played.insert(played.end(), output.begin(), output.end());
    }
    const bool sought = model->SeekFrame(70);
    const std::vector<int16_t> after_seek(model->Output().begin(), model->Output().end());

    // A muted track goes silent, and comes back once the mute ends. A seek keeps the mute. The mute reaches the engine:
    // with the only track muted, the sound ends with its note's 48 ticks.
    const auto muted = std::make_shared<SequenceModel>(archive, sequence, AuxEffects{}, 50);
    muted->SetMutes(1);
    std::vector<int16_t> silent;
    for (bool more = muted->SeekFrame(0); more && silent.size() < 300 * kWiiFrame; more = muted->RunFrame())
    {
        silent.insert(silent.end(), muted->Output().begin(), muted->Output().end());
    }
    const bool muted_seek = muted->SeekFrame(70) && !HasSound(muted->Output());
    muted->SetMutes(0);
    std::vector<int16_t> unmuted;
    for (int frame = 0; frame < 10 && muted->RunFrame(); frame++)
    {
        unmuted.insert(unmuted.end(), muted->Output().begin(), muted->Output().end());
    }
    SequenceReader reader;
    reader.Request(archive, 0);
    const std::optional<SequenceReader::Result> read = WaitForRead(reader);

    SEQLIB_CHECK_EQ(model->FrameSamples(), 96);
    SEQLIB_CHECK_EQ(model->SampleRate(), 32000);
    SEQLIB_CHECK_EQ(sequence->timebase, 48);
    SEQLIB_REQUIRE(sequence->tracks.size() == 1 && sequence->tracks[0].notes.size() == 1);
    const hokuto::song::Note& note = sequence->tracks[0].notes[0];
    SEQLIB_CHECK(note.start == 0 && note.length == 48 && note.key == 60 && note.velocity == 100);
    SEQLIB_REQUIRE(played.size() == expected.size());
    SEQLIB_CHECK(HasSound(std::span<const int16_t>(played)));
    SEQLIB_CHECK(played == expected);
    SEQLIB_CHECK(sought && after_seek.size() == kWiiFrame &&
                 std::equal(after_seek.begin(), after_seek.end(), expected.begin() + 70 * kWiiFrame));
    SEQLIB_CHECK(read && read->sequence && *read->sequence == *sequence && !read->archive);
    SEQLIB_CHECK(silent.size() > 100 * kWiiFrame && silent.size() < 300 * kWiiFrame);
    SEQLIB_CHECK(!HasSound(std::span<const int16_t>(silent)));
    SEQLIB_CHECK(muted_seek && HasSound(std::span<const int16_t>(unmuted)));
}

// A conversion for the Wii gives the sound in the Wii's archive, and the read of the converted sound follows the
// conversion's build of NW4R SND with the same starting values.
void TestReaderConvertsForWii()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const std::shared_ptr<const ntdlib::rsar::SoundArchive> wii = OpenWiiArchive(kWiiChangesBytecode);
    const ntdlib::SndBuild kBuild{.date = 20080620};
    const StartValues kValues = {{0, 1}};
    const auto convert = [&wii, &kBuild]
    {
        return SequenceReader::EngineSound{
            .archive = nullptr, .sound = 0, .warnings = {"A warning."}, .wii = wii, .wii_build = kBuild};
    };
    SequenceReader reader;
    reader.Request(archive, kLoopSound, kValues, {}, convert);
    const std::optional<SequenceReader::Result> read = WaitForRead(reader);

    SEQLIB_CHECK(read && read->sequence && *read->sequence == ReadSequence(*archive, kLoopSound, kValues));
    SEQLIB_CHECK(read && read->engine && read->engine->wii == wii && !read->engine->archive);
    SEQLIB_CHECK(read && read->engine && read->engine->warnings == std::vector<std::string>{"A warning."});
    SEQLIB_CHECK(read && read->engine_sequence && *read->engine_sequence == ReadSequence(*wii, 0, kValues, {}, kBuild));
    SEQLIB_CHECK(read && read->engine_error.empty());
}

// ntdlib's model of the Wii starts the sequence when the sound starts, and plays the first tick in its frame 0. The
// performer counts its frames from that frame (PerformOptionsOf). The sequence of kWiiChangesBytecode compares variable
// 0 with 1 at ticks 1, 49 and 97, every 48 ticks. A tick lasts 10.4167 ms at 120 BPM and 48 ticks to a quarter note.
// Frame F runs the ticks from 3F ms up to 3 (F + 1) ms. Frame 336 therefore runs tick 97, at 1,010.4 ms. A change at
// 1,008 ms comes in frame 336, before the comparison, and the pass from tick 97 is the loop. A change at 1,011 ms comes
// in frame 337, after it, and the loop starts at tick 145, in frame 503. Hokuto's model makes the changes in the same
// frames as the model run directly. The sound's commands read variable 0, with the choices -1 and 1.
void TestWiiReadChanges()
{
    const std::shared_ptr<const ntdlib::rsar::SoundArchive> archive = OpenWiiArchive(kWiiChangesBytecode);
    const auto run = [&archive](std::optional<uint64_t> change_frame)
    {
        ntdlib::rvl::snd::ArchiveModel direct;
        direct.build = export_::BuildOfRsarVersion(archive->Version());
        direct.StartWithRenderer(*archive);
        ntdlib::rvl::snd::SeqPlayer* player = direct.engine->StartSequence(0);
        std::pair<std::vector<uint32_t>, std::vector<int16_t>> ticks_and_output;
        for (uint64_t frame = 0; player != nullptr && frame < 600; frame++)
        {
            if (frame == change_frame)
            {
                player->SetVariable(*direct.engine, 0, 1);
            }
            if (!direct.RunFrame(ticks_and_output.second))
            {
                break;
            }
            ticks_and_output.first.push_back(player->TickCount());
        }

        return ticks_and_output;
    };

    const auto played = [&archive](const Changes& changes)
    {
        Sequence sequence = ReadSequence(*archive, 0);
        sequence.changes = changes;
        const auto model =
            std::make_shared<SequenceModel>(archive, std::make_shared<const Sequence>(sequence), AuxEffects{}, 50);
        std::vector<int16_t> output;
        for (bool more = model->SeekFrame(0); more && output.size() < 600 * 96 * 2; more = model->RunFrame())
        {
            output.insert(output.end(), model->Output().begin(), model->Output().end());
        }

        return output;
    };

    const auto [ticks, unchanged] = run(std::nullopt);
    const std::vector<int16_t> in_336 = run(336).second;
    const std::vector<int16_t> in_337 = run(337).second;
    const Sequence before = ReadSequence(*archive, 0, {}, {{1008, 0, 1}});
    const Sequence after = ReadSequence(*archive, 0, {}, {{1011, 0, 1}});

    SEQLIB_REQUIRE(ticks.size() == 600);
    SEQLIB_CHECK(ticks[0] == 1 && ticks[335] == 97 && ticks[336] == 98);
    SEQLIB_CHECK(before.loop == Loop({97, 145}));
    SEQLIB_CHECK(after.loop == Loop({145, 193}));
    SEQLIB_CHECK(after.read_variables == std::vector<Variable>({{0, {-1, 1}, false}}));
    SEQLIB_CHECK(after.variables == after.read_variables);
    SEQLIB_REQUIRE(after.tracks.size() == 1 && after.tracks[0].notes.size() == 4);
    SEQLIB_CHECK(IsNote(after.tracks[0].notes[3], 145, 48, 72, 100));
    SEQLIB_CHECK(in_336 != unchanged && in_337 != unchanged && in_336 != in_337);
    SEQLIB_CHECK(played({{1008, 0, 1}}) == in_336);
    SEQLIB_CHECK(played({{1011, 0, 1}}) == in_337);
}

// The output of `frames` frames of ntdlib's model of the Wii U, run directly from the start of sound `sound`, with
// `effects` on the aux buses. The TV's bus A returns at `return_volume`, in 1.15. With `enhanced`, the model mixes in
// its enhanced mode.
std::vector<int16_t> RunWiiUModel(const sar::SoundArchive& archive, uint32_t sound, std::size_t frames,
                                  std::vector<cafe::EffectSetup> effects = {},
                                  uint16_t return_volume = ntdlib::cafe::ax::kUnityVolume, bool enhanced = false)
{
    cafe::EngineOptions options;
    options.enhanced = enhanced;
    options.effects = std::move(effects);
    cafe::ArchiveModel model(archive, std::move(options));
    model.system.SetAuxReturnVolume(ntdlib::cafe::ax::System::Device::kTv, 0, 0, return_volume);
    std::vector<int16_t> samples;
    if (model.engine->StartSequence(sound) == nullptr)
    {
        return samples;
    }

    for (std::size_t frame = 0; frame < frames; frame++)
    {
        model.engine->RunFrame();
        const std::span<const int16_t> output = model.system.Output();
        samples.insert(samples.end(), output.begin(), output.end());
    }

    return samples;
}

// The output of `model` from its first frame until its output ends, or until `samples` samples.
std::vector<int16_t> RunToEnd(SequenceModel& model, std::size_t samples)
{
    std::vector<int16_t> played;
    for (bool more = model.SeekFrame(0); more && played.size() < samples; more = model.RunFrame())
    {
        const std::span<const int16_t> output = model.Output();
        played.insert(played.end(), output.begin(), output.end());
    }

    return played;
}

// The index after the last sample of `samples` that's louder than ±8, or 0.
std::size_t SoundEnd(std::span<const int16_t> samples)
{
    const auto loud = [](int16_t sample)
    {
        return std::abs(sample) > 8;
    };
    const auto last = std::find_if(samples.rbegin(), samples.rend(), loud);
    return static_cast<std::size_t>(samples.rend() - last);
}

// Each effect of the Wii U on aux bus A plays as ntdlib's model plays it on the TV's bus A, and changes the output. Its
// tail sounds after the note's release, and the output ends a second after the tail: a delay's echoes can come back
// after a quiet gap. A seek back to a snapshot restores the effect's state. An effect on bus B changes nothing. The
// sequence doesn't send there, and ntdlib's model of AX drops a send to a bus without an effect. The output then ends a
// second after the note. An I3DL2 reverb plays on the Switch only. Without it, the output ends with the note.
void TestWiiUEffects()
{
    constexpr std::size_t kWiiUFrame = cafe::ArchiveModel::kFrameSamples * 2;
    constexpr std::size_t kFrames = 3000;
    const std::shared_ptr<const sar::SoundArchive> archive = OpenWiiUSendArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kNoteSound));
    const std::vector<int16_t> dry = RunWiiUModel(*archive, kNoteSound, kFrames);
    const std::size_t dry_end = SoundEnd(dry);

    std::size_t wrong = 0;
    for (const cafe::EffectType type : {cafe::EffectType::kReverbHi, cafe::EffectType::kReverbStd,
                                        cafe::EffectType::kChorus, cafe::EffectType::kDelay})
    {
        const hokuto::song::WiiUEffect effect =
            hokuto::song::WiiUEffectOf(type, hokuto::song::WiiUStartingParameters(type));
        const std::vector<int16_t> direct =
            RunWiiUModel(*archive, kNoteSound, kFrames, {{cafe::kEffectDeviceTv, 0, type, effect.words, {}}});
        const auto model = ModelOf(archive, sequence, 50, {{{effect}, {}, {}}});
        const std::vector<int16_t> played = RunToEnd(*model, direct.size());
        const bool sought = model->SeekFrame(70);
        const std::span<const int16_t> after_seek = model->Output();

        const std::size_t end = SoundEnd(direct);
        const bool matches = played.size() > end && played.size() < direct.size() &&
                             std::equal(played.begin(), played.end(), direct.begin());
        const bool changes = !std::equal(dry.begin(), dry.begin() + static_cast<std::ptrdiff_t>(end), direct.begin());
        const bool tail = end > dry_end && played.size() >= end + 330 * kWiiUFrame;
        const bool restores = sought && after_seek.size() == kWiiUFrame &&
                              std::equal(after_seek.begin(), after_seek.end(), direct.begin() + 70 * kWiiUFrame);
        wrong += matches && changes && tail && restores ? 0 : 1;
    }

    const hokuto::song::WiiUEffect reverb = hokuto::song::WiiUEffectOf(
        cafe::EffectType::kReverbHi, hokuto::song::WiiUStartingParameters(cafe::EffectType::kReverbHi));
    const auto on_b = ModelOf(archive, sequence, 50, {{{}, {reverb}, {}}});
    const auto i3dl2 = ModelOf(archive, sequence, 50, {{{Reverb()}, {}, {}}});
    const std::vector<int16_t> played_on_b = RunToEnd(*on_b, dry.size());
    const std::vector<int16_t> played_i3dl2 = RunToEnd(*i3dl2, dry.size());

    SEQLIB_CHECK(dry_end > 100 * kWiiUFrame);
    SEQLIB_CHECK_EQ(wrong, 0);
    SEQLIB_CHECK(played_on_b.size() > dry_end && std::equal(played_on_b.begin(), played_on_b.end(), dry.begin()));
    SEQLIB_CHECK(played_i3dl2.size() > dry_end && played_i3dl2.size() < played_on_b.size() &&
                 std::equal(played_i3dl2.begin(), played_i3dl2.end(), dry.begin()));
    SEQLIB_CHECK(!HasSound(std::span<const int16_t>(dry).subspan(played_i3dl2.size())));
}

// The longest run of frames of `samples` from frame `from` up to sample `to` that hold no sound (nothing louder than
// ±8), in frames of `frame` samples.
std::size_t LongestQuiet(std::span<const int16_t> samples, std::size_t frame, std::size_t from, std::size_t to)
{
    std::size_t longest = 0;
    std::size_t quiet = 0;
    for (std::size_t at = from; (at + 1) * frame <= to; at++)
    {
        quiet = xsflib::rip::Audible(samples.subspan(at * frame, frame)) ? 0 : quiet + 1;
        longest = std::max(longest, quiet);
    }

    return longest;
}

// The first frame of `samples` from frame `from` whose samples are all 0, in frames of `frame` samples, or the number
// of whole frames if none is.
std::size_t FirstSilentFrame(std::span<const int16_t> samples, std::size_t frame, std::size_t from)
{
    const auto sound = [](int16_t sample)
    {
        return sample != 0;
    };

    std::size_t at = from;
    while ((at + 1) * frame <= samples.size() && std::ranges::any_of(samples.subspan(at * frame, frame), sound))
    {
        at++;
    }

    return at;
}

// A delay of 1.5 s on the Wii U's bus A leaves gaps of more than a second between a note's echoes. Like ntdlib's model
// run directly, the output plays on through the gaps. The effect may ring for a second and the delay's time after the
// last echo louder than ±8: 834 frames of 3 ms. The output then ends. Quieter echoes still give samples of ±8 at most
// there, and a delay that feeds back can give them for ever.
void TestWiiUEffectsRing()
{
    constexpr std::size_t kWiiUFrame = cafe::ArchiveModel::kFrameSamples * 2;
    constexpr std::size_t kFrames = 8000;
    const std::shared_ptr<const sar::SoundArchive> archive = OpenWiiUSendArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kNoteSound));
    std::vector<float> parameters = hokuto::song::WiiUStartingParameters(cafe::EffectType::kDelay);
    parameters[0] = 1500.0f;
    parameters[3] = 1500.0f;
    const hokuto::song::WiiUEffect delay = hokuto::song::WiiUEffectOf(cafe::EffectType::kDelay, parameters);
    const std::vector<int16_t> dry = RunWiiUModel(*archive, kNoteSound, kFrames);
    const std::vector<int16_t> direct =
        RunWiiUModel(*archive, kNoteSound, kFrames, {{cafe::kEffectDeviceTv, 0, delay.type, delay.words, {}}});

    const auto model = ModelOf(archive, sequence, 50, {{{delay}, {}, {}}});
    const std::vector<int16_t> played = RunToEnd(*model, direct.size());

    const std::size_t end = SoundEnd(direct);
    SEQLIB_REQUIRE(end > 0);
    const std::size_t gap = LongestQuiet(direct, kWiiUFrame, SoundEnd(dry) / kWiiUFrame + 1, end);
    const std::size_t after_last = played.size() > end ? (played.size() - end) / kWiiUFrame : 0;
    const std::size_t silence = FirstSilentFrame(direct, kWiiUFrame, (end - 1) / kWiiUFrame + 834);
    SEQLIB_CHECK(gap > 334);
    SEQLIB_CHECK(played.size() < direct.size() && std::equal(played.begin(), played.end(), direct.begin()));
    SEQLIB_CHECK(after_last >= 834);
    SEQLIB_CHECK_EQ(played.size(), ((end - 1) / kWiiUFrame + 835) * kWiiUFrame);
    SEQLIB_CHECK(silence > (end - 1) / kWiiUFrame + 834);
}

// In enhanced mixing, a Wii U archive plays on ntdlib's model of AX in its enhanced mode. The model's frames match the
// model run directly, in floating point, after a seek back to the snapshot at frame 50. So does the player's output
// after its first frame, from the model's first frame that runs a tick. The player's first frame fades in. The output
// differs from strict mixing.
void TestWiiUEnhancedMixing()
{
    constexpr std::size_t kWiiUFrame = cafe::ArchiveModel::kFrameSamples * 2;
    const std::shared_ptr<const sar::SoundArchive> archive = OpenWiiUArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kNoteSound));
    cafe::EngineOptions options;
    options.enhanced = true;
    cafe::ArchiveModel direct(*archive, options);
    const cafe::SequenceSoundPlayer* sound = direct.engine->StartSequence(kNoteSound);
    SEQLIB_REQUIRE(sound != nullptr);
    std::vector<float> expected;
    std::vector<int16_t> rounded;
    std::size_t first = 0;
    for (std::size_t frame = 0; frame < 600; frame++)
    {
        direct.engine->RunFrame();
        const std::span<const float> output = direct.system.OutputFloat();
        expected.insert(expected.end(), output.begin(), output.end());
        rounded.insert(rounded.end(), direct.system.Output().begin(), direct.system.Output().end());
        first += sound->TickCount() == 0 ? 1 : 0;
    }
    const std::vector<int16_t> strict = RunWiiUModel(*archive, kNoteSound, 600);

    const std::shared_ptr<SequenceModel> model = ModelOf(archive, sequence, 50, {}, Mixing::kEnhanced);
    const bool sought = model->SeekFrame(70);
    const std::span<const float> sought_output = model->FloatOutput();
    const std::vector<float> after_seek(sought_output.begin(), sought_output.end());
    const auto clock = std::make_shared<PlaybackClock>(model->FrameSamples());
    SequencePlayer player(model, 0, clock);
    std::vector<float> heard(expected.size());
    const std::size_t rendered = player.Render(heard.data(), heard.size() / 2);
    heard.resize(rendered * 2);

    SEQLIB_REQUIRE(expected.size() == rounded.size() && strict.size() == rounded.size());
    SEQLIB_CHECK(sought && after_seek.size() == kWiiUFrame &&
                 std::equal(after_seek.begin(), after_seek.end(), expected.begin() + 70 * kWiiUFrame));
    SEQLIB_REQUIRE(rendered < expected.size() / 2 && heard.size() > 2 * kWiiUFrame);
    SEQLIB_CHECK(std::equal(heard.begin() + kWiiUFrame, heard.end() - kWiiUFrame,
                            expected.begin() + static_cast<std::ptrdiff_t>((first + 1) * kWiiUFrame)));
    SEQLIB_CHECK(rounded != strict && HasSound(std::span<const int16_t>(rounded)));
}

// A reverb's modes become their whole numbers, and the other parameters their floats' bits. A negative mode becomes 0.
// Parameters past the structure's words are left out.
void TestWiiUEffectWords()
{
    const std::vector<float> kParameters = {0.5f, 2.0f, 0.25f, 0.75f, 1.0f, 3.0f, 0.5f, -2.0f, 0.0f, 1.0f, 9.0f};

    const hokuto::song::WiiUEffect reverb = hokuto::song::WiiUEffectOf(cafe::EffectType::kReverbStd, kParameters);
    const hokuto::song::WiiUEffect delay = hokuto::song::WiiUEffectOf(cafe::EffectType::kDelay, kParameters);

    SEQLIB_REQUIRE(reverb.words.size() == 10 && delay.words.size() == 5);
    SEQLIB_CHECK(reverb.type == cafe::EffectType::kReverbStd && delay.type == cafe::EffectType::kDelay);
    SEQLIB_CHECK(reverb.words[0] == 0x3F000000 && reverb.words[4] == 0x3F800000 && reverb.words[9] == 0x3F800000);
    SEQLIB_CHECK(reverb.words[5] == 3 && reverb.words[6] == 0x3F000000 && reverb.words[7] == 0);
    SEQLIB_CHECK(delay.words[2] == 0x3E800000 && delay.words[3] == 0x3F400000 && delay.words[4] == 0x3F800000);
    for (const cafe::EffectType type : {cafe::EffectType::kReverbHi, cafe::EffectType::kReverbStd,
                                        cafe::EffectType::kChorus, cafe::EffectType::kDelay})
    {
        SEQLIB_CHECK_EQ(hokuto::song::WiiUStartingParameters(type).size(), cafe::EffectParamWords(type));
    }
}

// A note sent to aux bus A (0xD9 with 127) as the Wii holds it.
const std::vector<uint8_t> kWiiSendBytecode = {0xD9, 127, 0x3C, 0x64, 0x30, 0xFF};

// The same note after a wait of 12 ticks (0x80 with 12). The effects are on the buses by then.
const std::vector<uint8_t> kWiiLateSendBytecode = {0xD9, 127, 0x80, 12, 0x3C, 0x64, 0x30, 0xFF};

// The output of `frames` frames of ntdlib's model of the Wii, run directly from the start of the archive's sound 0,
// with `effects` on the aux buses. Bus A returns at `return_volume`, in 1.15. The model follows `build` of NW4R SND, or
// else the build that the archive's version gives.
std::vector<int16_t> RunWiiModel(const ntdlib::rsar::SoundArchive& archive, std::size_t frames,
                                 const std::vector<ntdlib::rvl::snd::EffectSetup>& effects = {},
                                 uint16_t return_volume = ntdlib::rvl::ax::kMaxVolume,
                                 const std::optional<ntdlib::SndBuild>& build = {})
{
    ntdlib::rvl::snd::ArchiveModel model;
    model.build = build.value_or(export_::BuildOfRsarVersion(archive.Version()));
    model.StartWithRenderer(archive);
    model.engine->SetEffects(effects, {}, 0);
    std::vector<int16_t> samples;
    if (model.engine->StartSequence(0) == nullptr)
    {
        return samples;
    }

    // NW4R SND returns the bus at full volume as it appends the bus's first effect, in the first frame. A game sets the
    // return volume after that.
    for (std::size_t frame = 0; frame < frames; frame++)
    {
        model.ax.SetAuxReturnVolume(0, return_volume);
        if (!model.RunFrame(samples))
        {
            break;
        }
    }

    return samples;
}

// The setup of `effect` on aux bus A in the form that ntdlib's engine takes.
ntdlib::rvl::snd::EffectSetup SetupOnA(const hokuto::song::WiiEffect& effect)
{
    ntdlib::rvl::snd::EffectSetup setup;
    setup.type = effect.type;
    std::copy(effect.words.begin(), effect.words.end(), setup.params.begin());

    return setup;
}

// Each effect of the Wii on aux bus A plays as ntdlib's model of NW4R SND and AX plays it run directly, and changes the
// output. A seek back to a snapshot restores the effect's state. At half volume, bus A returns as AX returns it at
// 0x4000. An effect of the Wii U changes nothing on the Wii.
void TestWiiEffects()
{
    constexpr std::size_t kWiiFrame = 96 * 2;
    constexpr std::size_t kFrames = 700;
    const std::shared_ptr<const ntdlib::rsar::SoundArchive> archive = OpenWiiArchive(kWiiSendBytecode);
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, 0));
    const std::vector<int16_t> dry = RunWiiModel(*archive, kFrames);

    std::size_t wrong = 0;
    for (const ntdlib::rvl::snd::EffectType type :
         {ntdlib::rvl::snd::EffectType::kReverbHi, ntdlib::rvl::snd::EffectType::kReverbHi2008,
          ntdlib::rvl::snd::EffectType::kChorus, ntdlib::rvl::snd::EffectType::kDelay,
          ntdlib::rvl::snd::EffectType::kReverbStd, ntdlib::rvl::snd::EffectType::kReverbStd2008})
    {
        const hokuto::song::WiiEffect effect =
            hokuto::song::WiiEffectOf(type, hokuto::song::WiiStartingParameters(type));
        const std::vector<int16_t> direct = RunWiiModel(*archive, kFrames, {SetupOnA(effect)});
        const auto model = std::make_shared<SequenceModel>(archive, sequence, AuxEffects{{{effect}, {}, {}}}, 50);
        const std::vector<int16_t> played = RunToEnd(*model, direct.size());
        const bool sought = model->SeekFrame(70);
        const std::span<const int16_t> after_seek = model->Output();

        const bool restores = sought && after_seek.size() == kWiiFrame &&
                              std::equal(after_seek.begin(), after_seek.end(), direct.begin() + 70 * kWiiFrame);
        wrong += played == direct && direct != dry && restores ? 0 : 1;
    }

    const hokuto::song::WiiEffect reverb =
        hokuto::song::WiiEffectOf(ntdlib::rvl::snd::EffectType::kReverbHi2008,
                                  hokuto::song::WiiStartingParameters(ntdlib::rvl::snd::EffectType::kReverbHi2008));
    const std::vector<int16_t> full = RunWiiModel(*archive, kFrames, {SetupOnA(reverb)});
    const std::vector<int16_t> half = RunWiiModel(*archive, kFrames, {SetupOnA(reverb)}, 0x4000);
    const auto halved = std::make_shared<SequenceModel>(archive, sequence, AuxEffects{{{reverb}, {}, {}}}, 50);
    halved->SetBusVolumes({0.5f, 1.0f, 1.0f});
    const std::vector<int16_t> played_half = RunToEnd(*halved, half.size());
    const hokuto::song::WiiUEffect wii_u = hokuto::song::WiiUEffectOf(
        cafe::EffectType::kReverbHi, hokuto::song::WiiUStartingParameters(cafe::EffectType::kReverbHi));
    const auto other = std::make_shared<SequenceModel>(archive, sequence, AuxEffects{{{wii_u}, {}, {}}}, 50);
    const std::vector<int16_t> played_other = RunToEnd(*other, dry.size());

    SEQLIB_REQUIRE(dry.size() == kFrames * kWiiFrame);
    SEQLIB_CHECK(HasSound(std::span<const int16_t>(dry)));
    SEQLIB_CHECK_EQ(wrong, 0);
    SEQLIB_CHECK(played_half == half && half != full && half != dry);
    SEQLIB_CHECK(played_other == dry);
}

// A short note's echoes on a delay on bus A go on after the dry sound has ended, and after the model without the delay
// ends. Quiet gaps lie between them. The note waits until the bus has the delay: a game appends its effects in the
// first frame after the sound starts. The output plays on through the gaps as the model run directly plays them. It
// ends a second and the delay's time after the last echo louder than ±8. With a delay of 160 ms, that's 388 frames of 3
// ms. A delay of 1.5 s leaves gaps of more than a second between its echoes, and the output ends 834 frames after the
// last.
void TestWiiEffectsRing()
{
    constexpr std::size_t kWiiFrame = 96 * 2;
    constexpr std::size_t kFrames = 8000;
    const std::shared_ptr<const ntdlib::rsar::SoundArchive> archive = OpenWiiArchive(kWiiLateSendBytecode, false);
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, 0));
    const auto dry = std::make_shared<SequenceModel>(archive, sequence, AuxEffects{}, 50);
    const std::vector<int16_t> played_dry = RunToEnd(*dry, kFrames * kWiiFrame);
    std::vector<float> parameters = hokuto::song::WiiStartingParameters(ntdlib::rvl::snd::EffectType::kDelay);

    std::vector<std::size_t> gaps;
    std::vector<std::size_t> after_last;
    std::size_t wrong = 0;
    for (const float delay_ms : {160.0f, 1500.0f})
    {
        parameters[0] = delay_ms;
        const hokuto::song::WiiEffect delay =
            hokuto::song::WiiEffectOf(ntdlib::rvl::snd::EffectType::kDelay, parameters);
        const AuxEffects effects{{{delay}, {}, {}}};
        const std::vector<int16_t> direct = RunWiiModel(*archive, kFrames, {SetupOnA(delay)});
        const auto model = std::make_shared<SequenceModel>(archive, sequence, effects, 50);
        const std::vector<int16_t> played = RunToEnd(*model, direct.size());

        const std::size_t end = SoundEnd(direct);
        gaps.push_back(LongestQuiet(direct, kWiiFrame, played_dry.size() / kWiiFrame, end));
        after_last.push_back(played.size() > end ? (played.size() - end) / kWiiFrame : 0);
        wrong += end > played_dry.size() + 100 * kWiiFrame && end + 900 * kWiiFrame < direct.size() &&
                         std::equal(played.begin(), played.end(), direct.begin())
                     ? 0
                     : 1;
    }

    SEQLIB_CHECK_EQ(wrong, 0);
    SEQLIB_REQUIRE(gaps.size() == 2);
    SEQLIB_CHECK(gaps[0] > 10 && gaps[0] < 334 && gaps[1] > 334);
    SEQLIB_CHECK(after_last[0] >= 386 && after_last[0] <= 390);
    SEQLIB_CHECK(after_last[1] >= 832 && after_last[1] <= 836);
}

// A reverb of 2008's modes become their whole numbers, and the other parameters their floats' bits. A negative mode
// becomes 0. Parameters past the structure's words are left out. The starting values fill each structure.
void TestWiiEffectWords()
{
    const std::vector<float> kParameters = {0.5f, 2.0f, 0.25f, 0.75f, 1.0f, 3.0f, 0.5f, -2.0f, 0.0f, 1.0f, 9.0f};

    const hokuto::song::WiiEffect reverb =
        hokuto::song::WiiEffectOf(ntdlib::rvl::snd::EffectType::kReverbStd2008, kParameters);
    const hokuto::song::WiiEffect older =
        hokuto::song::WiiEffectOf(ntdlib::rvl::snd::EffectType::kReverbStd, kParameters);
    const hokuto::song::WiiEffect delay = hokuto::song::WiiEffectOf(ntdlib::rvl::snd::EffectType::kDelay, kParameters);

    SEQLIB_REQUIRE(reverb.words.size() == 10 && older.words.size() == 5 && delay.words.size() == 3);
    SEQLIB_CHECK(reverb.words[0] == 0x3F000000 && reverb.words[4] == 0x3F800000 && reverb.words[9] == 0x3F800000);
    SEQLIB_CHECK(reverb.words[5] == 3 && reverb.words[6] == 0x3F000000 && reverb.words[7] == 0);
    SEQLIB_CHECK(older.words[4] == 0x3F800000 && delay.words[2] == 0x3E800000);
    for (const ntdlib::rvl::snd::EffectType type :
         {ntdlib::rvl::snd::EffectType::kReverbHi, ntdlib::rvl::snd::EffectType::kReverbHi2008,
          ntdlib::rvl::snd::EffectType::kChorus, ntdlib::rvl::snd::EffectType::kDelay,
          ntdlib::rvl::snd::EffectType::kReverbStd, ntdlib::rvl::snd::EffectType::kReverbStd2008})
    {
        SEQLIB_CHECK_EQ(hokuto::song::WiiStartingParameters(type).size(), ntdlib::rvl::snd::EffectParamWords(type));
    }
}

// A tempo of 2,000 BPM (0xE1 with 0x07D0), then a note of 48 ticks, a wait of 48 ticks (0x80 with 48) and a note of key
// 72.
const std::vector<uint8_t> kWiiFastBytecode = {0xE1, 0x07, 0xD0, 0x3C, 0x64, 0x30, 0x80, 0x30, 0x48, 0x64, 0x30, 0xFF};

// The archive's version, 1.4, gives a build of NW4R SND of June 2008. That build holds a tempo to 1,023 BPM. The game's
// build of 2007 keeps 2,000 BPM, and the second note starts sooner. The sequence keeps the build that read it, and the
// model plays by it as ntdlib's model of that build plays run directly.
void TestWiiGameBuild()
{
    constexpr std::size_t kFrames = 400;
    const std::shared_ptr<const ntdlib::rsar::SoundArchive> archive = OpenWiiArchive(kWiiFastBytecode);
    const ntdlib::SndBuild kWiiSports{.date = 20070608};
    const auto by_version = std::make_shared<const Sequence>(ReadSequence(*archive, 0));
    const auto by_game = std::make_shared<const Sequence>(ReadSequence(*archive, 0, {}, {}, kWiiSports));
    const std::vector<int16_t> june = RunWiiModel(*archive, kFrames);
    const std::vector<int16_t> sports = RunWiiModel(*archive, kFrames, {}, ntdlib::rvl::ax::kMaxVolume, kWiiSports);
    const auto version_model = std::make_shared<SequenceModel>(archive, by_version, AuxEffects{}, 50);
    const std::vector<int16_t> played_version = RunToEnd(*version_model, june.size());
    const auto game_model = std::make_shared<SequenceModel>(archive, by_game, AuxEffects{}, 50);
    const std::vector<int16_t> played_game = RunToEnd(*game_model, sports.size());

    SEQLIB_REQUIRE(!by_version->tempo.empty() && !by_game->tempo.empty());
    SEQLIB_CHECK(by_version->tempo.front().bpm == 1023.0 && by_game->tempo.front().bpm == 2000.0);
    SEQLIB_CHECK(by_version->wii_build && by_version->wii_build->date == 20080620);
    SEQLIB_CHECK(by_game->wii_build == kWiiSports);
    SEQLIB_CHECK(june != sports);
    SEQLIB_CHECK(!played_version.empty() && std::equal(played_version.begin(), played_version.end(), june.begin()));
    SEQLIB_CHECK(!played_game.empty() && std::equal(played_game.begin(), played_game.end(), sports.begin()));
}

// A main.dol's banner dates its build of NW4R SND. The microcode of AX of October 2006 marks the build that comes with
// it. That build has no banner. Bytes without AX's microcode give no build. The main.dol of an extracted disc lies in
// sys, beside the files folder that holds the archive at any depth. A folder without one gives none.
void TestWiiBuildOfDol()
{
    const std::vector<uint8_t> sports = ntdlib::testing::MicrocodeDol(
        0x140, "<< NW4R    - SND \tfinal   build: Jun  8 2007 11:17:15 (0x4199_60831) >>");
    const std::vector<uint8_t> smooth_moves = ntdlib::testing::MicrocodeDol(0x120, "");
    const std::optional<ntdlib::SndBuild> sports_build = hokuto::song::ReadWiiBuild(sports);
    const std::optional<ntdlib::SndBuild> smooth_moves_build = hokuto::song::ReadWiiBuild(smooth_moves);
    const std::vector<uint8_t> kZeros(0x3000, 0);

    const TemporaryFolder folder;
    const std::filesystem::path disc = folder.Path() / "disc";
    std::filesystem::create_directories(disc / "files" / "Sound" / "Scene");
    std::filesystem::create_directories(disc / "sys");
    const std::filesystem::path archive = disc / "files" / "Sound" / "Scene" / "sound.brsar";
    xsflib::WriteFile(archive, std::vector<uint8_t>{1});
    xsflib::WriteFile(disc / "sys" / "main.dol", sports);
    const std::filesystem::path flat = folder.Path() / "sound.brsar";
    xsflib::WriteFile(flat, std::vector<uint8_t>{1});
    const std::optional<std::filesystem::path> found = hokuto::song::FindMainDol(archive);

    SEQLIB_CHECK(sports_build && sports_build->date == 20070608 && !sports_build->october_2006);
    SEQLIB_CHECK(smooth_moves_build && smooth_moves_build->date == 0 && smooth_moves_build->october_2006);
    SEQLIB_CHECK(!hokuto::song::ReadWiiBuild(kZeros));
    SEQLIB_CHECK(found && std::filesystem::equivalent(*found, disc / "sys" / "main.dol"));
    SEQLIB_CHECK(!hokuto::song::FindMainDol(flat));
}

// The note with its main send at 0 (0xDB with 0) and its send to aux bus A at 127, after a wait of 12 ticks (0x80 with
// 12). Only bus A's return reaches the output.
const std::vector<uint8_t> kWiiWetBytecode = {0xDB, 0, 0xD9, 127, 0x80, 12, 0x3C, 0x64, 0x30, 0xFF};

// The peaks of a frame of a model's output, left and right.
std::array<int32_t, 2> FramePeaks(std::span<const int16_t> frame)
{
    std::array<int32_t, 2> peaks = {0, 0};
    for (std::size_t i = 0; i < frame.size(); i++)
    {
        int32_t& peak = peaks[i % 2];
        peak = std::max(peak, std::abs(static_cast<int32_t>(frame[i])));
    }

    return peaks;
}

// A run of a model's meters: the frames in which a meter differs from the peaks of the output, the frames with sound,
// and the frames in which another meter isn't silent.
struct MeterRun
{
    std::size_t wrong = 0;
    std::size_t sounding = 0;
    std::size_t noisy = 0;
};

// Runs `model` from frame `first`, for `frames` frames at most. In each frame, the meter of track `track`, or else of
// aux bus `bus`, has to hold the output's peaks. Every other meter has to stay silent. With neither, the output has to
// stay silent too.
MeterRun RunMeters(SequenceModel& model, uint64_t first, std::size_t frames, int track, int bus = -1)
{
    constexpr std::array<int32_t, 2> kSilent = {0, 0};
    MeterRun run;
    std::size_t frame = 0;
    for (bool more = model.SeekFrame(first); more && frame < frames; more = model.RunFrame(), frame++)
    {
        const std::array<int32_t, 2> peaks = FramePeaks(model.Output());
        const std::array<int32_t, 2> meter = track >= 0 ? model.TrackPeaks(track) : model.BusPeaks(bus);
        bool quiet = true;
        for (int other = 0; other < SequenceModel::kTracks; other++)
        {
            quiet = quiet && (other == track || model.TrackPeaks(other) == kSilent);
        }
        for (int other = 0; other < static_cast<int>(kAuxBuses); other++)
        {
            quiet = quiet && (other == bus || model.BusPeaks(other) == kSilent);
        }
        run.sounding += peaks != kSilent ? 1 : 0;
        run.wrong += meter == peaks ? 0 : 1;
        run.noisy += quiet ? 0 : 1;
    }

    return run;
}

// On the Wii, the note plays on the main bus alone, and track 0's meter holds the peaks of the output in each frame,
// from the start and after a seek. The other meters stay silent. Once the track is muted, its meter stays at 0. The
// output fades meanwhile. With the note's main send at 0, and no effect on bus A, the output and the meters stay
// silent. With the high-quality reverb on bus A, bus A's meter holds the peaks of the output in each frame, at full
// volume and at half volume, and the other meters stay silent. The reverb's crosstalk puts sound on the surround
// channel. The surround channel reaches the left and right a frame later. The model takes a snapshot every 50 frames
// while the sequence plays, and a seek back to each frame from 205 to 45 starts from one: the meter holds the output's
// peaks there too.
void TestWiiMeters()
{
    constexpr std::size_t kFrames = 700;
    const std::shared_ptr<const ntdlib::rsar::SoundArchive> archive = OpenWiiArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, 0));
    const auto model = std::make_shared<SequenceModel>(archive, sequence, AuxEffects{}, 50);
    const MeterRun heard = RunMeters(*model, 0, kFrames, 0);
    const MeterRun sought = RunMeters(*model, 70, 100, 0);
    const MeterRun before_mute = RunMeters(*model, 0, 100, 0);
    model->SetMutes(1);
    const MeterRun muted = RunMeters(*model, 100, kFrames, -1);

    const std::shared_ptr<const ntdlib::rsar::SoundArchive> wet = OpenWiiArchive(kWiiWetBytecode);
    const auto wet_sequence = std::make_shared<const Sequence>(ReadSequence(*wet, 0));
    const auto dry = std::make_shared<SequenceModel>(wet, wet_sequence, AuxEffects{}, 50);
    const MeterRun silent = RunMeters(*dry, 0, kFrames, -1);
    const hokuto::song::WiiEffect reverb =
        hokuto::song::WiiEffectOf(ntdlib::rvl::snd::EffectType::kReverbHi2008,
                                  hokuto::song::WiiStartingParameters(ntdlib::rvl::snd::EffectType::kReverbHi2008));
    const auto full = std::make_shared<SequenceModel>(wet, wet_sequence, AuxEffects{{{reverb}, {}, {}}}, 50);
    const MeterRun returned = RunMeters(*full, 0, kFrames, -1, 0);
    std::size_t seeks = 0;
    for (uint64_t frame = 205; frame >= 45; frame--)
    {
        const MeterRun seek = RunMeters(*full, frame, 1, -1, 0);
        seeks += seek.wrong == 0 && seek.noisy == 0 ? 1 : 0;
    }
    const auto half = std::make_shared<SequenceModel>(wet, wet_sequence, AuxEffects{{{reverb}, {}, {}}}, 50);
    half->SetBusVolumes({0.5f, 1.0f, 1.0f});
    const MeterRun halved = RunMeters(*half, 0, kFrames, -1, 0);
    const bool quieter = half->SeekFrame(300) && full->SeekFrame(300) && half->BusPeaks(0)[0] < full->BusPeaks(0)[0];

    SEQLIB_CHECK(heard.sounding > 600 && heard.wrong == 0 && heard.noisy == 0);
    SEQLIB_CHECK(sought.sounding == 100 && sought.wrong == 0 && sought.noisy == 0);
    SEQLIB_CHECK(before_mute.sounding > 90 && before_mute.wrong == 0 && before_mute.noisy == 0);
    SEQLIB_CHECK(muted.sounding > 0 && muted.noisy == 0);
    SEQLIB_CHECK(silent.sounding == 0 && silent.wrong == 0 && silent.noisy == 0);
    SEQLIB_CHECK(returned.sounding > 500 && returned.wrong == 0 && returned.noisy == 0);
    SEQLIB_CHECK_EQ(full->SnapshotCount(), 5);
    SEQLIB_CHECK_EQ(seeks, 161);
    SEQLIB_CHECK(halved.sounding > 500 && halved.wrong == 0 && halved.noisy == 0);
    SEQLIB_CHECK(quieter);
}

// A sequence plays with the values that its variables start with. The player's output matches ntdlib's model run
// directly, with the values set before the start as a game sets them. Before the model starts, its variables hold the
// starting values. The clock gives each frame's values, the global ones included: the sound sets variable 1 to 5 in its
// first tick, unless variable 0 starts at 1.
void TestPlayVariables()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenVariablesArchive();
    const auto unset = std::make_shared<const Sequence>(ReadSequence(*archive, kVariablesSound));
    const auto chosen = std::make_shared<const Sequence>(ReadSequence(*archive, kVariablesSound, {{0, 1}, {16, 3}}));
    const ModelRun unset_model = RunModel(*archive, kVariablesSound, 100);
    const ModelRun chosen_model = RunModel(*archive, kVariablesSound, 100, {}, {{0, 1}, {16, 3}});
    const auto unset_clock = std::make_shared<PlaybackClock>(kFrameSamples);
    const auto chosen_clock = std::make_shared<PlaybackClock>(kFrameSamples);
    const std::shared_ptr<SequenceModel> waiting = ModelOf(archive, chosen);
    SequencePlayer unset_player(ModelOf(archive, unset), 0, unset_clock);
    SequencePlayer chosen_player(ModelOf(archive, chosen), 0, chosen_clock);

    const std::array<int16_t, 32> before = waiting->Variables();
    const PlayerRun unset_run = RunPlayer(unset_player, *unset_clock, 60);
    const PlayerRun chosen_run = RunPlayer(chosen_player, *chosen_clock, 60);

    SEQLIB_CHECK(before[0] == 1 && before[1] == -1 && before[16] == 3 && before[17] == -1);
    SEQLIB_CHECK(HasSound(std::span<const float>(unset_run.samples)));
    SEQLIB_CHECK(HasSound(std::span<const float>(chosen_run.samples)));
    SEQLIB_CHECK(Matches(unset_run.samples, unset_model, FirstFrameReaching(unset_model, 1), 0));
    SEQLIB_CHECK(Matches(chosen_run.samples, chosen_model, FirstFrameReaching(chosen_model, 1), 0));
    SEQLIB_CHECK(unset_model.samples != chosen_model.samples);
    SEQLIB_REQUIRE(!unset_run.variables.empty() && !chosen_run.variables.empty());
    SEQLIB_REQUIRE(unset_run.variables[0] && chosen_run.variables[0]);
    SEQLIB_CHECK((*unset_run.variables[0])[0] == -1 && (*unset_run.variables[0])[1] == 5);
    SEQLIB_CHECK((*chosen_run.variables[0])[0] == 1 && (*chosen_run.variables[0])[1] == -1);
    SEQLIB_CHECK((*unset_run.variables[0])[16] == -1 && (*chosen_run.variables[0])[16] == 3);
}

// The model makes a change at the start of its frame. ntdlib's model run directly does the same with the game's call.
// The output follows the change from frame 203. That frame plays tick 97. The player's first frame is the model's frame
// 1: the first that plays a tick. The clock shows the variable's new value after frame 203, and the old one after frame
// 202.
void TestPlayChanges()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenChangesArchive();
    const Changes kChanges = {{1012, 0, 1}};
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kChangesSound, {}, kChanges));
    const ModelRun model = RunModel(*archive, kChangesSound, 300, {}, {}, kChanges);
    const ModelRun unchanged = RunModel(*archive, kChangesSound, 300);
    const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
    SequencePlayer player(ModelOf(archive, sequence), 0, clock);

    const PlayerRun run = RunPlayer(player, *clock, 260);

    const std::size_t first = FirstFrameReaching(model, 1);
    SEQLIB_REQUIRE(first == 1 && run.variables.size() > 210);
    SEQLIB_CHECK(model.samples != unchanged.samples);
    SEQLIB_CHECK(
        std::equal(model.samples.begin(), model.samples.begin() + 203 * kFrameSamples * 2, unchanged.samples.begin()));
    SEQLIB_CHECK(Matches(run.samples, model, first, 0));
    SEQLIB_REQUIRE(run.variables[202 - first] && run.variables[203 - first]);
    SEQLIB_CHECK((*run.variables[202 - first])[0] == -1 && (*run.variables[203 - first])[0] == 1);
}

// A snapshot keeps the changes before it. A seek back before a change makes the change again, and a seek past it
// restores the snapshot after it. Snapshots come every 100 frames here, in the frames up to the loop's end at tick 145:
// before frames 0, 100, 200 and 300. The change comes in frame 203.
void TestSeekAcrossChange()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenChangesArchive();
    const Changes kChanges = {{1012, 0, 1}};
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kChangesSound, {}, kChanges));
    const ModelRun model = RunModel(*archive, kChangesSound, 400, {}, {}, kChanges);
    const std::shared_ptr<SequenceModel> shared = ModelOf(archive, sequence, 100);

    std::size_t wrong = 0;
    for (const uint64_t frame : {350u, 120u, 250u, 205u, 203u, 202u, 300u})
    {
        if (!shared->SeekFrame(frame) || !std::ranges::equal(shared->Output(), ModelFrame(model, frame)))
        {
            wrong++;
        }
    }

    SEQLIB_CHECK_EQ(wrong, 0);
    SEQLIB_CHECK(shared->SnapshotCount() == 4);
}

// From a seek into the loop, the first frame fades in from silence. The frames after it are the model's. The first
// frame's position is the tick that playback starts from.
void TestPlayFromSeek()
{
    constexpr uint32_t kStart = 120;
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kLoopSound));
    const ModelRun model = RunModel(*archive, kLoopSound, 600);
    const std::size_t first = FirstFrameReaching(model, kStart + 1);
    const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
    clock->SetCycle(kGameLoop);
    SequencePlayer player(ModelOf(archive, sequence), kStart, clock);

    const PlayerRun run = RunPlayer(player, *clock, 300);

    SEQLIB_REQUIRE_EQ(run.samples.size(), 300 * kFrameSamples * 2);
    SEQLIB_CHECK(HasSound(std::span<const float>(run.samples).first(kFrameSamples * 2)));
    SEQLIB_CHECK(Fades(run.samples, 0, model, first, true));
    SEQLIB_CHECK(Matches(run.samples, model, first, kFrameSamples * 2));
    SEQLIB_REQUIRE(!run.marks.empty() && run.marks[0].has_value());
    SEQLIB_CHECK(*run.marks[0] == kStart);
}

// With the loop off, the output ends with the frame that reaches the end of the pass, and that frame fades out. Turned
// off during the second pass, the loop ends the output with that pass.
void TestLoopOff()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kLoopSound));
    const ModelRun model = RunModel(*archive, kLoopSound, 800);
    const std::size_t first = FirstFrameReaching(model, 1);
    const std::size_t first_pass_end = FirstFrameReaching(model, 192);
    const std::size_t second_pass_end = FirstFrameReaching(model, 288);

    const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
    SequencePlayer player(ModelOf(archive, sequence), 0, clock);
    const PlayerRun run = RunPlayer(player, *clock, 800);

    const auto later_clock = std::make_shared<PlaybackClock>(kFrameSamples);
    later_clock->SetCycle(kGameLoop);
    SequencePlayer later_player(ModelOf(archive, sequence), 0, later_clock);
    const PlayerRun first_part = RunPlayer(later_player, *later_clock, first_pass_end - first + 50);
    later_clock->SetCycle({});
    const PlayerRun second_part = RunPlayer(later_player, *later_clock, 800);

    const std::size_t frames = first_pass_end - first + 1;
    SEQLIB_CHECK(run.ended);
    SEQLIB_CHECK(clock->Ended());
    SEQLIB_REQUIRE_EQ(run.samples.size(), frames * kFrameSamples * 2);
    std::vector<float> before_end = run.samples;
    before_end.resize((frames - 1) * kFrameSamples * 2);
    SEQLIB_CHECK(Matches(before_end, model, first, 0));
    SEQLIB_CHECK(Fades(run.samples, frames - 1, model, first_pass_end, false));

    SEQLIB_CHECK(!first_part.ended);
    SEQLIB_CHECK(second_part.ended);
    SEQLIB_CHECK(later_clock->Ended());
    SEQLIB_CHECK_EQ((first_part.samples.size() + second_part.samples.size()) / 2,
                    (second_pass_end - first + 1) * kFrameSamples);
}

// A sequence without a loop ends once its last note has faded: when the model is no longer busy.
void TestPlayToEnd()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kNoteSound));
    const ModelRun model = RunModel(*archive, kNoteSound, 400);
    const std::size_t first = FirstFrameReaching(model, 1);
    const auto idle = std::find(model.busy.begin() + static_cast<std::ptrdiff_t>(first), model.busy.end(), false);
    const auto end = static_cast<std::size_t>(idle - model.busy.begin());
    const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
    SequencePlayer player(ModelOf(archive, sequence), 0, clock);

    const PlayerRun run = RunPlayer(player, *clock, 400);
    float after_end[2] = {};
    const std::size_t rendered_after_end = player.Render(after_end, 1);

    SEQLIB_CHECK(!sequence->loop.has_value());
    SEQLIB_CHECK(end < model.busy.size());
    SEQLIB_CHECK(run.ended);
    SEQLIB_CHECK(clock->Ended());
    SEQLIB_CHECK_EQ(run.samples.size(), (end - first) * kFrameSamples * 2);
    SEQLIB_CHECK(HasSound(std::span<const float>(run.samples)));
    SEQLIB_CHECK(Matches(run.samples, model, first, 0));
    SEQLIB_CHECK_EQ(rendered_after_end, 0);
}

// An archive's bytes name its console: CSAR the 3DS, a big-endian FSAR the Wii U, and a little-endian one the Switch.
// Each opens with its console's settings of ntdlib's reader and converter, and its sequences take its console's rules.
void TestArchiveFlavors()
{
    for (const auto& [flavor, engine] :
         {std::pair{sar::Flavor::kCtr, export_::Engine::kCtr}, std::pair{sar::Flavor::kCafe, export_::Engine::kCafe},
          std::pair{sar::Flavor::kNx, export_::Engine::kSwitch}})
    {
        sar_files::ArchiveSpec spec;
        spec.sounds = {{.name = "SEQ", .file = 0}};
        spec.files = {{.bytes = sar_files::Sequence(flavor, {0x3C, 0x64, 0x30, 0xFF}, {{"SEQ", 0}})}};
        const std::vector<uint8_t> bytes = sar_files::Archive(flavor, spec);
        SEQLIB_CHECK(hokuto::song::FlavorOfArchive(bytes) == flavor);
        const std::shared_ptr<const sar::SoundArchive> archive = hokuto::song::LoadArchive(bytes, "test");
        SEQLIB_CHECK(archive->ReadOptions().flavor == flavor && hokuto::song::SettingsOf(*archive).engine == engine);
        SEQLIB_CHECK(ReadSequence(*archive, 0).tracks.size() == 1);
    }
}

// The 3DS's player skips the commands of the LFOs past the first, of the first LFO's curve, phase and period, and of
// the send to bus C. The players of the Wii U and the Switch run them.
void TestRuns()
{
    using hokuto::song::Runs;
    using ntdlib::commands::Kind;
    SEQLIB_CHECK(Runs(sar::Flavor::kCtr, Kind::kSend, 1) && !Runs(sar::Flavor::kCtr, Kind::kSend, 2));
    SEQLIB_CHECK(Runs(sar::Flavor::kCtr, Kind::kLfoDepth, 0) && !Runs(sar::Flavor::kCtr, Kind::kLfoDepth, 1));
    SEQLIB_CHECK(!Runs(sar::Flavor::kCtr, Kind::kLfoCurve, 0) && !Runs(sar::Flavor::kCtr, Kind::kLfoPhase, 0));
    SEQLIB_CHECK(!Runs(sar::Flavor::kCtr, Kind::kLfoPeriod, 0) && Runs(sar::Flavor::kCtr, Kind::kLfoDelay, 0));
    SEQLIB_CHECK(Runs(sar::Flavor::kCtr, Kind::kTempo, 0) && Runs(sar::Flavor::kCtr, Kind::kNote, 0));
    SEQLIB_CHECK(Runs(sar::Flavor::kNx, Kind::kLfoCurve, 3) && Runs(sar::Flavor::kCafe, Kind::kSend, 2));
}

// A stream of the 3DS of 0.5 s at 32,728 Hz, in DSP-ADPCM, without a loop. Both channels hold the same sawtooth.
std::vector<uint8_t> CtrStream()
{
    std::vector<std::vector<int16_t>> channels(2, std::vector<int16_t>(16364));
    for (std::vector<int16_t>& channel : channels)
    {
        for (std::size_t i = 0; i < channel.size(); i++)
        {
            channel[i] = static_cast<int16_t>(static_cast<int>(i % 80) * 100 - 4000);
        }
    }

    return sar::MakeStream(sar::Flavor::kCtr, 0x02030100, sar::WaveEncoding::kDspAdpcm, 32728, channels, std::nullopt)
        .Write();
}

// A 3DS archive with sequence sound 0: key 60 at velocity 127 for 96 ticks on program 0, a wait of 96 ticks and the
// end. Program 0 plays a constant wave of 2 s. Wave sound 1 plays the wave once. Stream sound 2 plays CtrStream on a
// track of both channels.
std::shared_ptr<const sar::SoundArchive> CtrArchive()
{
    constexpr sar::Flavor kCtr = sar::Flavor::kCtr;
    sar::StreamSoundInfo stream;
    stream.valid_tracks = 1;
    stream.channel_count = 2;
    stream.tracks = {{.channels = {0, 1}}};
    sar_files::WaveSoundFileSpec wave_sounds;
    wave_sounds.waves = {{sar::kWaveArchiveItem << 24, 0}};
    wave_sounds.sounds = {{.notes = {{.wave = 0}}}};
    sar_files::ArchiveSpec spec;
    spec.sounds = {{.name = "SEQ_NOTE", .file = 0, .banks = {0}},
                   {.name = "WSD_NOTE", .kind = sar_files::SoundSpec::kWaveSound, .file = 3},
                   {.name = "STM_SAW", .kind = sar_files::SoundSpec::kStream, .file = 4, .stream = stream}};
    spec.banks = {{"BANK", 1}};
    spec.wave_archives = {{"WARC", 2}};
    spec.players = {{"PLAYER", 1}};
    spec.files = {{sar_files::Sequence(kCtr, {0x3C, 0x7F, 0x60, 0x80, 0x60, 0xFF}, {{"SEQ_NOTE", 0}})},
                  {sar_files::Bank(kCtr, sar_files::OneRegionBank())},
                  {sar_files::WaveArchive(kCtr, {sar_files::Wave(kCtr, sar_files::ConstantWave(false, 64000))})},
                  {sar_files::WaveSoundFile(kCtr, wave_sounds)},
                  {.external = "stream/STM_SAW.bcstm"}};

    return LoadArchive(sar_files::Archive(kCtr, spec), "test");
}

// The output of sound `sound` of the 3DS's `archive` on ntdlib's model run directly, without a preview, for `frames`
// frames at most. A stream sound plays `stream`. The test compares a preview's output with it.
std::vector<float> CtrSoundOnModel(const sar::SoundArchive& archive, uint32_t sound, std::span<const uint8_t> stream,
                                   std::size_t frames)
{
    hokuto::song::CtrModel model;
    std::vector<float> samples;
    const bool wave_sound = archive.Sounds()[sound].type == sar::SoundType::kWaveSound;
    if (!model.Start(archive) || (wave_sound ? model.Engine().StartWaveSound(sound) == nullptr
                                             : model.Engine().StartStreamSound(sound, stream) == nullptr))
    {
        return samples;
    }

    std::vector<float> frame(2 * hokuto::song::CtrModel::kFrameSamples);
    for (std::size_t i = 0; i < frames && model.IsBusy() && model.RunFrame(); i++)
    {
        hokuto::song::CopyOutput(model, frame);
        samples.insert(samples.end(), frame.begin(), frame.end());
    }

    return samples;
}

// A sequence of the 3DS plays on ntdlib's model of the DSP's audio firmware, and the player's output sounds. A note of
// a 3DS bank sounds too. A wave sound and a stream sound play as ntdlib's model plays them run directly, sample for
// sample, and end with their waves. A sequence sound refuses a sound's preview. ctr_test compares the 3DS's playback
// with ntdlib's archive mode.
void TestCtrPlayback()
{
    const std::shared_ptr<const sar::SoundArchive> archive = CtrArchive();
    const auto sounds = [](std::span<const float> samples)
    {
        return std::any_of(samples.begin(), samples.end(), [](float sample) { return sample != 0.0f; });
    };

    // The sequence plays on the model.
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, 0));
    SEQLIB_CHECK_EQ(sequence->tracks.size(), 1);
    const auto model = std::make_shared<SequenceModel>(archive, sequence);
    SEQLIB_CHECK(model->FrameSamples() == 160 && model->SampleRate() == 32728);
    const auto clock = std::make_shared<PlaybackClock>(model->FrameSamples());
    SequencePlayer player(model, 0, clock);
    std::vector<float> out(2 * 160 * 100);

    SEQLIB_CHECK_EQ(player.Render(out.data(), 160 * 100), 160 * 100);

    SEQLIB_CHECK(sounds(out) && !clock->Ended());

    // A note sounds.
    NotePreview note(archive, 0, 0);
    SEQLIB_CHECK(note.SampleRate() == 32728 && note.LongestFrames() == 2045 && note.HoldFrames() == 204);
    std::fill(out.begin(), out.end(), 0.0f);

    SEQLIB_CHECK_EQ(note.Render(out.data(), 160 * 100), 160 * 100);

    SEQLIB_CHECK(sounds(out));

    // The wave sound and the stream sound play as the model plays them.
    const std::vector<uint8_t> stream = CtrStream();
    SoundPreview wave_sound(archive, 1);
    SoundPreview stream_sound(archive, 2, stream);
    SEQLIB_CHECK(wave_sound.SampleRate() == 32728 && stream_sound.SampleRate() == 32728);

    const std::vector<float> wave_samples = RunPreview(wave_sound, 1000, 160);
    const std::vector<float> stream_samples = RunPreview(stream_sound, 1000, 160);

    SEQLIB_CHECK(sounds(wave_samples) && wave_samples == CtrSoundOnModel(*archive, 1, {}, 1000));
    SEQLIB_CHECK(sounds(stream_samples) && stream_samples == CtrSoundOnModel(*archive, 2, stream, 1000));

    // The wave lasts 2 s at 32,000 Hz, and the stream 0.5 s. Both end within a few frames of their samples.
    const std::size_t wave_frames = wave_samples.size() / (2 * 160);
    const std::size_t stream_frames = stream_samples.size() / (2 * 160);
    SEQLIB_CHECK(wave_frames > 64000 * 32728 / 32000 / 160 && wave_frames < 64000 * 32728 / 32000 / 160 + 16);
    SEQLIB_CHECK(stream_frames > 16364 / 160 && stream_frames < 16364 / 160 + 16);

    bool refused = false;
    try
    {
        SoundPreview sound(archive, 0);
    }
    catch (const std::runtime_error& error)
    {
        refused = std::string(error.what()) == "SEQ_NOTE is neither a wave sound nor a stream sound.";
    }

    SEQLIB_CHECK(refused);
}

// A sequence that the model can't start ends the output at once.
void TestUnplayable()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    Sequence missing = ReadSequence(*archive, kNoteSound);
    missing.sound = 7;
    const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
    SequencePlayer player(ModelOf(archive, std::make_shared<const Sequence>(missing)), 0, clock);
    float out[2] = {};

    const std::size_t rendered = player.Render(out, 1);

    SEQLIB_CHECK_EQ(rendered, 0);
    SEQLIB_CHECK(clock->Ended());
}

// A model with a snapshot every 10 frames plays the loop sequence past the end of its first pass. Seeks back into the
// intro, into the loop and to the start then restore snapshots. After the first frame's fade, each seek's output is the
// output of the model run directly. Snapshots cover the first pass only.
void TestSeekWithSnapshots()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kLoopSound));
    const ModelRun model = RunModel(*archive, kLoopSound, 800);
    const std::shared_ptr<SequenceModel> shared = ModelOf(archive, sequence, 10);
    const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
    clock->SetCycle(kGameLoop);
    SequencePlayer from_start(shared, 0, clock);
    RunPlayer(from_start, *clock, 600);

    std::size_t wrong = 0;
    for (const uint32_t start : {150u, 40u, 120u, 0u, 191u})
    {
        const auto seek_clock = std::make_shared<PlaybackClock>(kFrameSamples);
        seek_clock->SetCycle(kGameLoop);
        SequencePlayer player(shared, start, seek_clock);
        const PlayerRun run = RunPlayer(player, *seek_clock, 100);
        const std::size_t first = FirstFrameReaching(model, start + 1);
        const bool right = start > 0 ? Fades(run.samples, 0, model, first, true) &&
                                           Matches(run.samples, model, first, kFrameSamples * 2)
                                     : Matches(run.samples, model, first, 0);
        if (!right || run.samples.size() != 100 * kFrameSamples * 2)
        {
            wrong++;
        }
    }

    SEQLIB_CHECK_EQ(wrong, 0);
    SEQLIB_CHECK(shared->SnapshotCount() > 30 && shared->SnapshotCount() < 45);
}

// With the locators at ticks 48 and 144, inside the intro and the loop, playback from the start runs to the frame that
// reaches tick 144, and that frame fades out. The frame that plays tick 48 follows, faded in. Each pass from there is
// the model's output from that frame on, and the passes are the same. The position goes back to 48 at each pass.
void TestCycleLocators()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kLoopSound));
    const ModelRun model = RunModel(*archive, kLoopSound, 800);
    const std::size_t first = FirstFrameReaching(model, 1);
    const std::size_t left = FirstFrameReaching(model, 49);
    const std::size_t right = FirstFrameReaching(model, 144);
    const std::size_t first_pass = right - first + 1;
    const std::size_t pass = right - left + 1;
    const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
    clock->SetCycle({true, 48, 144});
    SequencePlayer player(ModelOf(archive, sequence), 0, clock);

    const PlayerRun run = RunPlayer(player, *clock, first_pass + 3 * pass);

    SEQLIB_REQUIRE_EQ(run.samples.size(), (first_pass + 3 * pass) * kFrameSamples * 2);
    SEQLIB_CHECK(Matches(FramesOf(run, 0, first_pass - 1), model, first, 0));
    SEQLIB_CHECK(Fades(run.samples, first_pass - 1, model, right, false));
    SEQLIB_CHECK(Fades(run.samples, first_pass, model, left, true));
    SEQLIB_CHECK(Matches(FramesOf(run, first_pass, pass - 1), model, left, kFrameSamples * 2));
    SEQLIB_CHECK(Fades(run.samples, first_pass + pass - 1, model, right, false));
    SEQLIB_CHECK(FramesOf(run, first_pass + pass, pass) == FramesOf(run, first_pass, pass));
    SEQLIB_CHECK(FramesOf(run, first_pass + 2 * pass, pass) == FramesOf(run, first_pass, pass));
    SEQLIB_REQUIRE(run.marks.size() > first_pass + pass && run.marks[first_pass] && run.marks[first_pass + pass]);
    SEQLIB_CHECK(*run.marks[first_pass] == 48.0 && *run.marks[first_pass + pass] == 48.0);
}

// The release sequence, with the right locator 100 ticks past its end. The position goes on past the end at the tempo
// there, through the note's release and the silence after it. The pass ends with the frame that reaches the right
// locator. No frame ends on the locator exactly. The cycle then goes back to the start. The second pass's first frame
// fades in, and after it the second pass repeats the first. Track 0's meter matches the output in each frame but the
// two that start the second and the third pass with a fade. In the silence, it's silent.
void TestCycleSilence()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kReleaseSound));
    const ModelRun model = RunModel(*archive, kReleaseSound, 600);
    const EndFrames ends = EndFramesOf(model);
    const uint32_t right = sequence->length + 100;
    const std::size_t second = FramePlayingPastEnd(model, ends, right) + 1 - ends.first;
    const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
    clock->SetCycle({true, 0, right});
    SequencePlayer player(ModelOf(archive, sequence), 0, clock);

    const PlayerRun run = RunPlayer(player, *clock, 2 * second + 10);

    const std::size_t sounding = ends.idle - ends.first;
    SEQLIB_CHECK(!run.ended);
    SEQLIB_REQUIRE(ends.idle < model.busy.size() && sounding < second && run.marks.size() == 2 * second + 10);
    SEQLIB_CHECK(Matches(FramesOf(run, 0, sounding), model, ends.first, 0));
    SEQLIB_CHECK(!HasSound(std::span<const float>(FramesOf(run, sounding, second - sounding))));
    SEQLIB_CHECK(run.marks[second] && *run.marks[second] == 0.0);
    SEQLIB_CHECK(Fades(run.samples, second, model, ends.first, true));
    SEQLIB_CHECK(FramesOf(run, second + 1, second - 1) == FramesOf(run, 1, second - 1));
    SEQLIB_CHECK_EQ(MeterMismatches(run, 0), 2);
}

// With the locators at ticks 40 and 80, in the release sequence's release after its end at tick 24, each pass ends with
// the frame that reaches the right locator. That frame fades out. The next pass starts with the frame that plays the
// left locator, faded in, and goes on as the model does from there. No frame ends on a locator exactly. The model takes
// a snapshot every 10 frames up to the end. A snapshot in the release would hold the wrong position.
void TestCycleInRelease()
{
    constexpr PlaybackClock::CycleRange kCycle{true, 40, 80};
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kReleaseSound));
    const ModelRun model = RunModel(*archive, kReleaseSound, 600);
    const EndFrames ends = EndFramesOf(model);
    const std::size_t left = FramePlayingPastEnd(model, ends, kCycle.start);
    const std::size_t right = FramePlayingPastEnd(model, ends, kCycle.end);
    const std::size_t first_pass = right - ends.first + 1;
    const std::size_t pass = right - left + 1;
    const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
    clock->SetCycle(kCycle);
    SequencePlayer player(ModelOf(archive, sequence, 10), 0, clock);

    const PlayerRun run = RunPlayer(player, *clock, first_pass + pass);

    SEQLIB_REQUIRE(ends.finished < left && right + 5 < ends.idle && run.marks.size() == first_pass + pass);
    SEQLIB_CHECK(Matches(FramesOf(run, 0, first_pass - 1), model, ends.first, 0));
    SEQLIB_CHECK(Fades(run.samples, first_pass - 1, model, right, false));
    SEQLIB_CHECK(Fades(run.samples, first_pass, model, left, true));
    SEQLIB_CHECK(Matches(FramesOf(run, first_pass, pass - 1), model, left, kFrameSamples * 2));
    SEQLIB_CHECK(Fades(run.samples, first_pass + pass - 1, model, right, false));
    SEQLIB_CHECK(run.marks[first_pass] && *run.marks[first_pass] == 40.0);
}

// Once the release sequence has ended, the position goes on at the tempo at the end while the note's release sounds.
// The output ends with the release.
void TestPositionPastEnd()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kReleaseSound));
    const ModelRun model = RunModel(*archive, kReleaseSound, 600);
    const EndFrames ends = EndFramesOf(model);
    const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
    SequencePlayer player(ModelOf(archive, sequence), 0, clock);

    const PlayerRun run = RunPlayer(player, *clock, 600);

    SEQLIB_REQUIRE(ends.first < ends.finished && ends.finished + 10 < ends.idle && ends.idle < model.busy.size());
    SEQLIB_REQUIRE_EQ(run.marks.size(), ends.idle - ends.first);
    std::size_t wrong = 0;
    for (std::size_t frame = ends.finished; frame < ends.idle; frame++)
    {
        const std::optional<double>& mark = run.marks[frame - ends.first];
        if (!mark || std::abs(*mark - PositionBefore(model, ends, frame)) > 1e-9)
        {
            wrong++;
        }
    }
    SEQLIB_CHECK_EQ(wrong, 0);
    SEQLIB_CHECK(run.ended);
}

// Playback that starts at tick 80, in the release sequence's release after its end at tick 24, plays the release from
// the frame that plays the start. That frame fades in. No frame ends on the start exactly.
void TestPlayFromRelease()
{
    constexpr uint32_t kStart = 80;
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kReleaseSound));
    const ModelRun model = RunModel(*archive, kReleaseSound, 600);
    const EndFrames ends = EndFramesOf(model);
    const std::size_t from = FramePlayingPastEnd(model, ends, kStart);
    const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
    SequencePlayer player(ModelOf(archive, sequence), kStart, clock);

    const PlayerRun run = RunPlayer(player, *clock, 600);

    SEQLIB_REQUIRE(ends.finished < from && from + 5 < ends.idle && ends.idle < model.busy.size());
    SEQLIB_CHECK(run.ended);
    SEQLIB_CHECK_EQ(run.samples.size(), (ends.idle - from) * kFrameSamples * 2);
    SEQLIB_CHECK(HasSound(std::span<const float>(run.samples)));
    SEQLIB_CHECK(Fades(run.samples, 0, model, from, true));
    SEQLIB_CHECK(Matches(run.samples, model, from, kFrameSamples * 2));
    SEQLIB_CHECK(!run.marks.empty() && run.marks[0] == static_cast<double>(kStart));
}

// Playback that starts after the right locator plays on. The loop sequence's output ends with the pass of its loop.
void TestCycleAfterRightLocator()
{
    constexpr uint32_t kStart = 120;
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kLoopSound));
    const ModelRun model = RunModel(*archive, kLoopSound, 800);
    const std::size_t first = FirstFrameReaching(model, kStart + 1);
    const std::size_t pass_end = FirstFrameReaching(model, 192);
    const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
    clock->SetCycle({true, 48, 96});
    SequencePlayer player(ModelOf(archive, sequence), kStart, clock);

    const PlayerRun run = RunPlayer(player, *clock, 800);

    SEQLIB_CHECK(run.ended);
    SEQLIB_CHECK_EQ(run.samples.size(), (pass_end - first + 1) * kFrameSamples * 2);
}

// A muted track plays on. With track 0 muted from the model's start, the loop sequence's output is silent, and its
// positions are those of the playback without the mute. Unmuted at tick 120, inside a note, the output ramps in across
// a frame and is then the model's output again. The sequence lacks track 5, and a mute of that track changes nothing. A
// seek keeps the mutes.
void TestMutes()
{
    constexpr std::size_t kMuted = 250;
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kLoopSound));
    const ModelRun model = RunModel(*archive, kLoopSound, 800);
    const std::size_t unmuted = FirstFrameReaching(model, 1) + kMuted;
    const std::shared_ptr<SequenceModel> shared = ModelOf(archive, sequence, 10);
    const auto player_from = [&shared](uint32_t start, uint32_t mutes)
    {
        auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
        clock->SetCycle(kGameLoop);
        clock->SetMutes(mutes);

        return std::make_pair(std::make_unique<SequencePlayer>(shared, start, clock), clock);
    };

    const auto [muted, clock] = player_from(0, 1u << 0);
    const PlayerRun silent = RunPlayer(*muted, *clock, kMuted);
    clock->SetMutes(0);
    const PlayerRun back = RunPlayer(*muted, *clock, 100);
    const auto [plain, plain_clock] = player_from(0, 0);
    const PlayerRun heard = RunPlayer(*plain, *plain_clock, kMuted);
    const auto [other, other_clock] = player_from(0, 1u << 5);
    const PlayerRun other_track = RunPlayer(*other, *other_clock, kMuted);
    const auto [sought, sought_clock] = player_from(120, 1u << 0);
    const PlayerRun silent_seek = RunPlayer(*sought, *sought_clock, 100);

    SEQLIB_CHECK(HasSound(std::span<const float>(heard.samples)) && other_track.samples == heard.samples);
    SEQLIB_CHECK(!HasSound(std::span<const float>(silent.samples)) && silent.marks == heard.marks);
    SEQLIB_CHECK(HasSound(std::span<const float>(FramesOf(back, 0, 1))) && RampsIn(back.samples, 0, model, unmuted));
    SEQLIB_CHECK(Matches(back.samples, model, unmuted, kFrameSamples * 2));
    SEQLIB_CHECK(silent_seek.samples.size() == 100 * kFrameSamples * 2);
    SEQLIB_CHECK(!HasSound(std::span<const float>(silent_seek.samples)));
}

// Track 0's meter holds the peaks of the output in each frame from the start, and the other tracks' meters stay silent.
// From a seek, the first frame fades in. Its meter shows the frame before the fade, and the frames after it match the
// output again. A muted track's meter stays silent.
void TestTrackMeters()
{
    constexpr std::size_t kFrames = 700;
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kLoopSound));
    const std::shared_ptr<SequenceModel> shared = ModelOf(archive, sequence, 10);
    const auto player_from = [&shared](uint32_t start, uint32_t mutes)
    {
        auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
        clock->SetCycle(kGameLoop);
        clock->SetMutes(mutes);

        return std::make_pair(std::make_unique<SequencePlayer>(shared, start, clock), clock);
    };

    const auto [plain, plain_clock] = player_from(0, 0);
    const PlayerRun heard = RunPlayer(*plain, *plain_clock, kFrames);
    const auto [sought, sought_clock] = player_from(120, 0);
    const PlayerRun seek = RunPlayer(*sought, *sought_clock, kFrames);
    const auto [muted, muted_clock] = player_from(0, 1u << 0);
    const PlayerRun silent = RunPlayer(*muted, *muted_clock, kFrames);

    SEQLIB_REQUIRE_EQ(heard.peaks.size(), kFrames);
    SEQLIB_CHECK(HasSound(std::span<const float>(heard.samples)));
    SEQLIB_CHECK_EQ(MeterMismatches(heard, 0), 0);
    SEQLIB_CHECK(TracksSilent(heard, 1));
    SEQLIB_REQUIRE_EQ(seek.peaks.size(), kFrames);
    const std::array<float, 2> faded = OutputPeaks(seek.samples, 0);
    SEQLIB_CHECK(seek.peaks[0].tracks[0][0] > faded[0] && seek.peaks[0].tracks[0][1] > faded[1]);
    SEQLIB_CHECK_EQ(MeterMismatches(seek, 1), 0);
    SEQLIB_CHECK(TracksSilent(seek, 1));
    SEQLIB_CHECK_EQ(silent.peaks.size(), kFrames);
    SEQLIB_CHECK(TracksSilent(silent, 0));
}

// The meter of a track panned to the left is louder on the left in each frame with sound. Each side of the meter holds
// the peaks of that side of the output.
void TestPannedMeter()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kPanSound));
    const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
    SequencePlayer player(ModelOf(archive, sequence), 0, clock);

    const PlayerRun run = RunPlayer(player, *clock, 400);

    // The frames with sound, and those of them that aren't louder on the left.
    std::size_t sounding = 0;
    std::size_t not_left = 0;
    for (const PlaybackClock::Peaks& peaks : run.peaks)
    {
        if (peaks.tracks[0][0] == 0.0f && peaks.tracks[0][1] == 0.0f)
        {
            continue;
        }

        sounding++;
        if (peaks.tracks[0][0] <= peaks.tracks[0][1])
        {
            not_left++;
        }
    }

    SEQLIB_CHECK(run.ended);
    SEQLIB_CHECK(sounding > 0 && not_left == 0);
    SEQLIB_CHECK_EQ(MeterMismatches(run, 0), 0);
    SEQLIB_CHECK(TracksSilent(run, 1));
}

// The frame of a run before which a model with effects stops: the first one after the start before which the engine is
// idle and after a frame that came out silent.
std::size_t TailEnd(const ModelRun& run, std::size_t first)
{
    std::size_t frame = first + 1;

    const auto silent = [&run](std::size_t f)
    {
        return !HasSound(std::span<const int16_t>(run.samples).subspan(f * kFrameSamples * 2, kFrameSamples * 2));
    };
    while (frame < run.busy.size() && (run.busy[frame] || !silent(frame - 1)))
    {
        frame++;
    }

    return frame;
}

// A reverb on aux bus A plays as ntdlib's model plays it, given the same reverb, and its tail sounds on for seconds
// after the note's release. Playback ends at the first silent frame after that. A reverb on bus B changes nothing. The
// sequence doesn't send to bus B. A seek restores the reverb's state from a snapshot.
void TestAuxEffects()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenSendArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kSendSound));
    const ModelRun model = RunModel(*archive, kSendSound, 1600, AuxReverb());
    const ModelRun dry = RunModel(*archive, kSendSound, 1600);
    const std::size_t first = FirstFrameReaching(model, 1);
    const std::size_t idle = EndFramesOf(dry).idle;
    const std::size_t end = TailEnd(model, first);

    const auto play = [&](std::shared_ptr<SequenceModel> shared, uint32_t start, std::size_t frames)
    {
        const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
        SequencePlayer player(std::move(shared), start, clock);
        return RunPlayer(player, *clock, frames);
    };

    const PlayerRun on_a =
        play(ModelOf(archive, sequence, SequenceModel::kSnapshotInterval, AuxEffectsWithReverb()), 0, 1500);
    const PlayerRun on_b =
        play(ModelOf(archive, sequence, SequenceModel::kSnapshotInterval, {{{}, {Reverb()}, {}}}), 0, 1500);

    SEQLIB_CHECK(Matches(on_a.samples, model, first, 0) && !Matches(on_a.samples, dry, first, 0));
    SEQLIB_CHECK(on_a.ended && end > idle + 200 && end < model.busy.size());
    SEQLIB_CHECK_EQ(on_a.samples.size(), (end - first) * kFrameSamples * 2);
    SEQLIB_CHECK(on_b.ended && Matches(on_b.samples, dry, first, 0));
    SEQLIB_CHECK(on_b.samples.size() < (idle + 10 - first) * kFrameSamples * 2);

    // Seeks into the note on a model with a snapshot every 10 frames.
    const auto shared = ModelOf(archive, sequence, 10, AuxEffectsWithReverb());
    play(shared, 0, 200);
    std::size_t wrong = 0;
    for (const uint32_t start : {30u, 10u, 45u})
    {
        const PlayerRun run = play(shared, start, 100);
        const std::size_t from = FirstFrameReaching(model, start + 1);
        if (!Fades(run.samples, 0, model, from, true) || !Matches(run.samples, model, from, kFrameSamples * 2))
        {
            wrong++;
        }
    }
    SEQLIB_CHECK_EQ(wrong, 0);
    SEQLIB_CHECK(shared->SnapshotCount() > 5);
}

// The peaks of each channel of what frame `frame` of `run` adds to the same frame of `without`, as amplitudes.
std::array<float, 2> DifferencePeaks(const ModelRun& run, const ModelRun& without, std::size_t frame)
{
    std::array<float, 2> peaks = {0.0f, 0.0f};
    for (std::size_t i = 0; i < kFrameSamples * 2; i++)
    {
        float& peak = peaks[i % 2];
        peak = std::max(peak, std::abs(SampleOf(run, frame, i) - SampleOf(without, frame, i)));
    }

    return peaks;
}

// A bus's volume scales what the bus adds to the output. With aux bus A at 0.5, the player plays the model run directly
// with sub-mix 0's channels of bus A at 0.5. It does so from the start, after a seek to a snapshot taken at full
// volume, and from the frame after a change during playback. A catch-up runs at the clock's volumes. A change that
// comes with a new model takes effect in the frame that crossfades to the new model, in both models. Bus A's meter
// holds the peaks of what the bus adds: the output at full volume less the output with the bus silent. At 0.5, it holds
// half of them, within a sample's rounding. The meters of buses B and C stay silent. The sequence doesn't send there.
void TestBusVolumes()
{
    constexpr AuxVolumes kHalf = {0.5f, 1.0f, 1.0f};
    constexpr std::array<float, 2> kSilent = {0.0f, 0.0f};
    const std::shared_ptr<const sar::SoundArchive> archive = OpenSendArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kSendSound));
    const ModelRun full = RunModel(*archive, kSendSound, 600, AuxReverb());
    const ModelRun half = RunModel(*archive, kSendSound, 600, AuxReverb(), {}, {}, kHalf);
    const ModelRun silent = RunModel(*archive, kSendSound, 600, AuxReverb(), {}, {}, {0.0f, 1.0f, 1.0f});
    const std::size_t first = FirstFrameReaching(full, 1);
    const auto shared = ModelOf(archive, sequence, 10, AuxEffectsWithReverb());
    const auto play = [&shared](uint32_t start, const AuxVolumes& volumes, std::size_t frames)
    {
        const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
        clock->SetBusVolumes(volumes);
        SequencePlayer player(shared, start, clock);
        return RunPlayer(player, *clock, frames);
    };

    const PlayerRun at_full = play(0, kFullVolumes, 400);
    const PlayerRun at_half = play(0, kHalf, 400);
    const PlayerRun sought = play(30, kHalf, 100);
    const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
    SequencePlayer player(shared, 0, clock);
    const PlayerRun before_change = RunPlayer(player, *clock, 100);
    clock->SetBusVolumes(kHalf);
    const PlayerRun after_change = RunPlayer(player, *clock, 100);
    const auto catch_up_clock = std::make_shared<PlaybackClock>(kFrameSamples);
    catch_up_clock->SetBusVolumes(kHalf);
    catch_up_clock->SetModelFrames(first + 50);
    ModelCatchUp catch_up;
    catch_up.Start(ModelOf(archive, sequence, 10, AuxEffectsWithReverb()), catch_up_clock);
    const std::shared_ptr<SequenceModel> caught_up = WaitForCatchUp(catch_up);
    const auto switch_clock = std::make_shared<PlaybackClock>(kFrameSamples);
    SequencePlayer switching(shared, 0, switch_clock);
    RunPlayer(switching, *switch_clock, 50);
    switch_clock->SetBusVolumes(kHalf);
    switch_clock->Offer(ModelOf(archive, sequence, 10, AuxEffectsWithReverb()));
    const PlayerRun switched = RunPlayer(switching, *switch_clock, 50);

    const std::size_t from = FirstFrameReaching(half, 31);
    SEQLIB_CHECK(Matches(at_full.samples, full, first, 0) && Matches(at_half.samples, half, first, 0));
    SEQLIB_CHECK(!Matches(at_half.samples, full, first, 0));
    SEQLIB_CHECK(Fades(sought.samples, 0, half, from, true) && Matches(sought.samples, half, from, kFrameSamples * 2));
    SEQLIB_CHECK(before_change.samples.size() == 100 * kFrameSamples * 2 &&
                 after_change.samples.size() == 100 * kFrameSamples * 2);
    SEQLIB_CHECK(Matches(before_change.samples, full, first, 0) && Matches(after_change.samples, half, first + 100, 0));
    SEQLIB_REQUIRE(caught_up != nullptr);
    SEQLIB_CHECK(caught_up->Frames() == first + 50 &&
                 std::ranges::equal(caught_up->Output(), ModelFrame(half, first + 49)));
    SEQLIB_CHECK(Crossfades(switched.samples, 0, half, half, first + 50) &&
                 Matches(switched.samples, half, first + 50, kFrameSamples * 2));
    SEQLIB_REQUIRE(at_full.peaks.size() == 400 && at_half.peaks.size() == 400);
    std::size_t sounding = 0;
    std::size_t wrong = 0;
    for (std::size_t frame = 0; frame < at_full.peaks.size(); frame++)
    {
        const std::array<float, 2> expected = DifferencePeaks(full, silent, first + frame);
        const PlaybackClock::Peaks& at = at_full.peaks[frame];
        const PlaybackClock::Peaks& halved = at_half.peaks[frame];
        const auto near_half = [&expected, &halved](std::size_t side)
        {
            return std::abs(halved.buses[0][side] - expected[side] * 0.5f) <= 1.0f / 32768.0f;
        };

        sounding += expected != kSilent ? 1 : 0;
        const bool right = at.buses[0] == expected && near_half(0) && near_half(1) && at.buses[1] == kSilent &&
                           at.buses[2] == kSilent && halved.buses[1] == kSilent && halved.buses[2] == kSilent;
        wrong += right ? 0 : 1;
    }
    SEQLIB_CHECK(sounding > 200);
    SEQLIB_CHECK_EQ(wrong, 0);
}

// Bus C takes its volume as bus A does, and its meter holds what it adds to the output. With the note sent to bus C and
// the reverb there, at 0.5, the player plays the model run directly with sub-mix 0's channels of bus C at 0.5. The
// clock keeps each bus's volume.
void TestBusCVolume()
{
    constexpr AuxVolumes kHalfC = {1.0f, 1.0f, 0.5f};
    constexpr AuxVolumes kEach = {0.25f, 0.5f, 2.0f};
    constexpr std::array<float, 2> kSilent = {0.0f, 0.0f};
    const std::shared_ptr<const sar::SoundArchive> archive = OpenSendArchive(kSendCBytecode);
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kSendSound));
    atk::EngineOptions options;
    options.aux_reverbs[2] = Reverb();
    const ModelRun full = RunModel(*archive, kSendSound, 400, options);
    const ModelRun half = RunModel(*archive, kSendSound, 400, options, {}, {}, kHalfC);
    const ModelRun silent = RunModel(*archive, kSendSound, 400, options, {}, {}, {1.0f, 1.0f, 0.0f});
    const std::size_t first = FirstFrameReaching(full, 1);
    const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
    clock->SetBusVolumes(kHalfC);
    SequencePlayer player(ModelOf(archive, sequence, 10, {{{}, {}, {Reverb()}}}), 0, clock);
    PlaybackClock each(kFrameSamples);

    const PlayerRun run = RunPlayer(player, *clock, 300);
    each.SetBusVolumes(kEach);

    SEQLIB_CHECK(Matches(run.samples, half, first, 0) && !Matches(run.samples, full, first, 0));
    SEQLIB_REQUIRE(run.peaks.size() == 300);
    std::size_t sounding = 0;
    std::size_t wrong = 0;
    for (std::size_t frame = 0; frame < run.peaks.size(); frame++)
    {
        const std::array<float, 2> expected = DifferencePeaks(full, silent, first + frame);
        const PlaybackClock::Peaks& at = run.peaks[frame];
        const auto near_half = [&expected, &at](std::size_t side)
        {
            return std::abs(at.buses[2][side] - expected[side] * 0.5f) <= 1.0f / 32768.0f;
        };

        sounding += expected != kSilent ? 1 : 0;
        wrong += near_half(0) && near_half(1) && at.buses[0] == kSilent && at.buses[1] == kSilent ? 0 : 1;
    }
    SEQLIB_CHECK(sounding > 200);
    SEQLIB_CHECK_EQ(wrong, 0);
    SEQLIB_CHECK(each.BusVolumes() == kEach);
}

// On the Wii U, a bus's volume is its return volume on the TV. With aux bus A's effect at 0.5, the model plays as
// ntdlib's model plays with the TV's bus A returning at 0x4000. Bus A's meter holds half its peaks at full volume,
// within a sample's rounding. The meters of buses B and C stay silent. The quieter tail ends the output sooner.
void TestWiiUBusVolumes()
{
    constexpr std::size_t kFrames = 1500;
    const std::shared_ptr<const sar::SoundArchive> archive = OpenWiiUSendArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kNoteSound));
    const hokuto::song::WiiUEffect reverb = hokuto::song::WiiUEffectOf(
        cafe::EffectType::kReverbHi, hokuto::song::WiiUStartingParameters(cafe::EffectType::kReverbHi));
    const std::vector<cafe::EffectSetup> setups = {
        {cafe::kEffectDeviceTv, 0, cafe::EffectType::kReverbHi, reverb.words, {}}};
    const std::vector<int16_t> full = RunWiiUModel(*archive, kNoteSound, kFrames, setups);
    const std::vector<int16_t> half = RunWiiUModel(*archive, kNoteSound, kFrames, setups, 0x4000);

    const auto peaks_of = [&](const AuxVolumes& volumes)
    {
        const auto model = ModelOf(archive, sequence, 50, {{{reverb}, {}, {}}});
        model->SetBusVolumes(volumes);
        std::vector<int16_t> played;
        std::vector<std::array<std::array<int32_t, 2>, kAuxBuses>> peaks;
        for (bool more = model->SeekFrame(0); more && played.size() < full.size(); more = model->RunFrame())
        {
            const std::span<const int16_t> output = model->Output();
            played.insert(played.end(), output.begin(), output.end());
            peaks.push_back({model->BusPeaks(0), model->BusPeaks(1), model->BusPeaks(2)});
        }

        return std::make_pair(played, peaks);
    };

    const auto [played_full, peaks_full] = peaks_of(kFullVolumes);
    const auto [played_half, peaks_half] = peaks_of({0.5f, 1.0f, 1.0f});

    SEQLIB_REQUIRE(played_half.size() > 400 * cafe::ArchiveModel::kFrameSamples * 2 &&
                   played_half.size() < half.size());
    SEQLIB_CHECK(played_half.size() < played_full.size());
    SEQLIB_CHECK(std::equal(played_full.begin(), played_full.end(), full.begin()));
    SEQLIB_CHECK(std::equal(played_half.begin(), played_half.end(), half.begin()));
    SEQLIB_CHECK(!std::equal(played_half.begin(), played_half.end(), full.begin()));
    std::size_t sounding = 0;
    std::size_t wrong = 0;
    constexpr std::array<int32_t, 2> kSilent = {0, 0};
    for (std::size_t frame = 0; frame < std::min(peaks_full.size(), peaks_half.size()); frame++)
    {
        const std::array<int32_t, 2>& at_full = peaks_full[frame][0];
        const std::array<int32_t, 2>& at_half = peaks_half[frame][0];
        sounding += at_full != kSilent ? 1 : 0;
        const bool halved = std::abs(2 * at_half[0] - at_full[0]) <= 2 && std::abs(2 * at_half[1] - at_full[1]) <= 2;
        const bool quiet = peaks_full[frame][1] == kSilent && peaks_full[frame][2] == kSilent &&
                           peaks_half[frame][1] == kSilent && peaks_half[frame][2] == kSilent;
        wrong += halved && quiet ? 0 : 1;
    }
    SEQLIB_CHECK(sounding > 300);
    SEQLIB_CHECK_EQ(wrong, 0);
}

// A seek to a frame runs the model up to that frame: from its start, from a snapshot, from where it is, or not at all
// when its last frame is the one. Its output is then that frame of the model run directly. A seek past the end of the
// output fails.
void TestSeekFrame()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kReleaseSound));
    const ModelRun model = RunModel(*archive, kReleaseSound, 600);
    const EndFrames ends = EndFramesOf(model);
    const std::shared_ptr<SequenceModel> shared = ModelOf(archive, sequence, 10);

    std::size_t wrong = 0;
    for (const uint64_t frame : {120u, 35u, 35u, 36u, 35u, 0u, 200u})
    {
        const bool plays = shared->SeekFrame(frame);
        if (!plays || shared->Frames() != frame + 1 || !std::ranges::equal(shared->Output(), ModelFrame(model, frame)))
        {
            wrong++;
        }
    }
    const bool past_end = shared->SeekFrame(ends.idle + 10);

    SEQLIB_REQUIRE(ends.idle > 200 && ends.idle + 10 < model.busy.size());
    SEQLIB_CHECK_EQ(wrong, 0);
    SEQLIB_CHECK(!past_end);
    SEQLIB_CHECK(shared->SnapshotCount() > 3);
}

// A model with a reverb on bus A takes over from a model without it, in the send sequence's note. A catch-up runs the
// new model up to the player's frame first. The frame of the switch fades the old output out and the new one in. From
// the next frame on, the output is the reverb's, up to the end of its tail. With track 0 muted, a model that hasn't run
// takes over in silence. The muted note sends nothing to the reverb, and the output ends with the note.
void TestSwitch()
{
    constexpr std::size_t kBefore = 50;
    const std::shared_ptr<const sar::SoundArchive> archive = OpenSendArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kSendSound));
    const ModelRun wet = RunModel(*archive, kSendSound, 1600, AuxReverb());
    const ModelRun dry = RunModel(*archive, kSendSound, 1600);
    const std::size_t first = FirstFrameReaching(dry, 1);
    const std::size_t switched = first + kBefore;
    const std::size_t end = TailEnd(wet, first);
    const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
    SequencePlayer player(ModelOf(archive, sequence), 0, clock);
    ModelCatchUp catch_up;

    const PlayerRun before = RunPlayer(player, *clock, kBefore);
    const uint64_t player_frame = clock->ModelFrames();
    catch_up.Start(ModelOf(archive, sequence, SequenceModel::kSnapshotInterval, AuxEffectsWithReverb()), clock);
    const std::shared_ptr<SequenceModel> caught_up = WaitForCatchUp(catch_up);
    SEQLIB_REQUIRE(caught_up != nullptr);
    const uint64_t caught_up_frame = caught_up->Frames();
    clock->Offer(caught_up);
    const PlayerRun after = RunPlayer(player, *clock, 1600);
    const auto muted_clock = std::make_shared<PlaybackClock>(kFrameSamples);
    muted_clock->SetMutes(1u << 0);
    SequencePlayer muted_player(ModelOf(archive, sequence), 0, muted_clock);
    RunPlayer(muted_player, *muted_clock, kBefore);
    muted_clock->Offer(ModelOf(archive, sequence, SequenceModel::kSnapshotInterval, AuxEffectsWithReverb()));
    const PlayerRun muted = RunPlayer(muted_player, *muted_clock, 100);

    SEQLIB_REQUIRE(!std::ranges::equal(ModelFrame(wet, switched), ModelFrame(dry, switched)));
    SEQLIB_CHECK(Matches(before.samples, dry, first, 0));
    SEQLIB_CHECK(player_frame == switched && caught_up_frame == switched);
    SEQLIB_REQUIRE(after.samples.size() > kFrameSamples * 2);
    SEQLIB_CHECK(Crossfades(after.samples, 0, dry, wet, switched));
    SEQLIB_CHECK(Matches(after.samples, wet, switched, kFrameSamples * 2));
    SEQLIB_CHECK(after.ended && after.samples.size() == (end - switched) * kFrameSamples * 2);
    SEQLIB_CHECK(!HasSound(std::span<const float>(muted.samples)));
    SEQLIB_CHECK(muted.ended && muted.samples.size() == (EndFramesOf(dry).idle - switched) * kFrameSamples * 2);
}

// A model whose output has ended before the player's frame takes over with nothing to play. The send sequence plays
// with the reverb into its tail, after the note's release, and a model without the reverb takes over. The old output
// fades out across the next frame, and the output ends. Inside the cycle, silence follows up to the right locator,
// about 43 ticks later. No frame starts on it exactly. The next pass plays the new model from the start, after a frame
// that fades in.
void TestSwitchAfterEnd()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenSendArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kSendSound));
    const ModelRun wet = RunModel(*archive, kSendSound, 1600, AuxReverb());
    const ModelRun dry = RunModel(*archive, kSendSound, 1600);
    const EndFrames ends = EndFramesOf(dry);
    const std::size_t first = ends.first;
    const std::size_t played = ends.idle + 50 - first;
    const auto right = static_cast<uint32_t>(PositionBefore(dry, ends, first + played + 90));
    const std::size_t second = FramePlayingPastEnd(dry, ends, right) + 1 - first;

    const auto play = [&](const PlaybackClock::CycleRange& cycle, std::size_t frames)
    {
        const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
        clock->SetCycle(cycle);
        SequencePlayer player(ModelOf(archive, sequence, SequenceModel::kSnapshotInterval, AuxEffectsWithReverb()), 0,
                              clock);

        const PlayerRun before = RunPlayer(player, *clock, played);
        clock->Offer(ModelOf(archive, sequence));
        const PlayerRun after = RunPlayer(player, *clock, frames);
        return std::make_tuple(before, after, clock->Ended());
    };

    const auto [before, after, ended] = play({}, 100);
    const auto [cycle_before, cycle_after, cycle_ended] = play({true, 0, right}, second - played + 20);

    SEQLIB_REQUIRE(first + played + 100 < TailEnd(wet, first) && second > played + 80);
    SEQLIB_CHECK(Matches(before.samples, wet, first, 0));
    SEQLIB_CHECK(HasSound(std::span<const float>(after.samples)));
    SEQLIB_CHECK(after.ended && after.samples.size() == kFrameSamples * 2);
    SEQLIB_CHECK(Fades(after.samples, 0, wet, first + played, false));
    SEQLIB_CHECK(ended);
    SEQLIB_CHECK(cycle_before.samples == before.samples && !cycle_after.ended && !cycle_ended);
    SEQLIB_CHECK(Fades(cycle_after.samples, 0, wet, first + played, false));
    SEQLIB_CHECK(!HasSound(std::span<const float>(FramesOf(cycle_after, 1, second - played - 1))));
    SEQLIB_CHECK(Fades(cycle_after.samples, second - played, dry, first, true));
    SEQLIB_CHECK(Matches(FramesOf(cycle_after, second - played, 20), dry, first, kFrameSamples * 2));
}

// In the silence after the end of the sequence inside the cycle, a model takes over as it is. The send sequence plays
// without effects, with the right locator 100 ticks past its end, and a model with the reverb takes over in the
// silence. The silence goes on to the right locator. The second pass then plays the reverb's output from the start,
// after a frame that fades in.
void TestSwitchInSilence()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenSendArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kSendSound));
    const ModelRun wet = RunModel(*archive, kSendSound, 1600, AuxReverb());
    const ModelRun dry = RunModel(*archive, kSendSound, 1600);
    const EndFrames ends = EndFramesOf(dry);
    const uint32_t right = sequence->length + 100;
    const std::size_t second = FramePlayingPastEnd(dry, ends, right) + 1 - ends.first;
    const std::size_t silent = ends.idle + 10 - ends.first;
    const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
    clock->SetCycle({true, 0, right});
    SequencePlayer player(ModelOf(archive, sequence), 0, clock);

    const PlayerRun before = RunPlayer(player, *clock, silent);
    clock->Offer(ModelOf(archive, sequence, SequenceModel::kSnapshotInterval, AuxEffectsWithReverb()));
    const PlayerRun after = RunPlayer(player, *clock, second - silent + 20);

    SEQLIB_REQUIRE(silent < second && !after.ended);
    SEQLIB_CHECK(!HasSound(std::span<const float>(FramesOf(before, ends.idle - ends.first, 10))));
    SEQLIB_CHECK(!HasSound(std::span<const float>(FramesOf(after, 0, second - silent))));
    SEQLIB_CHECK(Fades(after.samples, second - silent, wet, ends.first, true));
    SEQLIB_CHECK(Matches(FramesOf(after, second - silent, 20), wet, ends.first, kFrameSamples * 2));
}

// A frame that seeks takes an offered model as it is: the first frame, and the first frame of each pass of the cycle
// after the first. A model with the reverb, offered before playback starts, plays the send sequence's first pass, with
// the locators in the note. A model without the reverb, offered at the end of the first pass, plays the second pass
// from the left locator, after a frame that fades in.
void TestSwitchAtSeek()
{
    constexpr PlaybackClock::CycleRange kCycle{true, 10, 30};
    const std::shared_ptr<const sar::SoundArchive> archive = OpenSendArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kSendSound));
    const ModelRun wet = RunModel(*archive, kSendSound, 1600, AuxReverb());
    const ModelRun dry = RunModel(*archive, kSendSound, 1600);
    const std::size_t first = FirstFrameReaching(dry, 1);
    const std::size_t left = FirstFrameReaching(dry, kCycle.start + 1);
    const std::size_t right = FirstFrameReaching(dry, kCycle.end);
    const std::size_t first_pass = right - first + 1;
    const std::size_t pass = right - left + 1;
    const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
    clock->SetCycle(kCycle);
    SequencePlayer player(ModelOf(archive, sequence), 0, clock);

    clock->Offer(ModelOf(archive, sequence, SequenceModel::kSnapshotInterval, AuxEffectsWithReverb()));
    const PlayerRun first_part = RunPlayer(player, *clock, first_pass);
    clock->Offer(ModelOf(archive, sequence));
    const PlayerRun second_part = RunPlayer(player, *clock, pass);

    SEQLIB_REQUIRE(!std::ranges::equal(ModelFrame(wet, left + 1), ModelFrame(dry, left + 1)));
    SEQLIB_CHECK(Matches(FramesOf(first_part, 0, first_pass - 1), wet, first, 0));
    SEQLIB_CHECK(Fades(first_part.samples, first_pass - 1, wet, right, false));
    SEQLIB_CHECK(Fades(second_part.samples, 0, dry, left, true));
    SEQLIB_CHECK(Matches(FramesOf(second_part, 0, pass - 1), dry, left, kFrameSamples * 2));
}

// The program's switch: the render worker plays the send sequence without effects into a ring, and the test reads the
// ring as the device would. A catch-up runs a model with the reverb up to the player's frame on its thread. The worker
// plays meanwhile. The clock then offers the model. The output is the model's without effects up to one frame that
// crossfades, and the reverb's after it, up to the end of its tail.
void TestSwitchWhilePlaying()
{
    constexpr std::size_t kBefore = 20;
    const std::shared_ptr<const sar::SoundArchive> archive = OpenSendArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kSendSound));
    const ModelRun wet = RunModel(*archive, kSendSound, 1600, AuxReverb());
    const ModelRun dry = RunModel(*archive, kSendSound, 1600);
    const std::size_t first = FirstFrameReaching(dry, 1);
    const std::size_t end = TailEnd(wet, first);
    hokuto::audio::FrameRing ring(16'384);
    hokuto::audio::RenderWorker worker(ring, std::chrono::milliseconds(100), 256);
    const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
    ModelCatchUp catch_up;
    std::vector<float> samples;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);

    // Reads up to `frames` frames of the model, or up to the end of the output.
    const auto read = [&](std::size_t frames)
    {
        std::vector<float> block(256 * 2);
        while (samples.size() < frames * kFrameSamples * 2 && std::chrono::steady_clock::now() < deadline &&
               (worker.Playing() || ring.Available() > 0))
        {
            const std::size_t got = ring.Read(block.data(), block.size() / 2);
            samples.insert(samples.end(), block.begin(), block.begin() + static_cast<std::ptrdiff_t>(got * 2));
            if (got == 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    };

    worker.Play(std::make_unique<SequencePlayer>(ModelOf(archive, sequence), 0, clock));
    read(kBefore);
    catch_up.Start(ModelOf(archive, sequence, SequenceModel::kSnapshotInterval, AuxEffectsWithReverb()), clock);
    const std::shared_ptr<SequenceModel> caught_up = WaitForCatchUp(catch_up);
    SEQLIB_REQUIRE(caught_up != nullptr);
    clock->Offer(caught_up);
    read(1600);

    // The frame that crossfades is the first that differs from the output without effects.
    std::size_t switched = 0;
    while (
        (switched + 1) * kFrameSamples * 2 <= samples.size() &&
        Matches(std::vector<float>(samples.begin() + static_cast<std::ptrdiff_t>(switched * kFrameSamples * 2),
                                   samples.begin() + static_cast<std::ptrdiff_t>((switched + 1) * kFrameSamples * 2)),
                dry, first + switched, 0))
    {
        switched++;
    }
    const std::vector<float> after(samples.begin() + static_cast<std::ptrdiff_t>(switched * kFrameSamples * 2),
                                   samples.end());
    SEQLIB_REQUIRE(switched >= kBefore && switched + first + 10 < EndFramesOf(dry).idle);
    SEQLIB_CHECK(Crossfades(after, 0, dry, wet, first + switched));
    SEQLIB_CHECK(Matches(after, wet, first + switched, kFrameSamples * 2));
    SEQLIB_CHECK_EQ(samples.size(), (end - first) * kFrameSamples * 2);
}

// A model that plays the ticks at other times takes over at the player's position in ticks. The loop's sequence plays
// at tempo 120 for 150 frames, up to about tick 72. A catch-up runs the sequence at tempo 60 up to that tick: about 300
// frames. After the switch, the position goes on from there at 0.24 ticks a frame, and the output is the slow model's.
// A switch by frames would have gone back to about tick 36.
void TestSwitchByTicks()
{
    constexpr std::size_t kBefore = 150;
    const std::shared_ptr<const sar::SoundArchive> archive = OpenSlowArchive();
    const auto loop = std::make_shared<const Sequence>(ReadSequence(*archive, kLoopSound));
    const auto slow = std::make_shared<const Sequence>(ReadSequence(*archive, kSlowSound));
    const ModelRun slow_run = RunModel(*archive, kSlowSound, 800);
    const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
    SequencePlayer player(ModelOf(archive, loop), 0, clock);
    ModelCatchUp catch_up;

    RunPlayer(player, *clock, kBefore);
    const double ticks = clock->ModelTicks();
    catch_up.Start(ModelOf(archive, slow), clock, true);
    const std::shared_ptr<SequenceModel> caught_up = WaitForCatchUp(catch_up);
    SEQLIB_REQUIRE(caught_up != nullptr);
    const double caught_up_ticks = caught_up->After();
    clock->Offer(caught_up, true);
    const PlayerRun switched = RunPlayer(player, *clock, 1);
    const double switched_ticks = clock->ModelTicks();
    const uint64_t switched_frame = clock->ModelFrames() - 1;
    const PlayerRun after = RunPlayer(player, *clock, 100);

    SEQLIB_CHECK(!SameTiming(*loop, *slow) && SameTiming(*loop, *loop) && catch_up.ByTicks());
    SEQLIB_CHECK(ticks > 60.0 && ticks < 80.0);
    SEQLIB_CHECK(caught_up_ticks >= ticks && caught_up_ticks <= ticks + 1.0);
    SEQLIB_REQUIRE(switched.samples.size() == kFrameSamples * 2);
    SEQLIB_CHECK(switched_ticks >= ticks && switched_ticks <= ticks + 2.0);
    SEQLIB_CHECK(switched_frame > kBefore * 2 - 10 && switched_frame < kBefore * 2 + 10);
    SEQLIB_CHECK(Matches(after.samples, slow_run, switched_frame + 1, 0));
    SEQLIB_CHECK(std::abs(clock->ModelTicks() - switched_ticks - 24.0) <= 1.0);
}

// A catch-up runs a new model up to the clock's frame, with the clock's mutes. The model's frame then matches the model
// run directly. Without a clock, it runs through the sequence's first pass, and its snapshots cover it. A run stops
// where the model's output ends. A take cancels a run that's going.
void TestCatchUp()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const auto loop = std::make_shared<const Sequence>(ReadSequence(*archive, kLoopSound));
    const auto note = std::make_shared<const Sequence>(ReadSequence(*archive, kNoteSound));
    const ModelRun model = RunModel(*archive, kLoopSound, 800);
    const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
    clock->SetModelFrames(300);
    ModelCatchUp catch_up;

    catch_up.Start(ModelOf(archive, loop), clock);
    const std::shared_ptr<SequenceModel> caught_up = WaitForCatchUp(catch_up);
    clock->SetMutes(1u << 0);
    catch_up.Start(ModelOf(archive, loop), clock);
    const std::shared_ptr<SequenceModel> muted = WaitForCatchUp(catch_up);
    catch_up.Start(ModelOf(archive, loop, 10), nullptr);
    const std::shared_ptr<SequenceModel> first_pass = WaitForCatchUp(catch_up);
    clock->SetModelFrames(std::numeric_limits<uint64_t>::max());
    catch_up.Start(ModelOf(archive, note), clock);
    const std::shared_ptr<SequenceModel> ended = WaitForCatchUp(catch_up);
    catch_up.Start(ModelOf(archive, loop), clock);
    const bool running = catch_up.Pending() && !catch_up.Finished();
    const std::shared_ptr<SequenceModel> cancelled = catch_up.Take();

    SEQLIB_REQUIRE(caught_up && muted && first_pass && ended && cancelled);
    SEQLIB_CHECK(caught_up->Frames() == 300 && std::ranges::equal(caught_up->Output(), ModelFrame(model, 299)));
    SEQLIB_CHECK(HasSound(caught_up->Output()) && muted->Frames() == 300 && !HasSound(muted->Output()));
    SEQLIB_CHECK(first_pass->Frames() == FirstFrameReaching(model, loop->length) + 1);
    SEQLIB_CHECK(first_pass->SnapshotCount() > 30);
    SEQLIB_CHECK(ended->Frames() > 0 && !ended->RunFrame());
    SEQLIB_CHECK(running && !catch_up.Pending() && cancelled->Frames() > 0);
}

// The lead lasts 0.5 s, or as long as the longest tail of the effects takes to fall by 60 dB, up to 20 s. Without
// effects, it lasts 100 frames of 5 ms. nn::audio's I3DL2 preset 1 falls in 1.7492 s: 350 frames. On the Wii U, an
// effect's decay counts a quarter longer. A high reverb with a pre-delay of 0.5 s and a fused time of 4 s then falls in
// 5.5 s: 1,834 frames of 3 ms. A delay of 500 ms that halves its echoes falls in 1.25 * 1.5 s / log10(2), about 6.23 s:
// 2,077 frames. A delay whose echoes never fall gives 20 s: 6,667 frames. With a chorus, every frame mixes. A model is
// in the lead before frame `frame` from frame `frame` + 1 - the lead on.
void TestLeadFrames()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kLoopSound));
    const std::shared_ptr<const sar::SoundArchive> wii_u = OpenWiiULoopArchive();
    const auto wii_u_loop = std::make_shared<const Sequence>(ReadSequence(*wii_u, kLoopSound));
    std::vector<float> reverb = hokuto::song::WiiUStartingParameters(cafe::EffectType::kReverbHi);
    reverb[0] = 0.5f;
    reverb[1] = 4.0f;
    std::vector<float> delay = hokuto::song::WiiUStartingParameters(cafe::EffectType::kDelay);
    delay[0] = 500.0f;
    delay[1] = 0.5f;
    delay[3] = 500.0f;
    std::vector<float> endless = delay;
    endless[1] = 1.0f;
    const std::vector<float> chorus = hokuto::song::WiiUStartingParameters(cafe::EffectType::kChorus);

    const auto lead = [&wii_u, &wii_u_loop](cafe::EffectType type, const std::vector<float>& parameters)
    {
        const hokuto::song::WiiUEffect effect = hokuto::song::WiiUEffectOf(type, parameters);
        return ModelOf(wii_u, wii_u_loop, SequenceModel::kSnapshotInterval, {{{effect}, {}, {}}})->LeadFrames();
    };

    const std::shared_ptr<SequenceModel> model = ModelOf(archive, sequence);
    const bool from_start = model->InLead(99) && !model->InLead(100);
    const bool sought = model->SeekFrame(0);
    const bool after_first = model->InLead(100) && !model->InLead(101);
    const bool by_ticks = model->InLeadOfTick(40.0) && !model->InLeadOfTick(1000.0);
    const auto with_chorus = ModelOf(wii_u, wii_u_loop, SequenceModel::kSnapshotInterval,
                                     {{{hokuto::song::WiiUEffectOf(cafe::EffectType::kChorus, chorus)}, {}, {}}});

    SEQLIB_CHECK_EQ(model->LeadFrames(), 100);
    SEQLIB_CHECK_EQ(ModelOf(archive, sequence, SequenceModel::kSnapshotInterval, AuxEffectsWithReverb())->LeadFrames(),
                    350);
    SEQLIB_CHECK_EQ(lead(cafe::EffectType::kReverbHi, reverb), 1834);
    SEQLIB_CHECK_EQ(lead(cafe::EffectType::kDelay, delay), 2077);
    SEQLIB_CHECK_EQ(lead(cafe::EffectType::kDelay, endless), 6667);
    SEQLIB_CHECK(with_chorus->LeadFrames() == std::numeric_limits<uint64_t>::max() && with_chorus->InLead(1'000'000) &&
                 with_chorus->InLeadOfTick(1'000'000.0));
    SEQLIB_CHECK(from_start && sought && after_first && by_ticks);
}

// Frames without mixing play the sequence in silence. The loop's sequence plays from its start with the mixing off from
// frame 1 to frame 299. Its position follows the model run directly, and its output and meters stay silent. The mixing
// then comes on again. Once the lead's 100 frames have mixed, the output is the model's. The mixing then goes off once
// more. A seek to the model's last frame leaves the model where it is, and turns the mixing on: the next frame is the
// model's.
void TestFramesWithoutMixing()
{
    constexpr std::size_t kStretchEnd = 300;
    constexpr std::size_t kFrames = 1050;
    constexpr std::array<int32_t, 2> kSilent = {0, 0};
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kLoopSound));
    const ModelRun model = RunModel(*archive, kLoopSound, kFrames + 1);
    const std::shared_ptr<SequenceModel> shared = ModelOf(archive, sequence);

    bool silent = shared->SeekFrame(0);
    shared->SetMixing(false);
    std::size_t moved_wrong = 0;
    for (std::size_t frame = 1; frame < kStretchEnd; frame++)
    {
        silent = silent && shared->RunFrame() && !HasSound(shared->Output()) && shared->TrackPeaks(0) == kSilent;
        moved_wrong += shared->After() == static_cast<double>(model.after[frame]) ? 0 : 1;
    }
    shared->SetMixing(true);
    const uint64_t settled = kStretchEnd + shared->LeadFrames();
    std::size_t wrong = 0;
    for (std::size_t frame = kStretchEnd; frame < kFrames; frame++)
    {
        const bool ran = shared->RunFrame();
        wrong += !ran || (frame >= settled && !std::ranges::equal(shared->Output(), ModelFrame(model, frame))) ? 1 : 0;
    }
    shared->SetMixing(false);
    const bool stayed = shared->SeekFrame(kFrames - 1) && shared->Frames() == kFrames;
    const bool mixes = shared->RunFrame() && std::ranges::equal(shared->Output(), ModelFrame(model, kFrames));

    SEQLIB_REQUIRE(HasSound(
        std::span<const int16_t>(model.samples).subspan(kFrameSamples * 2, (kStretchEnd - 1) * kFrameSamples * 2)));
    SEQLIB_REQUIRE(HasSound(ModelFrame(model, kFrames)));
    SEQLIB_CHECK_EQ(shared->LeadFrames(), 100);
    SEQLIB_CHECK(silent);
    SEQLIB_CHECK_EQ(moved_wrong, 0);
    SEQLIB_CHECK_EQ(wrong, 0);
    SEQLIB_CHECK(stayed && mixes);
}

// The lowest ratio of signal to difference, in dB, of 20 frames of `model` from each of `frames`, after a seek to it,
// against the same frames of `run`. A seek that fails, or an output that ends sooner, gives minus infinity.
double SeekRatio(SequenceModel& model, const ModelRun& run, std::initializer_list<uint64_t> frames)
{
    constexpr std::size_t kSamples = 20 * kFrameSamples * 2;
    double lowest = std::numeric_limits<double>::infinity();
    for (const uint64_t frame : frames)
    {
        std::vector<int16_t> played;
        for (bool more = model.SeekFrame(frame); more && played.size() < kSamples; more = model.RunFrame())
        {
            played.insert(played.end(), model.Output().begin(), model.Output().end());
        }
        const std::size_t at = frame * kFrameSamples * 2;
        if (played.size() != kSamples || at + kSamples > run.samples.size())
        {
            return -std::numeric_limits<double>::infinity();
        }
        lowest =
            std::min(lowest, SignalToDifference(played, std::span<const int16_t>(run.samples).subspan(at, kSamples)));
    }

    return lowest;
}

// A seek far from the snapshots runs the frames before the lead without mixing. The loop's sequence seeks to frame 1450
// from its start, with a snapshot every 10 frames of its first pass. The frames before frame 1351 don't mix, and the
// lead's 100 frames do. The output is then the model's run directly, at that frame and the 19 after it. So it is after
// seeks back: to frame 1050, past the first pass's snapshots, and to frame 250, from a snapshot that didn't mix. A seek
// to tick 700 gives the frame that plays it. With nn::audio's I3DL2 preset 1 on the send loop's sequence, the lead
// lasts 350 frames. The reverb's tail falls by 60 dB in them. After the same seeks, the output lies 60 dB or more below
// the model's. A model of that sequence that has run 300 frames without mixing seeks to frame 400, with its mixing
// still off. The seek restores a snapshot before the lead and mixes the lead, and the frames after the seek mix. The
// output then lies 60 dB or more below the model's too.
void TestSeekWithoutMixing()
{
    constexpr std::size_t kFrames = 1500;
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kLoopSound));
    const ModelRun model = RunModel(*archive, kLoopSound, kFrames);
    const std::shared_ptr<const sar::SoundArchive> send_archive = OpenSendArchive(kSendLoopBytecode);
    const auto send = std::make_shared<const Sequence>(ReadSequence(*send_archive, kSendSound));
    const ModelRun wet = RunModel(*send_archive, kSendSound, kFrames, AuxReverb());
    const ModelRun dry = RunModel(*send_archive, kSendSound, kFrames);

    const double exact = SeekRatio(*ModelOf(archive, sequence, 10), model, {1450, 1050, 250});
    const std::shared_ptr<SequenceModel> by_ticks = ModelOf(archive, sequence, 10);
    const bool sought = by_ticks->Seek(700);
    const std::size_t tick_frame = FirstFrameReaching(model, 701);
    const double reverb = SeekRatio(*ModelOf(send_archive, send, 10, AuxEffectsWithReverb()), wet, {1450, 1050, 250});
    const std::shared_ptr<SequenceModel> unsettled = ModelOf(send_archive, send, 10, AuxEffectsWithReverb());
    bool ran = unsettled->SeekFrame(0);
    unsettled->SetMixing(false);
    for (int frame = 1; frame < 301; frame++)
    {
        ran = ran && unsettled->RunFrame();
    }
    const double after_unmixed = SeekRatio(*unsettled, wet, {400});

    SEQLIB_REQUIRE(HasSound(ModelFrame(model, 1450)) && HasSound(ModelFrame(model, 1050)) &&
                   HasSound(ModelFrame(model, 250)));
    SEQLIB_REQUIRE(!std::ranges::equal(ModelFrame(wet, 1450), ModelFrame(dry, 1450)));
    SEQLIB_CHECK(std::isinf(exact) && exact > 0.0);
    SEQLIB_CHECK(sought && by_ticks->Frames() == tick_frame + 1 &&
                 std::ranges::equal(by_ticks->Output(), ModelFrame(model, tick_frame)));
    SEQLIB_CHECK(reverb >= 60.0);
    SEQLIB_CHECK(ran && after_unmixed >= 60.0);
}

// A tempo of 0 stops the player's clock with a note sounding: the sequence turns the note wait off, and sets a tempo of
// 0 at tick 24. The model plays on without ticks. Like a catch-up without a clock, a seek past the tick and a catch-up
// in ticks stop there. A catch-up in frames runs to its frame.
void TestStoppedClock()
{
    const std::vector<uint8_t> kStopping = {0xB0, 48,   0xE1, 120, 0x00, 0xC7, 0,  60,   100, 96,  0x80,
                                            24,   0xE1, 0,    0,   60,   100,  48, 0x80, 48,  0xFF};
    const std::shared_ptr<const sar::SoundArchive> archive = OpenSendArchive(kStopping);
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kSendSound));
    const auto by_ticks = std::make_shared<PlaybackClock>(kFrameSamples);
    by_ticks->SetModelTicks(200.0);
    const auto by_frames = std::make_shared<PlaybackClock>(kFrameSamples);
    by_frames->SetModelFrames(300);

    const auto sought = ModelOf(archive, sequence);
    const bool reached = sought->Seek(200);
    ModelCatchUp ticks_catch_up;
    ticks_catch_up.Start(ModelOf(archive, sequence), by_ticks, true);
    const std::shared_ptr<SequenceModel> in_ticks = WaitForCatchUp(ticks_catch_up);
    ModelCatchUp pass_catch_up;
    pass_catch_up.Start(ModelOf(archive, sequence), nullptr);
    const std::shared_ptr<SequenceModel> through_pass = WaitForCatchUp(pass_catch_up);
    ModelCatchUp frames_catch_up;
    frames_catch_up.Start(ModelOf(archive, sequence), by_frames);
    const std::shared_ptr<SequenceModel> in_frames = WaitForCatchUp(frames_catch_up);

    SEQLIB_CHECK(!reached && sought->ClockStopped() && sought->After() <= 25.0);
    SEQLIB_REQUIRE(in_ticks != nullptr && through_pass != nullptr && in_frames != nullptr);
    SEQLIB_CHECK(in_ticks->ClockStopped() && in_ticks->After() <= 25.0);
    SEQLIB_CHECK(through_pass->ClockStopped() && through_pass->After() <= 25.0);
    SEQLIB_CHECK(in_frames->ClockStopped() && in_frames->Frames() == 300 && HasSound(in_frames->Output()));
}

// A catch-up far from the model's start runs the frames before the lead without mixing. The loop's sequence catches up
// to frame 1450, and by ticks to tick 700. Its last frame is then the model's run directly. With nn::audio's I3DL2
// preset 1 on the send loop's sequence, the 20 frames from there lie 60 dB or more below the model's.
void TestCatchUpWithoutMixing()
{
    constexpr std::size_t kSamples = 20 * kFrameSamples * 2;
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const auto loop = std::make_shared<const Sequence>(ReadSequence(*archive, kLoopSound));
    const ModelRun model = RunModel(*archive, kLoopSound, 1500);
    const std::shared_ptr<const sar::SoundArchive> send_archive = OpenSendArchive(kSendLoopBytecode);
    const auto send = std::make_shared<const Sequence>(ReadSequence(*send_archive, kSendSound));
    const ModelRun wet = RunModel(*send_archive, kSendSound, 1500, AuxReverb());
    const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
    clock->SetModelFrames(1450);
    clock->SetModelTicks(700.0);
    ModelCatchUp catch_up;

    catch_up.Start(ModelOf(archive, loop), clock);
    const std::shared_ptr<SequenceModel> by_frames = WaitForCatchUp(catch_up);
    catch_up.Start(ModelOf(archive, loop), clock, true);
    const std::shared_ptr<SequenceModel> by_ticks = WaitForCatchUp(catch_up);
    catch_up.Start(ModelOf(send_archive, send, SequenceModel::kSnapshotInterval, AuxEffectsWithReverb()), clock);
    const std::shared_ptr<SequenceModel> reverb = WaitForCatchUp(catch_up);
    SEQLIB_REQUIRE(by_frames && by_ticks && reverb);
    std::vector<int16_t> played(reverb->Output().begin(), reverb->Output().end());
    while (played.size() < kSamples && reverb->RunFrame())
    {
        played.insert(played.end(), reverb->Output().begin(), reverb->Output().end());
    }
    const std::size_t tick_frame = FirstFrameReaching(model, 700);

    SEQLIB_CHECK(by_frames->Frames() == 1450 && std::ranges::equal(by_frames->Output(), ModelFrame(model, 1449)));
    SEQLIB_CHECK(by_ticks->Frames() == tick_frame + 1 &&
                 std::ranges::equal(by_ticks->Output(), ModelFrame(model, tick_frame)));
    SEQLIB_REQUIRE(reverb->Frames() == 1469 && played.size() == kSamples);
    SEQLIB_CHECK(
        SignalToDifference(played, std::span<const int16_t>(wet.samples).subspan(1449 * kFrameSamples * 2, kSamples)) >=
        60.0);
}

// A catch-up in ticks can stop short of the clock's position. A model of another engine then stops at the sample that
// the audio device plays. 0.1 s at the loop's tempo is 20 frames of 5 ms. The model stops in the frame that reaches
// that tick. The model counts whole ticks. A seek to its next tick goes on from there, up to the first frame past that
// tick. The frame is the model's run directly.
void TestCatchUpBehind()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const auto loop = std::make_shared<const Sequence>(ReadSequence(*archive, kLoopSound));
    const ModelRun model = RunModel(*archive, kLoopSound, 1500);
    const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
    clock->SetModelTicks(700.0);
    ModelCatchUp catch_up;

    catch_up.Start(ModelOf(archive, loop), clock, true, 0.1);
    const std::shared_ptr<SequenceModel> behind = WaitForCatchUp(catch_up);
    SEQLIB_REQUIRE(behind != nullptr);
    const double per_frame = behind->TicksPerFrame(700.0);
    const double target = 700.0 - 20.0 * per_frame;
    const double after = behind->After();
    const auto next = static_cast<uint64_t>(std::ceil(after));
    const bool sought = behind->Seek(next);
    std::size_t tick_frame = 0;
    while (tick_frame < model.after.size() && model.after[tick_frame] <= static_cast<double>(next))
    {
        tick_frame++;
    }

    SEQLIB_CHECK(per_frame > 0.0 && after >= target && after < target + std::max(per_frame, 1.0));
    SEQLIB_CHECK(sought && behind->Frames() == tick_frame + 1 &&
                 std::ranges::equal(behind->Output(), ModelFrame(model, tick_frame)));
}

// On the Wii U, a seek far from the snapshots runs the frames before the lead without mixing too. The lead lasts 0.5 s:
// 167 frames of 3 ms. The loop's sequence seeks to frame 2417 from its start, with a snapshot every 50 frames of its
// first pass. It then seeks back to frame 1417, past the first pass's snapshots, and to frame 100. The output is the
// model's run directly, at each frame and the 19 after it. So it is in enhanced mixing.
void TestWiiUSeekWithoutMixing()
{
    constexpr std::size_t kWiiUFrame = cafe::ArchiveModel::kFrameSamples * 2;
    constexpr std::size_t kSamples = 20 * kWiiUFrame;
    const std::shared_ptr<const sar::SoundArchive> archive = OpenWiiULoopArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kLoopSound));

    std::size_t wrong = 0;
    std::size_t silent = 0;
    for (const Mixing mixing : {Mixing::kStrict, Mixing::kEnhanced})
    {
        const std::vector<int16_t> direct =
            RunWiiUModel(*archive, kLoopSound, 2500, {}, ntdlib::cafe::ax::kUnityVolume, mixing == Mixing::kEnhanced);
        const std::shared_ptr<SequenceModel> model = ModelOf(archive, sequence, 50, {}, mixing);
        for (const uint64_t frame : {2417u, 1417u, 100u})
        {
            std::vector<int16_t> played;
            for (bool more = model->SeekFrame(frame); more && played.size() < kSamples; more = model->RunFrame())
            {
                played.insert(played.end(), model->Output().begin(), model->Output().end());
            }
            SEQLIB_REQUIRE(direct.size() >= frame * kWiiUFrame + kSamples);
            const auto at = direct.begin() + static_cast<std::ptrdiff_t>(frame * kWiiUFrame);
            wrong += played.size() == kSamples && std::equal(played.begin(), played.end(), at) ? 0 : 1;
            silent += HasSound(std::span<const int16_t>(at, at + static_cast<std::ptrdiff_t>(kSamples))) ? 0 : 1;
        }
        wrong += model->LeadFrames() == 167 ? 0 : 1;
    }

    SEQLIB_REQUIRE_EQ(silent, 0);
    SEQLIB_CHECK_EQ(wrong, 0);
}

// In enhanced mixing, the player writes the output of ntdlib's model in its enhanced mode, run directly, in floating
// point: from the start, and from a seek that restores a snapshot, after the frame that fades in. The output differs
// from strict mixing. A model of the other mode takes over playback at the player's frame with a crossfade, as after a
// switch in the mixer.
void TestEnhancedMixing()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kLoopSound));
    atk::EngineOptions options;
    options.enhanced = true;
    const ModelRun enhanced = RunModel(*archive, kLoopSound, 800, options);
    const ModelRun strict = RunModel(*archive, kLoopSound, 800);
    const std::shared_ptr<SequenceModel> shared = ModelOf(archive, sequence, 10, {}, Mixing::kEnhanced);
    const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
    clock->SetCycle(kGameLoop);
    SequencePlayer from_start(shared, 0, clock);
    const auto seek_clock = std::make_shared<PlaybackClock>(kFrameSamples);
    seek_clock->SetCycle(kGameLoop);
    SequencePlayer from_seek(shared, 120, seek_clock);
    const auto switch_clock = std::make_shared<PlaybackClock>(kFrameSamples);
    switch_clock->SetCycle(kGameLoop);
    SequencePlayer switching(ModelOf(archive, sequence), 0, switch_clock);

    const PlayerRun run = RunPlayer(from_start, *clock, 300);
    const PlayerRun seek = RunPlayer(from_seek, *seek_clock, 100);
    const PlayerRun before = RunPlayer(switching, *switch_clock, 50);
    switch_clock->Offer(ModelOf(archive, sequence, SequenceModel::kSnapshotInterval, {}, Mixing::kEnhanced));
    const PlayerRun after = RunPlayer(switching, *switch_clock, 50);

    constexpr std::size_t kFrame = kFrameSamples * 2;
    const std::size_t first = FirstFrameReaching(enhanced, 1);
    const std::size_t seek_first = FirstFrameReaching(enhanced, 121);
    SEQLIB_REQUIRE(enhanced.floats.size() == enhanced.samples.size() && strict.floats.empty());
    SEQLIB_REQUIRE(run.samples.size() == 300 * kFrame && seek.samples.size() == 100 * kFrame);
    SEQLIB_REQUIRE(before.samples.size() == 50 * kFrame && after.samples.size() == 50 * kFrame);

    const auto from = [&enhanced](std::size_t model_frame)
    {
        return enhanced.floats.begin() + static_cast<std::ptrdiff_t>(model_frame * kFrame);
    };
    SEQLIB_CHECK(std::equal(run.samples.begin(), run.samples.end(), from(first)));
    SEQLIB_CHECK(std::equal(seek.samples.begin() + static_cast<std::ptrdiff_t>(kFrame), seek.samples.end(),
                            from(seek_first + 1)));
    SEQLIB_CHECK(enhanced.samples != strict.samples && HasSound(std::span<const int16_t>(enhanced.samples)));
    SEQLIB_CHECK(Matches(before.samples, strict, first, 0));
    bool crossfades = true;
    for (std::size_t i = 0; i < kFrame; i++)
    {
        const float through = static_cast<float>(i / 2) / static_cast<float>(kFrameSamples);
        const float out = SampleOf(strict, first + 50, i) * (1.0f - through);
        const float in = enhanced.floats[(first + 50) * kFrame + i] * through;
        crossfades = crossfades && after.samples[i] == in + out;
    }
    SEQLIB_CHECK(crossfades);
    SEQLIB_CHECK(
        std::equal(after.samples.begin() + static_cast<std::ptrdiff_t>(kFrame), after.samples.end(), from(first + 51)));
}

// The dry render of a sequence for the Export window matches ntdlib's model run directly with the aux buses at volume
// 0, from the first frame, in strict and in enhanced mixing. It lasts the seconds that it's given, or until the output
// ends. The send sequence's note reaches the output through aux bus A too, at full volume. Its dry render leaves that
// out, and ends once the note has faded. A render that's cancelled stops.
void TestDryRender()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenSendArchive();
    const auto loop = std::make_shared<const Sequence>(ReadSequence(*archive, kLoopSound));
    const auto send = std::make_shared<const Sequence>(ReadSequence(*archive, kSendSound));
    const std::atomic<bool> kGo{false};
    const std::atomic<bool> kStop{true};
    constexpr AuxVolumes kSilent = {0.0f, 0.0f, 0.0f};
    atk::EngineOptions options;
    options.enhanced = true;

    const DryRender looped = RenderDry(archive, loop, Mixing::kStrict, 3.0, kGo);
    const DryRender enhanced = RenderDry(archive, loop, Mixing::kEnhanced, 3.0, kGo);
    const DryRender dry = RenderDry(archive, send, Mixing::kStrict, 1.0, kGo);
    const ModelRun looped_model = RunModel(*archive, kLoopSound, 600, {}, {}, {}, kSilent);
    const ModelRun enhanced_model = RunModel(*archive, kLoopSound, 600, options, {}, {}, kSilent);
    const ModelRun dry_model = RunModel(*archive, kSendSound, 200, {}, {}, {}, kSilent);
    const ModelRun wet_model = RunModel(*archive, kSendSound, 200);

    const auto scaled = [](const ModelRun& run)
    {
        std::vector<float> floats(run.samples.size());
        std::transform(run.samples.begin(), run.samples.end(), floats.begin(),
                       [](int16_t sample) { return static_cast<float>(sample) / 32768.0f; });

        return floats;
    };

    SEQLIB_CHECK_EQ(looped.rate, 48000);
    SEQLIB_CHECK_EQ(looped.samples.size(), std::size_t{600 * kFrameSamples * 2});
    SEQLIB_CHECK(looped.samples == scaled(looped_model));
    SEQLIB_CHECK(enhanced.samples == enhanced_model.floats);
    const std::vector<float> dry_floats = scaled(dry_model);
    const std::vector<float> wet_floats = scaled(wet_model);
    SEQLIB_REQUIRE(dry.samples.size() < dry_floats.size());
    SEQLIB_CHECK(std::equal(dry.samples.begin(), dry.samples.end(), dry_floats.begin()));
    SEQLIB_CHECK(!HasSound(std::span<const float>(dry_floats).subspan(dry.samples.size())));
    SEQLIB_CHECK(HasSound(std::span<const float>(dry.samples)));
    SEQLIB_CHECK(!std::equal(dry.samples.begin(), dry.samples.end(), wet_floats.begin()));
    SEQLIB_CHECK(RenderDry(archive, loop, Mixing::kStrict, 3.0, kStop).samples.empty());
}

// An I3DL2 reverb's parameters go into nn::audio's set in the order of its fields, and come back out the same.
void TestI3dl2Order()
{
    std::array<float, hokuto::song::kI3dl2Parameters> parameters{};
    std::iota(parameters.begin(), parameters.end(), 1.0f);

    const ntdlib::nx::audio::I3dl2ReverbParameterSet reverb = I3dl2Reverb(parameters);

    SEQLIB_CHECK(reverb.room_gain == 1.0f && reverb.room_hf_gain == 2.0f && reverb.late_reverb_decay_time == 3.0f);
    SEQLIB_CHECK(reverb.late_reverb_hf_decay_ratio == 4.0f && reverb.reflections_gain == 5.0f &&
                 reverb.reflections_delay == 6.0f);
    SEQLIB_CHECK(reverb.reverb_gain == 7.0f && reverb.late_reverb_delay == 8.0f &&
                 reverb.late_reverb_diffusion == 9.0f);
    SEQLIB_CHECK(reverb.late_reverb_density == 10.0f && reverb.hf_reference == 11.0f && reverb.dry_gain == 12.0f);
    SEQLIB_CHECK(I3dl2Parameters(reverb) == parameters);
}

// The test archive's bank holds programs 0, 1 and 3, and its wave archive two waves. Items that the archive lacks give
// nothing.
void TestPreviewItems()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();

    const std::vector<uint32_t> programs = ProgramsOf(*archive, 0);

    SEQLIB_CHECK(programs == std::vector<uint32_t>({0, 1, 3}));
    SEQLIB_CHECK(ProgramsOf(*archive, 1).empty());
    SEQLIB_CHECK_EQ(WaveCountOf(*archive, 0), 2);
    SEQLIB_CHECK_EQ(WaveCountOf(*archive, 1), 0);
}

// Each item's file holds the bytes that the test wrote into the archive: a sequence sound's sequence file, the bank,
// the wave archive, each of its waves, and a wave sound's file. An item that the archive lacks throws, and so does a
// sound of the other kind.
void TestItemFiles()
{
    const sar_files::ArchiveSpec spec = MadeUpWaveSoundSpec();
    const TemporaryFile file(sar_files::Archive(false, spec));
    const std::shared_ptr<const sar::SoundArchive> archive = OpenArchive(file.Path());
    const auto bytes = [&archive](const Item& item)
    {
        const std::span<const uint8_t> data = ItemFile(*archive, item);
        return std::vector<uint8_t>(data.begin(), data.end());
    };

    const std::vector<std::vector<uint8_t>> waves = TestWaveFiles(false);

    SEQLIB_CHECK(bytes({ItemKind::kSequence, kNoteSound, 0}) == spec.files[1].bytes);
    SEQLIB_CHECK(bytes({ItemKind::kBank, 0, 0}) == spec.files[2].bytes);
    SEQLIB_CHECK(bytes({ItemKind::kWaveArchive, 0, 0}) == spec.files[3].bytes);
    SEQLIB_CHECK(bytes({ItemKind::kWave, 0, 0}) == waves[0]);
    SEQLIB_CHECK(bytes({ItemKind::kWave, 0, 1}) == waves[1]);
    SEQLIB_CHECK(bytes({ItemKind::kWaveSound, kWaveSound, 0}) == spec.files[6].bytes);
    SEQLIB_CHECK(ThrowsRuntimeError([&] { ItemFile(*archive, {ItemKind::kSequence, kWaveSound, 0}); }));
    SEQLIB_CHECK(ThrowsRuntimeError([&] { ItemFile(*archive, {ItemKind::kWaveSound, kNoteSound, 0}); }));
    SEQLIB_CHECK(ThrowsRuntimeError([&] { ItemFile(*archive, {ItemKind::kSequence, 5, 0}); }));
    SEQLIB_CHECK(ThrowsRuntimeError([&] { ItemFile(*archive, {ItemKind::kBank, 1, 0}); }));
    SEQLIB_CHECK(ThrowsRuntimeError([&] { ItemFile(*archive, {ItemKind::kWaveArchive, 1, 0}); }));
    SEQLIB_CHECK(ThrowsRuntimeError([&] { ItemFile(*archive, {ItemKind::kWave, 0, 2}); }));
}

// An item's file takes the item's name and its format's extension. A wave takes its archive's name and its index, with
// zeros in front up to the digits of the archive's last index. A name loses the characters that a file name can't hold.
// The user sees a wave as its archive's name and its index.
void TestItemNames()
{
    sar_files::ArchiveSpec spec = TestArchiveSpec();
    spec.banks[0].first = "BANK:TEST";
    const TemporaryFile file(sar_files::Archive(false, spec));
    const std::shared_ptr<const sar::SoundArchive> archive = OpenArchive(file.Path());

    const std::string sequence = ItemFileName(*archive, {ItemKind::kSequence, kNoteSound, 0});
    const std::string bank = ItemFileName(*archive, {ItemKind::kBank, 0, 0});
    const std::string wave_archive = ItemFileName(*archive, {ItemKind::kWaveArchive, 0, 0});

    SEQLIB_CHECK(sequence == "SEQ_NOTE.bfseq");
    SEQLIB_CHECK(bank == "BANK_TEST.bfbnk");
    SEQLIB_CHECK(wave_archive == "WARC_TEST.bfwar");
    SEQLIB_CHECK(ItemFileName(*archive, {ItemKind::kWave, 0, 1}, 2) == "WARC_TEST_1.bfwav");
    SEQLIB_CHECK(ItemFileName(*archive, {ItemKind::kWave, 0, 3}, 11) == "WARC_TEST_03.bfwav");
    SEQLIB_CHECK(ItemFileName(*archive, {ItemKind::kWave, 0, 10}, 11) == "WARC_TEST_10.bfwav");
    SEQLIB_CHECK(ItemFileName(*archive, {ItemKind::kWave, 0, 7}, 300) == "WARC_TEST_007.bfwav");
    SEQLIB_CHECK(ItemName(*archive, {ItemKind::kBank, 0, 0}) == "BANK:TEST");
    SEQLIB_CHECK(ItemName(*archive, {ItemKind::kWave, 0, 1}) == "WARC_TEST, wave 1");
}

// The message about a folder that can't be made ends with one full stop. The Mac's reasons have none, such as "File
// exists". Those of Windows have one, such as "Access is denied."
void TestFolderFailure()
{
    const ReasonCategory kMac("File exists");
    const ReasonCategory kWindows("Access is denied.");

    const std::string without = FolderFailure("BANK", std::error_code(17, kMac));
    const std::string with = FolderFailure("BANK", std::error_code(5, kWindows));

    SEQLIB_CHECK(without == "Hokuto can't make the folder BANK. File exists.");
    SEQLIB_CHECK(with == "Hokuto can't make the folder BANK. Access is denied.");
}

// Three wave sound files hold five wave sounds. The first file's wave sounds come first, and one of them comes last. A
// sound group with a name holds the first file's first two wave sounds, and one without a name the second file's. The
// third file's wave sound lies in a group of another file only. Each file lists its wave sounds in order, under its
// group's name, WSDSET_ and the unnamed group's index, or its first wave sound's name. Each wave sound gives its file
// as a wave sound file, under the file's name. A sequence sound gives none.
void TestWaveSoundSets()
{
    sar_files::ArchiveSpec spec = TestArchiveSpec();
    const auto add = [&spec](const char* name, uint32_t file)
    {
        sar_files::SoundSpec sound;
        sound.name = name;
        sound.kind = sar_files::SoundSpec::kWaveSound;
        sound.file = file;
        spec.sounds.push_back(sound);
    };
    add("WSD_A1", 6);
    add("WSD_A2", 6);
    add("WSD_B", 7);
    add("WSD_C", 8);
    add("WSD_A3", 6);
    for (const uint8_t value : std::initializer_list<uint8_t>{1, 2, 3})
    {
        spec.files.push_back({{'F', 'W', 'S', 'D', value, value, value, value}, "", false});
    }
    spec.sound_groups = {{"WSDSET:A", 4, 5, {6}}, {"", 6, 6, {7}}, {"WSDSET_OTHER", 7, 7, {9}}};
    const TemporaryFile file(sar_files::Archive(false, spec));
    const std::shared_ptr<const sar::SoundArchive> archive = OpenArchive(file.Path());
    const auto bytes = [&archive](const Item& item)
    {
        const std::span<const uint8_t> data = ItemFile(*archive, item);
        return std::vector<uint8_t>(data.begin(), data.end());
    };

    const std::vector<WaveSoundSet> sets = WaveSoundSets(*archive);

    SEQLIB_CHECK_EQ(sets.size(), std::size_t{3});
    SEQLIB_CHECK(sets.size() == 3 && sets[0].file_id == 6 && sets[0].name == "WSDSET:A" &&
                 sets[0].sounds == std::vector<uint32_t>({4, 5, 8}));
    SEQLIB_CHECK(sets.size() == 3 && sets[1].file_id == 7 && sets[1].name == "WSDSET_1" &&
                 sets[1].sounds == std::vector<uint32_t>({6}));
    SEQLIB_CHECK(sets.size() == 3 && sets[2].file_id == 8 && sets[2].name == "WSD_C" &&
                 sets[2].sounds == std::vector<uint32_t>({7}));
    SEQLIB_CHECK(ItemName(*archive, {ItemKind::kWaveSoundFile, 8, 0}) == "WSDSET:A");
    SEQLIB_CHECK(ItemFileName(*archive, {ItemKind::kWaveSoundFile, 4, 0}) == "WSDSET_A.bfwsd");
    SEQLIB_CHECK(ItemFileName(*archive, {ItemKind::kWaveSoundFile, 6, 0}) == "WSDSET_1.bfwsd");
    SEQLIB_CHECK(bytes({ItemKind::kWaveSoundFile, 5, 0}) == spec.files[6].bytes);
    SEQLIB_CHECK(bytes({ItemKind::kWaveSoundFile, 7, 0}) == spec.files[8].bytes);
    SEQLIB_CHECK(hokuto::song::ItemFormat(ItemKind::kWaveSoundFile, sar::Flavor::kCafe) == "BFWSD");
    SEQLIB_CHECK(ThrowsRuntimeError([&] { ItemName(*archive, {ItemKind::kWaveSoundFile, kNoteSound, 0}); }));
    SEQLIB_CHECK(WaveSoundSets(*OpenTestArchive()).empty());
}

// A wave goes out as a WAV file of 16-bit PCM: the samples that the test wrote into it, at its rate, with its loop. A
// mono wave stays mono. The Wii U's waves give the same files. A wave that the archive lacks throws.
void TestWaveAsWav()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const std::shared_ptr<const sar::SoundArchive> wii_u = OpenWiiUArchive();
    seqlib::Pcm mono;
    mono.loop = true;
    mono.loop_end = 480;
    mono.channels.resize(1);
    seqlib::Pcm stereo = mono;
    stereo.channels.resize(2);
    for (int i = 0; i < 480; i++)
    {
        mono.channels[0].push_back(static_cast<int16_t>(SawSample(i, false)));
        stereo.channels[0].push_back(static_cast<int16_t>(SawSample(i, false)));
        stereo.channels[1].push_back(static_cast<int16_t>(SawSample(i, true)));
    }

    const std::vector<uint8_t> mono_file = WaveAsWav(*archive, 0, 0);
    const std::vector<uint8_t> stereo_file = WaveAsWav(*archive, 0, 1);

    SEQLIB_CHECK(mono_file == seqlib::WavFile(mono, 48000));
    SEQLIB_CHECK(stereo_file == seqlib::WavFile(stereo, 48000));
    SEQLIB_CHECK(WaveAsWav(*wii_u, 0, 1) == stereo_file);
    SEQLIB_CHECK_EQ(WaveChannels(*archive, 0, 0), 1);
    SEQLIB_CHECK_EQ(WaveChannels(*archive, 0, 1), 2);
    SEQLIB_CHECK(ThrowsRuntimeError([&] { WaveAsWav(*archive, 0, 2); }));
    SEQLIB_CHECK(ThrowsRuntimeError([&] { WaveAsWav(*archive, 1, 0); }));
}

// The sounds of WaveSoundArchiveSpec, after the test archive's four sequence sounds.
constexpr uint32_t kHighSound = 4;
constexpr uint32_t kPannedSound = 5;
constexpr uint32_t kSilentSound = 6;
constexpr uint32_t kPastSound = 7;
constexpr uint32_t kLostSound = 8;
constexpr uint32_t kPlainSound = 9;
constexpr uint32_t kNoNoteSound = 10;
constexpr uint32_t kNoWaveSound = 11;
constexpr uint32_t kBackwardsSound = 12;

// The test archive with a wave sound file, file 6, after the four sequence sounds, and a second wave archive, file 7,
// with the test's waves the other way round. The wave sounds play waves of the second wave archive. WSD_HIGH plays the
// stereo wave at the sound's pitch of 1.5, with the sound's pan. Its note's envelope sustains below full level.
// WSD_PANNED's WAV file holds the saw at its note's pitch of 0.5, and the note sets a pan. The sound's envelope fades
// it in. Its first note event comes in its second track, and its third track holds two more. WSD_PLAIN plays the saw at
// a pitch just over 1, at 48,000.7 Hz. Each of the others fails. WSD_SILENT plays no note. WSD_PAST names a wave sound
// past the file's eight. WSD_LOST plays a wave of a third wave archive, and the archive lacks it. WSD_NO_NOTE's event
// names a note that the sound lacks. WSD_NO_WAVE's note names a wave that the file lacks. WSD_BACKWARDS's pitch is -1.
sar_files::ArchiveSpec WaveSoundArchiveSpec(bool big_endian)
{
    sar_files::Region stereo;
    stereo.wave = 1;
    stereo.adshr = std::array<uint8_t, 5>{127, 127, 100, 127, 127};
    sar_files::Region low;
    low.pitch = 0.5f;
    low.pan = 100;
    sar_files::Region other;
    other.pitch = 2.0f;
    sar_files::Region lost;
    lost.wave = 2;
    sar_files::Region past;
    past.wave = 3;
    const sar_files::NoteEventSpec kFirst{0.0f, 0.0f, 0};
    sar_files::WaveSoundSpec high;
    high.pan = 30;
    high.pitch = 1.5f;
    high.tracks = {{kFirst}};
    high.notes = {stereo};
    sar_files::WaveSoundSpec panned;
    panned.adshr = std::array<uint8_t, 5>{100, 127, 127, 127, 127};
    panned.tracks = {{}, {kFirst}, {{0.0f, 0.0f, 1}, {0.5f, 0.0f, 0}}};
    panned.notes = {low, other};
    sar_files::WaveSoundSpec silent;
    silent.tracks = {{}};
    sar_files::WaveSoundSpec lost_sound = high;
    lost_sound.notes = {lost};
    sar_files::WaveSoundSpec plain;
    plain.pitch = 1.0000146f;
    plain.tracks = {{kFirst}};
    plain.notes = {sar_files::Region{}};
    sar_files::WaveSoundSpec no_note;
    no_note.tracks = {{kFirst}};
    sar_files::WaveSoundSpec no_wave = plain;
    no_wave.notes = {past};
    sar_files::WaveSoundSpec backwards = plain;
    backwards.pitch = -1.0f;
    sar_files::WaveSoundFileSpec file;
    file.waves = {{0x05000001, 1}, {0x05000001, 0}, {0x05000002, 0}};
    file.sounds = {high, panned, silent, lost_sound, plain, no_note, no_wave, backwards};

    sar_files::ArchiveSpec spec = TestArchiveSpec(big_endian);
    const std::array<std::pair<const char*, uint32_t>, 9> kSounds = {{{"WSD_HIGH", 0},
                                                                      {"WSD_PANNED", 1},
                                                                      {"WSD_SILENT", 2},
                                                                      {"WSD_PAST", 8},
                                                                      {"WSD_LOST", 3},
                                                                      {"WSD_PLAIN", 4},
                                                                      {"WSD_NO_NOTE", 5},
                                                                      {"WSD_NO_WAVE", 6},
                                                                      {"WSD_BACKWARDS", 7}}};
    for (const auto& [name, index] : kSounds)
    {
        sar_files::SoundSpec sound;
        sound.name = name;
        sound.kind = sar_files::SoundSpec::kWaveSound;
        sound.file = 6;
        sound.wave_sound_index = index;
        spec.sounds.push_back(sound);
    }
    const std::vector<std::vector<uint8_t>> waves = TestWaveFiles(big_endian);
    spec.wave_archives.push_back({"WARC_SECOND", 7});
    spec.files.push_back({sar_files::WaveSoundFile(big_endian, file), "", false});
    spec.files.push_back({sar_files::WaveArchive(big_endian, {waves[1], waves[0]}), "", false});

    return spec;
}

// WaveSoundNoteOf gives a wave sound's note 0, and the sound's pitch times the note's. The consoles take the sound's
// pitch alone. The WAV file holds the wave's samples and loop at the rate that WaveSoundNoteOf's pitch gives, to the
// nearest hertz. The Wii U's wave sounds give the same files. A wave sound reports the note events of all its tracks, a
// pan away from the centre and an envelope that shapes the sound, from the sound or from its note. The wave sounds that
// WaveSoundArchiveSpec makes to fail throw with a message for the user, and so does a sequence sound.
void TestWaveSoundAsWav()
{
    const TemporaryFile switch_file(sar_files::Archive(false, WaveSoundArchiveSpec(false)));
    const TemporaryFile wii_u_file(sar_files::Archive(true, WaveSoundArchiveSpec(true)));
    const std::shared_ptr<const sar::SoundArchive> archive = OpenArchive(switch_file.Path());
    const std::shared_ptr<const sar::SoundArchive> wii_u = OpenArchive(wii_u_file.Path());
    seqlib::Pcm saw;
    saw.loop = true;
    saw.loop_end = 480;
    saw.channels.resize(1);
    seqlib::Pcm stereo = saw;
    stereo.channels.resize(2);
    for (int i = 0; i < 480; i++)
    {
        saw.channels[0].push_back(static_cast<int16_t>(SawSample(i, false)));
        stereo.channels[0].push_back(static_cast<int16_t>(SawSample(i, false)));
        stereo.channels[1].push_back(static_cast<int16_t>(SawSample(i, true)));
    }

    const WaveSoundNote high = WaveSoundNoteOf(*archive, kHighSound);
    const WaveSoundNote panned = WaveSoundNoteOf(*archive, kPannedSound);
    const WaveSoundNote plain = WaveSoundNoteOf(*archive, kPlainSound);
    const std::vector<uint8_t> high_file = WaveSoundAsWav(*archive, kHighSound);
    const std::vector<uint8_t> panned_file = WaveSoundAsWav(*archive, kPannedSound);
    const std::vector<uint8_t> plain_file = WaveSoundAsWav(*archive, kPlainSound);
    WavePreview preview(DecodeWaveSound(*archive, kHighSound));

    const auto error = [&archive](uint32_t sound)
    {
        try
        {
            WaveSoundAsWav(*archive, sound);
        }
        catch (const std::runtime_error& thrown)
        {
            return std::string(thrown.what());
        }

        return std::string();
    };

    SEQLIB_CHECK(high.wave_archive == 1 && high.wave == 0 && high.pitch == 1.5f);
    SEQLIB_CHECK(high.panned && high.shaped && high.events == 1);
    SEQLIB_CHECK(panned.wave_archive == 1 && panned.wave == 1 && panned.pitch == 0.5f);
    SEQLIB_CHECK(panned.panned && panned.shaped && panned.events == 3);
    SEQLIB_CHECK(!plain.panned && !plain.shaped && plain.events == 1);
    SEQLIB_CHECK(high_file == seqlib::WavFile(stereo, 72000));
    SEQLIB_CHECK(panned_file == seqlib::WavFile(saw, 24000));
    SEQLIB_CHECK(plain_file == seqlib::WavFile(saw, 48001));
    SEQLIB_CHECK(WaveSoundAsWav(*wii_u, kHighSound) == high_file);
    SEQLIB_CHECK_EQ(preview.SampleRate(), 72000);
    SEQLIB_CHECK(WaveSoundWavFileName(*archive, kHighSound) == "WSD_HIGH.wav");
    SEQLIB_CHECK(SoundsSharingFile(*archive, kHighSound) ==
                 std::vector<uint32_t>({kPannedSound, kSilentSound, kPastSound, kLostSound, kPlainSound, kNoNoteSound,
                                        kNoWaveSound, kBackwardsSound}));
    SEQLIB_CHECK(error(kSilentSound) == "WSD_SILENT plays no note.");
    SEQLIB_CHECK(error(kPastSound) == "The wave sound file of WSD_PAST lacks wave sound 8.");
    SEQLIB_CHECK(error(kLostSound) == "The archive lacks the wave archive of the wave that WSD_LOST plays.");
    SEQLIB_CHECK(error(kNoNoteSound) == "WSD_NO_NOTE plays no note.");
    SEQLIB_CHECK(error(kNoWaveSound) == "The wave sound file of WSD_NO_WAVE lacks the wave that the sound plays.");
    SEQLIB_CHECK(error(kBackwardsSound) == "WSD_BACKWARDS plays its wave at a pitch of -1. No sample rate holds that.");
    SEQLIB_CHECK(error(kNoteSound) == "The archive lacks wave sound 1.");
}

// The test archive with a wave sound file of two sounds, after the four sequence sounds. Each sound plays note 0,
// whatever its note events name: the consoles read none of the events (ntdlib's WaveSoundPlayer). Note 0 holds the saw
// and a pitch of 0.5, and note 1 the stereo wave and a pitch of 2. The consoles read neither pitch. The WAV export
// takes note 0's, and the rate of 24 kHz follows the export. WSD_LATER's first note event names note 1. WSD_UNPLAYED
// holds a track without note events.
sar_files::ArchiveSpec NoteZeroArchiveSpec(bool big_endian)
{
    sar_files::Region zero;
    zero.pitch = 0.5f;
    sar_files::Region other;
    other.wave = 1;
    other.pitch = 2.0f;
    sar_files::WaveSoundSpec later;
    later.tracks = {{{0.0f, 0.0f, 1}, {0.5f, 0.0f, 0}}};
    later.notes = {zero, other};
    sar_files::WaveSoundSpec unplayed;
    unplayed.tracks = {{}};
    unplayed.notes = {zero, other};
    sar_files::WaveSoundFileSpec file;
    file.waves = {{0x05000001, 1}, {0x05000001, 0}};
    file.sounds = {later, unplayed};

    sar_files::ArchiveSpec spec = TestArchiveSpec(big_endian);
    const std::array<const char*, 2> kSounds = {"WSD_LATER", "WSD_UNPLAYED"};
    for (std::size_t i = 0; i < kSounds.size(); i++)
    {
        sar_files::SoundSpec sound;
        sound.name = kSounds[i];
        sound.kind = sar_files::SoundSpec::kWaveSound;
        sound.file = 6;
        sound.wave_sound_index = static_cast<uint32_t>(i);
        spec.sounds.push_back(sound);
    }
    const std::vector<std::vector<uint8_t>> waves = TestWaveFiles(big_endian);
    spec.wave_archives.push_back({"WARC_SECOND", 7});
    spec.files.push_back({sar_files::WaveSoundFile(big_endian, file), "", false});
    spec.files.push_back({sar_files::WaveArchive(big_endian, {waves[1], waves[0]}), "", false});

    return spec;
}

// A wave sound's WAV file holds the wave of its note 0 at the sound's pitch times the note's (see TestWaveSoundAsWav).
// A note event that names another note, and a sound without note events, change nothing. The Wii U's wave sounds give
// the same files.
void TestWaveSoundNoteZero()
{
    const TemporaryFile switch_file(sar_files::Archive(false, NoteZeroArchiveSpec(false)));
    const TemporaryFile wii_u_file(sar_files::Archive(true, NoteZeroArchiveSpec(true)));
    const std::shared_ptr<const sar::SoundArchive> archive = OpenArchive(switch_file.Path());
    const std::shared_ptr<const sar::SoundArchive> wii_u = OpenArchive(wii_u_file.Path());
    seqlib::Pcm saw;
    saw.loop = true;
    saw.loop_end = 480;
    saw.channels.resize(1);
    for (int i = 0; i < 480; i++)
    {
        saw.channels[0].push_back(static_cast<int16_t>(SawSample(i, false)));
    }

    const WaveSoundNote later = WaveSoundNoteOf(*archive, 4);
    const WaveSoundNote unplayed = WaveSoundNoteOf(*archive, 5);
    const std::vector<uint8_t> later_file = WaveSoundAsWav(*archive, 4);
    const std::vector<uint8_t> unplayed_file = WaveSoundAsWav(*archive, 5);

    SEQLIB_CHECK(later.wave_archive == 1 && later.wave == 1 && later.pitch == 0.5f && later.events == 2);
    SEQLIB_CHECK(unplayed.wave_archive == 1 && unplayed.wave == 1 && unplayed.pitch == 0.5f && unplayed.events == 0);
    SEQLIB_CHECK(later_file == seqlib::WavFile(saw, 24000));
    SEQLIB_CHECK(unplayed_file == seqlib::WavFile(saw, 24000));
    SEQLIB_CHECK(WaveSoundAsWav(*wii_u, 4) == later_file);
    SEQLIB_CHECK(WaveSoundAsWav(*wii_u, 5) == unplayed_file);
}

// A sequence file can hold the sequences of several sounds. With SEQ_NOTE on SEQ_LOOP's file, each of the two shares
// the file with the other, and both go out as the same bytes. SEQ_PAN shares its file with no sound.
void TestSharedSequenceFiles()
{
    sar_files::ArchiveSpec spec = TestArchiveSpec();
    spec.sounds[kNoteSound].file = 0;
    const TemporaryFile file(sar_files::Archive(false, spec));
    const std::shared_ptr<const sar::SoundArchive> archive = OpenArchive(file.Path());

    const std::vector<uint32_t> loop = SoundsSharingFile(*archive, kLoopSound);
    const std::vector<uint32_t> note = SoundsSharingFile(*archive, kNoteSound);
    const std::span<const uint8_t> loop_file = ItemFile(*archive, {ItemKind::kSequence, kLoopSound, 0});
    const std::span<const uint8_t> note_file = ItemFile(*archive, {ItemKind::kSequence, kNoteSound, 0});

    SEQLIB_CHECK(loop == std::vector<uint32_t>({kNoteSound}));
    SEQLIB_CHECK(note == std::vector<uint32_t>({kLoopSound}));
    SEQLIB_CHECK(SoundsSharingFile(*archive, kPanSound).empty());
    SEQLIB_CHECK(loop_file.data() == note_file.data() && loop_file.size() == note_file.size());
}

// A sequence file goes to the other console with the same commands in that console's byte order. A file for the Wii U
// takes version 1.0.0. ntdlib reads it as a file of that console. Converted back, it gives the archive's bytes. Bytes
// that aren't a sequence file stop the conversion with a message.
void TestOtherConsoleSequenceFile()
{
    sar_files::ArchiveSpec spec = TestArchiveSpec();
    spec.sounds[kNoteSound].file = 0;
    const TemporaryFile file(sar_files::Archive(false, spec));
    const std::shared_ptr<const sar::SoundArchive> archive = OpenArchive(file.Path());
    const std::span<const uint8_t> bytes = ItemFile(*archive, {ItemKind::kSequence, kLoopSound, 0});
    const std::vector<uint32_t> starts = {archive->Sounds()[kLoopSound].start_offset,
                                          archive->Sounds()[kNoteSound].start_offset};
    sar::Options options;
    options.flavor = sar::Flavor::kCafe;

    const std::vector<uint8_t> wii_u = OtherConsoleSequenceFile(*archive, kLoopSound, sar::Flavor::kCafe).file;
    const sar::Sequence parsed = sar::Sequence::Parse(wii_u, options);
    const std::span<const uint8_t> commands = parsed.data.first(std::min(parsed.data.size(), kWiiULoopBytecode.size()));
    const std::vector<uint8_t> back = ConvertSequenceFile(wii_u, sar::Flavor::kCafe, sar::Flavor::kNx, starts).file;
    const bool refused = ThrowsRuntimeError(
        [] { ConvertSequenceFile(std::vector<uint8_t>(64), sar::Flavor::kNx, sar::Flavor::kCafe, {}); });

    SEQLIB_CHECK(OtherConsole(sar::Flavor::kNx) == sar::Flavor::kCafe);
    SEQLIB_CHECK(OtherConsole(sar::Flavor::kCafe) == sar::Flavor::kNx);
    SEQLIB_CHECK(wii_u.size() == bytes.size() && wii_u[4] == 0xFE && wii_u[5] == 0xFF && parsed.big_endian);
    SEQLIB_CHECK(wii_u[8] == 0x00 && wii_u[9] == 0x01 && wii_u[10] == 0x00 && wii_u[11] == 0x00);
    SEQLIB_CHECK(std::equal(commands.begin(), commands.end(), kWiiULoopBytecode.begin(), kWiiULoopBytecode.end()));
    SEQLIB_CHECK(std::equal(back.begin(), back.end(), bytes.begin(), bytes.end()));
    SEQLIB_CHECK(refused);
}

// A bank, a wave sound file, a wave archive and a wave go to the other console with the same structures in that
// console's byte order. The bank is the one that the test builder writes for the Wii U, and so is the wave sound file
// but for its version, 1.0.0. Converted back with the Switch's versions, they give the archive's bytes. The wave
// archive takes version 1.0.0 and its waves 1.1.0. Each wave decodes to the Wii U's samples, with the same rate and
// loop. Bytes of another kind or of another console stop the conversion with a message, and a sequence file isn't
// converted here.
void TestOtherConsoleFiles()
{
    const sar_files::ArchiveSpec wii_u_spec = WaveSoundArchiveSpec(true);
    const TemporaryFile file(sar_files::Archive(false, WaveSoundArchiveSpec(false)));
    const std::shared_ptr<const sar::SoundArchive> archive = OpenArchive(file.Path());
    const std::span<const uint8_t> bank = ItemFile(*archive, {ItemKind::kBank, 0, 0});
    const std::span<const uint8_t> sounds = ItemFile(*archive, {ItemKind::kWaveSound, kHighSound, 0});
    const std::vector<std::vector<uint8_t>> wii_u_waves = TestWaveFiles(true);
    std::vector<uint8_t> older_sounds = wii_u_spec.files[6].bytes;
    xsflib::StoreBe32(&older_sounds[0x08], 0x00010000);
    sar::Options options;
    options.flavor = sar::Flavor::kCafe;
    options.rules = sar::Rules::kStopAtFirstProblem;

    const auto version = [](std::span<const uint8_t> bytes)
    {
        return xsflib::Reader(bytes, true).U32(0x08);
    };

    const auto same_wave = [&options](std::span<const uint8_t> ours, std::span<const uint8_t> theirs)
    {
        const sar::Wave a = sar::Wave::Parse(ours, options);
        const sar::Wave b = sar::Wave::Parse(theirs, options);
        const sar::Pcm x = sar::DecodeWave(ours, options);
        const sar::Pcm y = sar::DecodeWave(theirs, options);
        return x.channels == y.channels && x.sample_rate == y.sample_rate && a.loop == b.loop &&
               a.loop_start == b.loop_start && a.loop_end == b.loop_end && a.encoding == b.encoding;
    };

    const sar::Flavor kWiiU = sar::Flavor::kCafe;
    const sar::Flavor kSwitch = sar::Flavor::kNx;
    const std::vector<uint8_t> wii_u_bank = OtherConsoleFile(*archive, {ItemKind::kBank, 0, 0}, kWiiU);
    const std::vector<uint8_t> wii_u_sounds = OtherConsoleFile(*archive, {ItemKind::kWaveSound, kHighSound, 0}, kWiiU);
    const std::vector<uint8_t> wii_u_archive = OtherConsoleFile(*archive, {ItemKind::kWaveArchive, 0, 0}, kWiiU);
    const std::vector<uint8_t> wii_u_wave = OtherConsoleFile(*archive, {ItemKind::kWave, 0, 1}, kWiiU);
    const std::vector<uint8_t> bank_back =
        ConvertItemFile(wii_u_bank, ItemKind::kBank, kWiiU, kSwitch, ExportVersions(ItemKind::kBank, kSwitch));
    const std::vector<uint8_t> sounds_back = ConvertItemFile(wii_u_sounds, ItemKind::kWaveSound, kWiiU, kSwitch,
                                                             ExportVersions(ItemKind::kWaveSound, kSwitch));
    const std::vector<std::span<const uint8_t>> waves = sar::WaveArchive::Parse(wii_u_archive, options).waves;
    const bool other_kind =
        ThrowsRuntimeError([&] { ConvertItemFile(bank, ItemKind::kWaveSound, kSwitch, kWiiU, FileVersions{}); });
    const bool other_console =
        ThrowsRuntimeError([&] { ConvertItemFile(bank, ItemKind::kBank, kWiiU, kSwitch, FileVersions{}); });
    bool sequence = false;
    try
    {
        ConvertItemFile(bank, ItemKind::kSequence, kSwitch, kWiiU, FileVersions{});
    }
    catch (const std::invalid_argument&)
    {
        sequence = true;
    }

    SEQLIB_CHECK(wii_u_bank == wii_u_spec.files[2].bytes);
    SEQLIB_CHECK(wii_u_sounds == older_sounds);
    SEQLIB_CHECK(std::equal(bank_back.begin(), bank_back.end(), bank.begin(), bank.end()));
    SEQLIB_CHECK(std::equal(sounds_back.begin(), sounds_back.end(), sounds.begin(), sounds.end()));
    SEQLIB_CHECK(version(wii_u_archive) == 0x00010000 && waves.size() == 2);
    for (std::size_t i = 0; i < waves.size() && i < wii_u_waves.size(); i++)
    {
        SEQLIB_CHECK(version(waves[i]) == 0x00010100 && same_wave(waves[i], wii_u_waves[i]));
    }
    SEQLIB_CHECK(version(wii_u_wave) == 0x00010100 && same_wave(wii_u_wave, wii_u_waves[1]));
    SEQLIB_CHECK(other_kind && other_console && sequence);
}

// Whether two conversions of a bank hold the same files, samples and warnings.
bool SameBankFiles(const export_::BankConversion& a, const export_::BankConversion& b)
{
    if (a.sound_fonts.size() != b.sound_fonts.size() || a.warnings != b.warnings ||
        a.sfz.files.size() != b.sfz.files.size() || a.sfz.samples.size() != b.sfz.samples.size())
    {
        return false;
    }

    for (std::size_t i = 0; i < a.sound_fonts.size(); i++)
    {
        if (a.sound_fonts[i].sf2 != b.sound_fonts[i].sf2 || a.sound_fonts[i].first != b.sound_fonts[i].first ||
            a.sound_fonts[i].last != b.sound_fonts[i].last)
        {
            return false;
        }
    }

    for (std::size_t i = 0; i < a.sfz.files.size(); i++)
    {
        if (a.sfz.files[i].name != b.sfz.files[i].name || a.sfz.files[i].text != b.sfz.files[i].text)
        {
            return false;
        }
    }

    for (std::size_t i = 0; i < a.sfz.samples.size(); i++)
    {
        if (a.sfz.samples[i].name != b.sfz.samples[i].name || a.sfz.samples[i].wav != b.sfz.samples[i].wav)
        {
            return false;
        }
    }

    return true;
}

// A bank's instruments go out as the files that ntdlib's converter makes of the bank with the settings of the archive's
// console: a SoundFont with a preset for each of programs 0, 1 and 3, and an SFZ file for each, named after the bank
// and the program. The Wii U's bank converts with the Wii U's settings.
void TestBankExport()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const std::shared_ptr<const sar::SoundArchive> wii_u = OpenWiiUArchive();

    const export_::BankConversion bank = ConvertBank(*archive, 0, "Hokuto");
    const export_::BankConversion cafe = ConvertBank(*wii_u, 0, "Hokuto");

    SEQLIB_CHECK(SameBankFiles(bank, export_::ConvertBank(*archive, 0, export_::SwitchSettings(), "Hokuto")));
    SEQLIB_CHECK(SameBankFiles(cafe, export_::ConvertBank(*wii_u, 0, export_::CafeSettings(), "Hokuto")));
    SEQLIB_REQUIRE(bank.sound_fonts.size() == 1 && bank.sfz.files.size() == 3);
    SEQLIB_CHECK(bank.warnings.empty() && seqlib::sf2::Read(bank.sound_fonts[0].sf2).Count("phdr", 38) == 4);
    SEQLIB_CHECK(bank.sfz.files[0].name == "BANK_TEST_0.sfz" && bank.sfz.files[1].name == "BANK_TEST_1.sfz" &&
                 bank.sfz.files[2].name == "BANK_TEST_3.sfz");
}

// A bank's SoundFont takes the export's name. SoundFonts of runs of programs take the name and their programs, with the
// digits of the last program.
void TestSoundFontNames()
{
    export_::BankConversion whole;
    whole.sound_fonts.resize(1);
    export_::BankConversion runs;
    runs.sound_fonts.resize(3);
    runs.sound_fonts[0].last = 48;
    runs.sound_fonts[1].first = 52;
    runs.sound_fonts[1].last = 99;
    runs.sound_fonts[2].first = 100;
    runs.sound_fonts[2].last = 130;

    const std::vector<std::string> one = SoundFontNames(whole, "BANK_GM");
    const std::vector<std::string> three = SoundFontNames(runs, "BANK_GM");

    SEQLIB_CHECK(one == std::vector<std::string>({"BANK_GM.sf2"}));
    SEQLIB_CHECK(three ==
                 std::vector<std::string>({"BANK_GM_000-048.sf2", "BANK_GM_052-099.sf2", "BANK_GM_100-130.sf2"}));
}

// A bank that the archive lacks stops the export with a message, and so do a bank without its file and a bank without
// instruments.
void TestBankExportErrors()
{
    sar_files::ArchiveSpec spec = TestArchiveSpec();
    sar_files::BankSpec empty;
    empty.waves = {{0x05000000, 0}};
    empty.instruments = {std::nullopt};
    spec.banks.push_back({"BANK_EMPTY", 6});
    spec.banks.push_back({"BANK_OUTSIDE", 7});
    spec.files.push_back({sar_files::Bank(false, empty), "", false});
    spec.files.push_back({{}, "", true});
    const TemporaryFile file(sar_files::Archive(false, spec));
    const std::shared_ptr<const sar::SoundArchive> archive = OpenArchive(file.Path());
    const auto error = [&archive](uint32_t bank)
    {
        try
        {
            ConvertBank(*archive, bank, "Hokuto");
        }
        catch (const std::runtime_error& thrown)
        {
            return std::string(thrown.what());
        }

        return std::string();
    };

    const std::string without_instruments = error(1);
    const std::string without_file = error(2);
    const std::string missing = error(3);

    SEQLIB_CHECK(without_instruments == "BANK_EMPTY holds no instruments.");
    SEQLIB_CHECK(without_file == "The archive lacks the file of BANK_OUTSIDE.");
    SEQLIB_CHECK(missing == "The archive lacks bank 3.");
}

// A bank's SFZ files go into a folder with the bank's name, and their samples into the folder samples beside it. The
// files that the bank's folder holds already wait for the user's choice. A sample there with other bytes stops the
// export before it writes anything, and a sample with the same bytes stays.
void TestBankSfzLayout()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const export_::BankConversion bank = ConvertBank(*archive, 0, "Hokuto");
    SEQLIB_REQUIRE(!bank.sfz.samples.empty());
    const TemporaryFolder folder;
    std::vector<std::string> asked;
    const auto keep = [&asked](const std::vector<std::string>& existing)
    {
        asked = existing;

        return false;
    };
    const auto replace = [](const std::vector<std::string>&)
    {
        return true;
    };
    const std::filesystem::path sample = folder.Path() / "samples" / bank.sfz.samples[0].name;
    const std::vector<uint8_t> kOther = {1, 2, 3};

    const bool first = WriteBankSfz(bank, folder.Path(), "BANK_TEST", keep);
    const std::map<std::string, std::vector<uint8_t>> written = FilesUnder(folder.Path());
    std::filesystem::remove(folder.Path() / "BANK_TEST" / bank.sfz.files[1].name);
    const bool second = WriteBankSfz(bank, folder.Path(), "BANK_TEST", keep);
    const bool third = WriteBankSfz(bank, folder.Path(), "BANK_TEST", replace);
    xsflib::WriteFile(sample, kOther);
    std::filesystem::remove_all(folder.Path() / "BANK_TEST");
    const bool stopped = ThrowsRuntimeError([&] { WriteBankSfz(bank, folder.Path(), "BANK_TEST", replace); });

    SEQLIB_CHECK(first && !second && third && stopped);
    SEQLIB_CHECK(written.size() == bank.sfz.files.size() + bank.sfz.samples.size());
    SEQLIB_CHECK(written.contains("BANK_TEST/" + bank.sfz.files[0].name));
    SEQLIB_CHECK(written.contains("samples/" + bank.sfz.samples[0].name));
    SEQLIB_CHECK(asked == std::vector<std::string>({bank.sfz.files[0].name, bank.sfz.files[2].name}));
    SEQLIB_CHECK(!std::filesystem::exists(folder.Path() / "BANK_TEST"));
}

// The samples of a wave preview's output that differ from the saw that the test wrote into the wave. The right channel
// falls when `right_falls` is set, and rises like the left channel otherwise.
std::size_t WrongSawSamples(const std::vector<float>& samples, bool right_falls)
{
    std::size_t wrong = 0;
    for (std::size_t i = 0; i < samples.size(); i++)
    {
        const int written = SawSample(static_cast<int>(i / 2), i % 2 == 1 && right_falls);
        if (samples[i] != static_cast<float>(written) / 32768.0f)
        {
            wrong++;
        }
    }

    return wrong;
}

// A wave plays once at its rate, with the samples that the test wrote into it. A mono wave plays in both channels. A
// wave or a wave archive that the archive lacks throws.
void TestWavePreview()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    WavePreview mono(*archive, 0, 0);
    WavePreview stereo(*archive, 0, 1);

    const std::vector<float> mono_samples = RunPreview(mono, 10);
    const std::vector<float> stereo_samples = RunPreview(stereo, 10);

    SEQLIB_CHECK_EQ(mono.SampleRate(), 48000);
    SEQLIB_CHECK_EQ(mono_samples.size(), 480 * 2);
    SEQLIB_CHECK_EQ(WrongSawSamples(mono_samples, false), 0);
    SEQLIB_CHECK_EQ(stereo_samples.size(), 480 * 2);
    SEQLIB_CHECK_EQ(WrongSawSamples(stereo_samples, true), 0);
    SEQLIB_CHECK(ThrowsRuntimeError([&] { WavePreview missing(*archive, 0, 2); }));
    SEQLIB_CHECK(ThrowsRuntimeError([&] { WavePreview missing(*archive, 1, 0); }));
}

// A wave with a loop plays once without `loops`. With it, the wave plays to its loop's end, then round its loop until
// the preview stops. A wave without a loop plays once either way. The samples after the loop's end never play.
void TestLoopingWavePreview()
{
    sar::Pcm pcm;
    pcm.sample_rate = 32000;
    pcm.loop = true;
    pcm.loop_start = 3;
    pcm.loop_end = 7;
    pcm.channels = {{0, 1000, 2000, 3000, 4000, 5000, 6000, 7000}};
    sar::Pcm without = pcm;
    without.loop = false;
    WavePreview once(pcm);
    WavePreview round(pcm, true);
    WavePreview unlooped(without, true);

    const std::vector<float> once_samples = RunPreview(once, 1, 20);
    const std::vector<float> round_samples = RunPreview(round, 1, 20);
    const std::vector<float> unlooped_samples = RunPreview(unlooped, 1, 20);

    SEQLIB_CHECK_EQ(once_samples.size(), 7 * 2);
    SEQLIB_CHECK_EQ(unlooped_samples.size(), 7 * 2);
    SEQLIB_CHECK_EQ(round_samples.size(), 20 * 2);
    const std::array<int, 20> kPlayed = {0, 1, 2, 3, 4, 5, 6, 3, 4, 5, 6, 3, 4, 5, 6, 3, 4, 5, 6, 3};
    std::size_t wrong = 0;
    for (std::size_t i = 0; i < round_samples.size(); i++)
    {
        wrong += round_samples[i] != static_cast<float>(kPlayed[i / 2] * 1000) / 32768.0f ? 1 : 0;
    }
    SEQLIB_CHECK_EQ(wrong, 0);
}

// A note of the bank's instrument sounds as the loop sequence's first note: key 60 at velocity 100, held for 48 ticks.
// The frame that brings the sequence's tick count to 48 releases that note. Up to that frame, the preview's output is
// the model's output for the sequence. The preview's note holds for 1 s. The preview ends once the release has faded,
// long before its limit of 10 s.
void TestNotePreview()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const ModelRun model = RunModel(*archive, kLoopSound, 400);
    const std::size_t first = FirstFrameReaching(model, 1);
    const std::size_t held_together = FirstFrameReaching(model, 48) - first;
    NotePreview preview(archive, 0, 0);

    const std::vector<float> samples = RunPreview(preview, preview.LongestFrames() + 1);

    const std::size_t frames = samples.size() / (kFrameSamples * 2);
    SEQLIB_REQUIRE(frames > held_together && held_together > 90);
    const std::vector<float> held(samples.begin(),
                                  samples.begin() + static_cast<std::ptrdiff_t>(held_together * kFrameSamples * 2));
    SEQLIB_CHECK(HasSound(std::span<const float>(held)));
    SEQLIB_CHECK(Matches(held, model, first, 0));
    SEQLIB_CHECK(frames > preview.HoldFrames() && frames < preview.LongestFrames());
}

// A note of a sound's instrument sounds as the loop sequence's first note: key 60 at velocity 100 on program 0 of the
// bank in slot 0. Up to the frame that releases the sequence's note, the preview's output is the model's. The note
// holds until its flag lets it go, past the browser's hold of 1 s. The preview then ends once the release has faded.
// The bank in slot 1 plays the same note. An empty slot and a program without an instrument play nothing. A softer
// velocity plays more quietly, and another key plays other samples.
void TestHeldNotePreview()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const ModelRun model = RunModel(*archive, kLoopSound, 400);
    const std::size_t first = FirstFrameReaching(model, 1);
    const std::size_t held_together = FirstFrameReaching(model, 48) - first;
    const std::vector<uint32_t> banks = archive->Sounds()[kLoopSound].banks;
    const std::vector<uint32_t> in_slot_1 = {sar::kNoItem, banks.at(0)};
    const auto release = std::make_shared<std::atomic<bool>>(false);
    const auto never = std::make_shared<std::atomic<bool>>(false);
    NotePreview note(archive, banks, 0, 0, 60, 100, release);
    NotePreview second_slot(archive, in_slot_1, 1, 0, 60, 100, never);
    NotePreview empty_slot(archive, in_slot_1, 0, 0, 60, 100, never);
    NotePreview no_instrument(archive, banks, 0, 2, 60, 100, never);
    NotePreview soft(archive, banks, 0, 0, 60, 50, never);
    NotePreview higher(archive, banks, 0, 0, 72, 100, never);
    const std::size_t hold = note.HoldFrames() + 100;

    const std::vector<float> held = RunPreview(note, hold);
    release->store(true);
    const std::vector<float> after = RunPreview(note, note.LongestFrames());
    const std::vector<float> second_samples = RunPreview(second_slot, hold);
    const std::vector<float> soft_samples = RunPreview(soft, 100);
    const std::vector<float> higher_samples = RunPreview(higher, 100);

    SEQLIB_REQUIRE(held_together > 90 && held.size() == hold * kFrameSamples * 2);
    const std::vector<float> together(held.begin(),
                                      held.begin() + static_cast<std::ptrdiff_t>(held_together * kFrameSamples * 2));
    const std::span<const float> start = std::span<const float>(held).first(100 * kFrameSamples * 2);
    SEQLIB_CHECK(Matches(together, model, first, 0));
    SEQLIB_CHECK(HasSound(std::span<const float>(held).last(kFrameSamples * 2)));
    const std::size_t after_frames = after.size() / (kFrameSamples * 2);
    SEQLIB_CHECK(after_frames > 0 && hold + after_frames < note.LongestFrames());
    SEQLIB_CHECK(second_samples == held);
    SEQLIB_CHECK(RunPreview(empty_slot, 10).empty());
    SEQLIB_CHECK(RunPreview(no_instrument, 10).empty());
    SEQLIB_REQUIRE(soft_samples.size() == start.size() && higher_samples.size() == start.size());
    SEQLIB_CHECK(Peak(soft_samples) > 0.0f && Peak(soft_samples) < Peak(start));
    SEQLIB_CHECK(HasSound(std::span<const float>(higher_samples)));
    SEQLIB_CHECK(!std::equal(higher_samples.begin(), higher_samples.end(), start.begin()));
}

// A note that ignores note-off sounds until the preview's limit of 10 s. The last frame fades out to silence, from the
// level of the frame before it.
void TestNotePreviewLimit()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    NotePreview preview(archive, 0, 1);

    const std::vector<float> samples = RunPreview(preview, preview.LongestFrames() + 10);

    const std::size_t frames = samples.size() / (kFrameSamples * 2);
    SEQLIB_REQUIRE_EQ(frames, preview.LongestFrames());
    const std::span<const float> all(samples);
    const std::span<const float> before = all.subspan((frames - 2) * kFrameSamples * 2, kFrameSamples * 2);
    const std::span<const float> last = all.subspan((frames - 1) * kFrameSamples * 2, kFrameSamples * 2);
    SEQLIB_CHECK(HasSound(before) && HasSound(last.first(kFrameSamples)));
    SEQLIB_CHECK(std::abs(last[last.size() - 1]) <= 1.0f / static_cast<float>(kFrameSamples));
    SEQLIB_CHECK(std::abs(last[last.size() - 2]) <= 1.0f / static_cast<float>(kFrameSamples));
}

// The Wii U's archive holds the same bank and waves, in the Wii U's byte order. A wave plays the samples that the test
// wrote into it. A note plays on the Wii U's model, in frames of 3 ms at 48 kHz. It holds for 333 frames, sounds, and
// ends once its release has faded, before the limit of 3,333 frames. The bank in a sound's slot 1 plays the same note
// while both hold.
void TestWiiUPreviews()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenWiiUArchive();
    constexpr std::size_t kWiiUFrameSamples = cafe::ArchiveModel::kFrameSamples;
    const std::vector<uint32_t> in_slot_1 = {sar::kNoItem, archive->Sounds()[kNoteSound].banks.at(0)};
    WavePreview wave(*archive, 0, 1);
    NotePreview note(archive, 0, 0);
    NotePreview held(archive, in_slot_1, 1, 0, 60, 100, std::make_shared<std::atomic<bool>>(false));

    const std::vector<float> wave_samples = RunPreview(wave, 10);
    const std::vector<float> note_samples = RunPreview(note, note.LongestFrames() + 1, kWiiUFrameSamples);
    const std::vector<float> held_samples = RunPreview(held, 300, kWiiUFrameSamples);

    SEQLIB_CHECK(ProgramsOf(*archive, 0) == std::vector<uint32_t>({0, 1, 3}));
    SEQLIB_CHECK_EQ(WaveCountOf(*archive, 0), 2);
    SEQLIB_CHECK_EQ(wave_samples.size(), 480 * 2);
    SEQLIB_CHECK_EQ(WrongSawSamples(wave_samples, true), 0);
    SEQLIB_CHECK_EQ(note.SampleRate(), 48000);
    SEQLIB_CHECK_EQ(note.HoldFrames(), uint64_t{333});
    SEQLIB_CHECK_EQ(note.LongestFrames(), uint64_t{3333});
    const std::size_t frames = note_samples.size() / (kWiiUFrameSamples * 2);
    SEQLIB_CHECK(HasSound(std::span<const float>(note_samples)));
    SEQLIB_CHECK(frames > note.HoldFrames() && frames < note.LongestFrames());
    SEQLIB_REQUIRE_EQ(held_samples.size(), 300 * kWiiUFrameSamples * 2);
    SEQLIB_REQUIRE(note_samples.size() >= held_samples.size());
    SEQLIB_CHECK(std::equal(held_samples.begin(), held_samples.end(), note_samples.begin()));
}

// The output of ntdlib's model of the Wii run directly, as floats, with key 60 at velocity 100 on program `program` of
// the archive's bank, on `build` of NW4R SND: a note-off before frame `hold`, and frames until nothing sounds or
// `frames` have run.
std::vector<float> WiiNoteOnModel(const ntdlib::rsar::SoundArchive& archive, uint32_t program,
                                  const ntdlib::SndBuild& build, std::size_t hold, std::size_t frames)
{
    ntdlib::rvl::snd::ArchiveModel model;
    model.build = build;
    model.StartWithRenderer(archive);
    ntdlib::rvl::snd::NoteOnInfo info;
    info.prg_no = program;
    info.key = 60;
    info.velocity = 100;
    std::vector<float> samples;
    if (model.engine->NoteOn({0}, 0, info) == nullptr)
    {
        return samples;
    }

    std::vector<int16_t> output;
    for (std::size_t frame = 0; frame < frames && model.engine->IsBusy(); frame++)
    {
        if (frame == hold)
        {
            ntdlib::rvl::snd::ChannelManager& channels = model.engine->Channels();
            const std::vector<ntdlib::nwsnd::ChannelId> places(channels.InUse().begin(), channels.InUse().end());
            for (const ntdlib::nwsnd::ChannelId place : places)
            {
                channels.At(place).NoteOff(*model.engine);
            }
        }
        output.clear();
        if (!model.RunFrame(output))
        {
            break;
        }
        for (const int16_t sample : output)
        {
            samples.push_back(static_cast<float>(sample) / 32768.0f);
        }
    }

    return samples;
}

// A Wii bank's note plays on ntdlib's model of NW4R SND and AX, with the model of AX's microcode, in frames of 3 ms at
// 32 kHz. Its output is the model's output run directly with the same note: key 60 at velocity 100 on program 0, with a
// note-off after 333 frames. The preview ends with the model's output, before its limit of 3,333 frames. Here each
// region holds its level for a while after its attack, then decays, and its release ends soon. The archive's version,
// 1.4, gives the build of June 2008. That build follows the region's hold. A build of 2007 decays at once. A preview on
// that build plays as the model on that build, and otherwise than on the archive's. A note of a sound's instrument
// plays the same samples while it holds. A bank slot that the sound lacks, and a program without a sound, play nothing.
// A wave plays the samples that ntdlib decodes from it, at its rate. A mono wave plays in both channels.
void TestWiiPreviews()
{
    constexpr std::size_t kWiiFrameSamples = 96;
    std::vector<uint8_t> bytes = WiiArchiveBytes();
    constexpr std::array<uint8_t, 4> kTune = {0x3F, 0x80, 0x00, 0x00};
    std::size_t regions = 0;
    for (std::size_t at = 0; at + 0x14 <= bytes.size(); at++)
    {
        // A region: its wave's index, then its attack of 127 at byte 4, its volume of 100 at byte 0xD, its pan of 64 at
        // byte 0xE, and at byte 0x10 its tuning of 1.0. The decay lies at byte 5, the release at byte 7 and the hold at
        // byte 8.
        if (bytes[at + 4] == 127 && bytes[at + 0xD] == 100 && bytes[at + 0xE] == 64 &&
            std::memcmp(&bytes[at + 0x10], kTune.data(), kTune.size()) == 0)
        {
            bytes[at + 5] = 120;
            bytes[at + 7] = 120;
            bytes[at + 8] = 100;
            regions++;
        }
    }
    const std::shared_ptr<const ntdlib::rsar::SoundArchive> archive =
        LoadWiiArchive(std::move(bytes), "SOUND.brsar", ntdlib::rvl::snd::kRsarOptions);
    const ntdlib::SndBuild june_2008 = export_::BuildOfRsarVersion(archive->Version());
    const ntdlib::SndBuild of_2007 = export_::BuildOfRsarVersion(0x0103);
    const auto never = std::make_shared<std::atomic<bool>>(false);
    NotePreview note(archive, 0, 0);
    NotePreview older(archive, 0, 0, of_2007);
    NotePreview held(archive, std::vector<uint32_t>{0}, 0, 0, 60, 100, never);
    NotePreview no_slot(archive, std::vector<uint32_t>{0}, 1, 0, 60, 100, never);
    NotePreview silent(archive, 0, 3);
    const uint32_t bank_file = archive->Banks().at(0).file_id;
    const ntdlib::rsar::Pcm pcm = hokuto::song::DecodeItemWave(*archive, bank_file, 1);
    WavePreview wave(pcm);

    const std::vector<float> note_samples = RunPreview(note, note.LongestFrames() + 1, kWiiFrameSamples);
    const std::vector<float> older_samples = RunPreview(older, older.LongestFrames() + 1, kWiiFrameSamples);
    const std::vector<float> held_samples = RunPreview(held, 300, kWiiFrameSamples);
    const std::vector<float> expected = WiiNoteOnModel(*archive, 0, june_2008, 333, 3333);
    const std::vector<float> expected_older = WiiNoteOnModel(*archive, 0, of_2007, 333, 3333);
    const std::vector<float> wave_samples = RunPreview(wave, 1, pcm.Length() + 10);

    SEQLIB_CHECK_EQ(regions, 8);
    SEQLIB_CHECK(june_2008.SinceMarch2008() && !of_2007.SinceMarch2008());
    SEQLIB_CHECK_EQ(note.SampleRate(), 32000);
    SEQLIB_CHECK_EQ(note.HoldFrames(), uint64_t{333});
    SEQLIB_CHECK_EQ(note.LongestFrames(), uint64_t{3333});
    const std::size_t frames = note_samples.size() / (kWiiFrameSamples * 2);
    SEQLIB_CHECK(frames > note.HoldFrames() && frames < note.LongestFrames());
    SEQLIB_CHECK(HasSound(std::span<const float>(note_samples)));
    SEQLIB_CHECK(note_samples == expected);
    SEQLIB_CHECK(older_samples == expected_older);
    SEQLIB_CHECK(older_samples != note_samples);
    SEQLIB_REQUIRE_EQ(held_samples.size(), 300 * kWiiFrameSamples * 2);
    SEQLIB_REQUIRE(note_samples.size() >= held_samples.size());
    SEQLIB_CHECK(std::equal(held_samples.begin(), held_samples.end(), note_samples.begin()));
    SEQLIB_CHECK(RunPreview(no_slot, 10, kWiiFrameSamples).empty());
    SEQLIB_CHECK(RunPreview(silent, 10, kWiiFrameSamples).empty());
    const std::vector<uint32_t> programs = ProgramsOf(*archive, 0);
    SEQLIB_CHECK(!programs.empty() && programs.front() == 0);
    SEQLIB_CHECK(std::ranges::find(programs, 3u) == programs.end());
    SEQLIB_CHECK_EQ(wave.SampleRate(), 32000);
    SEQLIB_REQUIRE(pcm.channels.size() == 1 && wave_samples.size() == pcm.Length() * 2);
    std::size_t wrong = 0;
    for (std::size_t i = 0; i < wave_samples.size(); i++)
    {
        wrong += wave_samples[i] != static_cast<float>(pcm.channels[0][i / 2]) / 32768.0f ? 1 : 0;
    }
    SEQLIB_CHECK_EQ(wrong, 0);
    SEQLIB_CHECK(HasSound(std::span<const float>(wave_samples)));
}

// The ramp of `count` samples of a Wii wave that WiiRampWave makes: 200 steps of 100 from -10,000, again and again.
std::vector<int16_t> WiiRamp(uint32_t count)
{
    std::vector<int16_t> samples(count);
    for (uint32_t i = 0; i < count; i++)
    {
        samples[i] = static_cast<int16_t>(static_cast<int>(i % 200) * 100 - 10000);
    }

    return samples;
}

// An RWAV of WiiRamp's `count` samples, mono 16-bit PCM at 32 kHz. A wave that loops does so from its start.
std::vector<uint8_t> WiiRampWave(uint32_t count, bool loop)
{
    return ntdlib::testing::rsar_files::MakePcm16Rwav(count, WiiRamp(count), 1, loop);
}

// The output of ntdlib's model of the Wii run directly, as floats, with wave sound `sound` started on `build` of NW4R
// SND: frames until nothing sounds or `frames` have run.
std::vector<float> WiiWaveSoundOnModel(const ntdlib::rsar::SoundArchive& archive, uint32_t sound,
                                       const ntdlib::SndBuild& build, std::size_t frames)
{
    ntdlib::rvl::snd::ArchiveModel model;
    model.build = build;
    model.StartWithRenderer(archive);
    std::vector<float> samples;
    if (model.engine->StartWaveSound(sound) == nullptr)
    {
        return samples;
    }

    std::vector<int16_t> output;
    for (std::size_t frame = 0; frame < frames && model.engine->IsBusy(); frame++)
    {
        output.clear();
        if (!model.RunFrame(output))
        {
            break;
        }
        for (const int16_t sample : output)
        {
            samples.push_back(static_cast<float>(sample) / 32768.0f);
        }
    }

    return samples;
}

// The Wii's wave sounds, in archives of wave sounds that ntdlib's testing/rsar_files.h builds. The wave data of a wave
// sound file lists as a wave archive under the file's name: an RWAR from version 1.3 on, and before it the samples that
// the file's WAVE block describes, in DSP-ADPCM here. Only an RWAR goes out as a file. NW4R SND plays note 0 of a wave
// sound at the sound's pitch. The note's key, pan and pitch stay unread. A wave sound's WAV file holds the ramp that
// its note's wave holds, at the rate that the sound's pitch gives, with the wave's loop. The browser's preview plays a
// wave sound as ntdlib's model of NW4R SND and AX run directly, sample for sample, on the build that the archive's
// version gives. A wave that doesn't loop ends the preview, long before its limit. A wave that loops plays on. A wave
// sound without a note, one whose note's wave the file lacks and a sound past the table throw with a message for the
// user.
void TestWiiWaveSounds()
{
    namespace rsar_files = ntdlib::testing::rsar_files;
    constexpr std::size_t kWiiFrameSamples = 96;

    // WSD_0 plays wave 0 at a pitch of 1.5, away from the centre, through a note whose key, pan and pitch the player
    // leaves unread. WSD_1 plays the looped wave 1. WSD_2's note plays wave 5, and the file lacks that wave. WSD_3 has
    // no note.
    rsar_files::WaveSoundNote unread;
    unread.original_key = 62;
    unread.pan = 70;
    unread.pitch = 0.5f;
    rsar_files::WaveSoundSpec high;
    high.pitch = 1.5f;
    high.pan = 32;
    high.notes = {unread};
    rsar_files::WaveSoundSpec looped;
    looped.notes = {rsar_files::WaveSoundNote{.wave = 1}};
    rsar_files::WaveSoundSpec lost;
    lost.notes = {rsar_files::WaveSoundNote{.wave = 5}};
    const std::vector<rsar_files::WaveSoundSpec> specs = {high, looped, lost, rsar_files::WaveSoundSpec{}};
    std::vector<rsar_files::ArchiveWaveSound> entries(specs.size());
    for (std::size_t i = 0; i < entries.size(); i++)
    {
        entries[i].index = static_cast<int32_t>(i);
    }
    const std::vector<uint8_t> rwar = rsar_files::MakeRwarOf({WiiRampWave(9600, false), WiiRampWave(960, true)});

    // The samples of the file of version 1.2: two waves of 0x40 bytes of DSP-ADPCM. Each starts with its predictor and
    // scale. Each later frame of 8 bytes starts with predictor 0 and scale 11, and its nibbles hold 7 and -7 in turn.
    std::vector<uint8_t> old_waves = rsar_files::MakeOldWaveData();
    for (std::size_t i = 1; i < old_waves.size(); i++)
    {
        if (i % 0x40 != 0)
        {
            old_waves[i] = i % 8 == 0 ? 0x0B : 0x79;
        }
    }
    const std::shared_ptr<const ntdlib::rsar::SoundArchive> archive =
        LoadWiiArchive(rsar_files::MakeWaveSoundArchive(rsar_files::MakeWaveSoundFile(0x0103, specs), rwar, entries),
                       "SOUND.brsar", ntdlib::rvl::snd::kRsarOptions);
    const std::shared_ptr<const ntdlib::rsar::SoundArchive> old_archive = LoadWiiArchive(
        rsar_files::MakeWaveSoundArchive(rsar_files::MakeWaveSoundFile(0x0102, specs), old_waves, entries), "OLD.brsar",
        ntdlib::rvl::snd::kRsarOptions);
    const ntdlib::SndBuild build = export_::BuildOfRsarVersion(archive->Version());

    const std::vector<WiiWaveArchive> wave_archives = WaveArchivesOf(*archive);
    const std::vector<WiiWaveArchive> old_wave_archives = WaveArchivesOf(*old_archive);

    SEQLIB_REQUIRE(wave_archives.size() == 1 && old_wave_archives.size() == 1);
    SEQLIB_CHECK(wave_archives[0].file_id == 0 && wave_archives[0].rwar && wave_archives[0].name == "WSD_FILE_0");
    SEQLIB_CHECK(old_wave_archives[0].file_id == 0 && !old_wave_archives[0].rwar);
    SEQLIB_CHECK_EQ(WaveCountOf(*archive, 0), 2);
    SEQLIB_CHECK_EQ(WaveCountOf(*old_archive, 0), 2);
    const std::span<const uint8_t> rwar_file = ItemFile(*archive, {ItemKind::kWaveArchive, 0, 0});
    SEQLIB_CHECK(std::ranges::equal(rwar_file, rwar));
    SEQLIB_CHECK(ThrowsRuntimeError([&] { ItemFile(*old_archive, {ItemKind::kWaveArchive, 0, 0}); }));
    SEQLIB_CHECK(hokuto::song::DecodeItemWave(*old_archive, 0, 1).Length() > 0);

    const WaveSoundNote note = WaveSoundNoteOf(*archive, 0);
    const WaveSoundNote plain = WaveSoundNoteOf(*archive, 1);

    SEQLIB_CHECK(note.wave_archive == 0 && note.wave == 0 && note.pitch == 1.5f);
    SEQLIB_CHECK(note.panned && !note.shaped && note.events == 0);
    SEQLIB_CHECK(plain.wave_archive == 0 && plain.wave == 1 && plain.pitch == 1.0f && !plain.panned);

    // Each WAV file holds its wave at the wave's rate times the sound's pitch.
    seqlib::Pcm once;
    once.channels = {WiiRamp(9600)};
    seqlib::Pcm round;
    round.loop = true;
    round.loop_end = 960;
    round.channels = {WiiRamp(960)};

    SEQLIB_CHECK(WaveSoundAsWav(*archive, 0) == seqlib::WavFile(once, 48000));
    SEQLIB_CHECK(WaveSoundAsWav(*archive, 1) == seqlib::WavFile(round, 32000));
    SEQLIB_CHECK(DecodeWaveSound(*archive, 0).sample_rate == 48000);
    SEQLIB_CHECK(WaveSoundWavFileName(*archive, 0) == "WSD_0.wav");
    SEQLIB_CHECK(
        DecodeWaveSound(*old_archive, 0).sample_rate ==
        static_cast<uint32_t>(std::lround(hokuto::song::DecodeItemWave(*old_archive, 0, 0).sample_rate * 1.5)));

    const auto error = [&archive](uint32_t sound)
    {
        try
        {
            WaveSoundNoteOf(*archive, sound);
        }
        catch (const std::runtime_error& thrown)
        {
            return std::string(thrown.what());
        }

        return std::string();
    };
    SEQLIB_CHECK(error(2) == "The wave sound file of WSD_2 lacks the wave that the sound plays.");
    SEQLIB_CHECK(error(3) == "WSD_3 plays no note.");
    SEQLIB_CHECK(error(4) == "The archive lacks wave sound 4.");

    // The previews against the model. 9,600 samples at a pitch of 1.5 take 6,400 samples of the output: about 67
    // frames.
    SoundPreview once_preview(archive, 0);
    SoundPreview round_preview(archive, 1);
    SoundPreview old_preview(old_archive, 0);

    const std::vector<float> once_samples =
        RunPreview(once_preview, once_preview.LongestFrames() + 1, kWiiFrameSamples);
    const std::vector<float> round_samples = RunPreview(round_preview, 200, kWiiFrameSamples);
    const std::vector<float> old_samples = RunPreview(old_preview, 200, kWiiFrameSamples);

    SEQLIB_CHECK_EQ(once_preview.SampleRate(), 32000);
    SEQLIB_CHECK_EQ(once_preview.LongestFrames(), uint64_t{200000});
    SEQLIB_CHECK(HasSound(std::span<const float>(once_samples)));
    SEQLIB_CHECK(once_samples == WiiWaveSoundOnModel(*archive, 0, build, 1000));
    const std::size_t frames = once_samples.size() / (kWiiFrameSamples * 2);
    SEQLIB_CHECK(frames >= 67 && frames < 75);
    SEQLIB_REQUIRE_EQ(round_samples.size(), 200 * kWiiFrameSamples * 2);
    SEQLIB_CHECK(round_samples == WiiWaveSoundOnModel(*archive, 1, build, 200));
    SEQLIB_CHECK(HasSound(std::span<const float>(round_samples).subspan(190 * kWiiFrameSamples * 2)));
    SEQLIB_CHECK(old_samples == WiiWaveSoundOnModel(*old_archive, 0, build, 200));
    SEQLIB_CHECK(HasSound(std::span<const float>(old_samples)));
    SEQLIB_CHECK(ThrowsRuntimeError([&] { SoundPreview missing(archive, 2); }));
    SEQLIB_CHECK(ThrowsRuntimeError([&] { SoundPreview silent(archive, 3); }));
    SEQLIB_CHECK(ThrowsRuntimeError([&] { SoundPreview past(archive, 4); }));
}

// The output of the model of the archive's console for `frames` frames, with wave sound `sound` started on it, as
// floats.
template <class ArchiveModel>
std::vector<float> WaveSoundOnModel(const sar::SoundArchive& archive, uint32_t sound, std::size_t frames)
{
    ArchiveModel model(archive);
    model.engine->StartWaveSound(sound);
    std::vector<float> samples;
    for (std::size_t frame = 0; frame < frames; frame++)
    {
        model.engine->RunFrame();
        std::span<const int16_t> output;
        if constexpr (std::is_same_v<ArchiveModel, cafe::ArchiveModel>)
        {
            output = model.system.Output();
        }
        else
        {
            output = model.renderer.DeviceOutput();
        }
        for (const int16_t sample : output)
        {
            samples.push_back(static_cast<float>(sample) / 32768.0f);
        }
    }

    return samples;
}

// The peaks of the left and the right samples of interleaved stereo.
std::array<float, 2> SidePeaks(const std::vector<float>& samples)
{
    std::array<float, 2> peaks = {0.0f, 0.0f};
    for (std::size_t i = 0; i < samples.size(); i++)
    {
        peaks[i % 2] = std::max(peaks[i % 2], std::abs(samples[i]));
    }

    return peaks;
}

// A wave sound plays on the console's model as ntdlib's player of wave sounds plays it: the preview's output is the
// model's, sample for sample. The wave of the sound's first note plays: WSD_HIGH's stereo saw, and WSD_PLAIN's mono saw
// at the centre. The saws loop, and the previews play on. The wave sounds without a wave to play throw with a message
// for the user, and so does a sequence sound. A wave sound at a pitch of -1 plays without a failure. The Wii U's wave
// sounds play on its model the same way.
void TestSoundPreview()
{
    const TemporaryFile switch_file(sar_files::Archive(false, WaveSoundArchiveSpec(false)));
    const TemporaryFile wii_u_file(sar_files::Archive(true, WaveSoundArchiveSpec(true)));
    const std::shared_ptr<const sar::SoundArchive> archive = OpenArchive(switch_file.Path());
    const std::shared_ptr<const sar::SoundArchive> wii_u = OpenArchive(wii_u_file.Path());
    constexpr std::size_t kWiiUFrameSamples = cafe::ArchiveModel::kFrameSamples;
    SoundPreview high(archive, kHighSound);
    SoundPreview plain(archive, kPlainSound);
    SoundPreview backwards(archive, kBackwardsSound);
    SoundPreview wii_u_high(wii_u, kHighSound);

    const std::vector<float> high_samples = RunPreview(high, 200);
    const std::vector<float> plain_samples = RunPreview(plain, 200);
    const std::vector<float> wii_u_samples = RunPreview(wii_u_high, 200, kWiiUFrameSamples);
    const std::size_t backwards_frames = RunPreview(backwards, 50).size() / (kFrameSamples * 2);

    const auto message = [](const std::shared_ptr<const sar::SoundArchive>& from, uint32_t sound)
    {
        try
        {
            const SoundPreview preview(from, sound);
        }
        catch (const std::runtime_error& error)
        {
            return std::string(error.what());
        }

        return std::string();
    };

    SEQLIB_CHECK(high.SampleRate() == 48000 && high.LongestFrames() == 120000 && wii_u_high.LongestFrames() == 200000);
    SEQLIB_REQUIRE(high_samples.size() == 200 * kFrameSamples * 2 && plain_samples.size() == high_samples.size());
    SEQLIB_CHECK(high_samples == WaveSoundOnModel<atk::ArchiveModel>(*archive, kHighSound, 200));
    SEQLIB_CHECK(plain_samples == WaveSoundOnModel<atk::ArchiveModel>(*archive, kPlainSound, 200));
    SEQLIB_CHECK(wii_u_samples == WaveSoundOnModel<cafe::ArchiveModel>(*wii_u, kHighSound, 200));
    const std::array<float, 2> high_peaks = SidePeaks(high_samples);
    const std::array<float, 2> plain_peaks = SidePeaks(plain_samples);
    SEQLIB_CHECK(high_peaks[0] > 0.0f && high_peaks[1] > 0.0f);
    SEQLIB_CHECK(plain_peaks[0] > 0.0f && plain_peaks[0] == plain_peaks[1]);
    SEQLIB_CHECK(SidePeaks(wii_u_samples)[0] > 0.0f);
    SEQLIB_CHECK(backwards_frames > 0);
    SEQLIB_CHECK(message(archive, kSilentSound) == "WSD_SILENT plays no note.");
    SEQLIB_CHECK(message(archive, kPastSound) == "The wave sound file of WSD_PAST lacks wave sound 8.");
    SEQLIB_CHECK(message(archive, kLostSound) == "The archive lacks the wave archive of the wave that WSD_LOST plays.");
    SEQLIB_CHECK(message(archive, kNoNoteSound) == "WSD_NO_NOTE plays no note.");
    SEQLIB_CHECK(message(archive, kNoWaveSound) ==
                 "The wave sound file of WSD_NO_WAVE lacks the wave that the sound plays.");
    SEQLIB_CHECK(message(archive, kLoopSound) ==
                 archive->Sounds()[kLoopSound].name + " is neither a wave sound nor a stream sound.");
    SEQLIB_CHECK(message(wii_u, kSilentSound) == "WSD_SILENT plays no note.");
}

// A preview plays key 60, or the nearest key with a sound. Of two keys equally near, the lower one plays. An instrument
// without a sound plays no key.
void TestPreviewKey()
{
    SEQLIB_CHECK_EQ(NotePreview::KeyOf(InstrumentOn({{0, 127}})), 60);
    SEQLIB_CHECK_EQ(NotePreview::KeyOf(InstrumentOn({{70, 80}})), 70);
    SEQLIB_CHECK_EQ(NotePreview::KeyOf(InstrumentOn({{50, 55}, {66, 70}})), 55);
    SEQLIB_CHECK_EQ(NotePreview::KeyOf(InstrumentOn({{62, 62}, {58, 58}})), 58);
    SEQLIB_CHECK_EQ(NotePreview::KeyOf(InstrumentOn({})), -1);
}

// A sequence sound plays on the model from its first frame. The preview's output is the model's output for the sound,
// frame for frame, with the effects on the aux buses at the buses' volumes. The loop sequence sends its notes to bus A,
// and the reverb there at half volume plays otherwise than at full volume. It plays on to the preview's limit of 30 s,
// and the last frame fades out to silence. The note sequence ends once its last note has faded, long before the limit.
void TestSequencePreview()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenSendArchive(kSendLoopBytecode);
    constexpr AuxVolumes kHalf = {0.5f, 0.5f, 0.5f};
    const ModelRun model = RunModel(*archive, kSendSound, 400, AuxReverb(), {}, {}, kHalf);
    const ModelRun full = RunModel(*archive, kSendSound, 400, AuxReverb());
    const auto loop = std::make_shared<const Sequence>(ReadSequence(*archive, kSendSound));
    const auto note = std::make_shared<const Sequence>(ReadSequence(*archive, kNoteSound));
    SequencePreview preview(archive, loop, AuxEffectsWithReverb(), kHalf);
    SequencePreview ending(archive, note);

    const std::vector<float> samples = RunPreview(preview, preview.LongestFrames() + 10);
    const std::vector<float> ending_samples = RunPreview(ending, ending.LongestFrames());

    SEQLIB_CHECK_EQ(preview.SampleRate(), 48000);
    SEQLIB_CHECK_EQ(preview.LongestFrames(), uint64_t{6000});
    SEQLIB_REQUIRE_EQ(samples.size(), preview.LongestFrames() * kFrameSamples * 2);
    const std::vector<float> start(samples.begin(), samples.begin() + 400 * kFrameSamples * 2);
    SEQLIB_CHECK(HasSound(std::span<const float>(start)));
    SEQLIB_CHECK(Matches(start, model, 0, 0));
    SEQLIB_CHECK(model.samples != full.samples);
    const std::span<const float> last = std::span<const float>(samples).last(kFrameSamples * 2);
    SEQLIB_CHECK(std::abs(last[last.size() - 1]) <= 1.0f / static_cast<float>(kFrameSamples));
    const std::size_t ending_frames = ending_samples.size() / (kFrameSamples * 2);
    SEQLIB_CHECK(ending_frames > 0 && ending_frames < ending.LongestFrames());
    SEQLIB_CHECK(HasSound(std::span<const float>(ending_samples)));
}

} // namespace

int main()
{
    TestPositionOfTicks();
    TestSettingAt();
    TestTempoAt();
    TestMsOfTick();
    TestChangeFrame();
    TestPerformOptions();
    TestSplitParts();
    TestSplitPartsOnBeats();
    TestSplitPartsWithCalls();
    TestPlaybackClock();
    TestPeaksBetween();
    TestVariablesAt();
    TestReadSequence();
    TestHeldNote();
    TestNoteAcrossTheLoop();
    TestNoteInstruments();
    TestWiiUPlays();
    TestWiiPlays();
    TestReaderConvertsForWii();
    TestWiiReadChanges();
    TestWiiUEffects();
    TestWiiUEffectsRing();
    TestWiiUEnhancedMixing();
    TestWiiUEffectWords();
    TestWiiEffects();
    TestWiiEffectsRing();
    TestWiiEffectWords();
    TestWiiMeters();
    TestWiiGameBuild();
    TestWiiBuildOfDol();
    TestReadMix();
    TestReadLabels();
    TestReadCalls();
    TestReadVariables();
    TestWiiGlobalsStartAtZero();
    TestReadVariablePaths();
    TestListedVariablesChangeTheSound();
    TestReadChanges();
    TestReadErrors();
    TestGroupFiles();
    TestGroupFilePlaces();
    TestExportLayout();
    TestExportChanges();
    TestExportKeepsOtherSamples();
    TestExportTags();
    TestReportFor();
    TestWiiExport();
    TestReader();
    TestReaderAbandons();
    TestReaderBuilds();
    TestReaderConverts();
    TestPlayFromStart();
    TestPlayVariables();
    TestPlayChanges();
    TestSeekAcrossChange();
    TestPlayFromSeek();
    TestLoopOff();
    TestPlayToEnd();
    TestArchiveFlavors();
    TestRuns();
    TestCtrPlayback();
    TestUnplayable();
    TestSeekWithSnapshots();
    TestCycleLocators();
    TestCycleSilence();
    TestCycleInRelease();
    TestPositionPastEnd();
    TestPlayFromRelease();
    TestCycleAfterRightLocator();
    TestMutes();
    TestTrackMeters();
    TestPannedMeter();
    TestAuxEffects();
    TestBusVolumes();
    TestBusCVolume();
    TestWiiUBusVolumes();
    TestSeekFrame();
    TestSwitch();
    TestSwitchByTicks();
    TestSwitchAfterEnd();
    TestSwitchInSilence();
    TestSwitchAtSeek();
    TestSwitchWhilePlaying();
    TestCatchUp();
    TestLeadFrames();
    TestFramesWithoutMixing();
    TestSeekWithoutMixing();
    TestCatchUpWithoutMixing();
    TestCatchUpBehind();
    TestStoppedClock();
    TestWiiUSeekWithoutMixing();
    TestEnhancedMixing();
    TestDryRender();
    TestI3dl2Order();
    TestPreviewItems();
    TestItemFiles();
    TestItemNames();
    TestFolderFailure();
    TestWaveSoundSets();
    TestWaveAsWav();
    TestWaveSoundAsWav();
    TestWaveSoundNoteZero();
    TestSharedSequenceFiles();
    TestOtherConsoleSequenceFile();
    TestOtherConsoleFiles();
    TestBankExport();
    TestSoundFontNames();
    TestBankExportErrors();
    TestBankSfzLayout();
    TestWavePreview();
    TestLoopingWavePreview();
    TestNotePreview();
    TestHeldNotePreview();
    TestNotePreviewLimit();
    TestWiiUPreviews();
    TestWiiPreviews();
    TestWiiWaveSounds();
    TestSoundPreview();
    TestPreviewKey();
    TestSequencePreview();

    return xsflib::testing::Summary("song");
}
