// SPDX-License-Identifier: MIT

// A sequence's biquad filters on another console. Every console's player takes a biquad filter's coefficients from the
// same presets (ntdlib::nwsnd::kBiquadPresets): types 1 to 5, a low-pass, a high-pass and band-passes at 512, 1024 and
// 2048 Hz, made for a renderer at 32 kHz. Each value of a preset selects a row of its table. The Wii and the Wii U mix
// at 32 kHz, and the 3DS at about 32,728 Hz. The Switch mixes at 48 kHz, and the same coefficients filter 1.5 times
// higher there. The Switch adds the same filters made for 48 kHz as types 6 to 10, with the same rows. A track starts
// with type 0. Type 0 filters nothing. A game registers the types above the presets.
//
// A sequence that goes to the Switch takes types 6 to 10 in the place of types 1 to 5, with the same values. One that
// leaves the Switch takes types 1 to 5 in the place of types 6 to 10. Its types 1 to 5 keep their type and take other
// values. Each such value selects the row whose passband lies nearest the one that the Switch gives the value: the
// edges 3 dB below the passband's peak, in octaves, from 20 Hz to about 14.9 kHz (NearestBiquadValue). A low-pass keeps
// its cutoff within 0.04 octaves that way. A high-pass keeps its cutoff within 0.06 octaves up to the table's highest,
// about 6.5 kHz at 32 kHz. No value of a band-pass moves its band: its peak lies 0.58 octaves lower on the Wii and the
// Wii U, and 0.55 octaves lower on the 3DS. Its edges lie as near the Switch's as the table allows.
//
// A value takes another only where the analysis of the sequence's flow finds one type of 1 to 5 that it can play with,
// besides type 0 (BiquadTypesOf). A type or a value from a variable stays as it is. A random value takes other ends.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>

#include "ntdlib/commands/sequence.h"

#include "song/archive.h"

namespace hokuto::song
{

// What MapBiquads changed: the settings of a type that took another type, and the settings of a value that took another
// value. `band_passes` counts the settings of a band-pass that keep their type on the way from the Switch. No value
// moves a band-pass's band. `kept` counts the settings that keep a filter of types 1 to 5 that the other console plays
// at another rate: a type from a variable or a random number, and a value that plays with more than one type.
struct BiquadChanges
{
    std::size_t types = 0;
    std::size_t values = 0;
    std::size_t band_passes = 0;
    std::size_t kept = 0;
};

// The types of biquad filter that each value setting of `sequence` can play with, by the setting's item: 0 for the type
// that a track starts with, and -1 for a type from a variable or a random number. Tracks start at the items of
// `entries`, at the sequence's labels, at its first item and where its commands open tracks. A value plays with each
// type that a track can set until another value takes its place. A call can come back from each return that its
// subroutine reaches, and a loop's end can go back to each loop's start that reaches it. A return outside every call
// runs on. Nothing when the analysis would take more than `most` pairs of a type and a value.
std::optional<std::map<uint32_t, std::set<int>>> BiquadTypesOf(const ntdlib::commands::Sequence& sequence,
                                                               std::span<const uint32_t> entries,
                                                               std::size_t most = 4'000'000);

// The value from 1 to 127 of a biquad filter of preset `type`, from 1 to 5, whose passband on `to` lies nearest the one
// that the Switch gives `value`, from 1 to 127. Of the values whose passbands lie as near, the one nearest `value`. Any
// other type or value, and any value for the Switch, stays as it is.
uint8_t NearestBiquadValue(int type, uint8_t value, const Console& to);

// `sequence`, a sequence of `from`, with its biquad filters as `to` plays them. Tracks start at the items of `entries`,
// and where BiquadTypesOf starts them. A sequence between two consoles other than the Switch stays as it is.
BiquadChanges MapBiquads(ntdlib::commands::Sequence& sequence, std::span<const uint32_t> entries, const Console& from,
                         const Console& to);

// The warning for the user about `changes`, on the way from `from` to `to`, or nothing for no change.
std::optional<std::string> BiquadWarning(const BiquadChanges& changes, const Console& from, const Console& to);

// Whether `a` and `b` are the same command, but for the type or the value of a biquad filter's setting: a sequence that
// goes to another console and back can take other types and values there. A fixed value can differ, and so can a random
// value's ends. A variable can't.
bool SameButBiquads(const ntdlib::commands::Command& a, const ntdlib::commands::Command& b);

// Whether `a` and `b` hold the same commands and labels, but for the types and values of biquad filters' settings.
bool SameButBiquads(const ntdlib::commands::Sequence& a, const ntdlib::commands::Sequence& b);

} // namespace hokuto::song
