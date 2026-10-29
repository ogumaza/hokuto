// SPDX-License-Identifier: MIT

// Checks the edits of a sound's values in the archive's tables (src/edit/tables.h) on a sound archive of the Wii, the
// 3DS, the Wii U or the Switch. The tool imports the archive into a Hokuto file the way the program does. It then makes
// a round of edits for each value: the player, the volume, the pan mode, the pan curve, the player priority, the actor
// player, the bank in slot 0, a bank in the slot after a sound's list, the tracks, the channel priority and the release
// priority fix. Each round edits up to 64 of the sounds that have the value, spread over the sound table. The next
// player, bank, pan mode, pan curve or actor player takes the old one's place. A priority goes up by 1, and from 127 to
// 0. The volume halves. A sound with more than one track keeps its lowest, and a sound with one track allocates a
// second one. The release priority fix turns over. A slot after a sound's list takes the archive's first bank.
//
// After each round, the tool exports the edited file and checks four things:
//
// - ntdlib's strict reader reads each edited sound's new value from the archive, and every other value as it was.
// - The sequences of the archive's files and the fields of the sounds' starts are those of the file
//   (format::SequencesOfFiles and format::StartFieldsOfSounds). Longer lists of banks can move the files.
// - Edits back to the old values give the archive back byte for byte.
// - In the volume's round, ntdlib's model plays up to 8 of the edited sequence sounds for 4 s each, from the archive
//   before and after the round. The output's level falls by the ratio of the volumes, within 0.25 dB. A sound whose
//   output clips before the edit, or plays too quietly to measure, is counted apart. The console limits a voice's
//   volume. A sound whose level falls by less than the ratio plays again at half its new volume, and again, until its
//   level falls by the ratio from one volume to the next. It then counts apart. A sound whose level never does fails.
//
// Three more rounds give sounds values whose options their entries leave out: the pan, the priorities and the channel
// priority. Each round takes up to 64 of the sounds whose entries hold the option, spread over the sound table. The
// option leaves their entries (ntdlib's sar_tools.h), and the archive without it makes a new file. Its sounds have to
// hold the values that ntdlib's reader gives without the option. Through the edits, each sound then takes its old
// values. A sound whose old values are the reader's values without the option takes the next ones instead. The rounds
// above choose the next values the same way. After the round, the tool checks four things:
//
// - The export is the archive with every option, with the new values written in their places (ntdlib's
//   WriteTableValues), byte for byte. The new options lie where the tools lay them out, and every other value and item
//   stays.
// - ntdlib's strict reader reads each sound's values from the export.
// - The sequences of the archive's files and the fields of the sounds' starts are those of the file without the
//   option.
// - Edits back to the reader's values without the option give the archive without it back, byte for byte.
//
// On a 3DS archive, the volume's round plays on ntdlib's model of the DSP's audio firmware.
//
// On an archive of the Wii, the rounds of places don't run: each value has a fixed place. The rounds of the values that
// the archive doesn't hold check that each edit is refused (edit::HeldValuesOf): the actor player, the slot after a
// sound's list, and the pan before version 1.2 and the release priority fix before version 1.3. After each other round,
// the export has to be the archive's size, and only bytes of its INFO block may differ. The import of the export has to
// read each new value, and every other value as it was. The volume's round plays on ntdlib's model of the Wii. The
// import reads no main.dol: the file takes the build of NW4R SND that the archive's version gives. The rounds follow
// that build's rules.
//
// A bank's rounds leave out a sound whose list the edits can't give back: an empty list, or a list of more than one
// slot whose last slot is empty. The tool counts the rounds whose export moved the archive's files. It prints a line
// for each sound that differs, and a summary. It returns 1 when a value, a round trip, a level, the files' sequences or
// the starts' fields differ, or when an edit, an export or a read fails.
//
// With --model, the file goes into the model form after the import (format::InModelForm), and the archive that the
// model builds has to be the game's. After each round of values, the edited file has to save in both forms and open
// again as it was (tools/checked_archive.h). The rounds of places make their files in the native form: the model holds
// every option of each sound's entry.
//
// usage: hokuto_check_sounds [--model] <archive>

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
#include "edit/tables.h"
#include "format/archive.h"
#include "format/file.h"
#include "song/archive.h"
#include "song/sequence.h"
#include "song/sequence_model.h"

namespace
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

// A round of places: the option that leaves the entries, and its two values.
struct PlaceRound
{
    const char* name = "";
    sar::ValuePlaces option;
    std::array<SoundValue, 2> values{};
};

