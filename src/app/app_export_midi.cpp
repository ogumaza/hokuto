// SPDX-License-Identifier: MIT

// The MIDI export: the work that converts a sequence to a MIDI file with its instruments, on each console.

#include <cstring>
#include <exception>
#include <future>
#include <utility>

#include "ntdlib/rvl/snd/rsar_options.h"
#include "seqlib/file_name.h"

#include "app/app.h"
#include "app/app_export_parts.h"
#include "app/app_internal.h"
#include "app/engine_names.h"
#include "app/export_warnings.h"
#include "edit/other_console.h"
#include "edit/timelines.h"
#include "format/archive.h"
#include "format/drivers.h"
#include "format/file.h"
#include "song/archive.h"
#include "song/driver.h"
#include "song/midi_export.h"

namespace hokuto::app
{

using export_parts::Converts;

namespace
{

// The tags of the MIDI file of sound `sound` of `file`: the sound's title, and the sound's copyright or else the
// file's. The file's title names the whole file. Without a title of the sound, the MIDI file keeps the sound's name.
song::MidiTags MidiTagsOf(const format::File& file, uint32_t sound)
{
    const auto tag = [](const format::Tags& tags, const std::string& key)
    {
        const auto found = tags.find(key);
        return found != tags.end() ? found->second : std::string();
    };

    const auto of_sound = file.sound_tags.find(sound);
    const format::Tags sound_tags = of_sound != file.sound_tags.end() ? of_sound->second : format::Tags();
    song::MidiTags tags{tag(sound_tags, "title"), tag(sound_tags, "copyright")};
    if (tags.copyright.empty())
    {
        tags.copyright = tag(file.tags, "copyright");
    }

    return tags;
}

// Converts `sound` of `version` for `engine`, the engine of another console, into `work`. The measure plays the
// conversion, and the sequence read from it when the zones don't show the sound (`shown`). Returns the conversion's
// notes.
std::vector<std::string> ConvertForEngine(ExportWork& work, const std::shared_ptr<const format::File>& version,
                                          uint32_t sound, format::Engine engine, const song::StartValues& values,
                                          const song::Changes& changes, const std::string& name, bool shown)
{
    edit::OtherConsoleSong song = edit::ForEngine(*version, sound, engine);
    const std::vector<uint8_t> built = format::ExportArchive(edit::FoldTimelines(song.file));
    if (engine == format::Engine::kDs)
    {
        work.played = song::LoadDsArchive(built);
        work.conversion = song::ConvertToMidi(*work.played.Ds(), 0, values, "Hokuto");
        if (!shown)
        {
            work.played_sequence =
                std::make_shared<const song::Sequence>(song::ReadSequence(*work.played.Ds(), 0, values));
        }
    }
    else if (engine == format::Engine::kWii)
    {
        const std::optional<ntdlib::SndBuild> converted = format::BuildOfWiiArchive(*song.file.native);
        work.played = song::LoadWiiArchive(built, name, ntdlib::rvl::snd::kRsarOptions);
        work.conversion = song::ConvertToMidi(*work.played.Wii(), 0, values, changes, "Hokuto", converted);
        if (!shown)
        {
            work.played_sequence = std::make_shared<const song::Sequence>(
                song::ReadSequence(*work.played.Wii(), 0, values, changes, converted));
        }
    }
    else
    {
        work.played = song::LoadArchive(built, name);
        work.conversion = song::ConvertToMidi(*work.played.Sar(), 0, values, changes, "Hokuto");
        if (!shown)
        {
            work.played_sequence =
                std::make_shared<const song::Sequence>(song::ReadSequence(*work.played.Sar(), 0, values, changes));
        }
    }

    return std::move(song.warnings);
}

// Converts `sound` of `version` on its console into `work`. `archive` and `wii` serve when the zones play the
// version. Otherwise the work builds the version's archive into them.
void ConvertNative(ExportWork& work, const std::shared_ptr<const format::File>& version, uint32_t sound,
                   const song::StartValues& values, const song::Changes& changes,
                   const std::optional<ntdlib::SndBuild>& build, const std::string& name,
                   std::shared_ptr<const ntdlib::sar::SoundArchive>& archive,
                   std::shared_ptr<const ntdlib::rsar::SoundArchive>& wii)
{
    if (version->native->engine == format::Engine::kWii)
    {
        if (!wii)
        {
            wii = song::LoadWiiArchive(format::ExportArchive(edit::FoldTimelines(*version)), name,
                                       ntdlib::rvl::snd::kRsarOptions);
        }
        work.conversion = song::ConvertToMidi(*wii, sound, values, changes, "Hokuto", build);
    }
    else
    {
        if (!archive)
        {
            const format::File folded = edit::FoldTimelines(*version);
            archive = SarArchiveOf(folded, name);
        }
        work.conversion = song::ConvertToMidi(*archive, sound, values, changes, "Hokuto");
    }
}

// The sequence of `sound` in the archive of `version`. The warnings of a sound that the zones don't show come from it.
// `archive` and `wii` keep the archive that the work builds for the measure.
std::shared_ptr<const song::Sequence> ReadUnshownSequence(
    const std::shared_ptr<const format::File>& version, uint32_t sound, const song::StartValues& values,
    const song::Changes& changes, const std::optional<ntdlib::SndBuild>& build, const std::optional<song::DsGame>& game,
    const std::string& name, std::shared_ptr<const ntdlib::sar::SoundArchive>& archive,
    std::shared_ptr<const ntdlib::rsar::SoundArchive>& wii)
{
    if (version->native->engine == format::Engine::kGba)
    {
        const std::shared_ptr<const song::GbaRom> rom = GbaRomOf(*version);
        return std::make_shared<const song::Sequence>(song::ReadSequence(*rom, static_cast<int>(sound)));
    }
    else if (version->native->engine == format::Engine::kDs)
    {
        const std::shared_ptr<const song::DsArchive> ds = DsArchiveOf(*version, game);
        return std::make_shared<const song::Sequence>(song::ReadSequence(*ds, sound, values));
    }
    else if (version->native->engine == format::Engine::kDse)
    {
        const std::shared_ptr<const song::DseSet> set = DseSetOf(*version);
        return std::make_shared<const song::Sequence>(song::ReadSequence(*set, sound));
    }
    else if (version->native->driver)
    {
        const std::shared_ptr<const song::DriverRom> rom =
            song::LoadDriverRom(version->native->engine, format::PlayedBytes(*version));
        return std::make_shared<const song::Sequence>(rom->ReadSequence(sound));
    }
    else
    {
        if (!archive && !wii)
        {
            const format::File folded = edit::FoldTimelines(*version);
            if (version->native->engine == format::Engine::kWii)
            {
                wii = WiiArchiveOf(folded, name);
            }
            else
            {
                archive = SarArchiveOf(folded, name);
            }
        }
        return std::make_shared<const song::Sequence>(wii ? song::ReadSequence(*wii, sound, values, changes, build)
                                                          : song::ReadSequence(*archive, sound, values, changes));
    }
}

} // namespace

template <typename Source>
void App::StartSourceMidiWork(Export& job, uint32_t sound, MidiSource<Source> source)
{
    const std::shared_ptr<const format::File> version = history_->Current();
    job.file_name = seqlib::SafeFileName(source.name) + ".mid";
    const bool same = SameArchive(*version, *archive_version_);
    const bool shown = sequence_ && sequence_->sound == sound;
    const ExportTarget target = job.Target();
    std::vector<std::string> warnings =
        shown ? MidiWarnings(*sequence_, version->aux_effects, version->aux_volumes, target)
              : std::vector<std::string>();
    std::shared_ptr<const Source> from = same ? source.shown : nullptr;
    const auto convert = [version, from = std::move(from), source = std::move(source), shown, target,
                          warnings = std::move(warnings), tags = MidiTagsOf(*version, sound)]() mutable
    {
        ExportWork work;
        try
        {
            if (!from)
            {
                from = source.load(*version);
            }
            work.conversion = source.convert(*from);
            if (!shown)
            {
                work.sequence = std::make_shared<const song::Sequence>(source.read(*from));
                work.played = from;
                work.played_sequence = work.sequence;
                warnings = MidiWarnings(*work.sequence, version->aux_effects, version->aux_volumes, target);
            }
            work.conversion->midi = song::WithMidiTags(work.conversion->midi, tags);
            const std::vector<std::string> report = ConverterWarnings(song::ConversionReport(*work.conversion),
                                                                      source.sfz && target == ExportTarget::kMidiSfz);
            work.warnings = std::move(warnings);
            work.warnings.insert(work.warnings.end(), report.begin(), report.end());
        }
        catch (const std::exception& error)
        {
            work.error = error.what();
        }

        return work;
    };
    job.work = std::async(std::launch::async, convert);
}

void App::StartGbaMidiWork(Export& job, uint32_t song)
{
    // The ROM that the zones play serves when it came from the version. Otherwise the work first builds the version's
    // ROM. A saved file contains the same ROM. The MIDI file takes the tags. A song on another console's engine goes
    // out as the conversion for that console. A song that the zones don't show gives the warnings from its sequence,
    // and the measure plays that sequence.
    if (Converts(*history_->Current(), song))
    {
        StartMidiWork(job, song);
        return;
    }

    const auto load = [](const format::File& version)
    {
        return GbaRomOf(version);
    };
    const auto convert = [song](const song::GbaRom& rom)
    {
        return song::ConvertToMidi(rom, static_cast<int>(song), "Hokuto");
    };
    const auto read = [song](const song::GbaRom& rom)
    {
        return song::ReadSequence(rom, static_cast<int>(song));
    };
    StartSourceMidiWork(job, song,
                        MidiSource<song::GbaRom>{.name = song::GbaSongName(*played_.Gba(), static_cast<int>(song)),
                                                 .shown = played_.Gba(),
                                                 .load = load,
                                                 .convert = convert,
                                                 .read = read,
                                                 .sfz = false});
}

void App::StartDriverMidiWork(Export& job, uint32_t sound)
{
    // As for a song of the GBA: the ROM that the zones play serves when it came from the version, and the work
    // otherwise first builds the version's ROM. The MIDI file takes the tags. A song on another console's engine goes
    // out as the conversion for that console. A song that the zones don't show gives the warnings from its sequence,
    // and the measure plays that sequence.
    if (Converts(*history_->Current(), sound))
    {
        StartMidiWork(job, sound);
        return;
    }

    const auto load = [](const format::File& version)
    {
        return DriverRomOf(version);
    };
    const auto convert = [sound](const song::DriverRom& rom)
    {
        return rom.ConvertToMidi(sound, "Hokuto");
    };
    const auto read = [sound](const song::DriverRom& rom)
    {
        return rom.ReadSequence(sound);
    };
    StartSourceMidiWork(job, sound,
                        MidiSource<song::DriverRom>{
                            .name = played_.Driver()->SoundName(sound),
                            .shown = played_.Driver(),
                            .load = load,
                            .convert = convert,
                            .read = read,
                        });
}

void App::StartDsMidiWork(Export& job, uint32_t sound)
{
    // The archive that the zones play serves when it came from the version. Otherwise the work first builds the
    // version's archive. A saved file contains the same archive. The sequence plays with the zone's starting values,
    // and the MIDI file includes the tags. A sound on another console's engine goes out as the conversion for that
    // console. A sound that the zones don't show gives the warnings from its sequence, and the measure plays that
    // sequence.
    if (Converts(*history_->Current(), sound))
    {
        StartMidiWork(job, sound);
        return;
    }

    const auto load = [game = GameOf(played_.Ds())](const format::File& version)
    {
        return DsArchiveOf(version, game);
    };
    const song::StartValues values = StartValuesOf(sound);
    const auto convert = [sound, values](const song::DsArchive& archive)
    {
        return song::ConvertToMidi(archive, sound, values, "Hokuto");
    };
    const auto read = [sound, values](const song::DsArchive& archive)
    {
        return song::ReadSequence(archive, sound, values);
    };
    StartSourceMidiWork(job, sound,
                        MidiSource<song::DsArchive>{
                            .name = song::DsSoundName(*played_.Ds(), sound),
                            .shown = played_.Ds(),
                            .load = load,
                            .convert = convert,
                            .read = read,
                        });
}

void App::StartDseMidiWork(Export& job, uint32_t sound)
{
    // The set that the zones play serves when it came from the version. Otherwise the work first builds the version's
    // set. A saved file contains the same set. The MIDI file takes the tags. A sound on another console's engine goes
    // out as the conversion for that console. A sound that the zones don't show gives the warnings from its sequence,
    // and the measure plays that sequence.
    if (Converts(*history_->Current(), sound))
    {
        StartMidiWork(job, sound);
        return;
    }

    const auto load = [](const format::File& version)
    {
        return DseSetOf(version);
    };
    const auto convert = [sound](const song::DseSet& set)
    {
        return song::ConvertToMidi(set, sound, "Hokuto");
    };
    const auto read = [sound](const song::DseSet& set)
    {
        return song::ReadSequence(set, sound);
    };
    StartSourceMidiWork(job, sound,
                        MidiSource<song::DseSet>{
                            .name = song::DseSoundName(*played_.Dse(), sound),
                            .shown = played_.Dse(),
                            .load = load,
                            .convert = convert,
                            .read = read,
                        });
}

std::string App::MidiFileName(uint32_t sound) const
{
    return seqlib::SafeFileName(played_.Gba()      ? song::GbaSongName(*played_.Gba(), static_cast<int>(sound))
                                : played_.Ds()     ? song::DsSoundName(*played_.Ds(), sound)
                                : played_.Dse()    ? song::DseSoundName(*played_.Dse(), sound)
                                : played_.Driver() ? played_.Driver()->SoundName(sound)
                                : played_.Wii()    ? played_.Wii()->Sounds().at(sound).name
                                                   : played_.Sar()->Sounds().at(sound).name) +
           ".mid";
}

void App::StartMidiWork(Export& job, uint32_t sound)
{
    // The archive that the zones play serves when it came from the version and holds no timelines. Otherwise the work
    // first builds the version's archive. A saved file holds the same archive. A sound on another console's engine goes
    // out as the conversion for that console. A saved file holds the same conversion. Hokuto's warnings come before the
    // converter's. The MIDI file takes the tags.
    const std::shared_ptr<const format::File> version = history_->Current();
    const ExportTarget target = job.Target();
    const bool shown = sequence_ && sequence_->sound == sound;
    job.file_name = MidiFileName(sound);
    const bool same = SameArchive(*version, *archive_version_) && !(version->timelines && !version->timelines->empty());
    const format::Engine engine = format::EngineOf(*version, sound);
    const bool converts = Converts(*version, sound);
    std::vector<std::string> warnings =
        shown ? MidiWarnings(*sequence_, version->aux_effects, version->aux_volumes, target)
              : std::vector<std::string>();
    const auto convert = [version, archive = same && !converts ? played_.Sar() : nullptr,
                          wii = same && !converts ? played_.Wii() : nullptr, game = GameOf(played_.Ds()),
                          name = file_name_, sound, shown, converts, engine, target, values = StartValuesOf(sound),
                          changes = ChangesOf(sound), warnings = std::move(warnings),
                          tags = MidiTagsOf(*version, sound)]() mutable
    {
        ExportWork work;
        try
        {
            // The conversion's file holds the sound alone. A conversion for the Wii converts by the rules of its file's
            // build of NW4R SND, and a file of the Wii plays by the rules of the game's build.
            std::vector<std::string> notes;
            const std::optional<ntdlib::SndBuild> build =
                version->native->engine == format::Engine::kWii
                    ? std::optional(format::BuildOfWiiArchive(*version->native))
                    : std::nullopt;
            if (converts)
            {
                notes = ConvertForEngine(work, version, sound, engine, values, changes, name, shown);
            }
            else
            {
                ConvertNative(work, version, sound, values, changes, build, name, archive, wii);
            }

            // A sound that the zones don't show gives the warnings from its sequence in the file's archive. Without a
            // conversion for another engine, the measure plays that sequence.
            if (!shown)
            {
                work.sequence = ReadUnshownSequence(version, sound, values, changes, build, game, name, archive, wii);
                warnings = MidiWarnings(*work.sequence, version->aux_effects, version->aux_volumes, target);
                if (!converts)
                {
                    work.played = wii ? song::Played(wii) : song::Played(archive);
                    work.played_sequence = work.sequence;
                }
            }
            if (converts)
            {
                warnings.insert(warnings.begin(), EngineNote(EngineName(engine), EngineName(version->engine)));
            }

            work.conversion->midi = song::WithMidiTags(work.conversion->midi, tags);
            const std::vector<std::string> report =
                ConverterWarnings(song::ConversionReport(*work.conversion), target == ExportTarget::kMidiSfz);
            work.warnings = std::move(warnings);
            work.warnings.insert(work.warnings.begin() + (converts ? 1 : 0), notes.begin(), notes.end());
            work.warnings.insert(work.warnings.end(), report.begin(), report.end());
        }
        catch (const std::exception& error)
        {
            work.error = error.what();
        }

        return work;
    };
    job.work = std::async(std::launch::async, convert);
}

} // namespace hokuto::app
