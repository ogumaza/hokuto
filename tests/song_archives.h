// SPDX-License-Identifier: MIT

// This header contains the sound numbers, bytecodes and archives that the tests of the songs share. The comment below
// describes those tests.
//
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
// microcode in place of the DSP, and with each of the Wii's effects. A GBA song plays as ntdlib's player of MP2K
// songs run directly, in both mixing modes. A model that the program offers during playback takes over at the player's
// next frame. That frame crossfades from the old model's output to the new one's. While the render worker plays, a
// catch-up runs the new model up to the player's frame on a thread of its own. A model at another tempo takes over at
// the player's tick. A sequence plays with the values that its variables start with. Its output matches ntdlib's model
// with the same values. The starting values of the variables that a sound lists change what ntdlib's model plays, and
// those of the others don't. An export to MIDI writes the files of ntdlib's converter frame in the same places, with
// the changes at set times. It never writes over another export's samples. Its message about a folder that it can't
// make ends with one full stop. The dry render for the Export window's warnings matches ntdlib's model with the aux
// buses silent. A 3DS archive plays on ntdlib's model of the DSP's audio firmware.

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <future>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
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
#include "ntdlib/mp2k/archive_player.h"
#include "ntdlib/mp2k/commands.h"
#include "ntdlib/nx/atk/engine.h"
#include "ntdlib/nx/audio/i3dl2.h"
#include "ntdlib/rvl/snd/engine.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/decode.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/stream_file.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/consoles.h"
#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/sequence_export/frame.h"
#include "ntdlib/sequence_export/mp2k/convert.h"
#include "ntdlib/sequence_export/sfz.h"
#include "ntdlib/sound_build.h"
#include "ntdlib/testing/dol_files.h"
#include "ntdlib/testing/mp2k_cart.h"
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
#include "song/gba.h"
#include "song/items.h"
#include "song/midi_export.h"
#include "song/player.h"
#include "song/preview.h"
#include "song/sequence.h"
#include "song/sequence_preview.h"
#include "song/sequence_reader.h"
#include "temporary_files.h"

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
using hokuto::song::WiiMissingNote;
using hokuto::song::WiiWaveArchive;
using hokuto::song::WithMidiTags;
using hokuto::song::WriteBankSfz;
using hokuto::song::WriteMidiAndSfz;
using hokuto::song::WriteMidiAndSoundFont;
using hokuto::testing::TemporaryFile;
using hokuto::testing::TemporaryFolder;

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
inline int SawSample(int i, bool falling)
{
    return (falling ? 479 - i : i) * 40 - 9600;
}

// The instrument's wave: a looped saw of 480 samples at 48 kHz, as PCM16 in the Switch's byte order or, with
// `big_endian`, the Wii U's. Its samples differ from one another. A frame out of place then changes the output. With
// `falling`, the saw falls instead of rising.
inline std::vector<uint8_t> SawWave(bool falling, bool big_endian)
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
inline std::vector<std::vector<uint8_t>> TestWaveFiles(bool big_endian)
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
inline sar_files::ArchiveSpec TestArchiveSpec(bool big_endian = false)
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
inline std::vector<uint8_t> TestArchiveBytes()
{
    return sar_files::Archive(false, TestArchiveSpec());
}

// The test archive with a wave sound after its sequence sounds: sound kWaveSound. The sound's file holds 8 made-up
// bytes.
inline sar_files::ArchiveSpec MadeUpWaveSoundSpec()
{
    sar_files::ArchiveSpec spec = TestArchiveSpec();
    spec.sounds.push_back({.name = "WSD_TEST", .kind = sar_files::SoundSpec::kWaveSound, .file = 6});
    spec.files.push_back({{'F', 'W', 'S', 'D', 1, 2, 3, 4}, "", false});

    return spec;
}

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
inline std::shared_ptr<const sar::SoundArchive> OpenTestArchive()
{
    const TemporaryFile file(TestArchiveBytes());
    return OpenArchive(file.Path());
}

// The test archive as the Wii U holds it, opened the way the program opens one. The note's sequence holds its tempo in
// the Wii U's byte order. The test plays no other sequence of it.
inline std::shared_ptr<const sar::SoundArchive> OpenWiiUArchive()
{
    sar_files::ArchiveSpec spec = TestArchiveSpec(true);
    spec.files[1].bytes = sar_files::Sequence(true, kWiiUNoteBytecode, {{"Note", 0}});
    const TemporaryFile file(sar_files::Archive(true, spec));
    return OpenArchive(file.Path());
}

// The Wii U's test archive with the send sequence in place of the note's.
inline std::shared_ptr<const sar::SoundArchive> OpenWiiUSendArchive()
{
    sar_files::ArchiveSpec spec = TestArchiveSpec(true);
    spec.files[1].bytes = sar_files::Sequence(true, kWiiUSendBytecode, {{"Note", 0}});
    const TemporaryFile file(sar_files::Archive(true, spec));
    return OpenArchive(file.Path());
}

// The Wii U's test archive with the loop's sequence as the Wii U holds it. The test plays no other sequence of it.
inline std::shared_ptr<const sar::SoundArchive> OpenWiiULoopArchive()
{
    sar_files::ArchiveSpec spec = TestArchiveSpec(true);
    spec.files[0].bytes = sar_files::Sequence(true, kWiiULoopBytecode, {{"Loop", 0}});
    const TemporaryFile file(sar_files::Archive(true, spec));
    return OpenArchive(file.Path());
}

// The test archive with a fifth sequence, `bytecode`. The send sequence sends its note to aux bus A.
inline std::shared_ptr<const sar::SoundArchive> OpenSendArchive(const std::vector<uint8_t>& bytecode = kSendBytecode)
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
inline std::shared_ptr<const sar::SoundArchive> OpenSlowArchive()
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
inline std::shared_ptr<const sar::SoundArchive> OpenCallsArchive()
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
inline std::shared_ptr<const sar::SoundArchive> OpenInstrumentsArchive()
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
inline std::shared_ptr<const sar::SoundArchive> OpenHeldArchive(const std::vector<uint8_t>& bytecode)
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
inline std::shared_ptr<const sar::SoundArchive> OpenVariablesArchive()
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
inline std::shared_ptr<const sar::SoundArchive> OpenChangesArchive()
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
inline std::shared_ptr<const sar::SoundArchive> OpenVariablePathsArchive()
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

} // namespace
