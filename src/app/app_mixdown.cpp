// SPDX-License-Identifier: MIT

// File > Export > Audio Mixdown…: the Export Audio Mixdown window (mixdown_window.h) and its exports
// (mixdown_jobs.h). The window lists the songs of the open archive, ROM or set. A thread reads every song's sequence
// to show the songs' lengths, and reads them again after an edit. Add to Queue and the export's button collect what
// the export renders at that moment: the last edit, the variables, the mix and the shown song's muted tracks. The
// queue's exports then run one after another on another thread.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/files.h"

#include "app/app.h"
#include "app/app_internal.h"
#include "app/mixdown_jobs.h"
#include "app/mixdown_window.h"
#include "app/system_ui.h"
#include "app/widgets.h"
#include "edit/other_console.h"
#include "edit/sound_exports.h"
#include "format/archive.h"
#include "format/engines.h"
#include "song/batch.h"
#include "song/mixdown.h"

namespace hokuto::app
{

// The exports that run: the queue's rows that they belong to, their progress, and the user's stop.
struct MixdownRun
{
    std::vector<std::size_t> rows;
    std::deque<MixdownJobProgress> progress;
    std::atomic<bool> stop{false};
};

// What the thread that reads the songs finds: each song's sequence, and the rates of the console's mixer in strict
// and enhanced mixing.
struct MixdownReading
{
    std::vector<edit::BatchSequence> sequences;
    int strict_rate = 0;
    int enhanced_rate = 0;
};

// The Export Audio Mixdown window: the versions that its songs come from, the read of the songs, the sources of the
// queue's exports, and the exports that run. A read that a later edit replaced runs on to its end in `stale`.
struct MixdownWork
{
    MixdownView view;
    bool open = true;
    std::shared_ptr<const format::File> archive_version;
    std::shared_ptr<const format::File> version;
    std::shared_ptr<std::atomic<bool>> read_stop = std::make_shared<std::atomic<bool>>(false);
    std::future<MixdownReading> reading;
    std::vector<std::pair<std::shared_ptr<std::atomic<bool>>, std::future<MixdownReading>>> stale;
    MixdownReading read;
    std::vector<MixdownSources> sources; // for each row of the queue
    std::shared_ptr<MixdownRun> run;
    std::future<void> running;

    // Stops the read and the exports. The futures then wait for threads that end within a frame of a render.
    void Stop()
    {
        read_stop->store(true);
        for (auto& [stop, future] : stale)
        {
            stop->store(true);
        }
        if (run)
        {
            run->stop = true;
        }
    }

    // Whether every thread of the work has ended.
    bool Ended() const
    {
        const auto ready = [](const auto& future)
        {
            return !future.valid() || future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        };
        return ready(reading) && ready(running) &&
               std::ranges::all_of(stale, [&](const auto& read) { return ready(read.second); });
    }

