// SPDX-License-Identifier: MIT

// The edits of a DSE bank's splits, for the Bank Editor, and of a wave's values, for its properties. A program has
// splits of 48 bytes (ntdlib::dse::Program). A split plays a wave of the bank's table for a run of keys and velocities.
// A wave's parameters take 64 bytes of the table (ntdlib::dse::WaveInfo). An edit changes those bytes in place, in the
// run of the set that contains the bank file (edit/native_runs.h). The set keeps its size.
//
// DSE's driver reads no wave's rate. It plays a key at the pitch that the tuning and the coarse tuning of the split
// set, against the 32,728 Hz of the sound unit. The tools write the tuning from the wave's rate. A new rate therefore
// moves the tuning of each split that plays the wave, and the wave plays at the new rate.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "ntdlib/dse/formats.h"

#include "edit/ds_waves.h"
#include "format/file.h"
#include "song/dse.h"

namespace hokuto::edit
{

// The bytes of a split.
using DseSplit = std::array<uint8_t, ntdlib::dse::SplitView::kSize>;

// The places of a split's values that the Bank Editor edits. The driver reads the keys and the velocities at these
// places. The tools write each of them twice, 2 bytes on, and an edit writes both. The tuning uses 16 signed bits in
// 256ths of a semitone. The coarse tuning uses 8 signed bits in semitones. The root key is 60 less the coarse tuning.
// The tools write it after the coarse tuning, and the driver doesn't read it. An edit moves it with the coarse tuning.
// The attack level, the times and the sustain level are the driver's envelope.
inline constexpr std::size_t kDseBendRange = ntdlib::dse::SplitView::kBendRange;
inline constexpr std::size_t kDseLowKey = ntdlib::dse::SplitView::kLowKey;
inline constexpr std::size_t kDseHighKey = ntdlib::dse::SplitView::kHighKey;
inline constexpr std::size_t kDseLowVelocity = ntdlib::dse::SplitView::kLowVelocity;
inline constexpr std::size_t kDseHighVelocity = ntdlib::dse::SplitView::kHighVelocity;
inline constexpr std::size_t kDseWave = ntdlib::dse::SplitView::kWave;
inline constexpr std::size_t kDseTuning = ntdlib::dse::SplitView::kTuning;
inline constexpr std::size_t kDseRootKey = ntdlib::dse::SplitView::kRootKey;
inline constexpr std::size_t kDseCoarseTuning = ntdlib::dse::SplitView::kCoarseTuning;
inline constexpr std::size_t kDseVolume = ntdlib::dse::SplitView::kVolume;
inline constexpr std::size_t kDsePan = ntdlib::dse::SplitView::kPan;
inline constexpr std::size_t kDseKeyGroup = ntdlib::dse::SplitView::kKeyGroup;
inline constexpr std::size_t kDseMultiplier = ntdlib::dse::SplitView::kEnvelopeMultiplier;
inline constexpr std::size_t kDseAttackLevel = ntdlib::dse::SplitView::kAttackBegin;
inline constexpr std::size_t kDseAttack = ntdlib::dse::SplitView::kAttackTime;
inline constexpr std::size_t kDseDecay = ntdlib::dse::SplitView::kDecayTime;
inline constexpr std::size_t kDseSustain = ntdlib::dse::SplitView::kSustainLevel;
inline constexpr std::size_t kDseHold = ntdlib::dse::SplitView::kHoldTime;
inline constexpr std::size_t kDseDecay2 = ntdlib::dse::SplitView::kSustainTime;
inline constexpr std::size_t kDseRelease = ntdlib::dse::SplitView::kReleaseTime;

// The highest value of each byte that an edit of a split accepts.
inline constexpr int kHighestDseByte = 127;

// The key whose note plays a wave at the tuning's pitch when the coarse tuning is 0.
inline constexpr int kDseMiddleKey = 60;

// The root key of `split`, and the tuning. Each sets its value in `split`.
int DseRootKey(const DseSplit& split);
void SetDseRootKey(DseSplit& split, int key);
int DseTuning(const DseSplit& split);
void SetDseTuning(DseSplit& split, int tuning);

// The table's slot of the wave that `split` plays.
int DseSplitWave(const DseSplit& split);
void SetDseSplitWave(DseSplit& split, int wave);

// A split of a DSE bank: the index of the bank among the banks of the set, the program, and the index of the split
// among the splits of the program.
struct DseSplitPlace
{
    friend bool operator==(const DseSplitPlace&, const DseSplitPlace&) = default;

    uint32_t bank = 0;
    uint32_t program = 0;
    std::size_t split = 0;
};

// `file`, a Hokuto file of DSE's files, with the values of `split` at `place`. `set` is the set that `file` builds. The
// edit reaches each file of the set that contains the same bank. Only the values of the places above can change, and a
// value that the edit leaves unchanged isn't checked. Throws std::invalid_argument with a message for the user for a
// place that the bank doesn't have, a key or a velocity past 127, a low key above the high key, a low velocity above
// the high velocity, a wave that is missing from the bank's table, a root key past 127, another value past
// kHighestDseByte, and bytes that no run of the set contains.
format::File WithDseSplit(const format::File& file, const song::DseSet& set, const DseSplitPlace& place,
                          const DseSplit& split);

// The same, with `starts`, the starts of the pieces of `file`'s native layout (format::NativePieceStarts). An edit of a
// split keeps every piece's start, and a run of edits can find them once.
format::File WithDseSplit(const format::File& file, const song::DseSet& set, const DseSplitPlace& place,
                          const DseSplit& split, std::span<const uint64_t> starts);

// The values of a wave that its properties edit. The loop's start counts samples. Without a loop, it is 0.
struct DseWaveValues
{
    friend bool operator==(const DseWaveValues&, const DseWaveValues&) = default;

    uint32_t rate = 0;
    bool loop = false;
    uint32_t loop_start = 0;
};

// The rates that a DSE wave can take in its properties: those of edit/ds_waves.h.
inline constexpr uint32_t kLeastDseRate = kLeastDsRate;
inline constexpr uint32_t kHighestDseRate = kHighestDsRate;

// The values of wave `wave` of bank `bank` of `set`. Throws std::runtime_error with a message for the user for a wave
// that is missing from the bank.
DseWaveValues DseWaveValuesOf(const song::DseSet& set, uint32_t bank, uint32_t wave);

// `file` with `values` for wave `wave` of bank `bank`, in the bank's table and in the table of each bank that reads its
// waves from `bank`. A new rate replaces the rate of each table, and moves the tuning of each split that plays the wave
// there by the semitones between the old rate of that table and the new one. A loop starts at a word before the wave's
// end, and runs to the end. The end stays. Without a loop, the start of the loop is the first word of samples of the
// wave. The tools write it there. Throws std::invalid_argument with a message for the user for a square wave or noise,
// a rate past the limits, a rate that moves the tuning of a split past its 16 bits, a loop that starts off a word or at
// the end, and bytes that no run of the set contains. Throws std::runtime_error as DseWaveValuesOf does.
format::File WithDseWave(const format::File& file, const song::DseSet& set, uint32_t bank, uint32_t wave,
                         const DseWaveValues& values);

// The same, with `starts`, the starts of the pieces of `file`'s native layout (format::NativePieceStarts).
format::File WithDseWave(const format::File& file, const song::DseSet& set, uint32_t bank, uint32_t wave,
                         const DseWaveValues& values, std::span<const uint64_t> starts);

} // namespace hokuto::edit
