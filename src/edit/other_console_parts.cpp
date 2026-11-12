// SPDX-License-Identifier: MIT

// The parts of other_console_parts.h that the paths to another console share: lists in sentences, the guards of the
// path, the source sequence sound, the warning of merged banks, and the parts and values that a new sound copies from
// the source.

#include "edit/other_console_parts.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/sar/archive_model.h"

#include "edit/bytecode.h"
#include "edit/effects.h"
#include "edit/files.h"
#include "edit/tables.h"
#include "format/archive.h"
#include "format/engines.h"
#include "format/kinds.h"
#include "format/model.h"
#include "format/shared.h"
#include "song/archive.h"
#include "song/items.h"

namespace hokuto::edit::other_console_parts
{
namespace
{

// The values of a sound in the archive's tables that the new sound takes through the Sound sections' edits. The new
// sound plays on the new file's player. Its bank slots name the new banks.
constexpr std::array kCopiedValues = {
    SoundValue::kVolume,      SoundValue::kPanMode, SoundValue::kPanCurve,        SoundValue::kPlayerPriority,
    SoundValue::kActorPlayer, SoundValue::kTracks,  SoundValue::kChannelPriority, SoundValue::kReleasePriorityFix};

// Copies the effects on the aux buses of `file` that the console of `song`'s file runs (CopyParts). `to_wii` says
// whether that console is the Wii, and `console` names it in the warnings.
void CopyEffects(Song& song, const format::File& file, bool to_wii, const std::string& console)
{
    format::File& target = song.result.file;

    // The effects that the other console runs stay on their buses. The others stay behind. The effects of a file of the
    // Wii play on the Wii alone, and the Wii runs none of the other consoles' effects.
    const bool wii = file.native && file.native->engine == format::Engine::kWii;
    for (std::size_t bus = 0; bus < format::kAuxBuses; ++bus)
    {
        std::vector<std::string> left;
        for (const format::Effect& effect : file.aux_effects[bus])
        {
            if (!wii && RunsEffect(target.engine, effect.kind))
            {
                target.aux_effects[bus].push_back(effect);
            }
            else
            {
                left.emplace_back(EffectName(effect.kind));
            }
        }
        if (!left.empty())
        {
            const bool one = left.size() == 1;
            const std::string why = wii      ? "Hokuto plays Wii effects only on the Wii."
                                    : to_wii ? "The Wii uses AXFX effects instead."
                                             : "The " + console + " doesn't support " + (one ? "it." : "them.");
            song.result.warnings.push_back("The " + ListOf(left) + " on bus " +
                                           std::string(1, static_cast<char>('A' + bus)) +
                                           (one ? " is left out. " : " are left out. ") + why);
        }
    }
}

} // namespace

// `names` in a sentence, such as "A, B and C".
std::string ListOf(std::span<const std::string> names)
{
    std::string list;
    for (std::size_t i = 0; i < names.size(); ++i)
    {
        list += (i == 0 ? "" : i + 1 == names.size() ? " and " : ", ") + names[i];
    }

    return list;
}

bool SettingsLeftOut(const sar::SoundModel& settings, bool remote_filter)
{
    return settings.single_play || settings.option_17 || settings.settings_3d != sar::Sound3dModel{} ||
           settings.after_3d != 0 || (remote_filter && settings.remote_filter != 0) ||
           std::ranges::any_of(settings.user_options, [](const auto& option) { return option.has_value(); });
}

void AddSharingWarning(Song& song, std::size_t sharing)
{
    if (sharing > 0)
    {
        song.result.warnings.push_back("The sequence file also contains the commands of " + std::to_string(sharing) +
                                       (sharing == 1 ? " other sound" : " other sounds") +
                                       ". They are copied but unused.");
    }
}

bool GoesThroughConverter(const format::File& file)
{
    return file.native && (file.native->engine == format::Engine::kGba || file.native->driver ||
                           file.native->engine == format::Engine::kDs || file.native->engine == format::Engine::kDse);
}

void RequireTables(const format::File& file)
{
    if (!file.native || !file.native->tables)
    {
        throw std::invalid_argument(
            "Can't read the tables of this archive. Sending a song to another console requires them.");
    }
}

OtherConsoleSong AfterStep(const OtherConsoleSong& first, OtherConsoleSong next)
{
    next.warnings.insert(next.warnings.begin(), first.warnings.begin(), first.warnings.end());

    return next;
}

// The sequence that sound `sound` of `archive` plays and the item where it starts: the sequence of `file` that the
// sound's file holds, or the commands that Hokuto reads from the file's bytes. `sequences` gives the file's sequence of
// each of the archive's files (format::SequencesOfFiles). Throws std::invalid_argument with a message for the user for
// a sequence that can't be read, and for a start where no command starts.
std::pair<ntdlib::commands::Sequence, uint32_t> SequenceOf(const format::File& file, const sar::SoundArchive& archive,
                                                           std::span<const std::optional<uint32_t>> sequences,
                                                           uint32_t sound)
{
    const sar::SoundInfo& info = archive.Sounds()[sound];
    ntdlib::commands::Sequence sequence;
    if (info.file_id < sequences.size() && sequences[info.file_id] && *sequences[info.file_id] < file.sequences.size())
    {
        sequence = *file.sequences[*sequences[info.file_id]];
    }
    else
    {
        try
        {
            sequence =
                ReadIncoming(song::ItemFile(archive, {.kind = song::ItemKind::kSequence, .index = sound})).sequence;
        }
        catch (const std::runtime_error& error)
        {
            throw std::invalid_argument(error.what());
        }
    }

    const std::optional<uint32_t> start = ItemAtOffset(
        Offsets(sequence, song::Console{.engine = format::EngineOf(archive.ReadOptions().flavor)}), info.start_offset);
    if (!start)
    {
        throw std::invalid_argument("The sound doesn't start at a command of its sequence.");
    }

    return {std::move(sequence), *start};
}

SourceSequence SourceSequenceSound(const format::File& file, uint32_t sound)
{
    const format::NativeTables& tables = *file.native->tables;
    if (sound >= tables.sounds.size() || tables.sounds[sound].kind != sar::SoundType::kSequence)
    {
        throw std::invalid_argument("Only a sequence sound can be sent to another console.");
    }

    // The archive that the file exports, with its group files, and the sound's sequence.
    std::vector<uint8_t> bytes = format::ExportArchive(file);
    std::shared_ptr<const sar::SoundArchive> archive =
        song::LoadArchive(bytes, "the archive", format::ExportGroupFiles(file));
    std::vector<std::optional<uint32_t>> sequences = format::SequencesOfFiles(file, bytes, *archive);
    auto [sequence, start] = SequenceOf(file, *archive, sequences, sound);
    const uint32_t file_id = archive->Sounds()[sound].file_id;

    return {tables.sounds[sound],
            std::move(bytes),
            std::move(archive),
            std::move(sequences),
            std::move(sequence),
            start,
            file_id};
}

std::string MergedBankWarning(const std::string& one_bank, const std::string& bank_name,
                              std::span<const std::pair<std::string, std::string>> ranges, const WiiPrograms& programs)
{
    std::string warning = one_bank;
    if (ranges.size() == 1)
    {
        warning += "The notes use only the bank in one slot, " + ranges.front().first + ". It is placed " +
                   ranges.front().second + ".";
    }
    else if (!ranges.empty())
    {
        std::vector<std::string> placed;
        for (const auto& [name, place] : ranges)
        {
            placed.push_back(name + " " + place);
        }

        warning += "The notes use the banks in several slots. They are merged into one bank, " + bank_name + ": " +
                   ListOf(placed) + ".";
    }
    else
    {
        warning += "No bank that the notes use can be converted. The sound uses an empty bank.";
    }

    std::vector<std::string> changed;
    if (programs.bank_selects > 0)
    {
        changed.push_back(programs.bank_selects == 1
                              ? "1 bank select is now a program change"
                              : std::to_string(programs.bank_selects) + " bank selects are now program changes");
    }

    if (programs.programs > 0)
    {
        changed.push_back(programs.programs == 1
                              ? "1 program change is renumbered for its slot"
                              : std::to_string(programs.programs) + " program changes are renumbered for their slots");
    }
    if (!changed.empty())
    {
        warning += " " + ListOf(changed) + ".";
    }

    return warning;
}

// `song`'s file as a save and an open give it: the archive that its model lays out. The new sound, sound `added`, takes
// the settings of `source`, the model of the source sound, that the archive's tables hold beyond the Sound sections'
// values: its remote filter, its single play, its 3D settings, its options 17 and 28 to 31, and the value after its 3D
// settings. They stay behind with a warning when the new archive can't take them.
void Settle(Song& song, uint32_t added, const std::optional<sar::SoundModel>& source)
{
    const format::Model model = format::ModelOf(song.result.file);
    if (source)
    {
        format::Model with = model;
        sar::SoundModel& sound = with.archive.sounds.at(added);
        sound.remote_filter = source->remote_filter;
        sound.single_play = source->single_play;
        sound.settings_3d = source->settings_3d;
        sound.option_17 = source->option_17;
        sound.user_options = source->user_options;
        sound.after_3d = source->after_3d;
        try
        {
            song.result.file = format::WithModel(song.result.file, with);
            return;
        }
        catch (const std::exception& error)
        {
            song.result.warnings.push_back(std::string(kSettingsLeft) + format::AsSentence(error.what()));
        }
    }

    song.result.file = format::WithModel(song.result.file, model);
}

// The parts of `file` that go with sound `sound` into `song`'s file as sound `added`: the file's tags, the sound's
// tags, scenes and tracks' colours, the aux buses' volumes and the effects that the other console runs, the sound's
// mode of mixing, and the mode of its sequence, `sequence`, when it has one. The new file holds that sequence as
// `new_sequence`, when it holds it among its sequences.
void CopyParts(Song& song, const format::File& file, uint32_t sound, uint32_t added, std::optional<uint32_t> sequence,
               std::optional<uint32_t> new_sequence)
{
    format::File& target = song.result.file;
    const bool to_wii = target.native->engine == format::Engine::kWii;
    const bool to_ds = target.native->engine == format::Engine::kDs;
    const std::string console = to_ds    ? std::string("DS")
                                : to_wii ? std::string("Wii")
                                         : song::ConsoleName(FlavorOf(*target.native));
    target.tags = file.tags;
    if (const auto tags = file.sound_tags.find(sound); tags != file.sound_tags.end())
    {
        target.sound_tags[added] = tags->second;
    }

    for (const format::Scene& scene : file.scenes)
    {
        if (scene.sound == sound)
        {
            target.scenes.push_back(scene);
            target.scenes.back().sound = added;
        }
    }

    for (const auto& [place, colour] : file.track_colours)
    {
        if (place.sound == sound)
        {
            target.track_colours[{added, place.track}] = colour;
        }
    }

    const auto mixing = file.sound_mixing.find(sound);
    target.mixing = mixing != file.sound_mixing.end() ? mixing->second : file.mixing;
    if (const std::optional<std::string> refused = format::MixingRefused(target.engine, target.mixing))
    {
        target.mixing = format::Mixing::kStrict;
        song.result.warnings.push_back(*refused);
    }

    if (!to_ds)
    {
        target.aux_volumes = file.aux_volumes;
    }

    CopyEffects(song, file, to_wii, console);

    // A timeline stays a timeline, with its converter's hints. A song of NintendoWare's MIDI converter has another
    // layout on the DS.
    if (file.timelines && !to_ds)
    {
        target.timelines.emplace();
        const auto hints = sequence && new_sequence ? file.timelines->find(*sequence) : file.timelines->end();
        if (hints != file.timelines->end())
        {
            (*target.timelines)[*new_sequence] = hints->second;
        }
    }
}

// Gives sound `added` of `song`'s file `values`, the values of a sound in the archive's tables that kCopiedValues
// names, through the Sound sections' edits. A value that the new archive can't take stays behind with a warning.
void CopyValues(Song& song, uint32_t added, const format::NativeSound& values)
{
    for (const SoundValue value : kCopiedValues)
    {
        const uint32_t wanted = ValueOf(values, value, 0);
        if (ValueOf(song.result.file.native->tables->sounds[added], value, 0) == wanted)
        {
            continue;
        }

        try
        {
            song.result.file = ChangeSound(song.result.file, added, {.value = value, .slot = 0, .to = wanted});
        }
        catch (const std::invalid_argument& error)
        {
            song.result.warnings.push_back("The sound's " + NameOf(value) + " is left out. " +
                                           format::AsSentence(error.what()));
        }
    }
}

bool VariableSelectsPrograms(const WiiPrograms& programs)
{
    return std::ranges::any_of(programs.played, [](const auto& slot) { return !slot.second.has_value(); });
}

std::string SequenceSoundName(const format::NativeSound& source, uint32_t sound)
{
    return source.name.empty() ? "SEQ_" + std::to_string(sound) : source.name;
}

std::string DbText(double db)
{
    std::array<char, 16> text{};
    std::snprintf(text.data(), text.size(), "%.1f", db);

    return text.data();
}

} // namespace hokuto::edit::other_console_parts
