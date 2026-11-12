// SPDX-License-Identifier: MIT

// A sound of the Wii as a new file of the Wii U (FromWii), and the sequence and bank files of the Wii as files of the
// later consoles (edit/other_console.h).

#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/nwsnd/commands.h"
#include "ntdlib/rsar/convert.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/sequence_file.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/bank_file.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/sequence_file.h"
#include "ntdlib/sar/wave_file.h"
#include "ntdlib/sound_build.h"

#include "edit/bytecode.h"
#include "edit/new_items.h"
#include "edit/other_console.h"
#include "edit/other_console_parts.h"
#include "edit/replace.h"
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
using other_console_parts::AddSharingWarning;
using other_console_parts::AsLaterConsoles;
using other_console_parts::CopyParts;
using other_console_parts::CopyValues;
using other_console_parts::CountSkipped;
using other_console_parts::DeadData;
using other_console_parts::EncodedFile;
using other_console_parts::GlobalsWarning;
using other_console_parts::kWiiWaves;
using other_console_parts::PlaysOtherwise;
using other_console_parts::Settle;
using other_console_parts::SkippedWarning;
using other_console_parts::Song;
using other_console_parts::WaveArchiveNameOf;
using other_console_parts::WithoutItems;
using other_console_parts::WithStartingGlobals;

// The commands of a sound's sequence file, the item where the sound starts, and the file's sequence among the sequences
// of the file that it comes from, when that file holds it.
struct WiiSequence
{
    ntdlib::commands::Sequence sequence;
    uint32_t start = 0;
    std::optional<uint32_t> held;
};

// The commands of sequence sound `sound` of `archive`, the archive that the Wii's `file` exports, and where the sound
// starts (`places`, SoundPlaces). A sequence file that stays in the archive's bytes decodes by the rules of NW4R SND's
// `build`, from its labels and the starts of the sounds that play it. Throws std::invalid_argument with a message for
// the user for a file that the archive lacks or that Hokuto can't read.
WiiSequence WiiSequenceOf(const format::File& file, const ntdlib::rsar::SoundArchive& archive,
                          std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                          const ntdlib::SndBuild& build)
{
    if (sound < places.size() && places[sound])
    {
        const SoundPlace& place = *places[sound];
        return {*file.sequences.at(place.sequence),
                place.start_field ? file.native->fields.at(*place.start_field).to.offset : 0, place.sequence};
    }

    const uint32_t file_id = archive.Sounds()[sound].file_id;
    const std::optional<ntdlib::rsar::FileData> data = archive.File(file_id);
    if (!data)
    {
        throw std::invalid_argument("The archive has no sequence file for the sound.");
    }

    ntdlib::rsar::SequenceFile read;
    try
    {
        read = ntdlib::rsar::SequenceFile::Read(data->file);
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Can't read the sound's sequence file. ") +
                                    format::AsSentence(error.what()));
    }

    std::vector<ntdlib::nwsnd::LabelOffset> labels;
    for (const ntdlib::rsar::Sequence::Label& label : read.labels)
    {
        labels.push_back({label.name, label.offset});
    }

    std::vector<uint32_t> entries;
    std::size_t entry = 0;
    for (uint32_t other = 0; other < archive.Sounds().size(); ++other)
    {
        const ntdlib::rsar::SoundInfo& info = archive.Sounds()[other];
        if (info.type == ntdlib::rsar::SoundType::kSequence && info.file_id == file_id)
        {
            entry = other == sound ? entries.size() : entry;
            entries.push_back(info.start_offset);
        }
    }

    ntdlib::nwsnd::Decoded decoded;
    try
    {
        decoded = ntdlib::nwsnd::DecodeSequence(
            read.data, labels, song::GrammarOf(song::Console{.engine = format::Engine::kWii, .wii = build}), entries);
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Can't read the commands in the sound's sequence file. ") +
                                    format::AsSentence(error.what()));
    }

    if (decoded.opaque)
    {
        throw std::invalid_argument("The commands in the sound's sequence file overlap or jump into each other.");
    }

    return {std::move(decoded.sequence), decoded.entries.at(entry), std::nullopt};
}

} // namespace

