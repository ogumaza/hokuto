// SPDX-License-Identifier: MIT

// This header contains the archives and helpers that the tests of the track edits share.

#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/mp2k/song.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/sequence_file.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/sequence_file.h"
#include "ntdlib/sdat/layout.h"
#include "ntdlib/testing/dse_song_files.h"
#include "ntdlib/testing/mp2k_cart.h"
#include "ntdlib/testing/nds_files.h"
#include "ntdlib/testing/rsar_files.h"
#include "ntdlib/testing/sar_files.h"
#include "ntdlib/testing/sar_tools.h"
#include "ntdlib/testing/sdat_files.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "edit/instruments.h"
#include "edit/notes.h"
#include "edit/replace.h"
#include "edit/tracks.h"
#include "format/archive.h"
#include "format/file.h"
#include "song/archive.h"
#include "song/ds.h"
#include "song/dse.h"
#include "song/gba.h"
#include "song/sequence.h"
#include "sound_places.h"

using hokuto::edit::AddTrack;
using hokuto::edit::CopiedTrack;
using hokuto::edit::CopyTrack;
using hokuto::edit::DuplicateTrack;
using hokuto::edit::PasteTrack;
using hokuto::edit::Program;
using hokuto::edit::RemoveTrack;
using hokuto::edit::SoundPlace;
using hokuto::edit::TrackEdit;
using hokuto::format::File;
using hokuto::song::Note;
using hokuto::song::Sequence;
using hokuto::song::Track;
using hokuto::testing::PlacesOf;
using hokuto::testing::PlayedSequence;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using xsflib::testing::ErrorOf;

