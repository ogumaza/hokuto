// SPDX-License-Identifier: MIT

// The table of the edits of the GBA's other drivers: one row for each driver, in the order of format::kEngineFacts.

#include "edit/drivers.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"

#include "edit/brownie.h"
#include "edit/konami.h"
#include "edit/krawall.h"
#include "edit/quintet.h"
#include "edit/rare.h"
#include "edit/rd2.h"
#include "edit/ubimilan.h"
#include "format/engine.h"
#include "song/archive.h"
#include "song/driver.h"
#include "song/sequence.h"

namespace hokuto::edit
{

namespace
{

// One of the GBA's other drivers: its engine and its edits.
struct EditsRow
{
    format::Engine engine;
    const DriverEdits& (*edits)();
};

// A row for each of the GBA's other drivers, in the order of format::kEngineFacts.
constexpr std::array<EditsRow, song::kDriverEngineCount> kEditsRows = {{
    {format::Engine::kKonami, &KonamiEdits},
    {format::Engine::kRd2, &Rd2Edits},
    {format::Engine::kBrownie, &BrownieEdits},
    {format::Engine::kUbiMilan, &UbiMilanEdits},
    {format::Engine::kRare, &RareEdits},
    {format::Engine::kQuintet, &QuintetEdits},
    {format::Engine::kKrawall, &KrawallEdits},
}};

static_assert(song::RowsFollowDriverEngines(kEditsRows),
              "kEditsRows needs one row for each driver engine of kEngineFacts, in that order");

} // namespace

FixedTracksEdits::FixedTracksEdits(const char* added, const char* pasted) : added_(added), pasted_(pasted)
{
}

TrackEdit FixedTracksEdits::AddTrack(const format::File& /*file*/,
                                     std::span<const std::optional<SoundPlace>> /*places*/, uint32_t /*sound*/,
                                     const song::Sequence& /*song*/, std::optional<Program> /*instrument*/) const
{
    throw std::invalid_argument(added_);
}

TrackEdit FixedTracksEdits::DuplicateTrack(const format::File& /*file*/,
                                           std::span<const std::optional<SoundPlace>> /*places*/, uint32_t /*sound*/,
                                           int /*track*/) const
{
    throw std::invalid_argument(added_);
}

CopiedTrack FixedTracksEdits::CopyTrack(const format::File& /*file*/,
                                        std::span<const std::optional<SoundPlace>> /*places*/, uint32_t /*sound*/,
                                        const song::Sequence& /*song*/, int /*track*/) const
{
    throw std::invalid_argument(pasted_);
}

TrackEdit FixedTracksEdits::PasteTrack(const format::File& /*file*/,
                                       std::span<const std::optional<SoundPlace>> /*places*/, uint32_t /*sound*/,
                                       const song::Sequence& /*song*/, const CopiedTrack& /*copied*/) const
{
    throw std::invalid_argument(added_);
}

bool DriverEdits::NoteWaitOpens(const ntdlib::commands::Sequence& /*sequence*/) const
{
    return false;
}

std::vector<uint32_t> DriverEdits::EntryStarts(const ntdlib::commands::Sequence& /*sequence*/,
                                               std::span<const int> /*entries*/) const
{
    return {};
}

bool DriverEdits::OpensTrack(const ntdlib::commands::Sequence& /*sequence*/, const ntdlib::commands::Command& /*item*/,
                             int /*track*/) const
{
    return false;
}

bool DriverEdits::JumpsPastOpenings() const
{
    return false;
}

bool DriverEdits::CountsRepeats() const
{
    return false;
}

void DriverEdits::CheckMixable(const song::Console& /*console*/, int /*track*/, song::Mix /*setting*/) const
{
}

std::string DriverEdits::RefusedPan(const song::DriverRules& rules) const
{
    return "This revision of " + std::string(rules.Title()) + " has no track pan.";
}

bool DriverEdits::NotesSetVolume() const
{
    return false;
}

void DriverEdits::CheckMixEdit(const ntdlib::commands::Sequence& /*commands*/, const std::vector<uint32_t>& /*offsets*/,
                               std::span<const song::Stretch> /*stretches*/, song::Mix /*setting*/,
                               bool /*run_empty*/) const
{
}

void DriverEdits::IndexOpening(const ntdlib::commands::Sequence& /*commands*/, uint32_t /*first*/,
                               ntdlib::commands::Command& /*command*/) const
{
}

void DriverEdits::FollowPan(const song::Console& /*console*/, ntdlib::commands::Command& /*command*/) const
{
}

std::optional<NewNote> DriverEdits::PlaceDrawnNote(const format::File& /*file*/, uint32_t /*sequence*/,
                                                   const song::Track& /*track*/,
                                                   const std::optional<song::Stretch>& /*stretch*/,
                                                   const DrawnKeyNote& /*drawn*/) const
{
    return std::nullopt;
}

int DriverEdits::FirstNoteKey(const format::File& /*file*/, uint32_t /*sequence*/, int /*track*/,
                              const DrawnKeyNote& /*drawn*/, int /*transpose*/, int key) const
{
    return key;
}

std::optional<uint8_t> DriverEdits::NewNoteIndex(int /*track*/) const
{
    return std::nullopt;
}

std::optional<Edited> DriverEdits::AddSettings(const format::File& /*file*/, uint32_t /*sequence*/,
                                               std::span<const NewSetting> /*settings*/) const
{
    return std::nullopt;
}

std::optional<Edited> DriverEdits::MoveSettings(const format::File& /*file*/, uint32_t /*sequence*/,
                                                std::span<const SettingMove> /*moves*/) const
{
    return std::nullopt;
}

std::optional<std::vector<NoteLimits>> DriverEdits::LimitsOf(const format::File& /*file*/, uint32_t /*sequence*/,
                                                             std::span<const NotePlace> /*notes*/) const
{
    return std::nullopt;
}

int32_t DriverEdits::ShiftOfSetting(std::span<const song::Stretch> /*rows*/, const SettingMove& move) const
{
    return move.shift;
}

void DriverEdits::FitLimits(std::span<const NotePlace> /*notes*/, std::span<const ntdlib::commands::Kind> /*kinds*/,
                            std::span<const song::Stretch> /*rows*/, std::vector<NoteLimits>& /*limits*/) const
{
}

void DriverEdits::PrepareInsert(const format::File& /*file*/, uint32_t /*sequence*/,
                                const ntdlib::commands::Sequence& /*commands*/, uint32_t /*at*/,
                                ntdlib::commands::Command& /*command*/) const
{
}

bool DriverEdits::NotesHoldLengths() const
{
    return true;
}

std::optional<int> DriverEdits::SampleTranspose(const ntdlib::commands::Command& /*before*/,
                                                const ntdlib::commands::Command& /*command*/) const
{
    return std::nullopt;
}

std::optional<uint32_t> DriverEdits::RowTicks(const ntdlib::commands::Command& /*command*/,
                                              std::optional<uint32_t> ticks) const
{
    return ticks;
}

std::vector<std::pair<int, uint32_t>> DriverEdits::ListedStarts(const ntdlib::commands::Sequence& /*sequence*/,
                                                                uint32_t /*list*/) const
{
    return {};
}

bool DriverEdits::NotesPlayOnNextTrack() const
{
    return false;
}

void DriverEdits::CheckInstruments() const
{
}

void DriverEdits::CheckTrackInstrument(const song::Console& /*console*/, int /*track*/) const
{
}

void DriverEdits::CheckOpeningInstrument(const song::Console& /*console*/, int /*track*/) const
{
}

std::optional<std::string> DriverEdits::RefusedBankSelect() const
{
    return std::nullopt;
}

bool DriverEdits::SelectsProgram(const song::Console& /*console*/, int /*track*/,
                                 const ntdlib::commands::Command& /*command*/) const
{
    return true;
}

bool DriverEdits::BankSelectsProgram() const
{
    return false;
}

bool DriverEdits::NotesSelectSamples() const
{
    return false;
}

bool DriverEdits::PassesTime(const ntdlib::commands::Command& /*command*/) const
{
    return false;
}

uint8_t DriverEdits::ProgramIndex(const song::Console& /*console*/, int /*track*/) const
{
    return 0;
}

bool DriverEdits::ChannelsHoldPrograms() const
{
    return false;
}

std::optional<uint8_t> DriverEdits::ProgramChannel(const ntdlib::commands::Sequence& /*commands*/,
                                                   const std::vector<uint32_t>& /*offsets*/,
                                                   std::span<const song::Stretch> /*stretches*/) const
{
    return std::nullopt;
}

Edited InPlace(const format::File& file, uint32_t sequence, ntdlib::commands::Sequence after)
{
    Edited edited{file, {}, {}};
    for (uint32_t item = 0; item < after.items.size(); item++)
    {
        edited.items.emplace_back(item);
    }

    edited.file.sequences[sequence] = std::move(after);

    return edited;
}

Edited WritableByOffsets(const format::File& /*before*/, Edited edited, uint32_t sequence)
{
    CheckEncodes("Can't make this edit: ", [&] { Offsets(*edited.file.sequences[sequence], ConsoleOf(edited.file)); });

    return edited;
}

const SoundPlace& PlaceToEdit(std::span<const std::optional<SoundPlace>> places, uint32_t sound)
{
    if (sound >= places.size() || !places[sound])
    {
        throw std::invalid_argument("Can't find a sequence to edit for this sound.");
    }

    return *places[sound];
}

bool RunsDriver(const song::Console& console, format::Engine engine)
{
    return console.driver != nullptr && console.driver->DriverEngine() == engine;
}

const DriverEdits& EditsOf(const song::DriverRules& rules)
{
    for (const EditsRow& row : kEditsRows)
    {
        if (row.engine == rules.DriverEngine())
        {
            return row.edits();
        }
    }

    throw std::invalid_argument("Hokuto has no edits of the driver of this engine.");
}

} // namespace hokuto::edit
