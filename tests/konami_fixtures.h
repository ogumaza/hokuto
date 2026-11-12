// SPDX-License-Identifier: MIT

// This header contains the carts and helpers that the tests of Konami's driver share.

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/konami/archive_player.h"
#include "ntdlib/konami/commands.h"
#include "ntdlib/konami/layout.h"
#include "ntdlib/sequence_export/konami/convert.h"
#include "seqlib/midi/reader.h"
#include "seqlib/wav.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "driver_test_support.h"
#include "edit/bytecode.h"
#include "edit/commands.h"
#include "edit/instruments.h"
#include "edit/konami.h"
#include "edit/konami_samples.h"
#include "edit/mix.h"
#include "edit/notes.h"
#include "edit/other_console.h"
#include "edit/replace.h"
#include "edit/subroutines.h"
#include "edit/tracks.h"
#include "format/archive.h"
#include "format/file.h"
#include "format/text.h"
#include "song/archive.h"
#include "song/konami.h"
#include "song/midi_export.h"
#include "song/sequence.h"
#include "song/sequence_model.h"
#include "song/sequence_reader.h"

namespace
{

namespace format = hokuto::format;
namespace song = hokuto::song;
namespace edit = hokuto::edit;
namespace konami = ntdlib::konami;
using format::File;
using hokuto::testing::HasSound;
using hokuto::testing::Play;
using hokuto::testing::Played;
using hokuto::testing::RunDirect;
using hokuto::testing::StartsOf;
using hokuto::testing::TrackOf;
using song::KonamiRom;
using song::Mixing;
using song::Sequence;
using song::SequenceModel;
using xsflib::testing::ErrorOf;

constexpr uint32_t kRomBase = ntdlib::agb::kRomBase;

// The places of the cartridges' tables. An entry of the song table contains the address of the song, and 16 offsets of
// 2 bytes from there to its tracks.
constexpr uint32_t kSongTable = 0x100;
constexpr uint32_t kEntrySize = 36;
constexpr uint32_t kSong0 = kSongTable + 2 * kEntrySize;
constexpr uint32_t kSampleTable = 0x178;
constexpr uint32_t kHeader0 = kSampleTable + 8;
constexpr uint32_t kHeader1 = kHeader0 + 12 + 32;
constexpr uint32_t kCode = 0x400;
constexpr uint32_t kMixer = 0x600;
constexpr uint32_t kPitches = 0x700;
constexpr uint32_t kWaves = 0x800;
constexpr uint32_t kFrequencies = 0x1000;
constexpr uint32_t kNoises = 0x2000;
constexpr uint32_t kVolumes = 0x3000;
constexpr uint32_t kCartSize = 0x6000;

// The first revision's song 0. Track 4 plays samples, and loops from tick 12 to tick 36:
//
//     +0: 00          wait 0
//     +1: A8 40 00    note of sample 0 at volume 64, at tick 0
//     +4: 0A          wait 10
//     +5: E0          rest, at tick 10
//     +6: 02          wait 2
//     +7: F3 00 00    the loop's start, at tick 12
//    +10: 00          wait 0
//    +11: B8 30 01 02 note of sample 1 at volume 48, 2 semitones up, at tick 12
//    +15: 18          wait 24
//    +16: E0          rest, at tick 36
//    +17: 00          wait 0
//    +18: FF 01       loop the song, at tick 36
//
// Track 0 plays a square wave:
//
//    +20: 00          wait 0
//    +21: 81          duty 1: the channel's register holds 0x40, at tick 0
//    +22: 00          wait 0
//    +23: D3 18       note 24 at volume 3, at tick 0
//    +25: 0C          wait 12
//    +26: E0          rest, at tick 12
//    +27: 00          wait 0
//    +28: F3          the loop's start, at tick 12
//    +29: 00          wait 0
//    +30: E0          rest, at tick 12
//    +31: 18          wait 24
//    +32: FE          end until the song loops, at tick 36
//
// Every other track shares the bytes at +33: 00 FD, a wait of 0 and an end.
const std::vector<uint8_t> kFirstSong0 = {0x00, 0xA8, 0x40, 0x00, 0x0A, 0xE0, 0x02, 0xF3, 0x00, 0x00, 0x00, 0xB8,
                                          0x30, 0x01, 0x02, 0x18, 0xE0, 0x00, 0xFF, 0x01, 0x00, 0x81, 0x00, 0xD3,
                                          0x18, 0x0C, 0xE0, 0x00, 0xF3, 0x00, 0xE0, 0x18, 0xFE, 0x00, 0xFD};
constexpr uint32_t kFirstVoice = 0;
constexpr uint32_t kFirstSquare = 20;
constexpr uint32_t kFirstOthers = 33;

// Song 1, in both cartridges: track 5 plays a note of sample 1 at volume 3 for 8 ticks, and stops the song. The first
// revision stops it with FF 00, and the later one with FF. Every other track shares the bytes after it: 00 FD. In the
// later revision, FD ends the track until the song loops.
const std::vector<uint8_t> kFirstSong1 = {0x00, 0xA3, 0x01, 0x08, 0xE0, 0x00, 0xFF, 0x00, 0x00, 0xFD};
constexpr uint32_t kFirstSong1Others = 8;

// The later revision's song 0. Track 4 plays samples, and loops from tick 12 to tick 32. A call plays two commands from
// offset 9 of the track again, then goes on from the wait after the call:
//
//     +0: 00          wait 0
//     +1: A8 00       note of sample 0 at volume 8, at tick 0
//     +3: 0A          wait 10
//     +4: E0          rest, at tick 10
//     +5: 02          wait 2
//     +6: F3 00 00    the loop's start, at tick 12
//     +9: 04          wait 4
//    +10: A5 01       note of sample 1 at volume 5, at tick 16
//    +12: 06          wait 6
//    +13: E0          rest, at tick 22
//    +14: 00          wait 0
//    +15: 9F 09 00 02 call of 2 commands from +9, at tick 22: a note at tick 26 and a rest at tick 32
//    +19: 00          wait 0
//    +20: FE          loop the song, at tick 32
//
// Track 0 plays a square wave. Its last byte ends it until the song loops:
//
//    +21: 00          wait 0
//    +22: 81          duty 1, at tick 0
//    +23: 00          wait 0
//    +24: D3 18       note 24 at volume 3, at tick 0
//    +26: 0C          wait 12
//    +27: E0          rest, at tick 12
//    +28: 00          wait 0
//    +29: F3          the loop's start, at tick 12
//    +30: 00          wait 0
//    +31: E0          rest, at tick 12
//    +32: 14          wait 20
//    +33: FD          end until the song loops, at tick 32
//
// Every other track shares the bytes at +34: 00 FD.
const std::vector<uint8_t> kLaterSong0 = {0x00, 0xA8, 0x00, 0x0A, 0xE0, 0x02, 0xF3, 0x00, 0x00, 0x04, 0xA5, 0x01,
                                          0x06, 0xE0, 0x00, 0x9F, 0x09, 0x00, 0x02, 0x00, 0xFE, 0x00, 0x81, 0x00,
                                          0xD3, 0x18, 0x0C, 0xE0, 0x00, 0xF3, 0x00, 0xE0, 0x14, 0xFD, 0x00, 0xFD};
constexpr uint32_t kLaterSquare = 21;
constexpr uint32_t kLaterOthers = 34;
const std::vector<uint8_t> kLaterSong1 = {0x00, 0xA3, 0x01, 0x08, 0xE0, 0x00, 0xFF, 0x00, 0xFD};
constexpr uint32_t kLaterSong1Others = 7;

// Writes the low 16 bits of `value` at `at` of `bytes`, little-endian.
inline void Put16(std::vector<uint8_t>& bytes, uint32_t at, uint32_t value)
{
    xsflib::StoreLe16(&bytes[at], static_cast<uint16_t>(value));
}

// Writes `value` at `at` of `bytes`, little-endian.
inline void Put32(std::vector<uint8_t>& bytes, uint32_t at, uint32_t value)
{
    xsflib::StoreLe32(&bytes[at], value);
}

// Writes entry `song` of the song table. Each track starts at `others`, and tracks 0 and `voice` at the offsets given.
inline void PutEntry(std::vector<uint8_t>& bytes, int song, uint32_t address, uint32_t others, int voice,
                     uint32_t voice_at, std::optional<uint32_t> square_at = std::nullopt)
{
    const uint32_t entry = kSongTable + static_cast<uint32_t>(song) * kEntrySize;
    Put32(bytes, entry, kRomBase + address);
    for (uint32_t track = 0; track < 16; track++)
    {
        Put16(bytes, entry + 4 + 2 * track, others);
    }
    Put16(bytes, entry + 4 + 2 * static_cast<uint32_t>(voice), voice_at);
    if (square_at)
    {
        Put16(bytes, entry + 4, *square_at);
    }
}

// The samples, after the sample table. Sample 0 plays once at the mixer's rate: 32 points from 0 up by 8. Sample 1
// loops from point 4 at about three quarters of it: 16 points from 0 up by 16.
inline void PutSamples(std::vector<uint8_t>& bytes)
{
    Put32(bytes, kSampleTable, kRomBase + kHeader0);
    Put32(bytes, kSampleTable + 4, kRomBase + kHeader1);
    Put32(bytes, kHeader0, 0x1000);
    Put32(bytes, kHeader0 + 4, 32);
    Put32(bytes, kHeader0 + 8, 0xFFFFFFFF);
    for (uint32_t i = 0; i < 32; i++)
    {
        bytes[kHeader0 + 12 + i] = static_cast<uint8_t>(i * 8);
    }
    Put32(bytes, kHeader1, 0xC2D);
    Put32(bytes, kHeader1 + 4, 16);
    Put32(bytes, kHeader1 + 8, 4);
    for (uint32_t i = 0; i < 16; i++)
    {
        bytes[kHeader1 + 12 + i] = static_cast<uint8_t>(i * 16);
    }
}

// A run of the driver's Thumb code at `at`, and the word that its ldr loads, the address of a table, at `literal_at`.
inline void PutCode(std::vector<uint8_t>& bytes, uint32_t at, const std::vector<uint16_t>& code, uint32_t literal_at,
                    uint32_t table)
{
    for (uint32_t i = 0; i < code.size(); i++)
    {
        Put16(bytes, at + 2 * i, code[i]);
    }
    Put32(bytes, at + literal_at, kRomBase + table);
}

// The bytes that both revisions share: the samples, the tables, and the mixer's code that loads the sample table. The
// square waves' frequencies show their pitch. A voice's level grows with its volume and its side's pan level.
inline std::vector<uint8_t> EmptyCart(uint32_t pans)
{
    std::vector<uint8_t> bytes(kCartSize);
    bytes[0xB2] = 0x96; // the header's fixed value
    PutSamples(bytes);

    // The mixer: ldr r5, =table / ldrsh r0, [r7, #12] / movs r0, r0, lsl #17 / ldrhs r5, =second, then the size of a
    // voice's record and the two addresses.
    Put32(bytes, kMixer - 4, 0xE59F500C);
    Put32(bytes, kMixer, 0xE1D700FC);
    Put32(bytes, kMixer + 4, 0xE1B00880);
    Put32(bytes, kMixer + 8, 0x259F5004);
    Put32(bytes, kMixer + 12, 0xE2477018);
    Put32(bytes, kMixer + 16, kRomBase + kSampleTable);

    // The pitch table round its centre: 4096 * 2 ^ (pitch / 384).
    const std::array<uint16_t, 5> centre = {4081, 4089, 4096, 4103, 4111};
    for (uint32_t i = 0; i < centre.size(); i++)
    {
        Put16(bytes, kPitches + 2 * i, centre[i]);
    }
    for (uint32_t pitch = 0; pitch < 0x800; pitch++)
    {
        Put16(bytes, kFrequencies + 2 * pitch, 0x8000 | pitch);
    }
    for (uint32_t i = 0; i < 0x3000; i++)
    {
        const uint32_t volume = i / pans;
        const uint32_t pan = i % pans;
        bytes[kVolumes + i] = static_cast<uint8_t>(std::min<uint32_t>(volume * (pan + 1) * 64 / pans / 16, 127));
    }

    return bytes;
}

// The tables that each revision loads the same way: a song's wave images.
inline void PutWaveLoader(std::vector<uint8_t>& bytes)
{
    PutCode(bytes, kCode + 0x70, {0xB510, 0x1C04, 0x4B02, 0x2A00, 0xD000, 0x2080, 0x4008}, 0x10, kWaves);
}

inline std::vector<uint8_t> FirstCart()
{
    std::vector<uint8_t> bytes = EmptyCart(64);
    std::ranges::copy(kFirstSong0, bytes.begin() + kSong0);
    const uint32_t song1 = kSong0 + static_cast<uint32_t>(kFirstSong0.size());
    std::ranges::copy(kFirstSong1, bytes.begin() + song1);
    PutEntry(bytes, 0, kSong0, kFirstOthers, 4, kFirstVoice, kFirstSquare);
    PutEntry(bytes, 1, song1, kFirstSong1Others, 5, 0);

    PutCode(bytes, kCode, {0x00E0, 0x1900, 0x0080, 0x4904, 0x1842, 0x8811, 0x8850, 0x0400, 0x4301}, 0x18, kSongTable);
    PutCode(bytes, kCode + 0x20, {0x4B00, 0x403B, 0x20A0, 0x0200, 0x4038, 0x2800, 0xD100, 0x4901}, 0x14, kSampleTable);
    PutCode(bytes, kCode + 0x40, {0x78D2, 0x0191, 0x4801, 0x1809}, 0x0C, kVolumes);
    PutCode(bytes, kCode + 0x50, {0x4802, 0x2700, 0x5FE9, 0x0049, 0x1809, 0x880D}, 0x0C, kFrequencies);
    PutCode(bytes, kCode + 0x60, {0x4902, 0x2424, 0x5F18, 0x0040, 0x1840, 0x8800}, 0x0C, kNoises);
    PutWaveLoader(bytes);

    return bytes;
}

inline std::vector<uint8_t> LaterCart()
{
    std::vector<uint8_t> bytes = EmptyCart(16);
    std::ranges::copy(kLaterSong0, bytes.begin() + kSong0);
    const uint32_t song1 = kSong0 + static_cast<uint32_t>(kLaterSong0.size());
    std::ranges::copy(kLaterSong1, bytes.begin() + song1);
    PutEntry(bytes, 0, kSong0, kLaterOthers, 4, 0, kLaterSquare);
    PutEntry(bytes, 1, song1, kLaterSong1Others, 5, 0);

    PutCode(bytes, kCode, {0x00E8, 0x1940, 0x0080, 0x4904, 0x1843, 0x8819, 0x8858, 0x0400, 0x4301}, 0x18, kSongTable);
    PutCode(bytes, kCode + 0x20, {0x4902, 0x4803, 0x4030, 0x0080, 0x1840, 0x6805}, 0x0C, kSampleTable);
    Put32(bytes, kCode + 0x20 + 0x10, 0xFFF);
    PutCode(bytes, kCode + 0x40, {0x78F8, 0x0100, 0x4901, 0x1841, 0x200F, 0x4020}, 0x0C, kVolumes);
    PutCode(bytes, kCode + 0x50, {0x4802, 0x2200, 0x5EB9, 0x0049, 0x1809, 0x880C}, 0x0C, kFrequencies);
    PutCode(bytes, kCode + 0x60, {0x4902, 0x2424, 0x5F38, 0x0040, 0x1840, 0x8800}, 0x0C, kNoises);
    PutWaveLoader(bytes);

    // The command reader: ldrb r0, [r1], cmp r0, #0xFC and a ble, then the opcodes that the revision compares.
    const std::array<uint16_t, 7> reader = {0x7808, 0x28FC, 0xDD00, 0x28FF, 0x28FE, 0x28EF, 0x28FA};
    for (uint32_t i = 0; i < reader.size(); i++)
    {
        Put16(bytes, kCode + 0xA0 + 2 * i, reader[i]);
    }

    return bytes;
}

// The reference: ntdlib's player run directly up to `samples` stereo samples.
inline std::vector<int16_t> DirectStrict(const KonamiRom& rom, int song, std::size_t samples, int muted = -1)
{
    konami::ArchivePlayer player(rom.rom, rom.info);
    player.Start(song, {});
    if (muted >= 0)
    {
        player.MuteTrack(muted, true);
    }
    return RunDirect<int16_t>(player, samples);
}

// Whether a track's notes start at `starts` first. A later pass of a loop can follow them.
inline bool StartsAt(const Sequence& sequence, int number, const std::vector<uint32_t>& starts)
{
    const std::vector<uint32_t> found = StartsOf(sequence, number);
    return found.size() >= starts.size() && std::equal(starts.begin(), starts.end(), found.begin());
}

inline Sequence ReadOf(const File& file, uint32_t sound)
{
    return song::ReadSequence(*song::LoadKonamiRom(format::ExportArchive(file)), sound);
}

} // namespace
