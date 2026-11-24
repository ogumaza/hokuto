// SPDX-License-Identifier: MIT

#include "edit/sound_exports.h"

#include <algorithm>
#include <atomic>
#include <exception>
#include <stdexcept>
#include <string>
#include <thread>

#include "ntdlib/rvl/snd/rsar_options.h"
#include "seqlib/file_name.h"
#include "xsflib/files.h"

#include "edit/effects.h"
#include "edit/other_console.h"
#include "edit/timelines.h"
#include "format/archive.h"
#include "format/engines.h"
#include "song/archive.h"
#include "song/ds.h"
#include "song/starts.h"

namespace hokuto::edit
{

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

song::SequenceReader::EngineSound EngineSoundOf(const format::File& file, uint32_t sound, format::Engine engine,
                                                const std::string& name, bool fold)
{
    // The conversion's file contains the sound alone. A conversion for the Wii plays by the rules of its file's build
    // of NW4R SND.
    OtherConsoleSong converted = ForEngine(file, sound, engine);
    if (fold)
    {
        converted.file = FoldTimelines(converted.file);
    }
    const format::File& built = converted.file;
    song::SequenceReader::EngineSound engine_sound{.warnings = std::move(converted.warnings)};
    if (engine == format::Engine::kDs)
    {
        engine_sound.played = song::LoadDsArchive(format::ExportArchive(built));
        engine_sound.slot_programs = std::move(converted.slot_programs);
    }
    else if (engine == format::Engine::kWii)
    {
        engine_sound.played = song::LoadWiiArchive(format::ExportArchive(built), name, ntdlib::rvl::snd::kRsarOptions);
        engine_sound.wii_build = format::BuildOfWiiArchive(*built.native);
        engine_sound.slot_programs = std::move(converted.slot_programs);
    }
    else
    {
        engine_sound.played = song::LoadArchive(format::ExportArchive(built), name);
    }

    return engine_sound;
}

std::optional<format::Engine> OtherEngineOf(const format::File& file, uint32_t sound)
{
    const format::Engine engine = format::EngineOf(file, sound);
    if (engine == file.engine || EngineRefused(file, engine))
    {
        return std::nullopt;
    }

    return engine;
}

namespace
{

// How sound `sound` of `source` starts: the start without a name, and the variables' starting values and changes. A
// sound that the source gives no values takes the start's, and one that it gives no changes takes the start's changes.
struct BatchStart
{
    format::SoundStart start;
    song::StartValues values;
    song::Changes changes;
};

BatchStart BatchStartOf(const BatchSource& source, uint32_t sound)
{
    BatchStart started;
    const auto played = [sound](const format::SoundStart& kept)
    {
        return kept.sound == sound && kept.name.empty();
    };
    const auto start = std::find_if(source.starts.begin(), source.starts.end(), played);
    if (start != source.starts.end())
    {
        started.start = *start;
    }

    const auto values = source.start_values.find(sound);
    const auto changes = source.changes.find(sound);
    started.values = values != source.start_values.end() ? values->second : song::StartValuesOf(started.start);
    started.changes = changes != source.changes.end() ? changes->second : song::ChangesOf(started.start);

    return started;
}

} // namespace

std::vector<BatchSequence> ReadBatchSequences(const BatchSource& source, std::span<const song::BatchSound> sounds,
                                              std::size_t threads, const std::atomic<bool>* stop)
{
    std::vector<BatchSequence> read(sounds.size());
    ForEachOnThreads(
        sounds.size(), threads,
        [&](std::size_t i)
        {
            if (stop != nullptr && *stop)
            {
                read[i].error = "Stopped.";
                return;
            }

            const uint32_t sound = sounds[i].sound;
            const BatchStart started = BatchStartOf(source, sound);
            try
            {
                read[i].sequence = std::make_shared<const song::Sequence>(song::ReadBatchSequence(
                    source.played, sound, source.build, started.values, started.changes, started.start));
            }
            catch (const std::exception& error)
            {
                read[i].error = error.what();
                return;
            }

            // A sound on another console's engine plays its conversion. A conversion that fails leaves
            // the sound on the file's engine.
            const std::optional<format::Engine> engine =
                source.file ? OtherEngineOf(*source.file, sound) : std::nullopt;
            try
            {
                if (engine)
                {
                    song::SequenceReader::EngineSound converted =
                        EngineSoundOf(*source.file, sound, *engine, source.name);
                    read[i].engine_sequence = std::make_shared<const song::Sequence>(
                        song::ReadEngineSequence(*read[i].sequence, converted, started.values, started.changes));
                    read[i].engine = std::move(converted);
                }
            }
            catch (const std::exception&)
            {
                read[i].engine.reset();
                read[i].engine_sequence.reset();
            }
        });

    return read;
}

song::ModelOptions BatchModelOptions(const BatchSource& source, uint32_t sound, std::optional<song::Mixing> mixing)
{
    song::ModelOptions options;
    if (source.file)
    {
        options.effects = ModelEffects(source.file->aux_effects);
        options.mixing = PlayedMixing(*source.file, sound);
    }
    if (mixing)
    {
        options.mixing = *mixing;
    }

    return options;
}

std::vector<std::vector<song::MixdownFile>> MixdownFileNames(std::span<const song::BatchSound> sounds,
                                                             std::span<const BatchSequence> sequences,
                                                             const song::NamingScheme& scheme,
                                                             const std::string& project, bool mix, bool stems,
                                                             const std::function<bool(const std::string&)>& taken,
                                                             const std::map<uint32_t, uint32_t>& muted)
{
    // Every name of the batch, in order, then the files of each sound with the names made unique.
    std::vector<std::string> names;
    std::vector<std::vector<song::MixdownFile>> files(sounds.size());
    for (std::size_t i = 0; i < sounds.size() && i < sequences.size(); i++)
    {
        if (!sequences[i].sequence)
        {
            continue;
        }

        // The tracks of the conversion play on another console's engine.
        const song::Sequence& sequence =
            sequences[i].engine_sequence ? *sequences[i].engine_sequence : *sequences[i].sequence;
        const auto found_mutes = muted.find(sounds[i].sound);
        const uint32_t mutes = found_mutes != muted.end() ? found_mutes->second : 0;
        song::NameParts parts = {.project = project, .song = sounds[i].name, .number = sounds[i].sound};
        if (mix)
        {
            files[i].push_back({});
            names.push_back(song::SchemeName(scheme, parts));
        }
        for (const int track : stems ? song::StemTracks(sequence) : std::vector<int>())
        {
            if (track >= 0 && track < 32 && (mutes >> track & 1) != 0)
            {
                continue;
            }
            const auto found = std::ranges::find(sequence.tracks, track, &song::Track::number);
            parts.track = found->name.empty() ? "Track " + std::to_string(track) : found->name;
            files[i].push_back({.track = track});
            names.push_back(song::SchemeName(scheme, parts));
        }
    }

    const std::vector<std::string> unique = song::BatchFileNames(names, ".wav", taken);
    std::size_t next = 0;
    for (std::vector<song::MixdownFile>& sound_files : files)
    {
        for (song::MixdownFile& file : sound_files)
        {
            file.name = unique[next++];
        }
    }

    return files;
}

std::vector<std::string> MidiFileNames(std::span<const song::BatchSound> sounds, const song::NamingScheme& scheme,
                                       const std::string& project, const std::function<bool(const std::string&)>& taken)
{
    std::vector<std::string> names;
    for (const song::BatchSound& sound : sounds)
    {
        names.push_back(song::SchemeName(scheme, {.project = project, .song = sound.name, .number = sound.sound}));
    }

    return song::BatchFileNames(names, ".mid", taken);
}

song::SoundFiles WriteMixdownSound(const BatchSource& source, uint32_t sound, const BatchSequence& read,
                                   std::span<const song::MixdownFile> files, const song::MixdownSettings& settings,
                                   std::optional<song::Mixing> mixing, const std::filesystem::path& folder,
                                   const song::MixdownProgress& progress, const std::function<void()>& written)
{
    // On another console's engine, the model plays the conversion. Playback does the same.
    const song::ModelOptions options = BatchModelOptions(source, sound, mixing);
    const song::Played& played = read.engine ? read.engine->played : source.played;
    const std::shared_ptr<const song::Sequence>& sequence = read.engine ? read.engine_sequence : read.sequence;
    if (!sequence)
    {
        throw std::runtime_error(read.error);
    }
    const auto make_model = [&](bool dry)
    {
        song::ModelOptions model = options;
        model.dry = model.dry || dry;
        return std::make_shared<song::SequenceModel>(played, sequence, model);
    };
    const auto write = [&](const std::string& name, std::span<const uint8_t> bytes)
    {
        if (const std::optional<std::string> error = xsflib::WriteFile(folder / xsflib::PathFromUtf8(name), bytes))
        {
            throw std::runtime_error(*error);
        }
        if (written)
        {
            written();
        }
    };

    return song::RenderSoundFiles(make_model, *sequence, files, settings, write, progress);
}

void WriteBatchMidi(const BatchSource& source, uint32_t sound, const std::filesystem::path& midi, bool sfz)
{
    // A sound on another console's engine goes out as the conversion for that console. The Export window's MIDI
    // export converts it the same way.
    const BatchStart started = BatchStartOf(source, sound);
    const std::optional<format::Engine> engine = source.file ? OtherEngineOf(*source.file, sound) : std::nullopt;
    ntdlib::sequence_export::Conversion conversion;
    if (engine)
    {
        const song::SequenceReader::EngineSound converted =
            EngineSoundOf(*source.file, sound, *engine, source.name, true);
        conversion = song::ConvertBatchSound(converted.played, converted.sound, converted.wii_build, "Hokuto",
                                             started.values, started.changes, started.start);
    }
    else
    {
        conversion = song::ConvertBatchSound(source.played, sound, source.build, "Hokuto", started.values,
                                             started.changes, started.start);
    }
    if (source.file)
    {
        conversion.midi = song::WithMidiTags(conversion.midi, MidiTagsOf(*source.file, sound));
    }

    if (sfz)
    {
        song::WriteMidiAndSfz(conversion, midi);
    }
    else
    {
        song::WriteMidiAndSoundFont(conversion, midi);
    }
}

void ForEachOnThreads(std::size_t count, std::size_t threads, const std::function<void(std::size_t)>& work)
{
    std::atomic<std::size_t> next{0};
    const auto run = [&]
    {
        for (std::size_t i = next++; i < count; i = next++)
        {
            work(i);
        }
    };

    std::vector<std::thread> pool;
    for (std::size_t t = 1; t < std::min(std::max<std::size_t>(threads, 1), count); t++)
    {
        pool.emplace_back(run);
    }
    run();
    for (std::thread& thread : pool)
    {
        thread.join();
    }
}

} // namespace hokuto::edit
