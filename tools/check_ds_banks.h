// SPDX-License-Identifier: MIT

// The parts of hokuto_check_ds_banks that its files share: the places, the totals, the game and the rounds that
// check_ds_banks.cpp runs. check_ds_banks.cpp describes the checks.

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/ntr/snd/tables.h"
#include "ntdlib/ntr/sound.h"
#include "ntdlib/sdat/decode.h"
#include "ntdlib/sdat/files.h"
#include "ntdlib/sdat/formats.h"
#include "xsflib/console.h"
#include "xsflib/files.h"

#include "app/item_views.h"
#include "app/key_edits.h"
#include "check_support.h"
#include "edit/ds_banks.h"
#include "edit/ds_files.h"
#include "edit/ds_waves.h"
#include "format/archive.h"
#include "format/file.h"
#include "format/text.h"
#include "song/dry_render.h"
#include "song/ds.h"
#include "song/preview.h"
#include "song/sequence.h"

namespace hokuto::tools::ds_banks
{

namespace sdat = ntdlib::sdat;
using hokuto::edit::DsRegionPlace;
using hokuto::format::File;
using hokuto::song::DsArchive;
using hokuto::tools::HeapFull;
using hokuto::tools::RenderedDs;
using enum sdat::InstrumentType;

// The values of a region that the edits move, wrapping past this.
constexpr int kHighest = 127;
constexpr int kHighestDuty = 7;

// The seconds of each render of a sound on the console's model.
constexpr double kPlaySeconds = 3.0;

// A region that the Bank Editor shows, at the first place that shows it: its name, its place, the key that the keyboard
// plays there, its kind and values, where the archive stores its values and its kind, whether an instrument of one
// region at another program plays the same bytes, and the waves in the slots of its bank.
struct Place
{
    std::string name;
    DsRegionPlace place;
    int key = 60;
    sdat::Region region;
    std::size_t values = 0;
    std::size_t kind = 0;
    bool shared = false;
    std::vector<hokuto::app::DsWaveChoice> waves;
};

// What the check counted, for the summary.
struct Totals
{
    std::size_t archives = 0;
    std::size_t banks = 0;
    std::size_t unread = 0; // banks that the Bank Editor can't show
    std::size_t regions = 0;
    std::size_t edits = 0;
    std::size_t refused = 0;
    std::size_t kinds_kept = 0; // new kinds that the edit refuses as the tool expects
    std::size_t kept = 0;       // values that the edits leave as the archive stores them
    std::size_t levels = 0;
    std::size_t ending = 0;       // waves that don't loop
    std::size_t unread_holds = 0; // waves whose wave archive's file ntdlib can't read
    std::size_t past_range = 0;   // regions whose attack or decay is past the driver's tables
    std::size_t slow = 0;         // notes that reach their sustains too late
    std::size_t unsteady = 0;
    std::size_t ended = 0;
    std::size_t quiet = 0;
    std::size_t clipped = 0;
    double worst = 0.0; // the largest difference from the sustains' levels, in dB
    std::size_t wave_files = 0;
    std::size_t wave_edits = 0;
    std::size_t waves_apart = 0;  // waves of one word of samples, and waves that can't be decoded
    std::size_t unread_waves = 0; // wave archives' files that ntdlib can't read
    std::size_t structure_files = 0;
    std::size_t key_splits = 0;  // instruments of one region that became key splits
    std::size_t split_runs = 0;  // key splits' runs that split
    std::size_t same_sounds = 0; // sequences that played the same
    std::size_t silent_sounds = 0;
    std::size_t too_large = 0;     // sequences whose files no longer fit the DS's memory
    std::size_t unplayed = 0;      // sequences that didn't play before the edit
    std::size_t files_shared = 0;  // archives that share their file in the image with other data
    std::size_t copies = 0;        // copies that played as their instruments
    std::size_t copies_silent = 0; // of those, the ones that made no sound
    std::size_t copies_apart = 0;  // instruments that a preview can't play
    std::size_t drops = 0;         // waves dropped on a bank that played as they should
    std::size_t drops_silent = 0;  // of those, the ones that made no sound
    std::size_t drops_apart = 0;   // instruments that a preview can't play
    std::size_t failed = 0;
};

// The game's archive as a file and as bytes, the banks that the Bank Editor shows, and the regions that it shows, by
// the offset of their values in the archive.
struct Game
{
    std::string name;
    File file;
    std::vector<uint8_t> bytes;
    std::shared_ptr<const DsArchive> archive;
    std::vector<uint64_t> starts; // the starts of the file's pieces: the edits keep them
    std::vector<uint32_t> banks;
    std::map<std::size_t, Place> places;
};

// The region at `place` of `archive` as ntdlib reads it.
sdat::Region RegionAt(const DsArchive& archive, const DsRegionPlace& place);

// The round of envelopes.
std::map<std::size_t, sdat::Region> Envelopes(const Game& game, Totals& totals);

// The round of root keys and pans.
std::map<std::size_t, sdat::Region> KeysAndPans(const Game& game, Totals& totals);

// The round of waves and duties. A wave that the slots of the bank don't contain becomes their first wave.
std::map<std::size_t, sdat::Region> WavesAndDuties(const Game& game, Totals& totals);

// The round of kinds, and the regions whose edits the tool expects the edit to refuse.
std::map<std::size_t, sdat::Region> Kinds(const Game& game, std::set<std::size_t>& refusing);

// Checks that both forms of `edited`, a version of the game's file, export `exported`. A failure counts in the round
// `round`.
void CheckForms(const Game& game, const File& edited, const std::vector<uint8_t>& exported, const char* round,
                Totals& totals);

// Makes the edits of a round in a version of the game file, and checks the exported archive against the archive of the
// game. The edits of `refusing` have to be refused, and the others taken. Erases the refused edits from `edits`.
// Returns the archive that the version builds, or nothing when the round failed.
std::shared_ptr<const DsArchive> Round(const Game& game, const char* round, std::map<std::size_t, sdat::Region>& edits,
                                       const std::set<std::size_t>& refusing, Totals& totals);

// The round of waves. Each wave in the file of each wave archive gets three quarters of its rate, at least 256 Hz, and
// a loop from the word at the middle of its samples. The tool works out the parameters' bytes by the format's rules:
// the timer is 16,756,991 / the rate, rounded down, and the loop's start and length count words. The export has to hold
// those bytes and no other change. ntdlib has to read the new values, and each wave has to decode to its samples before
// the edit. The file has to come back from both its forms.
void Waves(const Game& game, Totals& totals);

// The round of sustains, and the levels of the held notes before and after it.
void Levels(const Game& game, Totals& totals);

// The round of structure.
void Structure(const Game& game, Totals& totals);

// Up to kMostCopies bank files each take a copy of an instrument at a free program.
void Copies(const Game& game, Totals& totals);

// Up to kMostDrops banks of other wave archives each take a dropped wave.
void Drops(const Game& game, Totals& totals);

} // namespace hokuto::tools::ds_banks
