// SPDX-License-Identifier: MIT

// File > Export of an item of the DS, of DSE and of the GBA: the Export window's offer and the work, on each console.

#include <algorithm>
#include <cstddef>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <utility>

#include "ntdlib/mp2k/song.h"
#include "ntdlib/sequence_export/sfz.h"
#include "seqlib/file_name.h"

#include "app/app.h"
#include "app/app_internal.h"
#include "app/app_item_export_parts.h"
#include "app/export_warnings.h"
#include "edit/gba_voices.h"
#include "format/archive.h"
#include "format/drivers.h"
#include "format/file.h"
#include "song/driver.h"
#include "song/gba_assembly.h"
#include "song/gba_samples.h"
#include "song/items.h"
#include "song/midi_export.h"

namespace hokuto::app
{

using item_export_parts::AddWavFiles;
using item_export_parts::BankExportWarnings;
using item_export_parts::BankFileName;
using item_export_parts::ForInstrument;
using item_export_parts::NoWavFiles;
using item_export_parts::WithoutExtension;
using ntdlib::sequence_export::ProgramSuffix;

void App::StartDriverItemExport(const BrowserPlace& place)
{
    const std::optional<ListedPlace> listed = ListedAt(place);
    if (!listed || (place.child >= 0 && !listed->child))
    {
        return;
    }
    const std::size_t group = listed->group;

    Export job;
    if (group == kSequencesGroup)
    {
        const uint32_t sound = listed->number;
        job.view = DriverSongExportView(played_.Driver()->SoundName(sound), played_.Driver()->WritesSfz());
        job.item = song::Item{.kind = song::ItemKind::kSequence, .index = sound};
        StartDriverMidiWork(job, sound);
        export_ = std::move(job);
        return;
    }

    song::Item item;
    item.kind = place.child >= 0       ? song::ItemKind::kWave
                : group == kBanksGroup ? song::ItemKind::kBank
                                       : song::ItemKind::kWaveArchive;
    item.wave = listed->child.value_or(0);
    job.view = DriverSamplesExportView(item.kind,
                                       place.child >= 0 ? played_.Driver()->SampleName(static_cast<int>(item.wave))
                                                        : "The samples that the ROM's songs play",
                                       played_.Driver()->WritesSfz());
    job.item = item;
    StartDriverItemWork(job);
    export_ = std::move(job);
}

void App::StartDseItemExport(const BrowserPlace& place)
{
    const std::optional<ListedPlace> listed = ListedAt(place);
    if (!listed || (place.child >= 0 && !listed->child))
    {
        return;
    }
    const std::size_t group = listed->group;
    const std::size_t index = listed->item;

    Export job;
    const uint32_t found = listed->number;
    if (group == kSequencesGroup)
    {
        job.view = DseSoundExportView(song::DseSoundName(*played_.Dse(), found),
                                      found < played_.Dse()->sounds.size() && played_.Dse()->sounds[found].song);
        job.item = song::Item{.kind = song::ItemKind::kSequence, .index = found};
        StartDseMidiWork(job, found);
        export_ = std::move(job);
        return;
    }

    song::Item item{.index = found};
    const bool child = place.child >= 0;
    item.kind = group == kBanksGroup ? song::ItemKind::kBank
                : child              ? song::ItemKind::kWave
                                     : song::ItemKind::kWaveArchive;
    item.wave = item.kind == song::ItemKind::kWave ? *listed->child : 0;
    job.view = DseItemExportView(item.kind, song::DseItemName(*played_.Dse(), item));
    if (group == kBanksGroup && child)
    {
        ForInstrument(job, *listed->child, browser_[group].items[index].child_numbers.back());
    }
    job.item = item;
    StartDseItemWork(job);
    export_ = std::move(job);
}

void App::StartDsItemExport(const BrowserPlace& place)
{
    const std::optional<ListedPlace> listed = ListedAt(place);
    if (!listed || (place.child >= 0 && !listed->child))
    {
        return;
    }
    const std::size_t group = listed->group;
    const std::size_t index = listed->item;

    Export job;
    const uint32_t found = listed->number;
    if (group == kSequencesGroup)
    {
        job.view = DsSequenceExportView(song::DsSoundName(*played_.Ds(), found));
        job.item = song::Item{.kind = song::ItemKind::kSequence, .index = found};
        StartDsMidiWork(job, found);
        export_ = std::move(job);
        return;
    }

    song::Item item{.index = found};
    const bool child = place.child >= 0;
    item.kind = group == kBanksGroup    ? song::ItemKind::kBank
                : group == kSoundsGroup ? song::ItemKind::kStream
                : child                 ? song::ItemKind::kWave
                                        : song::ItemKind::kWaveArchive;
    item.wave = item.kind == song::ItemKind::kWave ? *listed->child : 0;
    job.view = DsItemExportView(item.kind, song::DsItemName(*played_.Ds(), item));
    if (group == kBanksGroup && child)
    {
        ForInstrument(job, *listed->child, browser_[group].items[index].child_numbers.back());
    }
    job.item = item;
    StartDsItemWork(job);
    export_ = std::move(job);
}

void App::StartGbaItemExport(const BrowserPlace& place)
{
    const auto index = static_cast<std::size_t>(place.item);
    if (place.group == kBanksGroup && index < browser_[kBanksGroup].items.size())
    {
        const std::vector<uint32_t>& programs = browser_[kBanksGroup].items[index].child_numbers;
        const auto child = static_cast<std::size_t>(place.child);
        if (place.child >= 0 && child >= programs.size())
        {
            return;
        }

        const uint32_t voices = browser_[kBanksGroup].items[index].number;
        Export job;
        if (place.child >= 0)
        {
            job.program = programs[child];
            job.last_program = programs.back();
        }
        ntdlib::mp2k::Voice voice;
        const bool sample =
            job.program &&
            ntdlib::mp2k::ReadVoice(played_.Gba()->rom, voices + *job.program * edit::kGbaVoiceBytes, voice) &&
            song::GbaVoiceSample(*played_.Gba(), voice);
        job.view = VoiceGroupExportView(song::GbaVoiceGroupName(voices), job.program, sample);
        job.item = song::Item{.kind = song::ItemKind::kBank, .index = voices};
        StartGbaVoiceGroupWork(job);
        export_ = std::move(job);
        return;
    }
    if (place.group != kSequencesGroup || index >= browser_[kSequencesGroup].items.size())
    {
        return;
    }

    const uint32_t song = browser_[kSequencesGroup].items[index].number;
    Export job;
    job.view = GbaSongExportView(song::GbaSongName(*played_.Gba(), static_cast<int>(song)));
    job.item = song::Item{.kind = song::ItemKind::kSequence, .index = song};
    StartGbaMidiWork(job, song);
    export_ = std::move(job);
}

void App::StartDsItemWork(Export& job)
{
    // As for the other consoles, the dialog offers the file's name. A wave's name pads its index to the digits of its
    // wave archive's last wave. A bank's SoundFont takes the bank's name, and so does the folder of its SFZ files. An
    // instrument's files add its program, with the digits that the bank's SFZ files give it (ProgramSuffix).
    FollowArchiveVersion();
    const song::Item item = *job.item;
    const ExportTarget target = job.Target();
    const std::size_t waves = item.kind == song::ItemKind::kWave || item.kind == song::ItemKind::kWaveArchive
                                  ? song::DsWaveCount(*played_.Ds(), item.index)
                                  : 0;
    job.file_name = BankFileName(song::DsItemFileName(*played_.Ds(), item, waves), job, target);
    if (target == ExportTarget::kWav && item.kind != song::ItemKind::kWaveArchive)
    {
        job.file_name = WithoutExtension(job.file_name) + ".wav";
    }

    // A wave archive's WAV export leaves out the waves that can't be decoded, and warns of each.
    std::vector<uint32_t> programs;
    if (job.program)
    {
        programs.push_back(*job.program);
    }
    else if (item.kind == song::ItemKind::kBank)
    {
        programs = song::DsBankPrograms(*played_.Ds(), item.index);
    }
    const auto make = [archive = played_.Ds(), item, target, file_name = job.file_name, program = job.program,
                       programs = std::move(programs)](ExportWork& work)
    {
        std::vector<std::string> found;
        if (target == ExportTarget::kItemFile)
        {
            work.files.emplace_back(file_name, song::DsItemBytes(*archive, item));
        }
        else if (target == ExportTarget::kSoundFont || target == ExportTarget::kSfz)
        {
            work.bank = song::ConvertDsBank(*archive, item.index, programs);
            found = BankExportWarnings(*work.bank, target, program, file_name);
        }
        else if (item.kind == song::ItemKind::kWave)
        {
            work.files.emplace_back(file_name, song::PcmAsWav(song::DsWavePcm(*archive, item.index, item.wave),
                                                              song::DsItemName(*archive, item)));
        }
        else if (item.kind == song::ItemKind::kStream)
        {
            work.files.emplace_back(
                file_name, song::PcmAsWav(song::DsStreamPcm(*archive, item.index), song::DsItemName(*archive, item)));
        }
        else
        {
            const std::size_t count = song::DsWaveCount(*archive, item.index);
            const auto add = [&](uint32_t wave)
            {
                const song::Item each{.kind = song::ItemKind::kWave, .index = item.index, .wave = wave};
                work.files.emplace_back(
                    WithoutExtension(song::DsItemFileName(*archive, each, count)) + ".wav",
                    song::PcmAsWav(song::DsWavePcm(*archive, item.index, wave), song::DsItemName(*archive, each)));
            };
            found = AddWavFiles(std::views::iota(uint32_t{0}, static_cast<uint32_t>(count)), add);
            if (work.files.empty())
            {
                throw NoWavFiles(song::DsItemName(*archive, item), count == 0);
            }
        }
        work.warnings = std::move(found);
    };
    job.work = ExportWorkOf(make);
}

void App::StartDseItemWork(Export& job)
{
    // As for the DS, the dialog offers the file's name. The name of a wave pads its slot to the digits of the last slot
    // of its bank. A bank's SoundFont takes the bank's name, and so does the folder of its SFZ files. An instrument's
    // files add its program (ProgramSuffix). The Bank Editor and the properties edit DSE's banks and waves. The export
    // uses the set of the version that the history has reached.
    FollowArchiveVersion();
    const song::Item item = *job.item;
    const ExportTarget target = job.Target();
    const std::size_t waves = item.kind == song::ItemKind::kWave && item.index < played_.Dse()->banks.size()
                                  ? played_.Dse()->banks[item.index].bank->waves.size()
                                  : 0;
    job.file_name = BankFileName(song::DseItemFileName(*played_.Dse(), item, waves), job, target);

    std::vector<uint32_t> programs;
    if (job.program)
    {
        programs.push_back(*job.program);
    }
    else if (item.kind == song::ItemKind::kBank && item.index < played_.Dse()->banks.size())
    {
        programs = song::DseBankPrograms(*played_.Dse()->banks[item.index].bank);
    }
    const auto make = [set = played_.Dse(), item, target, file_name = job.file_name, program = job.program,
                       programs = std::move(programs)](ExportWork& work)
    {
        std::vector<std::string> found;
        if (target == ExportTarget::kItemFile || item.kind == song::ItemKind::kWave)
        {
            work.files.emplace_back(file_name, song::DseItemBytes(*set, item));
        }
        else if (target == ExportTarget::kSoundFont || target == ExportTarget::kSfz)
        {
            work.bank = song::ConvertDseBank(*set, item.index, programs);
            found = BankExportWarnings(*work.bank, target, program, file_name);
        }
        else
        {
            // A bank's WAV export leaves out the waves that can't be decoded, and warns of each.
            const std::vector<uint32_t> slots = song::DseBankWaves(*set->banks.at(item.index).bank);
            const std::size_t count = set->banks[item.index].bank->waves.size();
            const auto add = [&](uint32_t wave)
            {
                const song::Item each{.kind = song::ItemKind::kWave, .index = item.index, .wave = wave};
                work.files.emplace_back(song::DseItemFileName(*set, each, count), song::DseItemBytes(*set, each));
            };
            found = AddWavFiles(slots, add);
            if (work.files.empty())
            {
                throw NoWavFiles(song::DseItemName(*set, item), slots.empty());
            }
        }
        work.warnings = std::move(found);
    };
    job.work = ExportWorkOf(make);
}

void App::StartDriverItemWork(Export& job)
{
    // The export uses the ROM of the version that the history has reached. Replace… and the Bank Editor change the
    // samples. SFZ files go into a folder of the stem's name. The file of one sample is named after the stem too. The
    // folder of all the samples' SFZ files can't be "Samples": file systems that ignore case would take it for the
    // folder of samples.
    FollowArchiveVersion();
    const song::Item item = *job.item;
    const ExportTarget target = job.Target();
    const bool one = item.kind == song::ItemKind::kWave;
    const std::string stem = one                            ? played_.Driver()->SampleName(static_cast<int>(item.wave))
                             : target == ExportTarget::kSfz ? "All Samples"
                                                            : "Samples";
    job.file_name = target == ExportTarget::kSoundFont    ? stem + ".sf2"
                    : one && target == ExportTarget::kWav ? stem + ".wav"
                                                          : stem;
    const std::vector<int> samples = one ? std::vector<int>{static_cast<int>(item.wave)} : DriverSampleList();
    const auto make = [rom = played_.Driver(), target, one, stem, file_name = job.file_name, samples](ExportWork& work)
    {
        std::vector<std::string> found;
        if (target == ExportTarget::kSoundFont || target == ExportTarget::kSfz)
        {
            work.bank = rom->ConvertSamples(samples, "Hokuto");
            if (target == ExportTarget::kSoundFont)
            {
                found = SoundFontWarnings(*work.bank, stem);
            }
            else if (one && work.bank->sfz.files.size() == 1)
            {
                work.bank->sfz.files.front().name = file_name + ".sfz";
            }
            const std::vector<std::string> converter =
                ConverterWarnings(work.bank->warnings, target == ExportTarget::kSfz);
            found.insert(found.end(), converter.begin(), converter.end());
        }
        else if (one)
        {
            work.files.emplace_back(file_name, rom->SampleWav(samples.front()));
        }
        else
        {
            // A WAV export of the samples leaves out those that contain no samples, and warns of each.
            const auto add = [&](int sample)
            {
                work.files.emplace_back(rom->SampleName(sample) + ".wav", rom->SampleWav(sample));
            };
            found = AddWavFiles(samples, add);
            if (work.files.empty())
            {
                throw std::runtime_error("The songs in the ROM use no sample that Hokuto can read.");
            }
        }
        work.warnings = std::move(found);
    };
    job.work = ExportWorkOf(make);
}

void App::StartGbaVoiceGroupWork(Export& job)
{
    // The SoundFont is named after the voice group. The SoundFont of a voice adds its program to the name, padded to
    // the digits of the last program of the voice group (ProgramSuffix). The ROM that the zones play serves when it
    // came from the version. Otherwise the work first builds the version's ROM. A voice group plays the same voices in
    // both.
    const uint32_t voices = job.item->index;
    const std::optional<uint32_t> program = job.program;
    std::vector<int> programs;
    const std::vector<BrowserItem>& banks = browser_[kBanksGroup].items;
    const auto listed = std::ranges::find(banks, voices, &BrowserItem::number);
    if (program)
    {
        programs.push_back(static_cast<int>(*program));
    }
    else if (listed != banks.end())
    {
        for (const uint32_t selected : listed->child_numbers)
        {
            programs.push_back(static_cast<int>(selected));
        }
    }
    job.file_name = seqlib::SafeFileName(song::GbaVoiceGroupName(voices)) +
                    (program ? ProgramSuffix(*program, job.last_program) : "") + ".sf2";
    const std::shared_ptr<const format::File> version = history_->Current();
    const bool same = SameArchive(*version, *archive_version_);
    const auto convert = [version, rom = same ? played_.Gba() : nullptr, voices,
                          programs = std::move(programs)](ExportWork& work) mutable
    {
        if (!rom)
        {
            rom = GbaRomOf(*version);
        }
        work.bank = song::ConvertVoiceGroup(*rom, voices, programs, "Hokuto");
        work.warnings = ConverterWarnings(work.bank->warnings, false);
    };
    job.work = ExportWorkOf(convert);
}

void App::StartGbaAssemblyWork(Export& job)
{
    // As for the MIDI file, the ROM of the version.
    const std::shared_ptr<const format::File> version = history_->Current();
    const auto number = static_cast<int>(job.item->index);
    const std::string name = song::GbaSongName(*played_.Gba(), number);
    job.file_name = seqlib::SafeFileName(name) + ".s";
    const auto make = [version, rom = SameArchive(*version, *archive_version_) ? played_.Gba() : nullptr, number, name,
                       file_name = job.file_name](ExportWork& work) mutable
    {
        if (!rom)
        {
            rom = GbaRomOf(*version);
        }
        const std::string text = song::GbaSongAssembly(*rom, number, name);
        work.files.emplace_back(file_name, std::vector<uint8_t>(text.begin(), text.end()));
        work.warnings.push_back("The file refers to the voice group by its address.");
    };
    job.work = ExportWorkOf(make);
}

void App::StartGbaBankFileWork(Export& job)
{
    const uint32_t group = job.item->index;
    const std::optional<uint32_t> program = job.program;
    const bool aiff = job.Target() == ExportTarget::kAiff;
    job.file_name = seqlib::SafeFileName(song::GbaVoiceGroupName(group)) +
                    (program ? ProgramSuffix(*program, job.last_program) : "") +
                    (aiff      ? ".aif"
                     : program ? ".bin"
                               : ".s");
    const auto make = [version = history_->Current(),
                       rom = SameArchive(*history_->Current(), *archive_version_) ? played_.Gba() : nullptr, group,
                       program, aiff, file_name = job.file_name](ExportWork& work) mutable
    {
        if (!rom)
        {
            rom = GbaRomOf(*version);
        }
        if (!program)
        {
            song::GbaVoiceGroup found;
            found.address = group;
            const std::string text = song::GbaVoiceGroupAssembly(*rom, found);
            work.files.emplace_back(file_name, std::vector<uint8_t>(text.begin(), text.end()));
            work.warnings.push_back(
                "The file refers to each sample by its address. The voices play those samples only in this ROM, "
                "or after a project defines the sample names.");
        }
        else
        {
            ntdlib::mp2k::Voice voice;
            if (!ntdlib::mp2k::ReadVoice(rom->rom, group + *program * edit::kGbaVoiceBytes, voice) ||
                !song::GbaVoiceSample(*rom, voice))
            {
                throw std::invalid_argument("The voice has no sample to export.");
            }
            work.files.emplace_back(
                file_name, aiff ? song::GbaSampleAiff(*rom, voice.wave) : song::GbaSampleBin(*rom, voice.wave));
        }
    };
    job.work = ExportWorkOf(make);
}

} // namespace hokuto::app
