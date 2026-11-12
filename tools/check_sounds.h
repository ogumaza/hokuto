// SPDX-License-Identifier: MIT

// The parts of hokuto_check_sounds that its files share: the rounds, the totals, the original archive and the changes
// that the checks find. check_sounds.cpp describes the checks.

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/layout.h"
#include "ntdlib/testing/sar_tools.h"
#include "xsflib/bytes.h"
#include "xsflib/console.h"
#include "xsflib/files.h"

#include "checked_archive.h"
#include "checked_saves.h"
#include "edit/tables.h"
#include "format/archive.h"
#include "format/file.h"
#include "song/archive.h"
#include "song/sequence.h"
#include "song/sequence_model.h"

namespace hokuto::tools::sounds
{

namespace sar = ntdlib::sar;
using hokuto::edit::SoundChange;
using hokuto::edit::SoundValue;
using hokuto::format::File;
using hokuto::format::NativeSound;
using hokuto::format::NativeTables;

// A round's value. A round of a bank names its slot: 0, or kAfterList for the slot after each sound's list.
struct Round
{
    SoundValue value = SoundValue::kVolume;
    int slot = 0;
};

constexpr int kAfterList = -1;
constexpr std::array<Round, 11> kRounds = {{
    {SoundValue::kPlayer, 0},
    {SoundValue::kVolume, 0},
    {SoundValue::kPanMode, 0},
    {SoundValue::kPanCurve, 0},
    {SoundValue::kPlayerPriority, 0},
    {SoundValue::kActorPlayer, 0},
    {SoundValue::kBank, 0},
    {SoundValue::kBank, kAfterList},
    {SoundValue::kTracks, 0},
    {SoundValue::kChannelPriority, 0},
    {SoundValue::kReleasePriorityFix, 0},
}};

// The sounds that a round edits at most, and the sounds of the volume's round that the model plays at most.
constexpr std::size_t kRoundSounds = 64;
constexpr std::size_t kLevelSounds = 8;

// How long the model plays each sound, the least level of a sound's output that the tool measures, and how far the
// change of level can stray from the ratio of the volumes, in dB.
constexpr double kLevelSeconds = 4.0;
constexpr double kLeastLevel = 30.0;
constexpr double kTolerance = 0.25;

// What the check counted, for the summary.
struct Totals
{
    std::size_t sounds = 0;
    std::size_t sequences = 0;
    std::size_t edits = 0;
    std::size_t lists_apart = 0;
    std::size_t differ = 0;
    std::size_t round_trips = 0;
    std::size_t places = 0;
    std::size_t old_values = 0;
    std::size_t next_values = 0;
    std::size_t place_edits = 0;
    std::size_t place_layouts = 0;
    std::size_t moved = 0;
    std::size_t layouts = 0;
    std::size_t levels = 0;
    std::size_t clipped = 0;
    std::size_t quiet = 0;
    std::size_t wrong_levels = 0;
    std::size_t limited = 0;
    double worst = 0.0;
    std::size_t refused = 0;
    std::size_t failures = 0;
};

// The sequences of the files of an archive that a file builds, and the fields of its sounds' starts.
struct Layout
{
    friend bool operator==(const Layout&, const Layout&) = default;

    std::vector<std::optional<uint32_t>> sequences;
    std::vector<std::optional<uint32_t>> fields;
};

// The output of a sequence sound of an archive for kLevelSeconds on ntdlib's model, by the sound's place in the table,
// and the same at another volume of the sound.
using Player = std::function<std::vector<int16_t>(uint32_t)>;
using VolumePlayer = std::function<std::vector<int16_t>(uint32_t, uint32_t)>;

// Up to kRoundSounds of `candidates`, spread over them: one from each of kRoundSounds equal slices. `index` turns the
// choice within each slice from round to round.
template <typename Candidate>
std::vector<Candidate> Spread(const std::vector<Candidate>& candidates, std::size_t index)
{
    const std::size_t size = candidates.size();
    if (size <= kRoundSounds)
    {
        return candidates;
    }

    std::vector<Candidate> chosen;
    const std::size_t turn = index % (size / kRoundSounds);
    for (std::size_t k = 0; k < kRoundSounds; k++)
    {
        chosen.push_back(candidates[k * size / kRoundSounds + turn]);
    }

    return chosen;
}

// Whether `places` holds each option of `option`.
bool Holds(const sar::ValuePlaces& places, const sar::ValuePlaces& option);

// The change of `sound`, a sound of `tables`, in round `round`, or nothing for a sound without the value, or without
// another value to take.
std::optional<SoundChange> ChangeOf(const NativeTables& tables, const NativeSound& sound, const Round& round);

// `sound` after `change`. The tool works the values out by itself. A bank goes into its slot, or after the list's last
// slot.
NativeSound Changed(NativeSound sound, const SoundChange& change);

// Checks the levels of up to kLevelSounds sequence sounds of `changes` that played as `before` plays them and play as
// `after` plays them, from an archive with their new volumes. `at_volume` plays a sound at another volume.
void CheckLevels(const std::string& name, const File& file, const Player& before, const Player& after,
                 const VolumePlayer& at_volume, const std::vector<std::pair<uint32_t, SoundChange>>& changes,
                 Totals& totals);

// Whether an archive that holds `held` holds the value of `round`. The slot after a sound's list counts as a second
// slot.
bool Holds(const hokuto::edit::HeldValues& held, const Round& round);

// Checks the rounds of `bytes`, an archive of the Wii named `name`, and prints the summary. Returns the tool's code.
int CheckWiiArchive(const std::string& name, std::vector<uint8_t> bytes);

} // namespace hokuto::tools::sounds
