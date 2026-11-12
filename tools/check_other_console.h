// SPDX-License-Identifier: MIT

// The parts of hokuto_check_other_console that its files share: the plays of a sound on each console and the
// choices of the sounds to check. check_other_console.cpp describes the checks.

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sound_build.h"
#include "xsflib/console.h"
#include "xsflib/files.h"

#include "check_part.h"
#include "check_support.h"
#include "edit/bytecode.h"
#include "edit/ds_files.h"
#include "edit/other_console.h"
#include "edit/replace.h"
#include "format/archive.h"
#include "format/codes.h"
#include "format/drivers.h"
#include "format/file.h"
#include "format/text.h"
#include "measure/measure.h"
#include "song/archive.h"
#include "song/audibility.h"
#include "song/driver.h"
#include "song/dry_render.h"
#include "song/ds.h"
#include "song/dse.h"
#include "song/gba.h"
#include "song/items.h"
#include "song/konami.h"
#include "song/midi_export.h"
#include "song/sequence.h"
#include "song/streams.h"

namespace hokuto::tools::other_console
{

namespace edit = hokuto::edit;
namespace format = hokuto::format;
namespace sar = ntdlib::sar;
namespace song = hokuto::song;
using hokuto::tools::SpreadIndices;

// What ntdlib's performer reads of a sound, and the sound's render.
struct Played
{
    std::shared_ptr<const song::Sequence> sequence;
    song::DryRender render;
};

// What the tool searches for in a sound's commands.
using Test = std::function<bool(const ntdlib::commands::Command&)>;

// Sound `sound` of `archive`, played dry for `seconds` seconds.
Played Play(const std::shared_ptr<const sar::SoundArchive>& archive, uint32_t sound, double seconds);

// Sound `sound` of the Wii's `archive`, played dry for `seconds` seconds by the rules of `build` of NW4R SND.
Played PlayWii(const std::shared_ptr<const ntdlib::rsar::SoundArchive>& archive, uint32_t sound, double seconds,
               const ntdlib::SndBuild& build);

// Sequence `sound` of the DS's `archive`, played dry for `seconds` seconds.
Played PlayDs(const std::shared_ptr<const song::DsArchive>& archive, uint32_t sound, double seconds,
              song::Mixing mixing = song::Mixing::kStrict);

// Sound `sound` of DSE's `set`, played dry for `seconds` seconds.
Played PlayDse(const std::shared_ptr<const song::DseSet>& set, uint32_t sound, double seconds);

// Song `number` of the GBA's `rom`, played dry for `seconds` seconds.
Played PlayGba(const std::shared_ptr<const song::GbaRom>& rom, uint32_t number, double seconds);

// Sound `sound` of the GBA's `rom` with another of the GBA's drivers, played dry for `seconds` seconds.
Played PlayDriver(const std::shared_ptr<const song::DriverRom>& rom, uint32_t sound, double seconds);

// The largest difference in milliseconds between the times of the notes that `gba`, a sound of the GBA or the DS,
// starts in its first `seconds` and those of `sent`, or nothing when `sent` starts other notes. The notes count in the
// order of their times, with their keys and levels. The two performers read other numbers of passes of a loop: `sent`
// counts as many notes as `gba`. MP2K's velocity scales a note's level, and NintendoWare's engines scale it by the
// square of the velocity. With `squared`, a note of `sent` compares with the velocity whose square comes nearest the
// velocity on the GBA. The DS's velocities compare as they are.
std::optional<double> GbaNotesOff(const song::Sequence& gba, const song::Sequence& sent, double seconds, bool squared);

// Whether `a` and `b` start the same notes: each track's notes at their ticks, with their keys, velocities and
// instruments. The notes' lengths don't count: a wave that ends sooner than its note ends the note in another frame.
bool SameNotes(const song::Sequence& a, const song::Sequence& b);

// `sequence` with the banks of several slots in one bank. The Wii plays a sound with `slot_programs` this way
// (edit::OtherConsoleSong): a note of slot s and program p plays program slot_programs[s] + p of slot 0. The notes of a
// slot without a range stay as they are.
song::Sequence InOneBank(song::Sequence sequence, const std::map<int, uint32_t>& slot_programs);

// Whether `command` draws a random number.
bool Draws(const ntdlib::commands::Command& command);

// Whether `command` sets a biquad filter's type or value.
bool SetsBiquad(const ntdlib::commands::Command& command);

// Whether a command that sound `sound` of `game` reaches passes `test`. `where` is the sound's place in `file`. A sound
// whose file stays in the archive's bytes has no place. Its commands then come from the file's bytes. Replace… reads
// them the same way. A sound of the Wii without a place reaches nothing: `game` is null then.
bool SoundReaches(const sar::SoundArchive* game, const format::File& file, const std::optional<edit::SoundPlace>& where,
                  uint32_t sound, const Test& test);

// The sequence sounds of `file` that the tool checks: those of `names`, or else up to `most`, spread evenly, or all of
// them with 0. Only the sounds that `keep` keeps count, when it's given.
std::vector<uint32_t> Chosen(const format::File& file, std::size_t most, const std::set<std::string>& names,
                             const std::function<bool(uint32_t)>& keep);

// The songs of the GBA's `rom` that the tool checks, among the songs that play. Chosen chooses sequence sounds the same
// way.
std::vector<uint32_t> GbaChosen(const song::GbaRom& rom, std::size_t most, const std::set<std::string>& names);

// The sounds of the GBA's `rom` with another of the GBA's drivers that the tool checks. Chosen chooses sequence sounds
// the same way.
std::vector<uint32_t> DriverChosen(const song::DriverRom& rom, std::size_t most, const std::set<std::string>& names);

// The sequences of the DS's `archive` that the tool checks. Chosen chooses sequence sounds the same way.
std::vector<uint32_t> DsChosen(const song::DsArchive& archive, std::size_t most, const std::set<std::string>& names);

// The sounds of DSE's `set` that the tool checks. Chosen chooses sequence sounds the same way.
std::vector<uint32_t> DseChosen(const song::DseSet& set, std::size_t most, const std::set<std::string>& names);

} // namespace hokuto::tools::other_console