namespace
{

namespace sar = ntdlib::sar;
namespace rsar_files = ntdlib::testing::rsar_files;
namespace sar_files = ntdlib::testing::sar_files;
namespace sar_tools = ntdlib::testing::sar_tools;

// The archive's sounds.
constexpr uint32_t kSong = 0;
constexpr uint32_t kTwin = 1;
constexpr uint32_t kLoop = 2;
constexpr uint32_t kRound = 3;
constexpr uint32_t kRoundTwin = 4;
constexpr uint32_t kFull = 5;
constexpr uint32_t kPart = 6;
constexpr uint32_t kAlloc = 7;
constexpr uint32_t kChain = 8;
constexpr uint32_t kSource = 9;

// The first file's bytecode. Each command's offset comes first in its comment. The Switch's sequences give arguments in
// little-endian order.
const std::vector<uint8_t> kSongBytecode = {
    0x88, 1,    23,   0, 0, // 0: open track 1 at 23
    0xB0, 48,               // 5: timebase 48
    0xE1, 120,  0x00,       // 7: tempo 120
    0xC7, 0x00,             // 10: note wait off
    60,   100,  24,         // 12: key 60 for 24 ticks, at tick 0
    0x80, 48,               // 15: wait 48 ticks
    62,   100,  24,         // 17: key 62 for 24 ticks, at tick 48
    0x80, 48,               // 20: wait 48 ticks
    0xFF,                   // 22: the end, at tick 96
    0xC7, 0x00,             // 23: track 1: note wait off
    0x8A, 35,   0,    0,    // 25: call 35, at tick 0
    64,   100,  24,         // 29: key 64 for 24 ticks, at tick 24
    0x80, 24,               // 32: wait 24 ticks
    0xFF,                   // 34: the end, at tick 48
    67,   100,  12,         // 35: key 67 for 12 ticks
    0x80, 24,               // 38: wait 24 ticks
    0xFD,                   // 40: return
};

// The second file's bytecode: an intro of 96 ticks, then a loop of 48.
const std::vector<uint8_t> kLoopBytecode = {
    0xB0, 48,            // 0: timebase 48
    0xE1, 120,  0x00,    // 2: tempo 120
    0xC7, 0x00,          // 5: note wait off
    60,   100,  24,      // 7: key 60 for 24 ticks, at tick 0
    0x80, 96,            // 10: wait 96 ticks
    62,   100,  24,      // 12: key 62 for 24 ticks, at tick 96: the loop's start
    0x80, 48,            // 15: wait 48 ticks
    0x89, 12,   0,    0, // 17: jump 12, at tick 144
};

// The third file's bytecode: a loop back to the first command.
const std::vector<uint8_t> kRoundBytecode = {
    0xC7, 0x00,        // 0: note wait off
    60,   100,  24,    // 2: key 60 for 24 ticks
    0x80, 48,          // 5: wait 48 ticks
    0x89, 0,    0,  0, // 7: jump 0, at tick 48
};

// The fourth file's bytecode: track starts of tracks 1 to 15 at the end.
inline std::vector<uint8_t> FullBytecode()
{
    std::vector<uint8_t> bytes;
    for (uint8_t track = 1; track < 16; track++)
    {
        bytes.insert(bytes.end(), {0x88, track, 75, 0, 0});
    }
    bytes.push_back(0xFF);

    return bytes;
}

// The fifth file's bytecode. Track 0 allocates tracks 0 to 2, and opens tracks 1 and 2 after a wait. Both run the same
// commands.
const std::vector<uint8_t> kAllocBytecode = {
    0xFE, 0x07, 0x00,       // 0: allocate tracks 0 to 2
    0xC7, 0x00,             // 3: note wait off
    60,   100,  24,         // 5: key 60 for 24 ticks
    0x80, 24,               // 8: wait 24 ticks
    0x88, 1,    23,   0, 0, // 10: open track 1 at 23, at tick 24
    0x88, 2,    23,   0, 0, // 15: open track 2 at 23
    0x80, 24,               // 20: wait 24 ticks
    0xFF,                   // 22: the end, at tick 48
    0xC7, 0x00,             // 23: tracks 1 and 2: note wait off
    62,   100,  24,         // 25: key 62 for 24 ticks
    0x80, 24,               // 28: wait 24 ticks
    0xFF,                   // 30: the end, at tick 48
};

// The sixth file's bytecode. Track 0 opens track 1, and track 1 opens track 2.
const std::vector<uint8_t> kChainBytecode = {
    0x88, 1,   8,  0, 0, // 0: open track 1 at 8
    0x80, 48,            // 5: wait 48 ticks
    0xFF,                // 7: the end, at tick 48
    0x88, 2,   16, 0, 0, // 8: track 1: open track 2 at 16
    0x80, 48,            // 13: wait 48 ticks
    0xFF,                // 15: the end, at tick 48
    60,   100, 24,       // 16: track 2: key 60 for 24 ticks
    0x80, 24,            // 19: wait 24 ticks
    0xFF,                // 21: the end, at tick 48
};

// The seventh file's bytecode. Track 0 sets the tempo and opens track 1 after a wait. Track 1 selects slot 1.
const std::vector<uint8_t> kSourceBytecode = {
    0xE1, 150,  0x00,       // 0: tempo 150
    0xC7, 0x00,             // 3: note wait off
    0x80, 24,               // 5: wait 24 ticks
    0x88, 1,    18,   0, 0, // 7: open track 1 at 18, at tick 24
    65,   100,  24,         // 12: key 65 for 24 ticks
    0x80, 24,               // 15: wait 24 ticks
    0xFF,                   // 17: the end, at tick 48
    0xC7, 0x00,             // 18: track 1: note wait off
    0xB6, 1,                // 20: bank slot 1
    0x81, 0,                // 22: program 0
    67,   100,  12,         // 24: key 67 for 12 ticks, at tick 24
    0x80, 24,               // 27: wait 24 ticks
    0xFF,                   // 29: the end, at tick 48
};

// The archive's spec. Both banks play a looped wave for every key.
inline sar_files::ArchiveSpec TestSpec()
{
    sar_files::WaveSpec wave;
    wave.encoding = 1;
    wave.loop = true;
    wave.sample_rate = 48000;
    wave.loop_end = 64;
    wave.channels = {std::vector<uint8_t>(128, 0x10)};

    sar_files::Region region;
    region.original_key = 60;
    sar_files::VelocityTable velocities;
    velocities.entries = {{0, region}};
    sar_files::KeyTable keys;
    keys.entries = {{0, velocities}};
    sar_files::BankSpec bank;
    bank.waves = {{0x05000000, 0}};
    bank.instruments = {keys};

    sar_files::SoundSpec song;
    song.name = "SEQ_SONG";
    song.volume = 127;
    song.banks = {0};
    song.start_offset = 0;
    song.allocate_track_flags = 0b11;
    sar_files::SoundSpec twin = song;
    twin.name = "SEQ_TWIN";
    twin.allocate_track_flags = 0b111;
    sar_files::SoundSpec loop = song;
    loop.name = "SEQ_LOOP";
    loop.file = 1;
    loop.allocate_track_flags = 1;
    sar_files::SoundSpec round = loop;
    round.name = "SEQ_ROUND";
    round.file = 2;
    sar_files::SoundSpec round_twin = round;
    round_twin.name = "SEQ_ROUND_TWIN";
    round_twin.start_offset = std::nullopt;
    sar_files::SoundSpec full = song;
    full.name = "SEQ_FULL";
    full.file = 3;
    full.allocate_track_flags = 0xFFFF;
    sar_files::SoundSpec part = song;
    part.name = "SEQ_PART";
    part.start_offset = 23;
    sar_files::SoundSpec alloc = song;
    alloc.name = "SEQ_ALLOC";
    alloc.file = 4;
    sar_files::SoundSpec chain = song;
    chain.name = "SEQ_CHAIN";
    chain.file = 5;
    chain.allocate_track_flags = 0b111;
    sar_files::SoundSpec source = song;
    source.name = "SEQ_SOURCE";
    source.file = 8;
    source.banks = {1, 0};

    // The first six sequence files come first, then the first bank, the wave archive, the seventh sequence file and the
    // second bank.
    sar_files::ArchiveSpec spec;
    spec.sounds = {song, twin, loop, round, round_twin, full, part, alloc, chain, source};
    spec.banks = {{"BANK_TEST", 6}, {"BANK_OTHER", 9}};
    spec.wave_archives = {{"WARC_TEST", 7}};
    spec.players = {{"PLAYER_TEST", 1}};
    spec.files = {
        {sar_files::Sequence(false, kSongBytecode, {{"Start", 0}, {"Track1", 23}, {"Sub", 35}}), "", false},
        {sar_files::Sequence(false, kLoopBytecode, {{"Loop", 0}}), "", false},
        {sar_files::Sequence(false, kRoundBytecode, {{"Round", 0}}), "", false},
        {sar_files::Sequence(false, FullBytecode(), {{"Full", 0}}), "", false},
        {sar_files::Sequence(false, kAllocBytecode, {{"Alloc", 0}}), "", false},
        {sar_files::Sequence(false, kChainBytecode, {{"Chain", 0}}), "", false},
        {sar_files::Bank(false, bank), "", false},
        {sar_files::WaveArchive(false, {sar_files::Wave(false, wave)}), "", false},
        {sar_files::Sequence(false, kSourceBytecode, {{"Source", 0}, {"SourceTrack1", 18}}), "", false},
        {sar_files::Bank(false, bank), "", false},
    };

    return spec;
}

// A file that imports the archive laid out as NintendoWare's tools lay one out.
inline File ToolsFile()
{
    return hokuto::format::ImportSwitchArchive(sar_tools::Archive(sar::Flavor::kNx, TestSpec()));
}

// An archive as the program reads it.
using Archive = std::shared_ptr<const sar::SoundArchive>;

// The archive that `file` exports. The program reads an archive the same way.
inline Archive Read(const File& file)
{
    return hokuto::song::LoadArchive(hokuto::format::ExportArchive(file), "test.bfsar");
}

// The numbers of the tracks of `song`.
inline std::vector<int> Numbers(const Sequence& song)
{
    std::vector<int> numbers;
    for (const Track& track : song.tracks)
    {
        numbers.push_back(track.number);
    }

    return numbers;
}

// The start, length, key and velocity of each note of track `number` of `song`.
inline std::vector<std::vector<uint32_t>> Notes(const Sequence& song, int number)
{
    std::vector<std::vector<uint32_t>> notes;
    for (const Track& track : song.tracks)
    {
        for (const Note& note : track.notes)
        {
            if (track.number == number)
            {
                notes.push_back({note.start, note.length, note.key, note.velocity});
            }
        }
    }

    return notes;
}

// Whether `call` throws std::invalid_argument: the edit refuses.
template <typename Call>
bool Refused(const Call& call)
{
    return !ErrorOf<std::invalid_argument>(call).empty();
}

} // namespace
