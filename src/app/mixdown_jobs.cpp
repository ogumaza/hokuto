// SPDX-License-Identifier: MIT

#include "app/mixdown_jobs.h"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <vector>

#include "xsflib/files.h"

#include "song/batch.h"
#include "song/midi_export.h"

namespace hokuto::app
{
namespace
{

// The settings of a song of `job` from `sources`: the mix of the file, with the shown song's muted tracks.
song::MixdownSettings SongSettings(const MixdownJob& job, const MixdownSources& sources, uint32_t sound)
{
    song::MixdownSettings settings = job.setup.settings;
    settings.mix = sources.mix;
    settings.mix.mutes = sources.shown == sound ? sources.shown_mutes : 0;

    return settings;
}

// The status of an export of `songs` songs that wrote `done` songs, stopped `partial` songs after some of their files
// and failed `failed`, with the first failure's message.
std::string StatusText(std::size_t songs, std::size_t done, std::size_t partial, std::size_t failed, bool stopped,
                       const std::string& first_error)
{
    if (stopped)
    {
        return "Stopped. " + std::to_string(done) + " of " + std::to_string(songs) + " songs done" +
               (partial > 0 ? ", " + std::to_string(partial) + " in part." : ".");
    }
    if (failed == 0)
    {
        return "Done.";
    }

    const bool ended = !first_error.empty() && first_error.back() == '.';
    return std::to_string(done) + " done, " + std::to_string(failed) + " failed. " + first_error + (ended ? "" : ".");
}

// The notes of an export's status: the songs that fade at the limit of 10 minutes, and the songs whose tails sounded
// past their limit.
std::string Notes(std::vector<std::string> limited, std::vector<std::string> cut)
{
    const auto list = [](std::vector<std::string> names)
    {
        std::ranges::sort(names);
        std::string text;
        for (const std::string& name : names)
        {
            text += (text.empty() ? "" : ", ") + name;
        }
        return text + ".";
    };

    std::string notes;
    if (!limited.empty())
    {
        notes += " Fade at 10 minutes: " + list(std::move(limited));
    }
    if (!cut.empty())
    {
        notes += " Tails cut at the limit: " + list(std::move(cut));
    }

    return notes;
}

} // namespace

std::size_t MixdownThreads()
{
    return std::clamp<std::size_t>(std::thread::hardware_concurrency() / 2, 1, 4);
}

void RunMixdownJob(const MixdownSources& sources, const MixdownJob& job, MixdownJobProgress& progress,
                   const std::atomic<bool>& stop)
{
    progress.running = true;
    std::mutex error_mutex;
    std::string first_error;
    const auto failed = [&](const std::string& name, const std::string& error)
    {
        const std::lock_guard lock(error_mutex);
        if (first_error.empty())
        {
            first_error = name + ": " + error;
        }
        progress.failed++;
    };

    const std::filesystem::path folder = xsflib::PathFromUtf8(job.setup.folder);
    std::error_code made;
    std::filesystem::create_directories(folder, made);
    // A MIDI file's SoundFont 2 file is beside it, and its SFZ files in sfz/<name>.
    const auto taken = [&](const std::string& name)
    {
        const auto exists = [&](const std::filesystem::path& path)
        {
            std::error_code missing;
            return std::filesystem::exists(path, missing);
        };
        const std::filesystem::path file = folder / xsflib::PathFromUtf8(name);
        const std::filesystem::path stem = file.stem();
        return !job.setup.overwrite && (exists(file) ||
                                        (job.setup.type == MixdownFileType::kMidiSoundFont &&
                                         exists(std::filesystem::path(file).replace_extension(".sf2"))) ||
                                        (job.setup.type == MixdownFileType::kMidiSfz && exists(folder / "sfz" / stem)));
    };
    std::vector<std::string> limited;
    std::vector<std::string> cut;
    std::size_t partial = 0;

    if (made)
    {
        const std::lock_guard lock(progress.mutex);
        progress.status = "Can't create the folder: " + made.message();
        progress.failed = job.songs.size();
    }
    else if (job.setup.type == MixdownFileType::kWave)
    {
        const std::vector<edit::BatchSequence> sequences =
            edit::ReadBatchSequences(sources.source, job.songs, MixdownThreads());
        std::map<uint32_t, uint32_t> muted;
        if (sources.shown)
        {
            muted[*sources.shown] = sources.shown_mutes;
        }
        const std::vector<std::vector<song::MixdownFile>> files = edit::MixdownFileNames(
            job.songs, sequences, job.setup.naming, sources.project, job.setup.mix, job.setup.stems, taken, muted);
        std::size_t count = 0;
        for (const std::vector<song::MixdownFile>& song_files : files)
        {
            count += song_files.size();
        }
        progress.files = count;
        const auto file_written = [&progress]
        {
            progress.files_done++;
        };
        const song::MixdownProgress stopper = [&stop](std::size_t)
        {
            return !stop;
        };
        edit::ForEachOnThreads(job.songs.size(), MixdownThreads(),
                               [&](std::size_t i)
                               {
                                   if (stop)
                                   {
                                       return;
                                   }
                                   try
                                   {
                                       const uint32_t sound = job.songs[i].sound;
                                       const song::SoundFiles written =
                                           edit::WriteMixdownSound(sources.source, sound, sequences[i], files[i],
                                                                   SongSettings(job, sources, sound), job.setup.mixing,
                                                                   folder, stopper, file_written);
                                       progress.done += written.cancelled ? 0 : 1;
                                       const std::lock_guard lock(error_mutex);
                                       partial += written.cancelled && written.files > 0 ? 1 : 0;
                                       if (written.limited)
                                       {
                                           limited.push_back(job.songs[i].name);
                                       }
                                       if (written.tails_cut)
                                       {
                                           cut.push_back(job.songs[i].name);
                                       }
                                   }
                                   catch (const std::exception& error)
                                   {
                                       failed(job.songs[i].name, error.what());
                                   }
                               });
    }
    else
    {
        const std::vector<std::string> files = edit::MidiFileNames(job.songs, job.setup.naming, sources.project, taken);
        progress.files = files.size();
        for (std::size_t i = 0; i < job.songs.size() && !stop; i++)
        {
            try
            {
                edit::WriteBatchMidi(sources.source, job.songs[i].sound, folder / xsflib::PathFromUtf8(files[i]),
                                     job.setup.type == MixdownFileType::kMidiSfz);
                progress.done++;
            }
            catch (const song::EmptyConversion&)
            {
                failed(job.songs[i].name, "plays no notes");
            }
            catch (const std::exception& error)
            {
                failed(job.songs[i].name, error.what());
            }
            progress.files_done++;
        }
    }

    const std::lock_guard lock(progress.mutex);
    if (progress.status.empty())
    {
        progress.status = StatusText(job.songs.size(), progress.done, partial, progress.failed, stop, first_error) +
                          Notes(limited, cut);
    }
    progress.running = false;
}

} // namespace hokuto::app
