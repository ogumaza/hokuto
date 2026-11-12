// SPDX-License-Identifier: MIT

// A sound of another console as a new file of the Wii (ForWii), and the sequence, bank and wave sound files of the
// later consoles as files of the Wii (edit/other_console.h).

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

#include "ntdlib/commands/sequence.h"
#include "ntdlib/nwsnd/commands.h"
#include "ntdlib/rsar/archive_model.h"
#include "ntdlib/rsar/bank_file.h"
#include "ntdlib/rsar/convert.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/sequence_file.h"
#include "ntdlib/rsar/wave_file.h"
#include "ntdlib/sar/archive_model.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/sequence_file.h"
#include "ntdlib/sar/wave_file.h"
#include "ntdlib/sound_build.h"
#include "xsflib/binary.h"

#include "edit/bytecode.h"
#include "edit/other_console.h"
#include "edit/other_console_parts.h"
#include "edit/replace.h"
#include "edit/wii_programs.h"
#include "format/archive.h"
#include "format/drivers.h"
#include "format/kinds.h"
#include "format/model.h"
#include "format/shared.h"
#include "song/archive.h"
#include "song/biquads.h"
#include "song/items.h"

namespace hokuto::edit
{

namespace
{

namespace sar = ntdlib::sar;
using other_console_parts::AddOneBankWarnings;
using other_console_parts::AddSharingWarning;
using other_console_parts::AddTimeWarnings;
using other_console_parts::AfterStep;
using other_console_parts::AsWii;
using other_console_parts::BankNameOf;
using other_console_parts::CopyParts;
using other_console_parts::CopyValues;
using other_console_parts::CountSkipped;
using other_console_parts::DbText;
using other_console_parts::EncodedFile;
using other_console_parts::FromConverter;
using other_console_parts::GlobalsWarning;
using other_console_parts::GoesThroughConverter;
using other_console_parts::KeptOf;
using other_console_parts::kProgramsNotFollowed;
using other_console_parts::kSettingsLeft;
using other_console_parts::kVariablePrograms;
using other_console_parts::kWiiBankVersion;
using other_console_parts::kWiiSequenceVersion;
using other_console_parts::kWiiWaveSoundVersion;
using other_console_parts::LeftWavesWarning;
using other_console_parts::MergedWiiBank;
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
using other_console_parts::WaveArchiveWaves;
using other_console_parts::WavedBank;
using other_console_parts::WavedBankOf;
using other_console_parts::WavesOf;
using other_console_parts::WiiBank;
using other_console_parts::WiiBankFrom;
using other_console_parts::WiiCommands;
using other_console_parts::WithoutSkipped;
using other_console_parts::WithStartingGlobals;

// The sequence of a sound sent to the Wii, and the item where it starts (ForWii).
struct WiiSequence
{
    format::Shared<ntdlib::commands::Sequence> sequence;
    uint32_t start = 0;
};

// `played`, the commands of a sound of console `from` from item `start`, as the Wii's build `build` of NW4R SND reads
// them (ForWii). Adds the warnings about the commands that change to `song`.
WiiSequence WiiSequenceOf(Song& song, const ntdlib::commands::Sequence& played, uint32_t start, sar::Flavor from,
                          const ntdlib::SndBuild& build)
{
    // The commands that the Wii's build of NW4R SND skips stay out. The others go in as NW4R SND reads them (AsWii),
    // with commands at the sound's start that give the global variables the later consoles' starting value. The biquad
    // filters of the Switch take the Wii's nearest (MapBiquads).
    const song::Console reads{.engine = format::EngineOf(from)};
    const song::Console writes{.engine = format::Engine::kWii, .wii = build};
    auto [kept, kept_start] = WithoutSkipped(played, start, reads, writes);
    WiiCommands commands = AsWii(std::move(kept), from, build);
    const song::BiquadChanges biquads =
        song::MapBiquads(commands.sequence, std::span<const uint32_t>(&kept_start, 1), reads, writes);
    const auto [new_start, globals] = WithStartingGlobals(commands.sequence, kept_start, 0, -1);
    std::vector<format::Shared<ntdlib::commands::Sequence>> plain =
        format::WithoutChoices({format::Shared<ntdlib::commands::Sequence>(std::move(commands.sequence))});
    if (const std::optional<std::string> warning =
            SkippedWarning(CountSkipped(played, reads, writes), reads, writes, true))
    {
        song.result.warnings.push_back(*warning);
    }

    AddTimeWarnings(commands, song.result.warnings);
    if (const std::optional<std::string> warning = song::BiquadWarning(biquads, reads, writes))
    {
        song.result.warnings.push_back(*warning);
    }

    if (!globals.empty())
    {
        song.result.warnings.push_back(GlobalsWarning(globals, true));
    }

    return {std::move(plain.front()), new_start};
}

// The bank for the Wii of sound `source` of `archive` (ForWii). `programs` contains the programs of the sound's slots.
// Throws std::invalid_argument with a message for the user where its waves don't fit in `wii_wave_memory`.
WiiBank FitWiiBank(Song& song, const sar::SoundArchive& archive, const format::NativeSound& source,
                   const std::optional<WiiPrograms>& programs, std::size_t wii_wave_memory)
{
    const bool merged = programs && programs->slots.size() > 1;

    // The banks of the slots that the notes play, or the bank in the sound's first slot. The Wii's model keeps the
    // bank's waves in MEM2. When they don't fit, the bank takes only the instruments that the sound selects.
    WaveArchiveWaves read;
    const uint32_t first = source.banks.empty() ? sar::kNoItem : source.banks.front();
    const std::size_t warned = song.result.warnings.size();
    const auto bank_of = [&](bool trim)
    {
        if (merged)
        {
            return MergedWiiBank(song, archive, source.banks, *programs, read, trim);
        }
        if (first != sar::kNoItem)
        {
            return WiiBankFrom(song, archive, first, read, trim ? KeptOf(*programs, 0) : std::nullopt);
        }

        song.result.warnings.push_back(
            "The sound's first bank slot is empty. A Wii sound can use only one bank. The sound uses an empty bank.");

        return WiiBank{"BANK_0", ntdlib::rsar::BankFile::FromInstruments(kWiiBankVersion, {}, {}).Write()};
    };
    WiiBank bank = bank_of(false);
    const std::size_t whole = bank.files.wave_data.size();
    if (whole > wii_wave_memory)
    {
        const std::string holds = "Hokuto's Wii model has room for " + SizeText(wii_wave_memory) + " of waves.";
        if (!programs)
        {
            throw std::invalid_argument("The waves of the sound's bank use " + SizeText(whole) + ". " + holds +
                                        kProgramsNotFollowed);
        }

        song.result.warnings.resize(warned);
        bank = bank_of(true);
        const std::size_t kept_bytes = bank.files.wave_data.size();
        if (kept_bytes > wii_wave_memory)
        {
            const bool whole_slots = VariableSelectsPrograms(*programs);
            throw std::invalid_argument("The waves of the sound's bank use " + SizeText(kept_bytes) +
                                        ", even without the unused instruments. " + holds +
                                        (whole_slots ? kVariablePrograms : ""));
        }

        song.result.warnings.push_back("The bank keeps only the instruments that the sound uses. Its waves use " +
                                       SizeText(kept_bytes) + " instead of " + SizeText(whole) + ". " + holds);
    }

    return bank;
}

// The settings of sound `sound` of `bytes`, an archive of console `from`, that the Sound sections don't edit (ForWii).
// Adds the warnings about those that stay behind to `song`.
std::optional<sar::SoundModel> WiiSettingsOf(Song& song, std::span<const uint8_t> bytes, sar::Flavor from,
                                             uint32_t sound)
{
    // The sound's settings that the Sound sections don't edit: the remote filter goes along. The 3D settings, the
    // single play and the user parameters stay behind: NW4R SND reads other fields.
    std::optional<sar::SoundModel> settings;
    try
    {
        settings = sar::ReadArchiveModel(bytes, from).sounds.at(sound);
    }
    catch (const std::exception& error)
    {
        song.result.warnings.push_back(std::string(kSettingsLeft) + "Can't read them from this archive. " +
                                       format::AsSentence(error.what()));
    }

    if (settings && SettingsLeftOut(*settings, false))
    {
        song.result.warnings.push_back(
            "The sound's 3D settings, single play and user parameters are left out. NW4R SND reads other fields.");
    }

    return settings;
}

// The sound and its sequence in `song`'s file (ForWii).
struct AddedWiiSound
{
    uint32_t added = 0;
    uint32_t new_sequence = 0;
};

// Adds sound `sound`, `source` of the source archive, to `song`'s file for the Wii: the bank `bank`, and the sequence
// `sequence`. Throws std::invalid_argument with a message for the user where the model can't lay the archive out.
AddedWiiSound AddWiiArchive(Song& song, uint32_t sound, const format::NativeSound& source, WiiBank bank,
                            WiiSequence sequence)
{
    // The new file's archive: the bank, the sequence file and the sound.
    const std::string name = SequenceSoundName(source, sound);
    format::Model model = format::ModelOf(song.result.file);
    ntdlib::rsar::ArchiveModel& wii = *model.wii;
    const auto bank_file = static_cast<uint32_t>(wii.files.size());
    wii.files.push_back({.bytes = std::move(bank.files.file), .wave_data = std::move(bank.files.wave_data)});
    wii.banks.push_back({.name = bank.name, .file_id = bank_file});
    const auto sequence_file = static_cast<uint32_t>(wii.files.size());
    wii.files.emplace_back();
    const auto added = static_cast<uint32_t>(wii.sounds.size());
    wii.sounds.push_back({.name = name,
                          .type = ntdlib::rsar::SoundType::kSequence,
                          .file_id = sequence_file,
                          .player_id = 0,
                          .bank = static_cast<uint32_t>(wii.banks.size() - 1)});
    const auto new_sequence = static_cast<uint32_t>(song.result.file.sequences.size());
    song.result.file.sequences.push_back(std::move(sequence.sequence));
    model.sequences[sequence_file] = {.sequence = new_sequence, .version = kWiiSequenceVersion};
    model.starts[added] = sequence.start;
    try
    {
        song.result.file = format::WithModel(song.result.file, model);
    }
    catch (const xsflib::FormatError& error)
    {
        throw std::invalid_argument(std::string("Can't write the sound's sequence for the Wii. ") +
                                    format::AsSentence(error.what()));
    }

    return {added, new_sequence};
}

// The values of sound `source` in the archive's tables on the Wii (ForWii). A warning goes into `warnings` where the
// sound plays quieter.
format::NativeSound WiiValuesOf(const format::NativeSound& source, std::vector<std::string>& warnings)
{
    // The sound's values in the archive's tables. NW4R SND plays a sound's volume above 127 at 127.
    format::NativeSound values = source;
    if (values.volume > 127)
    {
        const double lost_db = 20.0 * std::log10(static_cast<double>(values.volume) / 127.0);
        warnings.push_back("The sound's volume is lowered from " + std::to_string(values.volume) +
                           " to 127, the most that the Wii plays. The sound is " + DbText(lost_db) + " dB quieter.");
        values.volume = 127;
    }

    return values;
}

} // namespace

OtherConsoleSong ForWii(const format::File& file, uint32_t sound, const std::optional<ntdlib::SndBuild>& wii_build,
                        std::size_t wii_wave_memory)
{
    if (GoesThroughConverter(file))
    {
        // A song of the GBA's drivers, a sequence of the DS or a sound of DSE goes to the Wii U first, and on from
        // there to the Wii.
        const OtherConsoleSong song = FromConverter(file, sound, false);
        return AfterStep(song, ForWii(song.file, 0, wii_build, wii_wave_memory));
    }

    RequireTables(file);
    if (file.native->engine == format::Engine::kWii)
    {
        throw std::invalid_argument("The sound already plays on the Wii.");
    }

    const auto [source, bytes, archive, sequences, sequence, start, file_id] = SourceSequenceSound(file, sound);
    const sar::Flavor from = FlavorOf(*file.native);
    if (Fixed(sequence))
    {
        throw std::invalid_argument(
            "The commands in the sequence file must stay at their offsets. Hokuto can't send such a sequence to the "
            "Wii yet.");
    }

    Song song;
    song.result.file = format::NewFile(format::Engine::kWii);
    if (wii_build)
    {
        format::Model built = format::ModelOf(song.result.file);
        built.wii_build = *wii_build;
        song.result.file = format::WithModel(song.result.file, built);
    }

    const ntdlib::SndBuild build = format::BuildOfWiiArchive(*song.result.file.native);

    // A sound of the Wii plays one bank, and NW4R SND has no bank select. The banks of the slots that the notes play go
    // in as one bank, one slot after another, and the program changes select their slots' programs there
    // (WiiProgramsOf).
    const std::optional<WiiPrograms> programs = SoundProgramsOf(*archive, source, sequence, start);
    const bool merged = programs && programs->slots.size() > 1;
    const ntdlib::commands::Sequence& played = merged ? programs->sequence : sequence;

    WiiSequence wii_sequence = WiiSequenceOf(song, played, start, from, build);
    WiiBank bank = FitWiiBank(song, *archive, source, programs, wii_wave_memory);

    if (merged)
    {
        SetSlotPrograms(song, *programs);
    }

    // The banks of the other slots stay behind.
    AddOneBankWarnings(song, *archive, source.banks, programs, merged, from, "Wii");

    const std::optional<sar::SoundModel> settings = WiiSettingsOf(song, bytes, from, sound);
    const auto [added, new_sequence] = AddWiiArchive(song, sound, source, std::move(bank), std::move(wii_sequence));

    CopyValues(song, added, WiiValuesOf(source, song.result.warnings));

    // The file as a save and an open give it: the archive that its model lays out, with the sound's remote filter.
    format::Model settled = format::ModelOf(song.result.file);
    settled.wii->sounds.at(added).remote_filter = settings ? settings->remote_filter : uint8_t{0};
    song.result.file = format::WithModel(song.result.file, settled);

    // The other sounds whose commands the sequence file holds.
    AddSharingWarning(song, song::SoundsSharingFile(*archive, sound).size());

    CopyParts(song, file, sound, added, file_id < sequences.size() ? sequences[file_id] : std::nullopt, new_sequence);

    return std::move(song.result);
}

ConvertedItem WiiSequenceFileOf(const sar::SoundArchive& archive, uint32_t sound)
{
    // The commands as the console reads them, from the labels and the starts of the sounds that play the file.
    if (sound >= archive.Sounds().size() || archive.Sounds()[sound].type != sar::SoundType::kSequence)
    {
        throw std::invalid_argument("The archive has no such sequence sound.");
    }

    const sar::Flavor from = archive.ReadOptions().flavor;
    std::span<const uint8_t> bytes;
    sar::SequenceFile read;
    try
    {
        bytes = song::ItemFile(archive, {.kind = song::ItemKind::kSequence, .index = sound});
        read = sar::SequenceFile::Read(bytes);
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Can't read the sequence file. ") + format::AsSentence(error.what()));
    }

    std::vector<uint32_t> starts = {archive.Sounds()[sound].start_offset};
    for (const uint32_t other : song::SoundsSharingFile(archive, sound))
    {
        starts.push_back(archive.Sounds()[other].start_offset);
    }

    const ntdlib::commands::Sequence sequence = SequenceFileCommands(read, bytes, starts);
    if (Fixed(sequence))
    {
        throw std::invalid_argument(
            "The commands in the sequence file must stay at their offsets. Hokuto can't convert such a sequence for "
            "the Wii yet.");
    }

    // The commands as the build of a new file of the Wii reads them.
    const ntdlib::SndBuild build = format::BuildOfWiiArchive(*format::NewFile(format::Engine::kWii).native);
    CommandsForWii commands = WiiCommandsOf(sequence, from, build);
    auto [data, offsets] = EncodedFile(
        commands.sequence, song::GrammarOf(song::Console{.engine = format::Engine::kWii, .wii = build}), "Wii");
    ntdlib::rsar::SequenceFile file;
    file.version = kWiiSequenceVersion;
    file.data = std::move(data);
    for (const ntdlib::nwsnd::LabelOffset& label : offsets)
    {
        file.labels.push_back({label.name, label.offset});
    }

    return {.file = file.Write(), .waves = {}, .warnings = std::move(commands.warnings)};
}

std::optional<std::string> WiiWaveRoomWarning(std::size_t bytes, std::size_t memory)
{
    if (bytes <= memory)
    {
        return std::nullopt;
    }

    return "The waves use " + SizeText(bytes) + ". Hokuto's Wii model has room for " + SizeText(memory) +
           " of waves and can't play them.";
}

ConvertedItem WiiBankFilesOf(const sar::SoundArchive& archive, uint32_t index, std::size_t wii_wave_memory)
{
    const uint32_t id = sar::ItemId(sar::kBankItem, index);
    WaveArchiveWaves read;
    try
    {
        const WavedBank<ntdlib::rsar::WaveFile> later =
            WavedBankOf<ntdlib::rsar::WaveFile>(archive, id, read, std::nullopt, ntdlib::rsar::WiiWaveOf);
        ntdlib::rsar::BankFile::Files files = ntdlib::rsar::WiiBankOf(later.file, kWiiBankVersion, later.waves).Write();
        ConvertedItem result{.file = std::move(files.file), .waves = std::move(files.wave_data), .warnings = {}};
        if (later.left > 0)
        {
            result.warnings.push_back(LeftWavesWarning(later.left, BankNameOf(archive, id)));
        }

        if (const std::optional<std::string> warning = WiiWaveRoomWarning(result.waves.size(), wii_wave_memory))
        {
            result.warnings.push_back(*warning);
        }

        return result;
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument("Can't convert bank " + BankNameOf(archive, id) + " for the Wii. " +
                                    format::AsSentence(error.what()));
    }
}

ConvertedItem WiiWaveSoundFilesOf(const sar::SoundArchive& archive, uint32_t sound, std::size_t wii_wave_memory)
{
    try
    {
        // Each entry of the file's table of waves takes its wave as a wave of the Wii.
        const sar::WaveSoundFile file = sar::WaveSoundFile::Parse(
            song::ItemFile(archive, {.kind = song::ItemKind::kWaveSoundFile, .index = sound}), archive.ReadOptions());
        WaveArchiveWaves read;
        std::vector<std::optional<ntdlib::rsar::WaveFile>> waves;
        std::size_t left = 0;
        for (const sar::WaveId& wave : file.waves)
        {
            const std::vector<std::vector<uint8_t>>& held = WavesOf(archive, wave.wave_archive_item, read);
            if (wave.index >= held.size())
            {
                waves.emplace_back();
                continue;
            }

            try
            {
                waves.push_back(ntdlib::rsar::WiiWaveOf(sar::WaveFile::Read(held[wave.index])));
            }
            catch (const std::exception&)
            {
                waves.emplace_back();
                ++left;
            }
        }

        ntdlib::rsar::WsdFile::Files files = ntdlib::rsar::WiiWsdOf(file, kWiiWaveSoundVersion, waves).Write();
        ConvertedItem result{.file = std::move(files.file), .waves = std::move(files.wave_data), .warnings = {}};
        if (left > 0)
        {
            result.warnings.push_back(
                std::to_string(left) +
                (left == 1 ? " wave of the file is left out because it uses an encoding that the Wii can't play, "
                             "such as IMA-ADPCM. Its notes are silent."
                           : " waves of the file are left out because they use an encoding that the Wii can't play, "
                             "such as IMA-ADPCM. Their notes are silent."));
        }

        if (const std::optional<std::string> warning = WiiWaveRoomWarning(result.waves.size(), wii_wave_memory))
        {
            result.warnings.push_back(*warning);
        }

        return result;
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Can't convert the wave sound file for the Wii. ") +
                                    format::AsSentence(error.what()));
    }
}

} // namespace hokuto::edit
