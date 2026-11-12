// SPDX-License-Identifier: MIT

// The parts of hokuto_check_driver_playback that its files share: the totals, the reference, and the functions that
// one file calls in another. check_driver_playback.cpp describes the checks.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/brownie/archive_player.h"
#include "ntdlib/brownie/commands.h"
#include "ntdlib/brownie/song.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/konami/archive_player.h"
#include "ntdlib/konami/commands.h"
#include "ntdlib/krawall/archive_player.h"
#include "ntdlib/krawall/commands.h"
#include "ntdlib/krawall/player.h"
#include "ntdlib/quintet/archive_player.h"
#include "ntdlib/quintet/commands.h"
#include "ntdlib/rare/archive_player.h"
#include "ntdlib/rare/commands.h"
#include "ntdlib/rare/driver.h"
#include "ntdlib/rd2/archive_player.h"
#include "ntdlib/rd2/commands.h"
#include "ntdlib/rd2/driver.h"
#include "ntdlib/ubimilan/archive_player.h"
#include "ntdlib/ubimilan/commands.h"
#include "xsflib/binary.h"
#include "xsflib/console.h"
#include "xsflib/files.h"
#include "xsflib/rip/length.h"

#include "check_playback_steps.h"
#include "format/brownie_items.h"
#include "format/drivers.h"
#include "format/quintet.h"
#include "format/ubimilan.h"
#include "song/brownie.h"
#include "song/driver.h"
#include "song/konami.h"
#include "song/krawall.h"
#include "song/player.h"
#include "song/quintet.h"
#include "song/rare.h"
#include "song/rd2.h"
#include "song/sequence.h"
#include "song/sequence_model.h"
#include "song/ubimilan.h"

namespace hokuto::tools::driver_playback
{

namespace song = hokuto::song;

// What ntdlib's player produces when it runs directly: the output in the player's units.
struct Reference
{
    std::vector<float> samples; // at a full scale of 1
};

struct Totals : playback_steps::StepTotals
{
    std::size_t sounds = 0;
    std::size_t looped = 0;
    std::size_t unread = 0;
    std::size_t failed = 0;
    std::size_t mutes = 0;
    std::size_t meters = 0;
    std::size_t commands = 0;    // the note commands that the tracks ran in the arrangement's ticks
    std::size_t lost = 0;        // those without a note of the arrangement at their tick
    std::size_t unmatched = 0;   // stretches before the loop's end whose commands the decoded offsets don't find
    std::size_t muted = 0;       // the note commands of tracks that the sound's version mutes
    std::size_t tied = 0;        // the note commands that a tie joins to the note before them
    std::size_t framed = 0;      // the note commands with a note of the arrangement within a frame of their tick
    std::size_t zero_volume = 0; // Quintet's note commands without a note of the arrangement, at volume 0
    std::size_t levels = 0;      // sounds whose reference holds a level other than 0 after the output's end
    float largest_level = 0.0f;  // the largest such level
    std::size_t handed = 0;      // Brownie Brown's sounds whose muted track's meter isn't 0 in the reference either
    std::size_t late = 0;        // Brownie Brown's note commands whose notes sound in a later frame
    std::size_t silent = 0;      // Brownie Brown's note commands without a note in tracks that never sound
    std::size_t krawall = 0;     // Krawall's sounds whose note commands count
    std::size_t slides = 0;      // Krawall's note commands without a note that slide to the note
    std::size_t unsampled = 0;   // Krawall's note commands without a note on a channel without a sample
    std::size_t delayed = 0;     // Krawall's note commands without a note whose delay reaches past the row
    std::size_t unheard = 0;     // Krawall's note commands without a note whose voice stays at level 0
    std::size_t heard = 0;       // Krawall's note commands without a note whose voice reaches a level
};

// The driver's ArchivePlayer in ntdlib, run directly.
class ReferencePlayer
{
public:
    virtual ~ReferencePlayer() = default;
    virtual void Start(uint32_t sound, bool enhanced) = 0;
    virtual void MuteTrack(int track) = 0;
    virtual void MeterTracks() = 0;
    virtual void RunFrame(std::vector<int16_t>& out) = 0;
    virtual void RunFrame(std::vector<float>& out) = 0;
    virtual std::array<int32_t, 2> TrackPeaks(int track) const = 0;
};

using Peaks = std::vector<std::array<int32_t, 2>>;

Peaks ReferencePeaks(const song::DriverRom& rom, uint32_t sound, bool enhanced, std::size_t samples, int tracks,
                     int muted = -1);

// The reference of `rom`'s driver.
std::unique_ptr<ReferencePlayer> MakeReference(const song::DriverRom& rom);

// The commands of sound `sound`'s sequence as the driver's bytes hold them from the sequence's start, and the offset of
// each item.
std::pair<ntdlib::commands::Sequence, std::vector<uint32_t>> DecodedCommands(const song::DriverRom& rom,
                                                                             uint32_t sound);

// Runs the reference, with track `muted` muted where it isn't -1.
Reference RunReference(const song::DriverRom& rom, uint32_t sound, bool enhanced, std::size_t samples, int muted = -1);

// Counts the note commands of the tracks' stretches that run in the arrangement's ticks, and those without a note of
// the arrangement at their tick. A track that the sound's version mutes plays no notes, and its commands count apart.
// On Brownie Brown's driver, a track whose meter stays at 0 in the reference up to the arrangement's end makes no
// sound: a wave channel that NR32 mutes, for one. Its commands without a note count apart too. On Quintet's driver, a
// note and a rest wait for their lengths. On Krawall, the commands without a note count apart by what ntdlib's Player
// does with their entries. The tool prints a line under `name` for a sound with a command whose voice reaches a level.
// `sample_rate` is the rate of the reference's output.
void CountCommands(const std::string& name, const song::DriverRom& rom, const song::Sequence& sequence, bool enhanced,
                   uint32_t sample_rate, Totals& totals);

} // namespace hokuto::tools::driver_playback
