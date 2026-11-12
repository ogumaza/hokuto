// SPDX-License-Identifier: MIT

// A sound of another console as a new file of the DS (edit/other_console.h).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/ntr/snd/commands.h"
#include "ntdlib/nwsnd/util.h"
#include "ntdlib/sar/archive_model.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/wave_file.h"
#include "ntdlib/sdat/files.h"
#include "ntdlib/sdat/formats.h"
#include "ntdlib/sdat/heap.h"
#include "ntdlib/sdat/layout.h"

#include "edit/bytecode.h"
#include "edit/ds_conversion.h"
#include "edit/ds_new_items.h"
#include "edit/other_console.h"
#include "edit/other_console_parts.h"
#include "edit/tables.h"
#include "edit/waves.h"
#include "edit/wii_programs.h"
#include "format/archive.h"
#include "format/drivers.h"
#include "format/kinds.h"
#include "format/model.h"
#include "format/shared.h"
#include "song/archive.h"

namespace hokuto::edit
{

namespace
{

namespace sar = ntdlib::sar;
namespace sdat = ntdlib::sdat;
using other_console_parts::AddOneBankWarnings;
using other_console_parts::AfterStep;
using other_console_parts::BankNameOf;
using other_console_parts::CopyParts;
using other_console_parts::DbText;
using other_console_parts::FromConverter;
using other_console_parts::FromWii;
using other_console_parts::GainAt;
using other_console_parts::GoesThroughConverter;
using other_console_parts::Held;
using other_console_parts::HeldOf;
using other_console_parts::KeptOf;
using other_console_parts::kProgramsNotFollowed;
using other_console_parts::kSettingsLeft;
using other_console_parts::kVariablePrograms;
using other_console_parts::ListOf;
using other_console_parts::MergedBank;
using other_console_parts::MergedBankOf;
using other_console_parts::Mono;
using other_console_parts::RequireTables;
using other_console_parts::SequenceOf;
using other_console_parts::SequenceSoundName;
using other_console_parts::SetSlotPrograms;
using other_console_parts::SettingsLeftOut;
using other_console_parts::SizeText;
using other_console_parts::SkippedWarning;
using other_console_parts::Song;
using other_console_parts::SoundProgramsOf;
using other_console_parts::SourceSequenceSound;
using other_console_parts::VariableSelectsPrograms;
using other_console_parts::WaveArchiveNameOf;
using other_console_parts::WaveArchiveWaves;
using other_console_parts::WavedBank;
using other_console_parts::WavedBankOf;

// The warning about `left` waves of bank `name` that Hokuto can't read for the DS.
std::string LeftDsWavesWarning(std::size_t left, const std::string& name)
{
    return std::to_string(left) + (left == 1 ? " wave of bank " : " waves of bank ") + name +
           (left == 1 ? " is left out because Hokuto can't read it. Its regions are silent."
                      : " are left out because Hokuto can't read them. Their regions are silent.");
}

// The rates that the waves of a sound take in turn when they don't fit on the DS at its mix rate: 3/4, 1/2, 3/8 and 1/4
// of that rate, near enough.
constexpr std::array<uint32_t, 4> kDsLowerRates = {24000, 16364, 12000, 8182};

// A wave of a later console that converts to the DS. ds_conversion reads it in this form.
sar::WaveFile WaveAsItIs(sar::WaveFile wave)
{
    return wave;
}

// The bank, its DS files and the highest sample rate of its waves (FitDsBank).
struct FittedDsBank
{
    MergedBank<sar::WaveFile> bank;
    std::optional<DsBankFiles> files;
    uint32_t most_rate = kDsMixRate;
};

// The bank for the DS of sound `source` of `archive` (FitDsBank): the banks of the slots that the notes play, merged,
// or the bank in the sound's first slot. With `trim`, the bank keeps only the instruments that `programs` selects.
// `read` keeps the wave archives read so far, and the warnings go into `song`.
MergedBank<sar::WaveFile> SoundBankForDs(Song& song, const sar::SoundArchive& archive,
                                         const format::NativeSound& source, const std::optional<WiiPrograms>& programs,
                                         WaveArchiveWaves& read, bool trim)
{
    const bool merged = programs && programs->slots.size() > 1;
    const uint32_t first = source.banks.empty() ? sar::kNoItem : source.banks.front();
    if (merged)
    {
        return MergedBankOf<sar::WaveFile>(song, archive, source.banks, *programs, read, trim, WaveAsItIs,
                                           "A DS sound can use only one bank because NITRO-Composer has no "
                                           "bank select. ",
                                           LeftDsWavesWarning);
    }

    if (first == sar::kNoItem)
    {
        song.result.warnings.push_back(
            "The sound's first bank slot is empty. A DS sound can use only one bank. The sound uses an empty "
            "bank.");
        return {"BANK_0", {}, {}};
    }

    const std::string name = BankNameOf(archive, first);
    try
    {
        WavedBank<sar::WaveFile> bank =
            WavedBankOf<sar::WaveFile>(archive, first, read, trim ? KeptOf(*programs, 0) : std::nullopt, WaveAsItIs);
        if (bank.left > 0)
        {
            song.result.warnings.push_back(LeftDsWavesWarning(bank.left, name));
        }

        return {name, std::move(bank.file), std::move(bank.waves)};
    }
    catch (const std::exception& error)
    {
        song.result.warnings.push_back("Bank " + name + " is left out. " + format::AsSentence(error.what()) +
                                       " The sound uses an empty bank instead.");
        return {name, {}, {}};
    }
}

// The gain of sound `source` at `level` for a mono wave at the centre, at least 1 (FitDsBank).
double StereoGain(const format::NativeSound& source, const MonoLevel& level)
{
    Mono mono;
    mono.pan_mode = ValueOf(source, SoundValue::kPanMode, 0);
    mono.pan_info = ntdlib::nwsnd::PanInfoOf(ValueOf(source, SoundValue::kPanCurve, 0));
    const float kCentre = 0.0f;

    return std::max(GainAt(mono, level, {&kCentre, 1}), 1.0);
}

// The DS files of `bank` with sample rates up to `most_rate`, or nothing where they don't fit in `room` bytes. The
// waves take the stereo gain of sound `source` (FitDsBank).
std::optional<DsBankFiles> DsFilesIn(const MergedBank<sar::WaveFile>& bank, uint32_t most_rate, std::size_t room,
                                     const format::NativeSound& source)
{
    if (DsWavesSize(bank.file, bank.waves, most_rate) > room)
    {
        return std::nullopt;
    }

    const auto stereo_gain = [&source](const MonoLevel& level)
    {
        return StereoGain(source, level);
    };

    // The heap puts each file on a multiple of 32 bytes. A large wave archive loads its waves one at a time, each
    // on such a multiple, with a table of 8 bytes for each wave (sdat::SoundHeap).
    DsBankFiles made = DsBankFilesOf(bank.file, bank.waves, stereo_gain, most_rate);
    if (made.bank.size() + made.waves.size() + 40 * made.wave_count + 96 > room)
    {
        return std::nullopt;
    }

    return made;
}

// The bank for the DS of sound `source` of `archive`, with its files and the highest sample rate of its waves (ForDs).
// `programs` contains the programs of the sound's slots. The files fit in `ds_memory` beside a sequence file of
// `sequence_size` bytes. Throws std::invalid_argument with a message for the user where they don't.
FittedDsBank FitDsBank(Song& song, const sar::SoundArchive& archive, const format::NativeSound& source,
                       const std::optional<WiiPrograms>& programs, std::size_t ds_memory, std::size_t sequence_size)
{
    // The banks of the slots that the notes play, or the bank in the sound's first slot. The DS's model keeps its files
    // in main memory. When they don't fit, the bank takes only the instruments that the sound selects.
    WaveArchiveWaves read;
    const std::size_t warned = song.result.warnings.size();

    // Encoding takes time, and a size past the room stays past it: the waves' bytes come to about what DsWavesSize
    // returns.
    const std::size_t room = ds_memory > sequence_size ? ds_memory - sequence_size : 0;
    const std::string holds =
        "Hokuto's DS model has room for " + SizeText(room) + " of banks and waves besides the sequence.";

    // Every instrument at the DS's rate, then only the instruments that the sound selects, and then those at lower
    // rates. Each step loses more of the sound than the one before it.
    MergedBank<sar::WaveFile> bank = SoundBankForDs(song, archive, source, programs, read, false);
    const std::size_t whole = DsWavesSize(bank.file, bank.waves);
    std::optional<DsBankFiles> files = DsFilesIn(bank, kDsMixRate, room, source);
    uint32_t most_rate = kDsMixRate;
    if (!files && programs)
    {
        song.result.warnings.resize(warned);
        bank = SoundBankForDs(song, archive, source, programs, read, true);
        files = DsFilesIn(bank, kDsMixRate, room, source);
        song.result.warnings.push_back(
            "The bank keeps only the instruments that the sound uses. With every instrument, the bank and its waves "
            "would use " +
            SizeText(whole) + ". " + holds);
    }

    for (const uint32_t rate : kDsLowerRates)
    {
        if (files)
        {
            break;
        }

        files = DsFilesIn(bank, rate, room, source);
        most_rate = rate;
    }
    if (!files)
    {
        throw std::invalid_argument(
            "The sound's bank and its waves use " + SizeText(DsWavesSize(bank.file, bank.waves, most_rate)) + " at " +
            std::to_string(most_rate) + " Hz" + (programs ? ", even without the unused instruments. " : ". ") + holds +
            (!programs                            ? kProgramsNotFollowed
             : VariableSelectsPrograms(*programs) ? kVariablePrograms
                                                  : ""));
    }

    if (most_rate < kDsMixRate)
    {
        song.result.warnings.push_back(
            "The waves don't fit in Hokuto's DS model at their sample rates. " + std::to_string(files->rates_lowered) +
            (files->rates_lowered == 1 ? " wave is" : " waves are") + " resampled to " + std::to_string(most_rate) +
            " Hz. Frequencies above " + std::to_string(most_rate / 2) + " Hz are lost. " + holds);
    }

    return {std::move(bank), std::move(files), most_rate};
}

// Adds the warnings about sound `sound` of `archive`, whose bytes `bytes` give, that the DS can't play as `source`
// sets it: its notes at once, and the settings that the DS has no place for (ForDs).
void AddDsSoundWarnings(Song& song, const sar::SoundArchive& archive, std::span<const uint8_t> bytes, uint32_t sound,
                        const format::NativeSound& source, sar::Flavor from)
{
    // The DS plays 16 channels at once, and a stereo wave uses one there.
    try
    {
        const Held at_once = HeldOf(archive, sound);
        if (at_once.mono_channels > kDsChannels)
        {
            song.result.warnings.push_back("The sound has up to " + std::to_string(at_once.mono_channels) +
                                           " notes at once. The DS has " + std::to_string(kDsChannels) +
                                           " channels. NITRO-Composer drops the notes with the lowest priority.");
        }
    }
    catch (const std::exception&)
    {
        // A read that fails here fails again for the user, in the arrangement. The count only adds a warning.
    }

    // The sound's settings that the DS has no place for.
    try
    {
        const sar::SoundModel settings = sar::ReadArchiveModel(bytes, from).sounds.at(sound);
        if (SettingsLeftOut(settings, true))
        {
            song.result.warnings.push_back(
                "The sound's 3D settings, single play, user parameters and remote filter are left out. DS sounds "
                "have no such settings.");
        }
    }
    catch (const std::exception& error)
    {
        song.result.warnings.push_back(std::string(kSettingsLeft) + "Can't read them from this archive. " +
                                       format::AsSentence(error.what()));
    }

    if (source.pan_mode != 0 || source.pan_curve != 0)
    {
        song.result.warnings.push_back(
            "The sound's pan mode and pan curve are left out. The DS has only one way to pan.");
    }
}

// Adds sound `sound`, `source` of the source archive, to `song`'s file for the DS: the wave archive and the bank of
// `bank` with `files`, and the sequence file `sequence_file` at volume `volume`. Returns the new sound's sequence.
// Throws std::invalid_argument with a message for the user where the files don't fit in `ds_memory` (ForDs).
uint32_t AddDsArchive(Song& song, uint32_t sound, const format::NativeSound& source,
                      const MergedBank<sar::WaveFile>& bank, const std::optional<DsBankFiles>& files,
                      const std::vector<uint8_t>& sequence_file, uint8_t volume, std::size_t ds_memory)
{
    // The new file's archive: the wave archive, the bank and the sound.
    const std::string sound_name = SequenceSoundName(source, sound);
    std::string bank_name = bank.name == sound_name ? bank.name + "_BANK" : bank.name;
    std::string waves_name = WaveArchiveNameOf(bank_name);
    if (waves_name == sound_name)
    {
        waves_name += "_WAVES";
    }

    format::File& made = song.result.file;
    std::array<uint16_t, 4> slots{0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
    if (!files->waves.empty())
    {
        made = AddDsWaveArchive(made, waves_name, files->waves);
        slots.front() = 0;
    }

    made = WithDsBankFile(made, 0, bank_name, files->bank, slots);
    const auto added = static_cast<uint32_t>(sdat::ReadArchiveModel(format::ExportArchive(made)).sequences.size());
    made = AddDsSequenceSound(made, sound_name, sequence_file,
                              DsSoundValues{.bank = 0,
                                            .volume = volume,
                                            .channel_priority = std::min<uint8_t>(source.channel_priority, 127),
                                            .player_priority = std::min<uint8_t>(source.player_priority, 127),
                                            .player = 0});

    // The model's heap has to hold what the sound loads. It places each file and wave on a multiple of 32 bytes.
    {
        const std::vector<uint8_t> built = format::ExportArchive(made);
        const sdat::Archive parsed = sdat::Archive::Parse(built);
        std::vector<uint8_t> memory(ds_memory);
        sdat::SoundHeap heap(memory, 0, 0, static_cast<uint32_t>(ds_memory));
        try
        {
            heap.LoadBank(parsed, 0);
            heap.LoadFile(parsed, parsed.sequences.at(added)->file_id);
        }
        catch (const std::runtime_error&)
        {
            throw std::invalid_argument("The sound's files don't fit in the " + SizeText(ds_memory) +
                                        " of memory in Hokuto's DS model.");
        }
    }

    return added;
}

// The sequence file of `commands` for the DS (ForDs). Throws std::invalid_argument with a message for the user where
// the DS's bytecode can't hold them.
std::vector<uint8_t> DsSequenceFileOf(const DsCommands& commands)
{
    try
    {
        return sdat::SequenceFile(ntdlib::ntr::snd::EncodeSequence(commands.sequence).bytes);
    }
    catch (const std::invalid_argument& error)
    {
        throw std::invalid_argument(std::string("Can't write the sound's sequence for the DS. ") +
                                    format::AsSentence(error.what()));
    }
}

// The volume of sound `source` on the DS, whose bank `files` scaled its waves by DsBankFiles::scale (ForDs). A warning
// goes into `warnings` where the sound plays quieter.
uint8_t DsSoundVolume(const format::NativeSound& source, const DsBankFiles& files, std::vector<std::string>& warnings)
{
    // A sound's volume above 127 reads past the DS's table of levels. A level that the bank's waves gave up to keep
    // their samples within 16 bits comes back through the sound's volume, up to that volume's limit.
    const double wanted = static_cast<double>(source.volume) / files.scale;
    const auto volume = static_cast<uint8_t>(std::min(std::lround(wanted), 127L));
    const double lost_db = 20.0 * std::log10(std::max(wanted, 127.0) / 127.0);
    if (lost_db >= 0.05)
    {
        warnings.push_back("The sound plays " + DbText(lost_db) + " dB quieter on the DS. " +
                           (files.scale < 1.0 ? "The samples of the waves can't exceed 16 bits. A sound's volume "
                                                "can't exceed 127."
                                              : "A sound's volume can't exceed 127."));
    }

    return volume;
}

// `song` after it converts on from the Wii U to the DS (ForDs), with the warnings of both steps.
OtherConsoleSong OnToDs(const OtherConsoleSong& song, std::size_t ds_memory)
{
    return AfterStep(song, ForDs(song.file, 0, ds_memory));
}

} // namespace

OtherConsoleSong ForDs(const format::File& file, uint32_t sound, std::size_t ds_memory)
{
    if (file.native && file.native->engine == format::Engine::kDs)
    {
        throw std::invalid_argument("The sound already plays on the DS.");
    }

    if (GoesThroughConverter(file))
    {
        // A song of the GBA's drivers or a sound of DSE goes to the Wii U first, and on from there to the DS.
        return OnToDs(FromConverter(file, sound, true), ds_memory);
    }

    RequireTables(file);
    if (file.native->engine == format::Engine::kWii)
    {
        // A sound of the Wii goes to the Wii U first, and on from there to the DS.
        return OnToDs(FromWii(file, sound), ds_memory);
    }

    const auto [source, bytes, archive, sequences, sequence, start, file_id] = SourceSequenceSound(file, sound);
    const sar::Flavor from = FlavorOf(*file.native);
    if (Fixed(sequence))
    {
        throw std::invalid_argument(
            "The commands in the sequence file must stay at their offsets. Hokuto can't send such a sequence to the "
            "DS yet.");
    }

    Song song;
    song.result.file = format::NewFile(format::Engine::kDs);

    // A sound of the DS plays one bank, and NITRO-Composer has no bank select. The banks of the slots that the notes
    // play go in as one bank, one slot after another, and the program changes select their slots' programs there
    // (WiiProgramsOf).
    const std::optional<WiiPrograms> programs = SoundProgramsOf(*archive, source, sequence, start);
    const bool merged = programs && programs->slots.size() > 1;

    // The commands as the DS reads them, and the sequence file.
    const song::Console reads{.engine = format::EngineOf(from)};
    const DsCommands commands = AsDs(merged ? programs->sequence : sequence, start, source.tracks, reads);
    if (!commands.stuck_variables.empty())
    {
        throw std::invalid_argument(
            "The sound uses every sound variable and some track variables too. The DS has no track variables, and "
            "no sound variable is free to replace them.");
    }

    const std::vector<uint8_t> sequence_file = DsSequenceFileOf(commands);
    AddDsCommandWarnings(commands, reads, song.result.warnings);

    auto [bank, files, most_rate] = FitDsBank(song, *archive, source, programs, ds_memory, sequence_file.size());

    if (merged)
    {
        SetSlotPrograms(song, *programs);
    }

    AddDsBankWarnings(*files, most_rate == kDsMixRate, song.result.warnings);
    AddOneBankWarnings(song, *archive, source.banks, programs, merged, from, "DS");
    AddDsSoundWarnings(song, *archive, bytes, sound, source, from);

    const uint8_t volume = DsSoundVolume(source, *files, song.result.warnings);
    const uint32_t added = AddDsArchive(song, sound, source, bank, files, sequence_file, volume, ds_memory);
    CopyParts(song, file, sound, added, file_id < sequences.size() ? sequences[file_id] : std::nullopt, std::nullopt);

    return std::move(song.result);
}

} // namespace hokuto::edit
