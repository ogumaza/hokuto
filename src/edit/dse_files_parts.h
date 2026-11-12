// SPDX-License-Identifier: MIT

// What dse_files.cpp, dse_bank_files.cpp and dse_banks.cpp share: the check for a DSE set, the programs that a song
// selects, a list of numbers for the user, the places of a wave's parameters, and a bank of a set by its number. Only
// those files include it.

#pragma once

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/dse/formats.h"

#include "format/file.h"
#include "song/dse.h"

namespace hokuto::edit::dse_files_parts
{

using ntdlib::commands::Sequence;

// Throws std::invalid_argument with a message for the user when `file` contains no DSE files.
void RequireSet(const format::File& file);

// The programs that the program changes of `tracks` select.
std::set<uint32_t> ProgramsOf(const Sequence& tracks);

// `numbers` in a list for the user: "1, 5, 9".
std::string NumberList(const std::set<uint32_t>& numbers);

// The place of the format's byte in a wave's parameters (ntdlib::dse::WaveInfo). WaveInfo reads the format as 16 bits
// at 0x12. Then the formats that hold samples, and the tuning of a doubled rate: 12 semitones in 256ths of a semitone.
inline constexpr std::size_t kWaveFormat = 0x13;
inline constexpr uint8_t kPcm8 = 0;
inline constexpr uint8_t kPcm16 = 1;
inline constexpr uint8_t kImaAdpcm = 2;
inline constexpr double kTuningOfDoubledRate = 12.0 * 256.0;

// Bank `bank` of `set`. Throws std::invalid_argument with a message for the user for a bank that `set` doesn't
// contain.
const song::DseBank& BankAt(const song::DseSet& set, uint32_t bank);

// Moves the tuning of each split of `model` that plays wave `wave` by `moved` 256ths of a semitone, for a new sample
// rate. Throws std::invalid_argument with a message for the user for a tuning out of range.
void RetuneSplits(ntdlib::dse::WaveBank& model, uint32_t wave, double moved);

} // namespace hokuto::edit::dse_files_parts
