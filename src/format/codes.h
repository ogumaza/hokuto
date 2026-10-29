// SPDX-License-Identifier: MIT

// The numbers and names that a .hok file gives its kinds of commands, its engines, its effects, its native archives and
// the kinds of sound in their tables. The binary form stores the numbers and the text form the names. Neither ever
// changes: a new kind, engine, effect or archive takes a new number. A kind of command's name is ntdlib's
// (ntdlib::commands::Name). An effect's parameters are named as ntdlib names them.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/sar/formats.h"

#include "format/file.h"

namespace hokuto::format
{

// The number of a kind of command, and the kind of a number.
uint16_t CodeOf(ntdlib::commands::Kind kind);
std::optional<ntdlib::commands::Kind> KindOfCode(uint16_t code);

// The number and the name of an engine, and the engine of each.
uint8_t CodeOf(Engine engine);
std::optional<Engine> EngineOfCode(uint8_t code);
std::string_view NameOf(Engine engine);
std::optional<Engine> EngineNamed(std::string_view name);

// The number and the name of a mixing mode, and the mode of each.
uint8_t CodeOf(Mixing mixing);
std::optional<Mixing> MixingOfCode(uint8_t code);
std::string_view NameOf(Mixing mixing);
std::optional<Mixing> MixingNamed(std::string_view name);

// The number and the name of a kind of native archive, by the flavor of ntdlib's reader, and the flavor of each.
// ntdlib's reader gives the Wii's archives no flavor. They take the number 4 and the name wii.
uint8_t CodeOf(ntdlib::sar::Flavor flavor);
std::optional<ntdlib::sar::Flavor> FlavorOfCode(uint8_t code);
std::string_view NameOf(ntdlib::sar::Flavor flavor);
std::optional<ntdlib::sar::Flavor> FlavorNamed(std::string_view name);
inline constexpr uint8_t kWiiArchiveCode = 4;
inline constexpr std::string_view kWiiArchiveName = "wii";

// The number and the name of a kind of effect, the kind of each, and the names of its parameters in their order.
uint8_t CodeOf(EffectKind kind);
std::optional<EffectKind> EffectKindOfCode(uint8_t code);
std::string_view NameOf(EffectKind kind);
std::optional<EffectKind> EffectKindNamed(std::string_view name);
std::span<const std::string_view> ParameterNamesOf(EffectKind kind);

// The number of choices of parameter `parameter` of `kind` when it's a mode, or 0. A mode's value is a whole number
// from 0 to one less than its choices. A reverb of the Wii U, and a reverb of the Wii of 2008, has two modes: the early
// reflections' 8 delays, from 5 ms to 40 ms, and the fused reverb's 6 rooms. The 3DS's surround and high-pass damping
// are switches: 0 for off and 1 for on.
uint32_t ChoicesOf(EffectKind kind, std::size_t parameter);

// Whether `kind` is an effect of the 3DS.
bool IsCtrKind(EffectKind kind);

// The values that a parameter takes: from `low` to `high`, and a whole number of steps from `low` where `step` isn't 0.
struct ParameterLimits
{
    float low;
    float high;
    float step;
};

// The limits of parameter `parameter` of `kind`, or nothing. Only the 3DS's effects have limits here, apart from their
// switches. A reverb's early reflection time and pre-delay time are whole milliseconds from 0 to 1,000, and its fused
// time from 0 to 20,000. A delay's time is a whole number of milliseconds from 1 to 5,000. A filter's size is a whole
// number of samples, a multiple of 160 from 160 to 32,000. Each gain and factor runs from 0 to 1.
std::optional<ParameterLimits> LimitsOf(EffectKind kind, std::size_t parameter);

// The number and the name of a kind of sound in an archive's tables, and the kind of each.
uint8_t CodeOf(ntdlib::sar::SoundType type);
std::optional<ntdlib::sar::SoundType> SoundTypeOfCode(uint8_t code);
std::string_view NameOf(ntdlib::sar::SoundType type);
std::optional<ntdlib::sar::SoundType> SoundTypeNamed(std::string_view name);

} // namespace hokuto::format