constexpr std::array<PlaceRound, 3> kPlaceRounds = {{
    {"pan", {true, false, false}, {SoundValue::kPanMode, SoundValue::kPanCurve}},
    {"priorities", {false, true, false}, {SoundValue::kPlayerPriority, SoundValue::kActorPlayer}},
    {"channel priority", {false, false, true}, {SoundValue::kChannelPriority, SoundValue::kReleasePriorityFix}},
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

// What the rounds start from: the archive's name and bytes, what ntdlib reads of them, the file that they make, and the
// places of the sounds' optional values in the file's tables.
struct Original
{
    std::string name;
    std::vector<uint8_t> bytes;
    std::shared_ptr<const sar::SoundArchive> archive;
    File file;
    std::vector<sar::ValuePlaces> places;
};

// The sequences of the files of an archive that a file builds, and the fields of its sounds' starts.
struct Layout
{
    friend bool operator==(const Layout&, const Layout&) = default;

    std::vector<std::optional<uint32_t>> sequences;
    std::vector<std::optional<uint32_t>> fields;
};

// What ntdlib reads of `archive`, an archive of `flavor`, by the strict rules.
sar::SoundArchive Strictly(const std::vector<uint8_t>& archive, sar::Flavor flavor)
{
    sar::Options options;
    options.flavor = flavor;
    options.rules = sar::Rules::kStopAtFirstProblem;

    return sar::SoundArchive::Load(archive, options);
}

// The layout of `bytes`, the archive that `file` builds. ntdlib reads the archive as `read`.
Layout LayoutOf(const File& file, const std::vector<uint8_t>& bytes, const sar::SoundArchive& read)
{
    return {hokuto::format::SequencesOfFiles(file, bytes, read), hokuto::format::StartFieldsOfSounds(file, bytes)};
}

// Whether a file of `after` lies elsewhere than in `before`.
bool FilesMoved(const sar::SoundArchive& before, const sar::SoundArchive& after)
{
    for (uint32_t id = 0; id < std::min(before.FileCount(), after.FileCount()); ++id)
    {
        if (before.FileOffset(id) != after.FileOffset(id))
        {
            return true;
        }
    }

    return false;
}

// Whether only a sequence sound has `value`.
bool OfSequenceSounds(SoundValue value)
{
    return value == SoundValue::kBank || value == SoundValue::kTracks || value == SoundValue::kChannelPriority ||
           value == SoundValue::kReleasePriorityFix;
}

// Whether `places` holds each option of `option`.
bool Holds(const sar::ValuePlaces& places, const sar::ValuePlaces& option)
{
    return (!option.pan || places.pan) && (!option.priority || places.priority) && (!option.channel || places.channel);
}

// Whether the edits of a bank slot can give `banks` back: a list of one slot, or a longer list whose last slot holds a
// bank. An edit that empties a slot drops the empty slots at the list's end.
bool ListComesBack(const std::vector<uint32_t>& banks)
{
    return banks.size() == 1 || (banks.size() > 1 && banks.back() != sar::kNoItem);
}

// The change of `sound`, a sound of `tables`, in round `round`, or nothing for a sound without the value, or without
// another value to take.
std::optional<SoundChange> ChangeOf(const NativeTables& tables, const NativeSound& sound, const Round& round)
{
    if (OfSequenceSounds(round.value) && sound.kind != sar::SoundType::kSequence)
    {
        return std::nullopt;
    }

    const auto players = static_cast<uint32_t>(tables.players.size());
    const auto banks = static_cast<uint32_t>(tables.banks.size());
    switch (round.value)
    {
    case SoundValue::kPlayer:
        {
            const uint32_t number = sar::ItemNumber(sound.player);
            if (players < 2 || sar::ItemType(sound.player) != sar::kPlayerItem || number >= players)
            {
                return std::nullopt;
            }

            return SoundChange{round.value, 0, sar::ItemId(sar::kPlayerItem, (number + 1) % players)};
        }

    case SoundValue::kVolume:
        if (sound.volume < 2)
        {
            return std::nullopt;
        }

        return SoundChange{round.value, 0, sound.volume / 2u};

    case SoundValue::kPanMode:
        return SoundChange{round.value, 0, sound.pan_mode == 0 ? 1u : 0u};

    case SoundValue::kPanCurve:
        return SoundChange{round.value, 0, (sound.pan_curve + 1u) % 9};

    case SoundValue::kPlayerPriority:
        return SoundChange{round.value, 0, (sound.player_priority + 1u) % 128};

    case SoundValue::kActorPlayer:
        return SoundChange{round.value, 0, (sound.actor_player + 1u) % 4};

    case SoundValue::kBank:
        {
            if (banks < 2 || !ListComesBack(sound.banks) || (round.slot == kAfterList && sound.banks.size() >= 4))
            {
                return std::nullopt;
            }
            if (round.slot == kAfterList)
            {
                return SoundChange{round.value, static_cast<int>(sound.banks.size()), sar::ItemId(sar::kBankItem, 0)};
            }

            const uint32_t held = sound.banks[0];
            const bool listed = sar::ItemType(held) == sar::kBankItem && sar::ItemNumber(held) < banks;
            const uint32_t next = listed ? (sar::ItemNumber(held) + 1) % banks : 0;
            return SoundChange{round.value, 0, sar::ItemId(sar::kBankItem, next)};
        }

    case SoundValue::kTracks:
        {
            const uint32_t tracks = sound.tracks & 0xFFFF;
            if (tracks == 0)
            {
                return std::nullopt;
            }

            const uint32_t lowest = tracks & (~tracks + 1);
            const uint32_t next = lowest == 0x8000 ? 0x4000 : lowest << 1;
            return SoundChange{round.value, 0, std::has_single_bit(tracks) ? tracks | next : lowest};
        }

    case SoundValue::kChannelPriority:
        return SoundChange{round.value, 0, (sound.channel_priority + 1u) % 128};

    case SoundValue::kReleasePriorityFix:
        break;
    }

    return SoundChange{round.value, 0, sound.release_priority_fix ? 0u : 1u};
}

// `sound` after `change`. The tool works the values out by itself. A bank goes into its slot, or after the list's last
// slot.
NativeSound Changed(NativeSound sound, const SoundChange& change)
{
    const auto byte = static_cast<uint8_t>(change.to);
    switch (change.value)
    {
    case SoundValue::kPlayer:
        sound.player = change.to;
        break;
    case SoundValue::kVolume:
        sound.volume = byte;
        break;
    case SoundValue::kPanMode:
        sound.pan_mode = byte;
        break;
    case SoundValue::kPanCurve:
        sound.pan_curve = byte;
        break;
    case SoundValue::kPlayerPriority:
        sound.player_priority = byte;
        break;
    case SoundValue::kActorPlayer:
        sound.actor_player = byte;
        break;
    case SoundValue::kBank:
        if (static_cast<std::size_t>(change.slot) == sound.banks.size())
        {
            sound.banks.push_back(change.to);
        }
        else
        {
            sound.banks[static_cast<std::size_t>(change.slot)] = change.to;
        }
        break;
    case SoundValue::kTracks:
        sound.tracks = change.to;
        break;
    case SoundValue::kChannelPriority:
        sound.channel_priority = byte;
        break;
    case SoundValue::kReleasePriorityFix:
        sound.release_priority_fix = change.to != 0;
        break;
    }

    return sound;
}

// Whether ntdlib read `info` with the values of `sound`.
bool SameValues(const sar::SoundInfo& info, const NativeSound& sound)
{
    const bool common = info.player_id == sound.player && info.volume == sound.volume &&
                        info.pan_mode == sound.pan_mode && info.pan_curve == sound.pan_curve &&
                        info.player_priority == sound.player_priority && info.actor_player_id == sound.actor_player;
    if (sound.kind != sar::SoundType::kSequence)
    {
        return common;
    }

    return common && info.banks == sound.banks && info.allocate_track_flags == sound.tracks &&
           info.channel_priority == sound.channel_priority && info.release_priority_fix == sound.release_priority_fix;
}

// The output of sequence sound `sound` of `archive` for kLevelSeconds on ntdlib's model.
std::vector<int16_t> Play(const std::shared_ptr<const sar::SoundArchive>& archive, uint32_t sound)
{
    const auto sequence = std::make_shared<const hokuto::song::Sequence>(hokuto::song::ReadSequence(*archive, sound));
    hokuto::song::SequenceModel model(archive, sequence);
    const auto samples = static_cast<std::size_t>(kLevelSeconds * model.SampleRate()) * 2;
    std::vector<int16_t> output;
    for (bool more = model.SeekFrame(0); more && output.size() < samples; more = model.RunFrame())
    {
        const std::span<const int16_t> frame = model.Output();
        output.insert(output.end(), frame.begin(), frame.end());
    }

    return output;
}

// The root mean square of `samples`.
double Level(const std::vector<int16_t>& samples)
{
    double sum = 0.0;
    for (const int16_t sample : samples)
    {
        sum += static_cast<double>(sample) * sample;
    }

    return samples.empty() ? 0.0 : std::sqrt(sum / static_cast<double>(samples.size()));
}

// The output of a sequence sound of an archive for kLevelSeconds on ntdlib's model, by the sound's place in the table,
// and the same at another volume of the sound.
using Player = std::function<std::vector<int16_t>(uint32_t)>;
using VolumePlayer = std::function<std::vector<int16_t>(uint32_t, uint32_t)>;

// Whether the level of sound `sound` at volume `volume`, `level`, falls by the ratio of the volumes from one halving of
// the volume to the next once the volume is low enough. `at_volume` plays the sound at another volume. The console
// limits a voice's volume, and a sound whose voices reach the limit falls by less than the ratio.
bool FollowsBelowLimit(uint32_t sound, uint32_t volume, double level, const VolumePlayer& at_volume)
{
    for (uint32_t high = volume; high >= 2; high /= 2)
    {
        const uint32_t low = high / 2;
        const double low_level = Level(at_volume(sound, low));
        if (low_level < kLeastLevel)
        {
            return false;
        }

        const double expected = 20.0 * std::log10(static_cast<double>(low) / high);
        if (std::abs(20.0 * std::log10(low_level / level) - expected) <= kTolerance)
        {
            return true;
        }
        level = low_level;
    }

    return false;
}

// Checks the levels of up to kLevelSounds sequence sounds of `changes` that played as `before` plays them and play as
// `after` plays them, from an archive with their new volumes. `at_volume` plays a sound at another volume.
void CheckLevels(const std::string& name, const File& file, const Player& before, const Player& after,
                 const VolumePlayer& at_volume, const std::vector<std::pair<uint32_t, SoundChange>>& changes,
                 Totals& totals)
{
    std::size_t played = 0;
    for (const auto& [sound, change] : changes)
    {
        const NativeSound& old = file.native->tables->sounds[sound];
        if (played == kLevelSounds || old.kind != sar::SoundType::kSequence)
        {
            continue;
        }

        // A sound that doesn't play before the edits stays out. One that plays only before them fails.
        std::vector<int16_t> was;
        try
        {
            was = before(sound);
        }
        catch (const std::exception&)
        {
            continue;
        }
        std::vector<int16_t> now;
        try
        {
            now = after(sound);
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s doesn't play at its new volume. %s\n", name.c_str(), old.name.c_str(), error.what());
            ++totals.failures;
            continue;
        }

        const double level = Level(was);
        if (level < kLeastLevel)
        {
            ++totals.quiet;
            continue;
        }

        ++played;
        const auto clips = [](int16_t sample)
        {
            return sample == INT16_MAX || sample == INT16_MIN;
        };
        if (std::any_of(was.begin(), was.end(), clips))
        {
            ++totals.clipped;
            continue;
        }

        ++totals.levels;
        const double expected = 20.0 * std::log10(static_cast<double>(change.to) / old.volume);
        const double measured = 20.0 * std::log10(Level(now) / level);
        if (std::abs(measured - expected) > kTolerance && measured > expected)
        {
            try
            {
                if (FollowsBelowLimit(sound, change.to, Level(now), at_volume))
                {
                    ++totals.limited;
                    continue;
                }
            }
            catch (const std::exception&)
            {
                // A sound that doesn't play at a lower volume takes the plain comparison of the levels.
            }
        }
        totals.worst = std::max(totals.worst, std::abs(measured - expected));
        if (std::abs(measured - expected) > kTolerance)
        {
            std::printf("%s: %s: the volume from %u to %u changes the level by %.3f dB, not %.3f dB.\n", name.c_str(),
                        old.name.c_str(), old.volume, change.to, measured, expected);
            ++totals.wrong_levels;
        }
    }
}

// Up to kRoundSounds of `candidates`, spread over them. `index` turns the choice from round to round.
template <typename Candidate>
std::vector<Candidate> Spread(const std::vector<Candidate>& candidates, std::size_t index)
{
    std::vector<Candidate> chosen;
    const std::size_t step = std::max<std::size_t>(1, candidates.size() / kRoundSounds);
    for (std::size_t i = index % step; i < candidates.size() && chosen.size() < kRoundSounds; i += step)
    {
        chosen.push_back(candidates[i]);
    }

    return chosen;
}

// Checks that the values of every sound that ntdlib reads from `read` are those of `expected`. `what` names the round
// in the messages.
void CheckValues(const std::string& name, const sar::SoundArchive& read, const std::vector<NativeSound>& expected,
                 const std::string& what, Totals& totals)
{
    for (std::size_t sound = 0; sound < expected.size(); ++sound)
    {
        if (sound >= read.Sounds().size() || !SameValues(read.Sounds()[sound], expected[sound]))
        {
            std::printf("%s: %s: the values read back otherwise after the round of the %s.\n", name.c_str(),
                        expected[sound].name.c_str(), what.c_str());
            ++totals.differ;
        }
    }
}

// Checks the sequences of the files and the fields of the sounds' starts of `exported`, the archive that `file` builds,
// against `layout`. ntdlib reads the archive as `read`. `what` names the round in the messages.
void CheckLayout(const std::string& name, const File& file, const std::vector<uint8_t>& exported,
                 const sar::SoundArchive& read, const Layout& layout, const std::string& what, Totals& totals)
{
    ++totals.layouts;
    if (LayoutOf(file, exported, read) != layout)
    {
        std::printf("%s: the files' sequences or the starts' fields differ after the round of the %s.\n", name.c_str(),
                    what.c_str());
        ++totals.differ;
    }
}

// Makes round `round` on up to kRoundSounds sounds of `original`, and checks the archive that the edits export, and the
// archive that the edits back export. `layout` holds the original's files' sequences and starts' fields. `index` turns
// the choice of sounds from round to round.
void CheckRound(const Original& original, const Layout& layout, const Round& round, std::size_t index, Totals& totals)
{
    // The sounds with the value, and a choice of them spread over the table.
    const std::string& name = original.name;
    const std::string what = hokuto::edit::NameOf(round.value);
    const NativeTables& tables = *original.file.native->tables;
    std::vector<std::pair<uint32_t, SoundChange>> candidates;
    for (uint32_t sound = 0; sound < tables.sounds.size(); ++sound)
    {
        const bool list = round.value == SoundValue::kBank && tables.sounds[sound].kind == sar::SoundType::kSequence &&
                          !ListComesBack(tables.sounds[sound].banks);
        totals.lists_apart += list && round.slot == 0 ? 1 : 0;
        if (const std::optional<SoundChange> change = ChangeOf(tables, tables.sounds[sound], round))
        {
            candidates.emplace_back(sound, *change);
        }
    }
    const std::vector<std::pair<uint32_t, SoundChange>> changes = Spread(candidates, index);

    // The edits, and the values that the tool works out.
    File edited = original.file;
    std::vector<NativeSound> expected = tables.sounds;
    std::vector<std::pair<uint32_t, SoundChange>> made;
    for (const auto& [sound, change] : changes)
    {
        try
        {
            edited = hokuto::edit::ChangeSound(edited, sound, change);
        }
        catch (const std::invalid_argument& error)
        {
            std::printf("%s: %s: the edit of the %s fails: %s\n", name.c_str(), tables.sounds[sound].name.c_str(),
                        what.c_str(), error.what());
            ++totals.failures;
            continue;
        }

        ++totals.edits;
        expected[sound] = Changed(expected[sound], change);
        made.emplace_back(sound, change);
    }

    // The values that the archive holds after the edits, and the places of its files and starts.
    std::vector<uint8_t> exported;
    std::shared_ptr<const sar::SoundArchive> after;
    try
    {
        hokuto::tools::CheckSaved(edited);
        exported = hokuto::format::ExportArchive(edited);
        const sar::SoundArchive read = Strictly(exported, original.file.native->flavor);
        CheckValues(name, read, expected, what, totals);
        CheckLayout(name, edited, exported, read, layout, what, totals);
        totals.moved += FilesMoved(*original.archive, read) ? 1 : 0;
        if (round.value == SoundValue::kVolume)
        {
            after = hokuto::song::LoadArchive(exported, name, hokuto::format::ExportGroupFiles(edited));
        }
    }
    catch (const std::exception& error)
    {
        std::printf("%s: the round of the %s fails: %s\n", name.c_str(), what.c_str(), error.what());
        ++totals.failures;
        return;
    }

    // The old values give the archive back.
    File back = edited;
    try
    {
        for (auto it = made.rbegin(); it != made.rend(); ++it)
        {
            const auto& [sound, change] = *it;
            const uint32_t old = hokuto::edit::ValueOf(tables.sounds[sound], change.value, change.slot);
            back = hokuto::edit::ChangeSound(back, sound, {change.value, change.slot, old});
        }
        if (hokuto::format::ExportArchive(back) != original.bytes)
        {
            std::printf("%s: the old values don't give the archive back after the round of the %s.\n", name.c_str(),
                        what.c_str());
            ++totals.round_trips;
        }
    }
    catch (const std::exception& error)
    {
        std::printf("%s: the edits back of the round of the %s fail: %s\n", name.c_str(), what.c_str(), error.what());
        ++totals.failures;
    }

    if (after)
    {
        const auto at_volume = [&](uint32_t sound, uint32_t volume)
        {
            const File lower = hokuto::edit::ChangeSound(original.file, sound, {SoundValue::kVolume, 0, volume});
            return Play(hokuto::song::LoadArchive(hokuto::format::ExportArchive(lower), name,
                                                  hokuto::format::ExportGroupFiles(lower)),
                        sound);
        };
        CheckLevels(
            name, original.file, [&](uint32_t sound) { return Play(original.archive, sound); },
            [&](uint32_t sound) { return Play(after, sound); }, at_volume, made, totals);
    }
}

// `archive`, an archive of `flavor`, with the values of `sounds` and `players` written over the bytes where its tables
// hold them (ntdlib's WriteTableValues). The export writes the tables' values the same way.
std::vector<uint8_t> WithValues(std::vector<uint8_t> archive, sar::Flavor flavor,
                                const std::vector<NativeSound>& sounds,
                                const std::vector<hokuto::format::NativePlayer>& players)
{
    std::vector<sar::SoundInfo> infos;
    for (const NativeSound& sound : sounds)
    {
        sar::SoundInfo& info = infos.emplace_back();
        info.player_id = sound.player;
        info.volume = sound.volume;
        info.pan_mode = sound.pan_mode;
        info.pan_curve = sound.pan_curve;
        info.player_priority = sound.player_priority;
        info.actor_player_id = sound.actor_player;
        info.banks = sound.banks;
        info.allocate_track_flags = sound.tracks;
        info.channel_priority = sound.channel_priority;
        info.release_priority_fix = sound.release_priority_fix;
    }
    std::vector<sar::PlayerInfo> limits;
    for (const hokuto::format::NativePlayer& player : players)
    {
        limits.push_back({player.name, player.sound_limit});
    }
    sar::WriteTableValues(archive, flavor, infos, limits);

    return archive;
}

// Makes round `round` of places on up to kRoundSounds sounds of `original` whose entries hold the round's option. The
// option leaves their entries, and the sounds take values through the edits. Checks the archive that the edits export,
// and the archive that the edits back export. `index` turns the choice of sounds from round to round.
void CheckPlaceRound(const Original& original, const PlaceRound& round, std::size_t index, Totals& totals)
{
    // The sounds whose entries hold the option, and a choice of them spread over the table.
    const std::string& name = original.name;
    const std::string what = std::string("places of the ") + round.name;
    const sar::Flavor flavor = original.file.native->flavor;
    const NativeTables& tables = *original.file.native->tables;
    std::vector<uint32_t> candidates;
    for (uint32_t sound = 0; sound < tables.sounds.size() && sound < original.places.size(); ++sound)
    {
        if (Holds(original.places[sound], round.option))
        {
            candidates.push_back(sound);
        }
    }
    const std::vector<uint32_t> chosen = Spread(candidates, index);
    if (chosen.empty())
    {
        return;
    }

    // The archive without the option makes a file. Its sounds hold the reader's values without the option, and the file
    // builds the archive.
    std::map<uint32_t, sar::ValuePlaces> stripped;
    for (const uint32_t sound : chosen)
    {
        stripped[sound] = round.option;
    }
    std::vector<uint8_t> bare;
    std::optional<sar::SoundArchive> bare_read;
    File file;
    std::vector<NativeSound> expected = tables.sounds;
    Layout layout;
    try
    {
        bare = ntdlib::testing::sar_tools::WithoutValuePlaces(original.bytes, flavor, stripped);
        file = hokuto::format::ImportArchive(bare, flavor, hokuto::format::ExportGroupFiles(original.file), {},
                                             original.archive->GroupFileGroups());
        const std::optional<std::vector<sar::ValuePlaces>> places = hokuto::format::TableValuePlacesOf(*file.native);
        const sar::SoundInfo unset;
        for (const uint32_t sound : chosen)
        {
            NativeSound& values = expected[sound];
            if (round.option.pan)
            {
                values.pan_mode = unset.pan_mode;
                values.pan_curve = unset.pan_curve;
            }
            if (round.option.priority)
            {
                values.player_priority = unset.player_priority;
                values.actor_player = unset.actor_player_id;
            }
            if (round.option.channel)
            {
                values.channel_priority = unset.channel_priority;
                values.release_priority_fix = unset.release_priority_fix;
            }
            if (!places || sound >= places->size() || Holds((*places)[sound], round.option))
            {
                std::printf("%s: %s: the entry holds the %s after the strip.\n", name.c_str(),
                            tables.sounds[sound].name.c_str(), round.name);
                ++totals.failures;
            }
        }
        if (!file.native->tables || file.native->tables->sounds != expected ||
            hokuto::format::ExportArchive(file) != bare)
        {
            std::printf("%s: the file without the %s doesn't hold the reader's values, or doesn't build its archive.\n",
                        name.c_str(), what.c_str());
            ++totals.failures;
            return;
        }
        bare_read.emplace(Strictly(bare, flavor));
        layout = LayoutOf(file, bare, *bare_read);
    }
    catch (const std::exception& error)
    {
        std::printf("%s: the strip of the %s fails: %s\n", name.c_str(), what.c_str(), error.what());
        ++totals.failures;
        return;
    }

    // The edits. A sound takes its old values, or the next ones where the old ones are the reader's values without the
    // option.
    const std::vector<NativeSound> unplaced = expected;
    File edited = file;
    for (const uint32_t sound : chosen)
    {
        const NativeSound& old = tables.sounds[sound];
        const sar::ValuePlaces needed = hokuto::format::NeededPlaces(old);
        const bool keeps = Holds(needed, round.option);
        totals.old_values += keeps ? 1 : 0;
        totals.next_values += keeps ? 0 : 1;
        ++totals.places;
        for (const SoundValue value : round.values)
        {
            const uint32_t to =
                keeps ? hokuto::edit::ValueOf(old, value, 0) : ChangeOf(tables, unplaced[sound], {value, 0})->to;
            if (to == hokuto::edit::ValueOf(expected[sound], value, 0))
            {
                continue;
            }

            const SoundChange change{value, 0, to};
            try
            {
                edited = hokuto::edit::ChangeSound(edited, sound, change);
            }
            catch (const std::invalid_argument& error)
            {
                std::printf("%s: %s: the edit of the %s fails: %s\n", name.c_str(), old.name.c_str(),
                            hokuto::edit::NameOf(value).c_str(), error.what());
                ++totals.failures;
                continue;
            }
            ++totals.place_edits;
            expected[sound] = Changed(expected[sound], change);
        }
    }

    // The export is the archive with every option and the new values. ntdlib reads them, and the files and the starts
    // keep their places in the file.
    try
    {
        const std::vector<uint8_t> exported = hokuto::format::ExportArchive(edited);
        const sar::SoundArchive read = Strictly(exported, flavor);
        if (exported != WithValues(original.bytes, flavor, expected, tables.players))
        {
            std::printf("%s: the export differs from the archive with every option after the round of the %s.\n",
                        name.c_str(), what.c_str());
            ++totals.differ;
        }
        CheckValues(name, read, expected, what, totals);
        CheckLayout(name, edited, exported, read, layout, what, totals);
        totals.moved += FilesMoved(*bare_read, read) ? 1 : 0;
        ++totals.place_layouts;
    }
    catch (const std::exception& error)
    {
        std::printf("%s: the round of the %s fails: %s\n", name.c_str(), what.c_str(), error.what());
        ++totals.failures;
        return;
    }

    // The reader's values without the option give the archive without it back.
    File back = edited;
    try
    {
        for (const uint32_t sound : chosen)
        {
            for (const SoundValue value : round.values)
            {
                back = hokuto::edit::ChangeSound(back, sound,
                                                 {value, 0, hokuto::edit::ValueOf(unplaced[sound], value, 0)});
            }
        }
        if (hokuto::format::ExportArchive(back) != bare)
        {
            std::printf(
                "%s: the values without the option don't give the archive without it back after the round of "
                "the %s.\n",
                name.c_str(), what.c_str());
            ++totals.round_trips;
        }
    }
    catch (const std::exception& error)
    {
        std::printf("%s: the edits back of the round of the %s fail: %s\n", name.c_str(), what.c_str(), error.what());
        ++totals.failures;
    }
}

// What the rounds of an archive of the Wii start from: the archive's name and bytes, what ntdlib reads of them for
// playback, the file that they make, and the values that the archive holds.
struct WiiOriginal
{
    std::string name;
    std::vector<uint8_t> bytes;
    std::shared_ptr<const ntdlib::rsar::SoundArchive> archive;
    File file;
    hokuto::edit::HeldValues held;
};

// Whether an archive that holds `held` holds the value of `round`. The slot after a sound's list counts as a second
// slot.
bool Holds(const hokuto::edit::HeldValues& held, const Round& round)
{
    switch (round.value)
    {
    case SoundValue::kActorPlayer:
        return held.actor_player;
    case SoundValue::kPanMode:
    case SoundValue::kPanCurve:
        return held.pan;
    case SoundValue::kReleasePriorityFix:
        return held.release_priority_fix;
    case SoundValue::kBank:
        return round.slot == 0 || held.bank_slots > 1;
    default:
        return true;
    }
}

// The output of sequence sound `sound` of the Wii's `archive` for kLevelSeconds on ntdlib's model of the Wii, with the
// settings of `build`.
std::vector<int16_t> PlayWii(const std::shared_ptr<const ntdlib::rsar::SoundArchive>& archive, uint32_t sound,
                             const ntdlib::SndBuild& build)
{
    const auto sequence =
        std::make_shared<const hokuto::song::Sequence>(hokuto::song::ReadSequence(*archive, sound, {}, {}, build));
    hokuto::song::SequenceModel model(archive, sequence);
    const auto samples = static_cast<std::size_t>(kLevelSeconds * model.SampleRate()) * 2;
    std::vector<int16_t> output;
    for (bool more = model.SeekFrame(0); more && output.size() < samples; more = model.RunFrame())
    {
        const std::span<const int16_t> frame = model.Output();
        output.insert(output.end(), frame.begin(), frame.end());
    }

    return output;
}

// The number of bytes in which `exported` differs from `bytes`, outside the INFO block of `bytes`, an archive of the
// Wii. The INFO block's offset and size lie at 0x18 and 0x1C of the header.
std::size_t DifferencesOutsideInfo(const std::vector<uint8_t>& bytes, const std::vector<uint8_t>& exported)
{
    const std::size_t info = xsflib::LoadBe32(&bytes[0x18]);
    const std::size_t end = info + xsflib::LoadBe32(&bytes[0x1C]);
    std::size_t differences = 0;
    for (std::size_t i = 0; i < std::min(bytes.size(), exported.size()); ++i)
    {
        differences += bytes[i] != exported[i] && (i < info || i >= end) ? 1 : 0;
    }

    return differences;
}

// The sequences of the files of `bytes`, an archive of the Wii that `file` builds, and the fields of its sounds'
// starts.
Layout WiiLayoutOf(const File& file, const std::vector<uint8_t>& bytes)
{
    const std::shared_ptr<const ntdlib::rsar::SoundArchive> read =
        hokuto::song::LoadWiiArchive(bytes, "export", ntdlib::rvl::snd::kRsarOptions);
    return {hokuto::format::SequencesOfFiles(file, bytes, *read), hokuto::format::StartFieldsOfSounds(file, bytes)};
}

// Makes round `round` on up to kRoundSounds sounds of `original`, an archive of the Wii. A value that the archive
// doesn't hold has to be refused. Otherwise the tool checks the archive that the edits export, and the archive that the
// edits back export. `layout` holds the original's files' sequences and starts' fields. `index` turns the choice of
// sounds from round to round.
void CheckWiiRound(const WiiOriginal& original, const Layout& layout, const Round& round, std::size_t index,
                   Totals& totals)
{
    const std::string& name = original.name;
    const std::string what = hokuto::edit::NameOf(round.value);
    const NativeTables& tables = *original.file.native->tables;
    std::vector<std::pair<uint32_t, SoundChange>> candidates;
    for (uint32_t sound = 0; sound < tables.sounds.size(); ++sound)
    {
        if (const std::optional<SoundChange> change = ChangeOf(tables, tables.sounds[sound], round))
        {
            candidates.emplace_back(sound, *change);
        }
    }
    const std::vector<std::pair<uint32_t, SoundChange>> changes = Spread(candidates, index);

    // A value that the archive doesn't hold.
    if (!Holds(original.held, round))
    {
        for (const auto& [sound, change] : changes)
        {
            try
            {
                hokuto::edit::ChangeSound(original.file, sound, change);
                std::printf("%s: %s: the edit of the %s isn't refused.\n", name.c_str(),
                            tables.sounds[sound].name.c_str(), what.c_str());
                ++totals.failures;
            }
            catch (const std::invalid_argument&)
            {
                ++totals.refused;
            }
        }
        return;
    }

    // The edits, and the values that the tool works out.
    File edited = original.file;
    std::vector<NativeSound> expected = tables.sounds;
    std::vector<std::pair<uint32_t, SoundChange>> made;
    for (const auto& [sound, change] : changes)
    {
        try
        {
            edited = hokuto::edit::ChangeSound(edited, sound, change);
        }
        catch (const std::invalid_argument& error)
        {
            std::printf("%s: %s: the edit of the %s fails: %s\n", name.c_str(), tables.sounds[sound].name.c_str(),
                        what.c_str(), error.what());
            ++totals.failures;
            continue;
        }

        ++totals.edits;
        expected[sound] = Changed(expected[sound], change);
        made.emplace_back(sound, change);
    }

    // The export holds the new values in place.
    std::vector<uint8_t> exported;
    std::shared_ptr<const ntdlib::rsar::SoundArchive> after;
    try
    {
        hokuto::tools::CheckSaved(edited);
        exported = hokuto::format::ExportArchive(edited);
        const File read = hokuto::format::ImportWiiArchive(exported);
        if (exported.size() != original.bytes.size() || DifferencesOutsideInfo(original.bytes, exported) != 0)
        {
            std::printf(
                "%s: the export moves bytes, or changes bytes outside its INFO block, after the round of the %s.\n",
                name.c_str(), what.c_str());
            ++totals.differ;
        }
        for (std::size_t sound = 0; sound < expected.size(); ++sound)
        {
            if (!read.native->tables || sound >= read.native->tables->sounds.size() ||
                read.native->tables->sounds[sound] != expected[sound])
            {
                std::printf("%s: %s: the values read back otherwise after the round of the %s.\n", name.c_str(),
                            expected[sound].name.c_str(), what.c_str());
                ++totals.differ;
            }
        }
        ++totals.layouts;
        if (WiiLayoutOf(edited, exported) != layout)
        {
            std::printf("%s: the files' sequences or the starts' fields differ after the round of the %s.\n",
                        name.c_str(), what.c_str());
            ++totals.differ;
        }
        if (round.value == SoundValue::kVolume)
        {
            after = hokuto::song::LoadWiiArchive(exported, name, ntdlib::rvl::snd::kRsarOptions);
        }
    }
    catch (const std::exception& error)
    {
        std::printf("%s: the round of the %s fails: %s\n", name.c_str(), what.c_str(), error.what());
        ++totals.failures;
        return;
    }

    // The old values give the archive back.
    File back = edited;
    try
    {
        for (auto it = made.rbegin(); it != made.rend(); ++it)
        {
            const auto& [sound, change] = *it;
            const uint32_t old = hokuto::edit::ValueOf(tables.sounds[sound], change.value, change.slot);
            back = hokuto::edit::ChangeSound(back, sound, {change.value, change.slot, old});
        }
        if (hokuto::format::ExportArchive(back) != original.bytes)
        {
            std::printf("%s: the old values don't give the archive back after the round of the %s.\n", name.c_str(),
                        what.c_str());
            ++totals.round_trips;
        }
    }
    catch (const std::exception& error)
    {
        std::printf("%s: the edits back of the round of the %s fail: %s\n", name.c_str(), what.c_str(), error.what());
        ++totals.failures;
    }

    if (after)
    {
        const ntdlib::SndBuild build = hokuto::format::BuildOfWiiArchive(*original.file.native);
        const auto at_volume = [&](uint32_t sound, uint32_t volume)
        {
            const File lower = hokuto::edit::ChangeSound(original.file, sound, {SoundValue::kVolume, 0, volume});
            return PlayWii(hokuto::song::LoadWiiArchive(hokuto::format::ExportArchive(lower), name,
                                                        ntdlib::rvl::snd::kRsarOptions),
                           sound, build);
        };
        CheckLevels(
            name, original.file, [&](uint32_t sound) { return PlayWii(original.archive, sound, build); },
            [&](uint32_t sound) { return PlayWii(after, sound, build); }, at_volume, made, totals);
    }
}

// Checks the rounds of `bytes`, an archive of the Wii named `name`, and prints the summary. Returns the tool's code.
int CheckWiiArchive(const std::string& name, std::vector<uint8_t> bytes)
{
    WiiOriginal original;
    original.name = name;
    original.bytes = std::move(bytes);
    Layout layout;
    try
    {
        original.archive = hokuto::song::LoadWiiArchive(original.bytes, name, ntdlib::rvl::snd::kRsarOptions);
        original.file = hokuto::format::ImportWiiArchive(original.bytes);
        original.file = hokuto::tools::CheckedForm(std::move(original.file), original.bytes);
        if (!original.file.native->tables || hokuto::format::ExportArchive(original.file) != original.bytes)
        {
            throw std::runtime_error("the file holds no tables, or doesn't export the archive");
        }
        original.held = hokuto::edit::HeldValuesOf(original.file);
        layout = WiiLayoutOf(original.file, original.bytes);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
        return 2;
    }

    Totals totals;
    totals.sounds = original.file.native->tables->sounds.size();
    for (const NativeSound& sound : original.file.native->tables->sounds)
    {
        totals.sequences += sound.kind == sar::SoundType::kSequence ? 1 : 0;
    }
    for (std::size_t i = 0; i < kRounds.size(); ++i)
    {
        CheckWiiRound(original, layout, kRounds[i], i, totals);
    }

    const uint16_t version = hokuto::format::WiiVersionOf(*original.file.native);
    std::printf(
        "%s: version %d.%d. %zu sounds, %zu of them sequence sounds. %zu edits in %zu rounds, and %zu edits refused of "
        "values that the archive doesn't hold. %zu values read back otherwise, and %zu rounds whose values back give "
        "other bytes. %zu exports checked for the files' sequences and the starts' fields. %zu levels measured, with "
        "the worst %.4f dB from the ratio of the volumes, and %zu further than %.2f dB. Apart: %zu sounds that clip, "
        "%zu too quiet to measure, and %zu whose voices reach the console's limit. %zu failures.\n",
        name.c_str(), version >> 8, version & 0xFF, totals.sounds, totals.sequences, totals.edits, kRounds.size(),
        totals.refused, totals.differ, totals.round_trips, totals.layouts, totals.levels, totals.worst,
        totals.wrong_levels, kTolerance, totals.clipped, totals.quiet, totals.limited, totals.failures);

    return totals.differ == 0 && totals.round_trips == 0 && totals.wrong_levels == 0 && totals.failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> arguments = xsflib::console::Arguments(argc, argv);
    hokuto::tools::TakeModelOption(arguments);
    if (arguments.size() != 2)
    {
        std::fprintf(stderr, "usage: hokuto_check_sounds [--model] <archive>\n");
        return 2;
    }

    // The archive and the group files beside it make a Hokuto file. The file has to export the archive. An archive of
    // the Wii takes other rounds.
    const std::filesystem::path path = xsflib::PathFromUtf8(arguments[1]);
    Original original;
    original.name = xsflib::Utf8(path.filename());
    const hokuto::tools::SavesLine saves_line(original.name);
    Layout layout;
    try
    {
        xsflib::ReadFile(path, original.bytes);
        if (original.bytes.size() >= 4 && std::memcmp(original.bytes.data(), "RSAR", 4) == 0)
        {
            return CheckWiiArchive(original.name, std::move(original.bytes));
        }
        original.archive = hokuto::song::OpenArchive(path);
        original.file =
            hokuto::format::ImportArchive(original.bytes, original.archive->ReadOptions().flavor,
                                          original.archive->GroupFiles(), {}, original.archive->GroupFileGroups());
        original.file = hokuto::tools::CheckedForm(std::move(original.file), original.bytes);
        const std::optional<std::vector<sar::ValuePlaces>> places =
            hokuto::format::TableValuePlacesOf(*original.file.native);
        if (!original.file.native->tables || !places || hokuto::format::ExportArchive(original.file) != original.bytes)
        {
            throw std::runtime_error("the file holds no tables, or doesn't export the archive");
        }
        original.places = *places;
        layout = LayoutOf(original.file, original.bytes, *original.archive);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", original.name.c_str(), error.what());
        return 2;
    }

    Totals totals;
    totals.sounds = original.file.native->tables->sounds.size();
    for (const NativeSound& sound : original.file.native->tables->sounds)
    {
        totals.sequences += sound.kind == sar::SoundType::kSequence ? 1 : 0;
    }

    for (std::size_t i = 0; i < kRounds.size(); ++i)
    {
        CheckRound(original, layout, kRounds[i], i, totals);
    }
    for (std::size_t i = 0; i < kPlaceRounds.size(); ++i)
    {
        CheckPlaceRound(original, kPlaceRounds[i], i, totals);
    }

    const std::string& name = original.name;
    std::printf(
        "%s: %zu sounds, %zu of them sequence sounds. %zu edits in %zu rounds. %zu lists of banks left out. %zu "
        "places given in %zu rounds by %zu edits, %zu of them with the old values and %zu with the next ones. %zu "
        "values read back otherwise, and %zu rounds whose values back give other bytes. %zu exports checked for the "
        "files' sequences and the starts' fields, %zu of them with files that moved. %zu levels measured, with the "
        "worst %.4f dB from the ratio of the volumes, and %zu further than %.2f dB. Apart: %zu sounds that clip, %zu "
        "too quiet to measure, and %zu whose voices reach the console's limit. %zu failures.\n",
        name.c_str(), totals.sounds, totals.sequences, totals.edits, kRounds.size(), totals.lists_apart, totals.places,
        totals.place_layouts, totals.place_edits, totals.old_values, totals.next_values, totals.differ,
        totals.round_trips, totals.layouts, totals.moved, totals.levels, totals.worst, totals.wrong_levels, kTolerance,
        totals.clipped, totals.quiet, totals.limited, totals.failures);

    return totals.differ == 0 && totals.round_trips == 0 && totals.wrong_levels == 0 && totals.failures == 0 ? 0 : 1;
}
