// SPDX-License-Identifier: MIT

// The names, texts and parsers that the files of the List Editor's model share (list_edits_text.cpp,
// list_edits_typing.cpp and list_edits_rows.cpp). The names are internal to them.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"

#include "app/list_edits.h"
#include "song/archive.h"

namespace hokuto::app::list_edits_parts
{

using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::Unit;

// The number of the LFOs, the buses that a track sends to, and the tracks.
inline constexpr int kLfos = 4;
inline constexpr int kBuses = 3;
inline constexpr int kTracks = 16;

// What the first data value of a command names.
enum class Addressed
{
    kNothing,
    kBus,
    kLfo,
    kVariable,
};

// What the first data value of a command of `kind` names.
Addressed AddressedBy(Kind kind);

// The column that holds the value of a command of `kind`, or nothing for a kind whose value shows elsewhere or not at
// all.
std::optional<ListColumn> ValueColumn(Kind kind);

// The suffix of a value of `unit`, such as " cents", or nothing.
std::string_view UnitSuffix(std::optional<Unit> unit);

// A length of `ticks` in the display's form, at `ticks_per_quarter`.
std::string TicksText(ntdlib::commands::Fixed ticks, int ticks_per_quarter);

// The suffix of a value of a command of `kind` on `console`.
std::string_view UnitSuffix(const song::Console& console, Kind kind);

// The edit that gives row `index` the command `command`, or the refusal of its argument.
ListEdit ChangeTo(std::size_t index, std::string name, Command command,
                  std::optional<std::string> refused = std::nullopt);

// The comment of a label's row: what goes to the label's item, such as "2 calls, 1 jump".
std::string EntriesText(const ListTrack& track, uint32_t item);

// The labels of `track`'s sequence, without its commands, for edit::LabelRefusal and edit::UniqueLabel.
ntdlib::commands::Sequence LabelNames(const ListTrack& track);

// The item of the command of row `index`, or nothing for a command that the sequence's offsets don't hold.
std::optional<uint32_t> ItemOfRow(const ListTrack& track, std::size_t index);

} // namespace hokuto::app::list_edits_parts