namespace other_console_parts
{

std::string WaveArchiveNameOf(const std::string& bank)
{
    return "WARC_" + (bank.starts_with("BANK_") ? bank.substr(5) : bank);
}

} // namespace other_console_parts

namespace
{

// Adds bank `index` of the Wii's `archive` to `song` as a bank of the Wii U, after a wave archive of its waves. The
// regions play with the values that NW4R SND's `build` reads (ntdlib::rsar::BankFileOf). Gives the new bank's item ID,
// or kNoItem for a bank that stays behind with a warning.
uint32_t AddConvertedWiiBank(Song& song, const ntdlib::rsar::SoundArchive& archive, uint32_t index,
                             const ntdlib::SndBuild& build)
{
    const std::string name = index < archive.Banks().size() && !archive.Banks()[index].name.empty()
                                 ? archive.Banks()[index].name
                                 : "BANK_" + std::to_string(index);
    ConvertedItem bank;
    try
    {
        bank = LaterBankFilesOf(archive, index, build, sar::Flavor::kCafe);
    }
    catch (const std::exception& error)
    {
        song.result.warnings.push_back("Bank " + name + " is left out. Its slot is empty. " +
                                       format::AsSentence(error.what()));
        return sar::kNoItem;
    }

    const auto place = static_cast<uint32_t>(song.result.file.native->tables->wave_archives.size());
    song.result.file = AddWaveArchive(song.result.file, WaveArchiveNameOf(name), bank.waves);
    song.result.file = AddBank(song.result.file, name, bank.file, {{kWiiWaves, place}});

    return sar::ItemId(sar::kBankItem, static_cast<uint32_t>(song.result.file.native->tables->banks.size() - 1));
}

// The values of sound `source`, a sound of the Wii's build `build`, in the tables of the Wii U (FromWii): the volume
// and the pan as the Wii plays them. The warnings go into `warnings`.
format::NativeSound WiiUValuesOf(const format::NativeSound& source, const ntdlib::SndBuild& build,
                                 std::vector<std::string>& warnings)
{
    format::NativeSound values = source;
    if (values.volume > 127)
    {
        warnings.push_back("The sound's volume is lowered from " + std::to_string(values.volume) +
                           " to 127. The Wii plays volumes above 127 at 127.");
        values.volume = 127;
    }

    if (build.October2006() && (values.pan_mode != 0 || values.pan_curve != 0))
    {
        warnings.push_back(
            "The sound is set to the dual pan mode and the square-root pan curve. The October 2006 build of NW4R "
            "SND supports only those.");
        values.pan_mode = 0;
        values.pan_curve = 0;
    }

    return values;
}

// The commands of a Wii sequence as a later console reads them: all of them (AsLaterConsoles), and those that stay. The
// commands that the Wii's build skips and the bytes that no command reaches stay out. `start` is the item where the
// sound starts among those that stay.
struct LaterCommands
{
    ntdlib::commands::Sequence later;
    ntdlib::commands::Sequence kept;
    uint32_t start = 0;
};

// The commands of `sequence`, read by `reads`, a console of the Wii, as `writes` reads them. `start` is the item where
// the sound starts in `sequence`.
LaterCommands LaterCommandsOf(const ntdlib::commands::Sequence& sequence, uint32_t start, const song::Console& reads,
                              const song::Console& writes)
{
    LaterCommands commands;
    commands.later = AsLaterConsoles(sequence, *reads.wii);
    std::vector<bool> leaves = DeadData(commands.later);
    for (std::size_t item = 0; item < commands.later.items.size(); ++item)
    {
        leaves[item] = leaves[item] || PlaysOtherwise(commands.later.items[item], reads, writes);
    }

    std::tie(commands.kept, commands.start) = WithoutItems(commands.later, start, leaves);

    return commands;
}

} // namespace

