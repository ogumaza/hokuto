// SPDX-License-Identifier: MIT

// The browser's Export… of an item, such as a bank, a wave or a voice group, on each console.
// app_item_export_ds_gba.cpp contains the exports of the DS, DSE and the GBA.

#include <cstddef>
#include <exception>
#include <filesystem>
#include <future>
#include <initializer_list>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <utility>

#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sequence_export/sfz.h"
#include "xsflib/ascii.h"
#include "xsflib/files.h"

#include "app/app.h"
#include "app/app_internal.h"
#include "app/app_item_export_parts.h"
#include "app/export_warnings.h"
#include "app/export_work.h"
#include "edit/other_console.h"
#include "edit/timelines.h"
#include "format/archive.h"
#include "format/drivers.h"
#include "format/file.h"
#include "format/kinds.h"
#include "song/aac.h"
#include "song/archive.h"
#include "song/driver.h"
#include "song/items.h"
#include "song/midi_export.h"
#include "song/streams.h"

namespace hokuto::app
{

using item_export_parts::AddWavFiles;
using item_export_parts::BankExportWarnings;
using item_export_parts::BankFileName;
using item_export_parts::ForInstrument;
using item_export_parts::NoWavFiles;
using item_export_parts::WithoutExtension;
using ntdlib::sequence_export::ProgramSuffix;

namespace
{

// `bank` with only its instrument at `program`, at that program.
ntdlib::sar::BankFile InstrumentAlone(ntdlib::sar::BankFile bank, uint32_t program)
{
    for (uint32_t other = 0; other < bank.instruments.size(); other++)
    {
        if (other != program && bank.instruments[other])
        {
            bank = edit::WithoutInstrument(std::move(bank), other);
        }
    }

    return bank;
}

// Exports stream `item` of `archive` into `work` as `file_name` for `target`. Returns the warnings of a conversion for
// another console.
std::vector<std::string> SarStreamFile(ExportWork& work, const format::File& version,
                                       const ntdlib::sar::SoundArchive& archive, const song::Item& item,
                                       ExportTarget target, const std::string& file_name)
{
    std::vector<std::string> found;
    // The file keeps the stream. The archive names it. An AAC stream takes the loop that the sound's entry
    // gives.
    const std::vector<uint8_t>& bytes = StreamBytes(version, archive, item.index);
    const std::optional<ntdlib::sar::Flavor> console = ConsoleOf(target);
    const song::AacStream aac =
        song::IsAdts(bytes) ? song::AacStreamOf(archive, item.index).value_or(song::AacStream()) : song::AacStream();
    work.files.emplace_back(
        file_name, target == ExportTarget::kItemFile  ? bytes
                   : target == ExportTarget::kWiiFile ? song::WiiStreamFile(bytes, aac)
                   : console
                       ? song::ConvertStreamFile(bytes, *console, song::ExportVersions(item.kind, *console).file, aac)
                       : song::StreamAsWav(bytes, aac));
    if (console)
    {
        found = StreamConversionWarnings(bytes, *console);
    }

    return found;
}

// Exports `item` of `archive` into `work` as `file_name`, converted to the Wii's format. Returns the conversion's
// warnings.
std::vector<std::string> WiiFileOfItem(ExportWork& work, const ntdlib::sar::SoundArchive& archive,
                                       const song::Item& item, const std::string& file_name)
{
    // A bank file or a wave sound file of the Wii takes its waves along, in an RWAR beside it under its
    // name.
    edit::ConvertedItem converted;
    switch (item.kind)
    {
    case song::ItemKind::kSequence:
        converted = edit::WiiSequenceFileOf(archive, item.index);
        break;
    case song::ItemKind::kBank:
        converted = edit::WiiBankFilesOf(archive, item.index);
        break;
    case song::ItemKind::kWaveSound:
    case song::ItemKind::kWaveSoundFile:
        converted = edit::WiiWaveSoundFilesOf(archive, item.index);
        break;
    case song::ItemKind::kWaveArchive:
    case song::ItemKind::kWave:
    case song::ItemKind::kStream:
        converted.file = song::WiiWaveFileOf(archive, item);
        break;
    }

    // An RWAR that doesn't fit in the Wii's model gets a warning (edit::WiiWaveRoomWarning). The
    // conversions of a bank and of a wave sound file add it. A wave archive gets it here.
    if (item.kind == song::ItemKind::kWaveArchive)
    {
        if (const std::optional<std::string> warning = edit::WiiWaveRoomWarning(converted.file.size()))
        {
            converted.warnings.push_back(*warning);
        }
    }

    work.files.emplace_back(file_name, std::move(converted.file));
    if (!converted.waves.empty())
    {
        std::filesystem::path beside = xsflib::PathFromUtf8(file_name);
        beside.replace_extension(".brwar");
        work.files.emplace_back(xsflib::Utf8(beside), std::move(converted.waves));
    }

    return std::move(converted.warnings);
}

// Exports `item` of `archive` into `work` as `file_name`, converted for `console`. Returns the conversion's warnings.
std::vector<std::string> OtherConsoleItem(ExportWork& work, const ntdlib::sar::SoundArchive& archive,
                                          const song::Item& item, ntdlib::sar::Flavor console,
                                          const std::string& file_name)
{
    std::vector<std::string> found;
    // A sequence's biquad filters take the types and values that come nearest on the other console.
    if (item.kind == song::ItemKind::kSequence)
    {
        song::ConvertedSequence converted = song::OtherConsoleSequenceFile(archive, item.index, console);
        work.files.emplace_back(file_name, std::move(converted.file));
        const song::Console from{.engine = format::EngineOf(archive.ReadOptions().flavor)};
        if (const std::optional<std::string> warning =
                song::BiquadWarning(converted.biquads, from, {.engine = format::EngineOf(console)}))
        {
            found.push_back(*warning);
        }
    }
    else
    {
        work.files.emplace_back(file_name, song::OtherConsoleFile(archive, item, console));
    }

    return found;
}

// Exports stream `item` of `archive` into `work` as `file_name` for `target`. Returns the warnings of a conversion for
// another console or a WAV file.
std::vector<std::string> WiiStreamFiles(ExportWork& work, const std::shared_ptr<const format::File>& version,
                                        const std::shared_ptr<const ntdlib::rsar::SoundArchive>& archive,
                                        const song::Item& item, ExportTarget target, const std::string& file_name)
{
    const std::vector<uint8_t>& bytes = StreamBytes(*version, *archive, item.index);
    const std::optional<ntdlib::sar::Flavor> console = ConsoleOf(target);
    work.files.emplace_back(
        file_name, target == ExportTarget::kItemFile ? bytes
                   : console ? song::ConvertStreamFile(bytes, *console, song::ExportVersions(item.kind, *console).file)
                             : song::StreamAsWav(bytes));
    return console                        ? StreamConversionWarnings(bytes, *console)
           : target == ExportTarget::kWav ? WiiStreamWavWarnings(bytes, format::BuildOfWiiArchive(*version->native))
                                          : std::vector<std::string>();
}

// Exports the file of `item` of `archive` into `work` as `file_name`, with its waves beside it where it has some.
// Returns the warning of waves that are left out.
std::vector<std::string> WiiFileWithWaves(ExportWork& work,
                                          const std::shared_ptr<const ntdlib::rsar::SoundArchive>& archive,
                                          const song::Item& item, const std::string& file_name)
{
    std::vector<std::string> found;
    // A bank file or a wave sound file takes its waves along, in an RWAR beside it under its name.
    work.files.emplace_back(file_name, song::ItemBytes(*archive, item));
    const bool bank = item.kind == song::ItemKind::kBank;
    if (bank || item.kind == song::ItemKind::kWaveSound || item.kind == song::ItemKind::kWaveSoundFile)
    {
        const uint32_t file_id =
            bank ? archive->Banks().at(item.index).file_id : archive->Sounds().at(item.index).file_id;
        std::filesystem::path beside = xsflib::PathFromUtf8(file_name);
        beside.replace_extension(".brwar");
        try
        {
            work.files.emplace_back(xsflib::Utf8(beside),
                                    song::ItemBytes(*archive, {song::ItemKind::kWaveArchive, file_id, 0}));
        }
        catch (const std::exception& error)
        {
            found.push_back(format::AsSentence(error.what()) + " Its waves are left out.");
        }
    }

    return found;
}

// Exports `item` of `archive` into `work` as `file_name`, converted for `console`. Returns the conversion's warnings.
std::vector<std::string> WiiConvertedItem(ExportWork& work, const std::shared_ptr<const format::File>& version,
                                          const std::shared_ptr<const ntdlib::rsar::SoundArchive>& archive,
                                          const song::Item& item, ntdlib::sar::Flavor console,
                                          const std::string& file_name)
{
    std::vector<std::string> found;
    // A bank takes its waves along, in a wave archive file beside it under its name.
    const ntdlib::SndBuild build = format::BuildOfWiiArchive(*version->native);
    edit::ConvertedItem converted;
    if (item.kind == song::ItemKind::kSequence)
    {
        converted = edit::LaterSequenceFileOf(*archive, item.index, build, console);
    }
    else if (item.kind == song::ItemKind::kBank)
    {
        try
        {
            converted = edit::LaterBankFilesOf(*archive, item.index, build, console);
        }
        catch (const std::exception& error)
        {
            throw std::runtime_error("Can't convert the bank for the " + song::ConsoleName(console) + ". " +
                                     format::AsSentence(error.what()));
        }
    }
    else
    {
        converted.file = song::LaterWaveFileOf(*archive, item, console);
    }
    work.files.emplace_back(file_name, std::move(converted.file));
    if (!converted.waves.empty())
    {
        std::filesystem::path beside = xsflib::PathFromUtf8(file_name);
        beside.replace_extension("." + xsflib::AsciiLower(song::ItemFormat(song::ItemKind::kWaveArchive, console)));
        work.files.emplace_back(xsflib::Utf8(beside), std::move(converted.waves));
    }
    found = std::move(converted.warnings);

    return found;
}

// Exports the waves of wave archive `item` of `archive` into `work` as WAV files. Returns a warning for each wave that
// is left out.
std::vector<std::string> WiiWavFolder(ExportWork& work,
                                      const std::shared_ptr<const ntdlib::rsar::SoundArchive>& archive,
                                      const song::Item& item)
{
    const std::size_t count = song::WaveCountOf(*archive, item.index);
    const auto add = [&](uint32_t wave)
    {
        work.files.emplace_back(song::WavFileName(*archive, item.index, wave, count),
                                song::WaveAsWav(*archive, item.index, wave));
    };
    std::vector<std::string> found = AddWavFiles(std::views::iota(uint32_t{0}, static_cast<uint32_t>(count)), add);
    if (work.files.empty())
    {
        throw NoWavFiles(song::ItemName(*archive, item), count == 0);
    }

    return found;
}

// Converts bank `item.index` of `archive` for a SoundFont or SFZ export to `file_name`, of instrument `program` alone
// where one is given. Returns the warnings.
std::vector<std::string> SarBankConversion(ExportWork& work, const ntdlib::sar::SoundArchive& archive,
                                           const song::Item& item, ExportTarget target,
                                           const std::optional<uint32_t>& program, const std::string& file_name)
{
    work.bank = song::ConvertBank(archive, item.index, "Hokuto");
    return BankExportWarnings(*work.bank, target, program, file_name);
}

// Adds the WAV files of the waves of wave archive `item` of `archive` to `work`. Returns the warnings.
std::vector<std::string> SarWaveArchiveWavs(ExportWork& work, const ntdlib::sar::SoundArchive& archive,
                                            const song::Item& item)
{
    const std::size_t count = song::WaveCountOf(archive, item.index);
    const auto add = [&](uint32_t wave)
    {
        work.files.emplace_back(song::WavFileName(archive, item.index, wave, count),
                                song::WaveAsWav(archive, item.index, wave));
    };
    std::vector<std::string> found = AddWavFiles(std::views::iota(uint32_t{0}, static_cast<uint32_t>(count)), add);

    // A wave archive without its file or without waves gives no WAV files. ItemFile throws when the archive
    // lacks the file.
    if (work.files.empty())
    {
        song::ItemFile(archive, item);
        throw NoWavFiles(song::ItemName(archive, item), count == 0);
    }

    return found;
}

// Gives the SFZ file of an instrument that goes out by itself, at `program`, the name of its folder, `folder`. The
// converter pads a program to the digits of the bank's last program.
void NameInstrumentSfz(ntdlib::sequence_export::BankConversion& conversion, const std::optional<uint32_t>& program,
                       const std::string& folder)
{
    if (program && conversion.sfz.files.size() == 1)
    {
        conversion.sfz.files.front().name = folder + ".sfz";
    }
}

} // namespace

namespace item_export_parts
{

std::string WithoutExtension(const std::string& name)
{
    const std::size_t dot = name.rfind('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

void ForInstrument(Export& job, uint32_t program, uint32_t last)
{
    job.program = program;
    job.last_program = last;
    job.view = InstrumentExportView(job.view.source, program);
}

std::runtime_error NoWavFiles(const std::string& name, bool none)
{
    return std::runtime_error(none ? name + " has no waves." : "Can't decode any wave in " + name + ".");
}

std::string BankFileName(const std::string& name, const Export& job, ExportTarget target)
{
    if (target != ExportTarget::kSoundFont && target != ExportTarget::kSfz)
    {
        return name;
    }

    std::string file_name = WithoutExtension(name);
    file_name += job.program ? ProgramSuffix(*job.program, job.last_program) : "";
    file_name += target == ExportTarget::kSoundFont ? ".sf2" : "";

    return file_name;
}

std::vector<std::string> BankExportWarnings(ntdlib::sequence_export::BankConversion& bank, ExportTarget target,
                                            const std::optional<uint32_t>& program, const std::string& file_name)
{
    NameInstrumentSfz(bank, program, file_name);
    std::vector<std::string> found;
    if (target == ExportTarget::kSoundFont)
    {
        found = SoundFontWarnings(bank, xsflib::Utf8(xsflib::PathFromUtf8(file_name).stem()));
    }
    const std::vector<std::string> converter = ConverterWarnings(bank.warnings, target == ExportTarget::kSfz);
    found.insert(found.end(), converter.begin(), converter.end());

    return found;
}

} // namespace item_export_parts

void App::StartItemExport(const BrowserPlace& place)
{
    if (export_ || !history_ || preview_ || !ArchiveOpen() || !archive_version_ || place.group < 0 || place.item < 0)
    {
        return;
    }

    // A song of the GBA's other drivers is exported as MIDI with SoundFont 2, the samples as a SoundFont 2 file or a
    // folder of WAV files, and a sample as a WAV file or a SoundFont 2 file.
    if (played_.Driver())
    {
        StartDriverItemExport(place);
        return;
    }

    // A sound of DSE is exported as MIDI with SoundFont 2 or SFZ, and a song as its file too. A bank, an instrument, a
    // bank with samples and a wave go out as their files, and as SoundFonts, SFZ files or WAV files. An instrument goes
    // out with its bank as the item.
    if (played_.Dse())
    {
        StartDseItemExport(place);
        return;
    }

    // A sequence of a DS archive is exported as MIDI with SoundFont 2 or SFZ. A bank, an instrument, a wave archive, a
    // wave and a stream go out as their files, and as SoundFonts, SFZ files or WAV files. An instrument goes out with
    // its bank as the item.
    if (played_.Ds())
    {
        StartDsItemExport(place);
        return;
    }

    // A song of a GBA ROM is exported as MIDI with SoundFont 2, and a voice group or one of its voices as a SoundFont 2
    // file.
    if (played_.Gba())
    {
        StartGbaItemExport(place);
        return;
    }
    StartSarItemExport(place);
}

void App::StartSarItemExport(const BrowserPlace& place)
{
    // The item at the place: a sound, a bank, a wave archive or a wave sound file, an instrument of a bank, a wave of a
    // wave archive, or a wave sound of a wave sound file. An instrument goes out with its bank as the item.
    const std::optional<ListedPlace> listed = ListedAt(place);
    if (!listed || (place.child >= 0 && !listed->child))
    {
        return;
    }
    const std::size_t group = listed->group;
    const std::size_t index = listed->item;

    const song::Item item = SarItemAt(place, group, index);

    Export job;
    job.view = played_.Wii() ? ItemExportView(*played_.Wii(), item) : ItemExportView(*played_.Sar(), item);
    if (group == kBanksGroup && place.child >= 0)
    {
        ForInstrument(job, *listed->child, browser_[group].items[index].child_numbers.back());
    }
    job.item = item;
    StartItemWork(job);
    export_ = std::move(job);
}

song::Item App::SarItemAt(const BrowserPlace& place, std::size_t group, std::size_t index) const
{
    song::Item item;
    item.index = browser_[group].items[index].number;
    switch (group)
    {
    case kSoundsGroup:
        {
            const bool stream = played_.Wii()
                                    ? item.index < played_.Wii()->Sounds().size() &&
                                          played_.Wii()->Sounds()[item.index].type == ntdlib::rsar::SoundType::kStream
                                    : item.index < played_.Sar()->Sounds().size() &&
                                          played_.Sar()->Sounds()[item.index].type == ntdlib::sar::SoundType::kStream;
            item.kind = stream ? song::ItemKind::kStream : song::ItemKind::kWaveSound;
            break;
        }

    case kSequencesGroup:
        item.kind = song::ItemKind::kSequence;
        break;

    case kBanksGroup:
        item.kind = song::ItemKind::kBank;
        break;

    case kWaveSoundFilesGroup:
        item.kind = place.child >= 0 ? song::ItemKind::kWaveSound : song::ItemKind::kWaveSoundFile;
        item.index = place.child >= 0
                         ? browser_[group].items[index].child_numbers.at(static_cast<std::size_t>(place.child))
                         : item.index;
        break;

    default:
        item.kind = place.child >= 0 ? song::ItemKind::kWave : song::ItemKind::kWaveArchive;
        item.wave =
            place.child >= 0 ? browser_[group].items[index].child_numbers.at(static_cast<std::size_t>(place.child)) : 0;
        break;
    }

    return item;
}

void App::StartWiiItemWork(Export& job)
{
    // As for the other consoles, the dialog offers the file's name. A wave's name pads its index to the digits of its
    // wave archive's last wave. A bank's SoundFont takes the bank's name, and so does the folder of its SFZ files. An
    // instrument's files add its program, with the digits that the bank's SFZ files give it (ProgramSuffix).
    job.file_name = WiiItemFileName(job);

    // The work exports the version that the history has reached. The archive that the zones play serves when it came
    // from the version, unless the version has timelines: their files go out with their subroutines. Otherwise the work
    // builds the version's archive first. A bank's instruments play by the rules of the file's build of NW4R SND. A
    // wave archive's WAV export leaves out the waves that can't be decoded, and warns of each. The file keeps a stream,
    // and the archive names it.
    const song::Item item = *job.item;
    const ExportTarget target = job.Target();
    const std::shared_ptr<const format::File> version = history_->Current();
    const bool timelines = version->timelines && !version->timelines->empty();
    const auto make =
        [version, archive = SameArchive(*version, *archive_version_) && !timelines ? played_.Wii() : nullptr,
         name = file_name_, item, target, file_name = job.file_name, program = job.program](ExportWork& work) mutable
    {
        if (!archive)
        {
            archive = song::LoadWiiArchive(format::ExportArchive(edit::FoldTimelines(*version)), name,
                                           ntdlib::rvl::snd::kRsarOptions);
        }
        std::vector<std::string> found;
        if (item.kind == song::ItemKind::kStream)
        {
            found = WiiStreamFiles(work, version, archive, item, target, file_name);
        }
        else if (target == ExportTarget::kItemFile)
        {
            found = WiiFileWithWaves(work, archive, item, file_name);
        }
        else if (const std::optional<ntdlib::sar::Flavor> console = ConsoleOf(target))
        {
            found = WiiConvertedItem(work, version, archive, item, *console, file_name);
        }
        else if (target == ExportTarget::kSoundFont || target == ExportTarget::kSfz)
        {
            // An instrument goes out from a copy of its bank that holds it alone, at its program.
            if (program)
            {
                const format::File folded = edit::FoldTimelines(*version);
                const format::File alone = edit::WithBankModel(
                    folded, *archive, item.index, InstrumentAlone(edit::BankModelOf(*archive, item.index), *program));
                archive = WiiArchiveOf(alone, name);
            }
            work.bank = song::ConvertBank(*archive, item.index, "Hokuto", format::BuildOfWiiArchive(*version->native));
            found = BankExportWarnings(*work.bank, target, program, file_name);
        }
        else if (item.kind == song::ItemKind::kWave)
        {
            work.files.emplace_back(file_name, song::WaveAsWav(*archive, item.index, item.wave));
        }
        else if (item.kind == song::ItemKind::kWaveSound)
        {
            work.files.emplace_back(file_name, song::WaveSoundAsWav(*archive, item.index));
        }
        else
        {
            found = WiiWavFolder(work, archive, item);
        }
        work.warnings = ItemWarnings(*archive, item, target, found);
    };
    job.work = ExportWorkOf(make);
}

std::string App::WiiItemFileName(const Export& job) const
{
    const song::Item item = *job.item;
    const ExportTarget target = job.Target();
    std::size_t waves = 0;
    if (item.kind == song::ItemKind::kWave || item.kind == song::ItemKind::kWaveArchive)
    {
        try
        {
            waves = song::WaveCountOf(*played_.Wii(), item.index);
        }
        catch (const std::exception&)
        {
            // The work reports the wave archive that can't be read.
        }
    }

    // A stream takes the name of its file beside the archive. Its file for another console takes the extension of that
    // console's format.
    std::string file_name = item.kind == song::ItemKind::kWave && target == ExportTarget::kWav
                                ? song::WavFileName(*played_.Wii(), item.index, item.wave, waves)
                            : item.kind == song::ItemKind::kWaveSound && target == ExportTarget::kWav
                                ? song::WaveSoundWavFileName(*played_.Wii(), item.index)
                                : song::ItemFileName(*played_.Wii(), item, waves);
    file_name = BankFileName(file_name, job, target);
    const std::optional<ntdlib::sar::Flavor> console = ConsoleOf(target);
    if (console || (item.kind == song::ItemKind::kStream && target == ExportTarget::kWav))
    {
        file_name = WithoutExtension(file_name);
        file_name += console ? "." + xsflib::AsciiLower(song::ItemFormat(item.kind, *console)) : ".wav";
    }

    return file_name;
}

void App::StartItemWork(Export& job)
{
    if (played_.Gba() && job.item->kind == song::ItemKind::kBank &&
        (job.Target() == ExportTarget::kItemFile || job.Target() == ExportTarget::kAiff))
    {
        StartGbaBankFileWork(job);
        return;
    }
    if (played_.Gba() && job.Target() == ExportTarget::kSoundFont)
    {
        StartGbaVoiceGroupWork(job);
        return;
    }
    if (played_.Gba() && job.Target() == ExportTarget::kItemFile && job.item->kind == song::ItemKind::kSequence)
    {
        StartGbaAssemblyWork(job);
        return;
    }
    if (job.Target() == ExportTarget::kMidiSoundFont || job.Target() == ExportTarget::kMidiSfz)
    {
        if (played_.Gba())
        {
            StartGbaMidiWork(job, job.item->index);
        }
        else if (played_.Ds())
        {
            StartDsMidiWork(job, job.item->index);
        }
        else if (played_.Dse())
        {
            StartDseMidiWork(job, job.item->index);
        }
        else if (played_.Driver())
        {
            StartDriverMidiWork(job, job.item->index);
        }
        else
        {
            StartMidiWork(job, job.item->index);
        }
        return;
    }
    if (played_.Wii())
    {
        StartWiiItemWork(job);
        return;
    }
    if (played_.Ds())
    {
        StartDsItemWork(job);
        return;
    }
    if (played_.Dse())
    {
        StartDseItemWork(job);
        return;
    }
    if (played_.Driver())
    {
        StartDriverItemWork(job);
        return;
    }

    StartSarItemWork(job);
}

void App::StartSarItemWork(Export& job)
{
    // The dialog offers the file's name. A wave's name pads its index to the digits of the archive's last wave. A wave
    // sound's WAV file takes the sound's name. A bank's SoundFont takes the bank's name, and so does the folder of its
    // SFZ files. An instrument's files add its program, with the digits that the bank's SFZ files give it
    // (ProgramSuffix). Edits leave the items' names as they are. The archive that the zones play then names them.
    job.file_name = SarItemFileName(job);

    // The work exports the version that the history has reached. The archive that the zones play serves when it came
    // from the version, unless the version has timelines: their files go out with their subroutines. Otherwise the work
    // builds the version's archive first. A wave archive's WAV export leaves out the waves that can't be decoded, and
    // warns of each. A bank's export lists what the converter reports.
    const song::Item item = *job.item;
    const ExportTarget target = job.Target();
    const std::shared_ptr<const format::File> version = history_->Current();
    const bool timelines = version->timelines && !version->timelines->empty();
    const auto make =
        [version, archive = SameArchive(*version, *archive_version_) && !timelines ? played_.Sar() : nullptr,
         name = file_name_, item, target, file_name = job.file_name, program = job.program](ExportWork& work) mutable
    {
        if (!archive)
        {
            const format::File folded = edit::FoldTimelines(*version);
            archive = SarArchiveOf(folded, name);
        }
        std::vector<std::string> found;
        if (item.kind == song::ItemKind::kStream)
        {
            found = SarStreamFile(work, *version, *archive, item, target, file_name);
        }
        else if (target == ExportTarget::kItemFile)
        {
            const std::span<const uint8_t> bytes = song::ItemFile(*archive, item);
            work.files.emplace_back(file_name, std::vector<uint8_t>(bytes.begin(), bytes.end()));
        }
        else if (target == ExportTarget::kWiiFile)
        {
            found = WiiFileOfItem(work, *archive, item, file_name);
        }
        else if (const std::optional<ntdlib::sar::Flavor> console = ConsoleOf(target))
        {
            found = OtherConsoleItem(work, *archive, item, *console, file_name);
        }
        else if (target == ExportTarget::kSoundFont || target == ExportTarget::kSfz)
        {
            // An instrument goes out from a copy of its bank that holds it alone, at its program.
            if (program)
            {
                const format::File folded = edit::FoldTimelines(*version);
                const format::File alone =
                    edit::WithBankModel(folded, *archive, item.index,
                                        InstrumentAlone(edit::BankModelOf(folded, *archive, item.index), *program));
                archive = SarArchiveOf(alone, name);
            }
            found = SarBankConversion(work, *archive, item, target, program, file_name);
        }
        else if (item.kind == song::ItemKind::kWave)
        {
            work.files.emplace_back(file_name, song::WaveAsWav(*archive, item.index, item.wave));
        }
        else if (item.kind == song::ItemKind::kWaveSound)
        {
            work.files.emplace_back(file_name, song::WaveSoundAsWav(*archive, item.index));
        }
        else
        {
            found = SarWaveArchiveWavs(work, *archive, item);
        }
        work.warnings = ItemWarnings(*archive, item, target, found);
    };
    job.work = ExportWorkOf(make);
}

std::string App::SarItemFileName(const Export& job) const
{
    const song::Item item = *job.item;
    const ExportTarget target = job.Target();
    std::string file_name;
    std::size_t waves = 0;
    if (item.kind == song::ItemKind::kWave || item.kind == song::ItemKind::kWaveArchive)
    {
        try
        {
            waves = song::WaveCountOf(*played_.Sar(), item.index);
        }
        catch (const std::exception&)
        {
            // The work reports the wave archive that can't be read.
        }
    }
    if (item.kind == song::ItemKind::kWave && target == ExportTarget::kWav)
    {
        file_name = song::WavFileName(*played_.Sar(), item.index, item.wave, waves);
    }
    else if (item.kind == song::ItemKind::kWaveSound && target == ExportTarget::kWav)
    {
        file_name = song::WaveSoundWavFileName(*played_.Sar(), item.index);
    }
    else if (item.kind == song::ItemKind::kStream && target == ExportTarget::kWav)
    {
        file_name = xsflib::Utf8(xsflib::PathFromUtf8(song::ItemFileName(*played_.Sar(), item)).stem()) + ".wav";
    }
    else
    {
        file_name = song::ItemFileName(*played_.Sar(), item, waves);
    }
    if (const std::optional<ntdlib::sar::Flavor> console = ConsoleOf(target))
    {
        // A file for another console takes the extension of that console's format.
        file_name = WithoutExtension(file_name);
        file_name += "." + xsflib::AsciiLower(song::ItemFormat(item.kind, *console));
    }
    else if (target == ExportTarget::kWiiFile)
    {
        file_name = WithoutExtension(file_name) + "." + xsflib::AsciiLower(song::WiiItemFormat(item.kind));
    }
    file_name = BankFileName(file_name, job, target);

    return file_name;
}

} // namespace hokuto::app
