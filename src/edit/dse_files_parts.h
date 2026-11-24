// SPDX-License-Identifier: MIT

// What dse_files.cpp, dse_bank_files.cpp and dse_banks.cpp share: the check for a DSE set, the programs that a song
// selects, a list of numbers for the user, the places of a wave's parameters, a bank of a set by its number, and the
// bytes of a bank file in either version. Only those files include it.

#pragma once

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

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

// The formats that hold samples (DseWaveFormat), and the tuning of a doubled rate: 12 semitones in 256ths of a
// semitone.
inline constexpr uint8_t kPcm8 = 0;
inline constexpr uint8_t kPcm16 = 1;
inline constexpr uint8_t kImaAdpcm = 2;
inline constexpr double kTuningOfDoubledRate = 12.0 * 256.0;

// Bank `bank` of `set`. Throws std::invalid_argument with a message for the user for a bank that `set` doesn't
// contain.
const song::DseBank& BankAt(const song::DseSet& set, uint32_t bank);

// The bytes of the bank file `file` as the set holds them: the bytes of a file of version 0x402, or else the bank's.
std::vector<uint8_t> HeldBankBytes(const song::DseBank& file);

// The bytes of the bank file `file` once it holds `model`. A file of version 0x415 takes model.Write(). A file of
// version 0x402 keeps its layout and its other bytes where `model` has the same tables (ntdlib::dse::OldestBank).
// Throws std::invalid_argument with a message for the user for a value that version 0x402 can't hold, such as a sample
// rate above 65,535 Hz.
std::vector<uint8_t> BankFileBytes(const song::DseBank& file, const ntdlib::dse::WaveBank& model);

// Moves the tuning of each split of `model` that plays wave `wave` by `moved` 256ths of a semitone, for a new sample
// rate. Throws std::invalid_argument with a message for the user for a tuning out of range.
void RetuneSplits(ntdlib::dse::WaveBank& model, uint32_t wave, double moved);

} // namespace hokuto::edit::dse_files_parts