namespace other_console_parts
{

OtherConsoleSong FromWii(const format::File& file, uint32_t sound)
{
    const format::NativeTables& tables = *file.native->tables;
    if (sound >= tables.sounds.size() || tables.sounds[sound].kind != sar::SoundType::kSequence)
    {
        throw std::invalid_argument("Only a sequence sound can be sent to another console.");
    }

    const format::NativeSound& source = tables.sounds[sound];

    // The archive that the file exports, and the sound's sequence and start.
    const std::vector<uint8_t> bytes = format::ExportArchive(file);
    const std::shared_ptr<const ntdlib::rsar::SoundArchive> archive =
        song::LoadWiiArchive(bytes, "the archive", ntdlib::rvl::snd::kRsarOptions);
    const ntdlib::SndBuild build = format::BuildOfWiiArchive(*file.native);
    const std::vector<std::optional<SoundPlace>> places = SoundPlaces(
        *archive, format::SequencesOfFiles(file, bytes, *archive), format::StartFieldsOfSounds(file, bytes));
    const WiiSequence read = WiiSequenceOf(file, *archive, places, sound, build);

    // The Wii's commands as the Wii U reads them, without the commands that the Wii's build skips and the bytes that no
    // command reaches.
    const song::Console reads{.engine = format::Engine::kWii, .wii = build};
    const song::Console writes{.engine = format::Engine::kWiiU};
    auto [later, kept, kept_start] = LaterCommandsOf(read.sequence, read.start, reads, writes);
    if (Fixed(kept))
    {
        throw std::invalid_argument(
            "The commands in the sequence file must stay at their offsets. Hokuto can't send such a Wii sequence to "
            "another console yet.");
    }

    const auto [start, globals] = WithStartingGlobals(kept, kept_start, -1, 0);

    Song song;
    song.result.file = format::NewFile(format::Engine::kWiiU);
    std::vector<uint32_t> slots;
    for (const uint32_t bank : source.banks)
    {
        slots.push_back(bank == sar::kNoItem ? sar::kNoItem
                                             : AddConvertedWiiBank(song, *archive, sar::ItemNumber(bank), build));
    }

    // The sound.
    const std::string name = source.name.empty() ? "SEQ_" + std::to_string(sound) : source.name;
    song.result.file = AddSequenceSound(song.result.file, std::nullopt, name, kept, start, {}, slots);
    const auto new_sequence = static_cast<uint32_t>(song.result.file.sequences.size() - 1);
    if (const std::optional<std::string> warning =
            SkippedWarning(CountSkipped(later, reads, writes), reads, writes, true))
    {
        song.result.warnings.push_back(*warning);
    }

    if (!globals.empty())
    {
        song.result.warnings.push_back(GlobalsWarning(globals, false));
    }

    // The sound's values in the archive's tables.
    const auto added = static_cast<uint32_t>(song.result.file.native->tables->sounds.size() - 1);
    CopyValues(song, added, WiiUValuesOf(source, build, song.result.warnings));
    const ntdlib::rsar::SoundInfo& info = archive->Sounds()[sound];
    if (info.remote_filter != 0 || info.user_params[0] != 0 || info.user_params[1] != 0)
    {
        song.result.warnings.push_back("The sound's remote filter and user parameters are left out.");
    }

    Settle(song, added, std::nullopt);

    // The other sounds whose commands the sequence file holds.
    AddSharingWarning(song, song::SoundsSharingFile(*archive, sound).size());

    CopyParts(song, file, sound, added, read.held, new_sequence);

    return std::move(song.result);
}

} // namespace other_console_parts

