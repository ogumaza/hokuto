// SPDX-License-Identifier: MIT

// What the writer and the assembler of GBA songs and voice groups share (song/gba_assembly.h). Only gba_assembly.cpp
// and gba_assembler.cpp include it.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

#include "ntdlib/mp2k/song.h"

namespace hokuto::song::gba_assembly_parts
{

inline constexpr int kLengthCodes = 49; // ClockLength's codes, from 0 to 48
inline constexpr uint8_t kCentre = 0x40;
inline constexpr uint8_t kHighestVolume = 0x7F;
inline constexpr uint8_t kReverbSet = 0x80;
inline constexpr std::size_t kBytesOnALine = 16;
inline constexpr std::size_t kVoiceBytes = ntdlib::mp2k::kVoiceSize;
inline constexpr uint32_t kKeys = 128;
inline constexpr uint32_t kPatternBytes = 16;

// The names of the driver's other commands.
inline constexpr std::array<std::pair<uint8_t, const char*>, 22> kCommandNames = {
    {{0xB1, "FINE"}, {0xB2, "GOTO"},  {0xB3, "PATT"},  {0xB4, "PEND"},  {0xB5, "REPT"}, {0xB9, "MEMACC"},
     {0xBA, "PRIO"}, {0xBB, "TEMPO"}, {0xBC, "KEYSH"}, {0xBD, "VOICE"}, {0xBE, "VOL"},  {0xBF, "PAN"},
     {0xC0, "BEND"}, {0xC1, "BENDR"}, {0xC2, "LFOS"},  {0xC3, "LFODL"}, {0xC4, "MOD"},  {0xC5, "MODT"},
     {0xC8, "TUNE"}, {0xCD, "XCMD"},  {0xCE, "EOT"},   {0xCF, "TIE"}}};

inline constexpr std::array<const char*, 12> kKeyNames = {"Cn", "Cs", "Dn", "Ds", "En", "Fn",
                                                          "Fs", "Gn", "Gs", "An", "As", "Bn"};
inline constexpr std::array<const char*, 3> kModTargets = {"mod_vib", "mod_tre", "mod_pan"};
inline constexpr std::array<const char*, 18> kMemoryOperations = {
    "mem_set",     "mem_add",     "mem_sub",     "mem_mem_set", "mem_mem_add", "mem_mem_sub",
    "mem_beq",     "mem_bne",     "mem_bhi",     "mem_bhs",     "mem_bls",     "mem_blo",
    "mem_mem_beq", "mem_mem_bne", "mem_mem_bhi", "mem_mem_bhs", "mem_mem_bls", "mem_mem_blo"};

// The names of keys and numbers in the text, and a little-endian word of `bytes` at `at`.
std::string KeyName(int key);
std::string TwoDigits(int value);
std::string ThreeDigits(int value);
uint32_t ReadWord(std::span<const uint8_t> bytes, std::size_t at);

} // namespace hokuto::song::gba_assembly_parts