    ~MixdownWork()
    {
        Stop();
    }
};

namespace
{

// Sets each song's length from its sequence and the window's settings. A song on another console's engine plays its
// conversion.
void ShowLengths(MixdownWork& work)
{
    for (MixdownSong& song : work.view.songs)
    {
        song.seconds.reset();
        song.error.clear();
    }
    for (std::size_t i = 0; i < work.view.songs.size() && i < work.read.sequences.size(); i++)
    {
        MixdownSong& song = work.view.songs[i];
        const edit::BatchSequence& read = work.read.sequences[i];
        const std::shared_ptr<const song::Sequence>& sequence =
            read.engine_sequence ? read.engine_sequence : read.sequence;
        song.error = read.error;
        if (sequence)
        {
            song.seconds = song::MixdownSeconds(*sequence, song::RangeOf(*sequence, work.view.setup.settings));
            song.loops = song::SongRange(*sequence).fade_at.has_value();
        }
    }
}

// The rate of the console's mixer that the window shows for `document`: the rate of the chosen mode, or of the file's
// one mode. Sounds in different modes, or a rate that isn't read yet, show none.
int ConsoleRate(const MixdownWork& work, const format::File& document)
{
    std::optional<song::Mixing> mixing = work.view.setup.mixing;
    if (!mixing && document.sound_mixing.empty())
    {
        mixing = document.mixing == format::Mixing::kEnhanced && work.view.enhanced ? song::Mixing::kEnhanced
                                                                                    : song::Mixing::kStrict;
    }
    if (!mixing)
    {
        return 0;
    }

    return *mixing == song::Mixing::kEnhanced ? work.read.enhanced_rate : work.read.strict_rate;
}

// Ends the read of the songs that runs, and starts another of `sounds` with `source`. The rates come from a model of
// the first song that reads.
void StartReading(MixdownWork& work, const edit::BatchSource& source, std::vector<song::BatchSound> sounds)
{
    if (work.reading.valid())
    {
        work.read_stop->store(true);
        work.stale.emplace_back(std::move(work.read_stop), std::move(work.reading));
        work.read_stop = std::make_shared<std::atomic<bool>>(false);
    }

    const bool enhanced = work.view.enhanced;
    work.reading =
        std::async(std::launch::async,
                   [source, sounds = std::move(sounds), stop = work.read_stop, enhanced]
                   {
                       MixdownReading read;
                       read.sequences = edit::ReadBatchSequences(source, sounds, MixdownThreads(), stop.get());
                       for (const edit::BatchSequence& song : read.sequences)
                       {
                           if (!song.sequence || *stop)
                           {
                               continue;
                           }
                           try
                           {
                               read.strict_rate =
                                   song::SequenceModel(source.played, song.sequence, {.mixing = song::Mixing::kStrict})
                                       .SampleRate();
                               read.enhanced_rate = enhanced ? song::SequenceModel(source.played, song.sequence,
                                                                                   {.mixing = song::Mixing::kEnhanced})
                                                                   .SampleRate()
                                                             : 0;
                               break;
                           }
                           catch (const std::exception&)
                           {
                           }
                       }
                       return read;
                   });
}

} // namespace

MixdownSources App::MixdownSourcesNow() const
{
    // The archive comes from the last edit that built one. The file's effects, mixing modes, engines and tags come
    // from the last edit. The variables and the mix are the ones that playback uses now.
    const std::shared_ptr<const format::File> version = history_->Current();
    MixdownSources sources;
    sources.source = {.played = played_,
                      .build = version->native && version->native->engine == format::Engine::kWii
                                   ? std::optional(format::BuildOfWiiArchive(*version->native))
                                   : std::nullopt,
                      .file = version,
                      .start_values = start_values_,
                      .changes = changes_,
                      .starts = version->starts,
                      .name = file_name_};
    sources.mix = {.mutes = 0, .bus_volumes = BusVolumes(), .gain = GainOfDecibels(master_.volume)};
    if (sequence_)
    {
        sources.shown = sequence_->sound;
        sources.shown_mutes = MutedTracks();
    }

    const std::string& path = import_path_.empty() ? document_path_ : import_path_;
    sources.project = xsflib::Utf8(xsflib::PathFromUtf8(path).stem());

    return sources;
}

void App::OpenMixdown()
{
    const format::File* document = Document();
    if (document == nullptr || !archive_version_)
    {
        return;
    }

    // The window opens again with its settings, its songs' choices and its queue.
    if (!mixdown_)
    {
        mixdown_ = std::make_shared<MixdownWork>();
        // The files go to a folder Mixdown beside the file.
        const std::string& source = import_path_.empty() ? document_path_ : import_path_;
        if (!source.empty())
        {
            mixdown_->view.setup.folder = xsflib::Utf8(xsflib::PathFromUtf8(source).parent_path() / "Mixdown");
        }
    }
    mixdown_->open = true;
}

bool App::MixdownMoving() const
{
    return mixdown_ && mixdown_->open && (mixdown_->run || mixdown_->reading.valid());
}

bool App::MixdownRunning() const
{
    // DrawMixdown clears `run` only in a frame. With the window closed, no frame may draw once the exports end.
    return mixdown_ && mixdown_->running.valid() &&
           mixdown_->running.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
}

bool App::AskToStopMixdown()
{
    ShowMessagesBeforeBox();

    // Cancel answers Escape, and Stop and Quit answers Return. In SDL3's right-to-left order, Stop and Quit comes on the
    // right on macOS and on the left on Windows. Each system places a dialog's main button there.
    const SDL_MessageBoxButtonData buttons[] = {{SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel"},
                                                {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Stop and Quit"}};
    SDL_MessageBoxData box{};
    box.flags = SDL_MESSAGEBOX_WARNING | SDL_MESSAGEBOX_BUTTONS_RIGHT_TO_LEFT;
    box.window = window_;
    box.title = "Hokuto";
    box.message = "An export is running. Stop it and quit?";
    box.numbuttons = 2;
    box.buttons = buttons;
    int chosen = 0;
    return ShowSystemBox(box, &chosen) && chosen == 1;
}

void App::DropMixdown()
{
    if (mixdown_)
    {
        mixdown_->Stop();
        dropped_mixdowns_.push_back(std::move(mixdown_));
        mixdown_.reset();
    }
}

void App::SetMixdownFolder(const std::string& folder)
{
    if (mixdown_ && !folder.empty())
    {
        mixdown_->view.setup.folder = folder;
    }
}

void App::DrawMixdown()
{
    // A dropped window's threads end within a frame of their renders.
    std::erase_if(dropped_mixdowns_, [](const std::shared_ptr<MixdownWork>& work) { return work->Ended(); });
    const format::File* document = Document();
    if (!mixdown_ || document == nullptr || !history_)
    {
        return;
    }

    MixdownWork& work = *mixdown_;
    std::erase_if(work.stale, [](const auto& read)
                  { return read.second.wait_for(std::chrono::seconds(0)) == std::future_status::ready; });
    if (work.reading.valid() && work.reading.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        work.read = work.reading.get();
        ShowLengths(work);
    }

    // The queue's rows follow the exports that run.
    bool finished = false;
    if (work.run)
    {
        for (std::size_t i = 0; i < work.run->rows.size(); i++)
        {
            MixdownJob& job = work.view.queue[work.run->rows[i]];
            MixdownJobProgress& progress = work.run->progress[i];
            job.done = progress.done;
            job.failed = progress.failed;
            job.files = progress.files;
            job.files_done = progress.files_done;
            job.running = progress.running;
            const std::lock_guard lock(progress.mutex);
            job.status = progress.status;
        }
        if (work.running.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            work.running.get();
            finished = !work.run->stop;
            work.run.reset();
        }
    }
    work.view.running = work.run != nullptr;

    // An export goes on while the window is closed.
    if (!work.open)
    {
        return;
    }

    // The songs follow the last edit while the window shows. An edit that builds a new archive lists its songs.
    const bool settling = Settling();
    work.view.settling = settling;
    if (!settling && (work.archive_version != archive_version_ || work.version != history_->Current()))
    {
        const MixdownSources sources = MixdownSourcesNow();
        const std::vector<song::BatchSound> sounds = song::BatchSounds(played_);
        if (work.archive_version != archive_version_)
        {
            FollowSongs(work.view, sounds, sources.shown);
            work.read = {};
        }
        work.archive_version = archive_version_;
        work.version = history_->Current();
        work.view.project = sources.project;
        work.view.enhanced = !format::MixingRefused(document->engine, format::Mixing::kEnhanced);
        work.view.sfz = !played_.Gba() && (!played_.Driver() || played_.Driver()->WritesSfz());
        if (!work.view.sfz && work.view.setup.type == MixdownFileType::kMidiSfz)
        {
            work.view.setup.type = MixdownFileType::kMidiSoundFont;
        }
        StartReading(work, sources.source, sounds);
    }
    work.view.console_rate = ConsoleRate(work, *document);

    const MixdownChoice choice = DrawMixdownWindow(work.view, &work.open);
    const MixdownActions actions = QueueActions(work.view);
    switch (choice)
    {
    case MixdownChoice::kNone:
        break;

    case MixdownChoice::kClose:
        work.open = false;
        break;

    case MixdownChoice::kChooseFolder:
        ShowDialog(Dialog::kMixdownFolder);
        break;

    case MixdownChoice::kChanged:
        ShowLengths(work);
        break;

    case MixdownChoice::kRemoveAll:
        if (actions.remove)
        {
            work.view.queue.clear();
            work.sources.clear();
        }
        break;

    case MixdownChoice::kStop:
        if (work.run)
        {
            work.run->stop = true;
        }
        break;

    case MixdownChoice::kAddToQueue:
    case MixdownChoice::kStart:
        {
            if (choice == MixdownChoice::kAddToQueue ? !actions.add : !actions.start)
            {
                break;
            }
            if (choice == MixdownChoice::kAddToQueue || !actions.waiting)
            {
                MixdownJob job{.setup = work.view.setup};
                for (const MixdownSong& song : work.view.songs)
                {
                    if (song.chosen)
                    {
                        job.songs.push_back({.sound = song.sound, .name = song.name});
                    }
                }
                work.view.queue.push_back(std::move(job));
                work.sources.push_back(MixdownSourcesNow());
            }
            if (choice == MixdownChoice::kAddToQueue)
            {
                break;
            }

            // The exports that wait run in the queue's order, one after another.
            auto run = std::make_shared<MixdownRun>();
            std::vector<std::pair<MixdownSources, MixdownJob>> jobs;
            for (std::size_t row = 0; row < work.view.queue.size(); row++)
            {
                if (work.view.queue[row].status.empty())
                {
                    run->rows.push_back(row);
                    run->progress.emplace_back();
                    jobs.emplace_back(work.sources[row], work.view.queue[row]);
                }
            }
            work.run = run;
            work.view.running = true;
            // The thread wakes the program's loop when it ends. The commands that wait for it are then enabled at once,
            // with the window closed too.
            work.running = std::async(std::launch::async,
                                      [run, jobs = std::move(jobs), wake = wake_event_]
                                      {
                                          for (std::size_t i = 0; i < jobs.size() && !run->stop; i++)
                                          {
                                              RunMixdownJob(jobs[i].first, jobs[i].second, run->progress[i], run->stop);
                                          }
                                          SDL_Event event{};
                                          event.type = wake;
                                          SDL_PushEvent(&event);
                                      });
            break;
        }
    }

    // The window closes after an export unless Keep Dialog Open is on, or an export failed.
    if (finished && !work.view.keep_open)
    {
        const bool failed = std::ranges::any_of(work.view.queue, [](const MixdownJob& job) { return job.failed > 0; });
        work.open = failed;
        if (!failed)
        {
            work.view.queue.clear();
            work.sources.clear();
        }
    }
}

} // namespace hokuto::app