ConvertedItem LaterSequenceFileOf(const ntdlib::rsar::SoundArchive& archive, uint32_t sound,
                                  const ntdlib::SndBuild& build, sar::Flavor to)
{
    // The commands as the build reads them, from the labels and the starts of the sounds that play the file.
    if (sound >= archive.Sounds().size() || archive.Sounds()[sound].type != ntdlib::rsar::SoundType::kSequence)
    {
        throw std::invalid_argument("The archive has no such sequence sound.");
    }

    const uint32_t file_id = archive.Sounds()[sound].file_id;
    const std::optional<ntdlib::rsar::FileData> data = archive.File(file_id);
    if (!data)
    {
        throw std::invalid_argument("The archive has no sequence file for the sound.");
    }

    ntdlib::rsar::SequenceFile read;
    try
    {
        read = ntdlib::rsar::SequenceFile::Read(data->file);
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Can't read the sequence file. ") + format::AsSentence(error.what()));
    }

    std::vector<uint32_t> starts;
    for (const ntdlib::rsar::SoundInfo& info : archive.Sounds())
    {
        if (info.type == ntdlib::rsar::SoundType::kSequence && info.file_id == file_id)
        {
            starts.push_back(info.start_offset);
        }
    }

    const song::Console reads{.engine = format::Engine::kWii, .wii = build};
    const ntdlib::commands::Sequence sequence = SequenceFileCommands(read, data->file, build, starts);

    // The commands as the later consoles read them, without those that the build skips and the bytes that no command
    // reaches. The biquad filters take the other console's types and values.
    const song::Console writes{.engine = format::EngineOf(to)};
    auto [later, kept, kept_start] = LaterCommandsOf(sequence, 0, reads, writes);
    if (Fixed(kept))
    {
        throw std::invalid_argument(
            "The commands in the sequence file must stay at their offsets. Hokuto can't convert such a Wii sequence "
            "for another console yet.");
    }

    const song::BiquadChanges biquads = song::MapBiquads(kept, {}, reads, writes);
    ConvertedItem result;
    if (const std::optional<std::string> warning =
            SkippedWarning(CountSkipped(later, reads, writes), reads, writes, true))
    {
        result.warnings.push_back(*warning);
    }

    if (const std::optional<std::string> warning = song::BiquadWarning(biquads, reads, writes))
    {
        result.warnings.push_back(*warning);
    }

    const ntdlib::commands::Sequence plain =
        *format::WithoutChoices({format::Shared<ntdlib::commands::Sequence>(std::move(kept))}).front();
    auto [bytes, offsets] = EncodedFile(plain, song::GrammarOf(to), song::ConsoleName(to));
    sar::SequenceFile file;
    file.flavor = to;
    file.version = song::ExportVersions(song::ItemKind::kSequence, to).file;
    file.data = std::move(bytes);
    for (const ntdlib::nwsnd::LabelOffset& label : offsets)
    {
        file.labels.push_back({label.name, label.offset});
    }

    result.file = file.Write();

    return result;
}

ConvertedItem LaterBankFilesOf(const ntdlib::rsar::SoundArchive& archive, uint32_t index, const ntdlib::SndBuild& build,
                               sar::Flavor to)
{
    if (index >= archive.Banks().size())
    {
        throw std::invalid_argument("The archive has no such bank.");
    }

    const uint32_t file_id = archive.Banks()[index].file_id;
    const std::optional<ntdlib::rsar::FileData> data = archive.File(file_id);
    if (!data)
    {
        throw std::invalid_argument(song::WiiMissingNote(archive, file_id, "The archive has no file for the bank."));
    }
    if (archive.WaveDataMissing(file_id))
    {
        throw std::invalid_argument(song::WiiMissingNote(archive, file_id, "The archive has no waves for the bank."));
    }

    // The bank's waves go into a wave archive of their own, in the bank's order.
    const ntdlib::rsar::Bank read =
        ntdlib::rsar::Bank::Parse(data->file, data->wave_data, ntdlib::rvl::snd::kRsarOptions);
    const song::FileVersions versions = song::ExportVersions(song::ItemKind::kWaveArchive, to);
    sar::WaveArchiveFile wave_archive{.flavor = to, .version = versions.file, .waves = {}};
    for (const ntdlib::rsar::Wave& wave : read.Waves())
    {
        wave_archive.waves.push_back(ntdlib::rsar::WaveFileOf(wave, to, versions.waves).Write());
    }

    return {.file = ntdlib::rsar::BankFileOf(read, build, to, song::ExportVersions(song::ItemKind::kBank, to).file,
                                             kWiiWaves)
                        .Write(),
            .waves = wave_archive.Write(),
            .warnings = {}};
}

} // namespace hokuto::edit
