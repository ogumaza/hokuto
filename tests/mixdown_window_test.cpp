// SPDX-License-Identifier: MIT

// Tests the Export Audio Mixdown window (app/mixdown_window.h) without a display: the example name of a naming scheme,
// the queue's columns and progress, the songs' lengths, the songs after a change of the file, the search, the queue's
// buttons, frames of the window that the user hasn't touched, and the exports of the queue (app/mixdown_jobs.h) on the
// test archive.

#include "app/mixdown_window.h"

#include <imgui.h>

#include <filesystem>
#include <fstream>

#include "seqlib/testing/check.h"
#include "xsflib/testing/check.h"

#include "app/mixdown_jobs.h"
#include "headless_imgui.h"
#include "song_archives.h"
#include "temporary_files.h"

namespace
{

using hokuto::app::Command;
using hokuto::app::DrawMixdownWindow;
using hokuto::app::FollowSongs;
using hokuto::app::MatchesFilter;
using hokuto::app::MixdownActions;
using hokuto::app::MixdownChannelsText;
using hokuto::app::MixdownChoice;
using hokuto::app::MixdownFileType;
using hokuto::app::MixdownFormatText;
using hokuto::app::MixdownJob;
using hokuto::app::MixdownJobDone;
using hokuto::app::MixdownLengthText;
using hokuto::app::MixdownPreview;
using hokuto::app::MixdownSetup;
using hokuto::app::MixdownSong;
using hokuto::app::MixdownView;
using hokuto::app::QueueActions;
using hokuto::app::WaitsForMixdown;
using hokuto::song::BatchSound;
using hokuto::song::NameToken;
using hokuto::song::SampleFormat;
using hokuto::song::StemEffects;

// A window of three songs, the second one chosen alone.
MixdownView ThreeSongs()
{
    MixdownView view;
    view.project = "mset";
    view.console_rate = 32728;
    view.songs = {{.sound = 0, .name = "BGM_CTR_MSET", .chosen = false, .seconds = 151.44},
                  {.sound = 1, .name = "BGM_CTR_MSET_NET", .chosen = true, .seconds = 610.0},
                  {.sound = 12, .name = "", .chosen = false, .error = "Can't read the sequence."}};
    view.setup.folder = "Mixdown";

    return view;
}

// The example uses the first chosen song. A token that comes out empty leaves out its separator, and a stem's
// example names its track. With the mix and the stems, the example shows both.
void TestPreview()
{
    MixdownView view = ThreeSongs();
    SEQLIB_CHECK(MixdownPreview(view) == "BGM_CTR_MSET_NET.wav");
    view.setup.stems = true;
    SEQLIB_CHECK(MixdownPreview(view) == "BGM_CTR_MSET_NET.wav, BGM_CTR_MSET_NET - Track 0.wav");
    view.setup.stems = false;

    view.setup.naming.tokens = {NameToken::kText, NameToken::kNumber, NameToken::kSong};
    SEQLIB_CHECK(MixdownPreview(view) == "001 - BGM_CTR_MSET_NET.wav");

    view.setup.naming.text = "Menu";
    view.setup.naming.separator = "_";
    view.setup.mix = false;
    view.setup.stems = true;
    SEQLIB_CHECK(MixdownPreview(view) == "Menu_001_BGM_CTR_MSET_NET_Track 0.wav");

    view.setup.type = MixdownFileType::kMidiSfz;
    view.setup.naming.tokens = {NameToken::kProject};
    SEQLIB_CHECK(MixdownPreview(view) == "mset.mid");
}

// The queue's Format and Channels columns, and the lengths in the list of songs.
void TestTexts()
{
    MixdownSetup setup;
    SEQLIB_CHECK(MixdownFormatText(setup, 32728) == "Wave, 32.728 kHz, 16-bit");
    setup.settings.form = {.rate = 48000, .format = SampleFormat::kPcm24, .normalise = true, .dither = true};
    SEQLIB_CHECK(MixdownFormatText(setup, 32728) == "Wave, 48 kHz, 24-bit, normalised");
    setup.settings.form = {.rate = 44100, .format = SampleFormat::kPcm16, .dither = true};
    SEQLIB_CHECK(MixdownFormatText(setup, 32728) == "Wave, 44.1 kHz, 16-bit, dither");

    SEQLIB_CHECK(MixdownChannelsText(setup) == "Mix");
    setup.stems = true;
    setup.settings.stem_effects = StemEffects::kDry;
    SEQLIB_CHECK(MixdownChannelsText(setup) == "Mix and Stems (Dry)");
    setup.mix = false;
    setup.settings.stem_effects = StemEffects::kStereoOut;
    SEQLIB_CHECK(MixdownChannelsText(setup) == "Stems (Through Stereo Out)");

    setup.type = MixdownFileType::kMidiSoundFont;
    SEQLIB_CHECK(MixdownFormatText(setup, 32728) == "MIDI and SoundFont 2");
    SEQLIB_CHECK(MixdownChannelsText(setup) == "All tracks");

    SEQLIB_CHECK(MixdownLengthText(151.44) == "2:31.4");
    SEQLIB_CHECK(MixdownLengthText(610.0) == "10:10.0");
    SEQLIB_CHECK(MixdownLengthText(59.96) == "1:00.0");
    SEQLIB_CHECK(MixdownLengthText(-1.0) == "0:00.0");

    // The progress counts the files once the export has counted them, and the songs before.
    MixdownJob job{.songs = {{.sound = 1}, {.sound = 2}}, .done = 1};
    SEQLIB_CHECK(MixdownJobDone(job) == 0.5f);
    job.files = 8;
    job.files_done = 6;
    SEQLIB_CHECK(MixdownJobDone(job) == 0.75f);
    SEQLIB_CHECK(MixdownJobDone(MixdownJob{}) == 0.0f);
}

// The window opens with the shown song chosen alone. After a change of the file, a song that stays keeps its choice,
// and a new song isn't chosen.
void TestFollowSongs()
{
    MixdownView view;
    FollowSongs(view, {{.sound = 0, .name = "A"}, {.sound = 1, .name = "B"}, {.sound = 2, .name = "C"}}, 1);
    SEQLIB_REQUIRE_EQ(view.songs.size(), 3);
    SEQLIB_CHECK(!view.songs[0].chosen && view.songs[1].chosen && !view.songs[2].chosen);

    view.songs[2].chosen = true;
    view.songs[2].seconds = 12.0;
    FollowSongs(view, {{.sound = 2, .name = "C"}, {.sound = 1, .name = "B2"}, {.sound = 5, .name = "E"}}, 5);
    SEQLIB_REQUIRE_EQ(view.songs.size(), 3);
    SEQLIB_CHECK(view.songs[0].sound == 2 && view.songs[0].chosen && !view.songs[0].seconds);
    SEQLIB_CHECK(view.songs[1].name == "B2" && view.songs[1].chosen);
    SEQLIB_CHECK(view.songs[2].sound == 5 && !view.songs[2].chosen);

    MixdownView none;
    FollowSongs(none, {{.sound = 0, .name = "A"}});
    SEQLIB_CHECK(!none.songs[0].chosen);
}

// The search matches part of a name, ignoring case.
void TestFilter()
{
    const MixdownSong song{.name = "BGM_Field_Day"};
    SEQLIB_CHECK(MatchesFilter(song, ""));
    SEQLIB_CHECK(MatchesFilter(song, "field"));
    SEQLIB_CHECK(MatchesFilter(song, "BGM_FIELD_DAY"));
    SEQLIB_CHECK(!MatchesFilter(song, "night"));
    SEQLIB_CHECK(!MatchesFilter(MixdownSong{}, "a"));
}

// The queue's buttons: an export requires chosen songs, a folder and, for WAV files, the mix or the stems. Nothing
// changes the queue while its exports run, and no export starts while the last edit's archive is being built.
void TestQueueActions()
{
    const auto same = [](const MixdownActions& actions, bool add, bool remove, bool start, bool waiting)
    {
        return actions.add == add && actions.remove == remove && actions.start == start && actions.waiting == waiting;
    };
    MixdownView view = ThreeSongs();
    SEQLIB_CHECK(same(QueueActions(view), true, false, true, false));

    view.setup.mix = false;
    SEQLIB_CHECK(same(QueueActions(view), false, false, false, false));
    view.setup.type = MixdownFileType::kMidiSoundFont;
    SEQLIB_CHECK(same(QueueActions(view), true, false, true, false));
    view.setup.folder.clear();
    SEQLIB_CHECK(same(QueueActions(view), false, false, false, false));
    view.setup.folder = "Mixdown";
    view.songs[1].chosen = false;
    SEQLIB_CHECK(same(QueueActions(view), false, false, false, false));

    // An export in the queue that hasn't run makes the button run the queue.
    view.queue.push_back(MixdownJob{.setup = view.setup, .songs = {{.sound = 1}}});
    SEQLIB_CHECK(same(QueueActions(view), false, true, true, true));
    view.settling = true;
    SEQLIB_CHECK(same(QueueActions(view), false, true, false, true));
    view.settling = false;
    view.running = true;
    view.songs[1].chosen = true;
    SEQLIB_CHECK(same(QueueActions(view), false, false, false, false));
    view.running = false;
    view.queue[0].status = "Done.";
    SEQLIB_CHECK(same(QueueActions(view), true, true, true, false));
}

// Frames of the window without input change nothing, with an export in the queue too, and the window stays open.
void TestFrames()
{
    hokuto::testing::CreateHeadlessContext();
    MixdownView view = ThreeSongs();
    view.queue.push_back(MixdownJob{.setup = view.setup, .songs = {{.sound = 1, .name = "BGM_CTR_MSET_NET"}}});
    view.queue.push_back(MixdownJob{.setup = view.setup, .songs = {}, .running = true});
    const MixdownView before = view;
    bool open = true;
    for (int frame = 0; frame < 3; frame++)
    {
        ImGui::NewFrame();
        SEQLIB_CHECK(DrawMixdownWindow(view, &open) == MixdownChoice::kNone);
        hokuto::testing::EndHeadlessFrame();
    }

    SEQLIB_CHECK(open);
    SEQLIB_CHECK(view.setup == before.setup);
    SEQLIB_CHECK(view.songs.size() == 3 && view.songs[1].chosen && !view.songs[0].chosen);
    ImGui::DestroyContext();
}

// A song name and a status wider than their columns draw cut short, and stay whole in the view.
void TestLongTexts()
{
    hokuto::testing::CreateHeadlessContext();
    MixdownView view = ThreeSongs();
    const std::string name(300, 'N');
    view.songs[0].name = name;
    const std::string status = "Done. Tails cut at the limit: " + std::string(300, 'S') + ".";
    view.queue.push_back(MixdownJob{.setup = view.setup, .songs = {{.sound = 0, .name = name}}, .status = status});
    bool open = true;
    for (int frame = 0; frame < 3; frame++)
    {
        ImGui::NewFrame();
        SEQLIB_CHECK(DrawMixdownWindow(view, &open) == MixdownChoice::kNone);
        hokuto::testing::EndHeadlessFrame();
    }

    SEQLIB_CHECK(open && view.songs[0].name == name && view.queue[0].status == status);
    ImGui::DestroyContext();
}

// While an export runs, the commands that close the file wait: New Project, Open, Recent Projects and Import > Sound
// Archive. Quit asks instead. Save, the other imports and the other exports don't close the file.
void TestWaitsForMixdown()
{
    for (const Command command :
         {Command::kNewProject, Command::kOpen, Command::kRecentProjects, Command::kImportSoundArchive})
    {
        SEQLIB_CHECK(WaitsForMixdown(command));
    }
    for (const Command command :
         {Command::kQuit, Command::kSave, Command::kSaveAs, Command::kImportSequence, Command::kExportAudioMixdown,
          Command::kExportMidiSoundFont, Command::kUndo, Command::kStartStop})
    {
        SEQLIB_CHECK(!WaitsForMixdown(command));
    }
}

// The files in `folder`, sorted.
std::vector<std::string> FilesIn(const std::filesystem::path& folder)
{
    std::vector<std::string> files;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(folder))
    {
        files.push_back(entry.path().filename().string());
    }
    std::ranges::sort(files);

    return files;
}

// An export writes the mix and the stems of each chosen song, and uses unique names the second time. A MIDI export
// writes a MIDI file and a SoundFont for each song, with unique names the second time too. The status names the songs
// whose tails were cut. A stopped export writes nothing.
void TestJobs()
{
    const TemporaryFolder folder;
    hokuto::app::MixdownSources sources;
    sources.source.played = hokuto::song::Played(OpenTestArchive());
    sources.project = "test";
    MixdownJob job{.setup = {.stems = true, .folder = folder.Path().string()}};
    job.setup.settings.loops = 1;
    job.setup.settings.fade_seconds = 0.1;
    job.setup.naming.tokens = {NameToken::kSong};
    job.songs = {{.sound = 0, .name = "SEQ_LOOP"}, {.sound = 3, .name = "SEQ_PAN"}};
    const std::atomic<bool> go{false};

    hokuto::app::MixdownJobProgress first;
    hokuto::app::RunMixdownJob(sources, job, first, go);
    hokuto::app::MixdownJobProgress second;
    hokuto::app::RunMixdownJob(sources, job, second, go);

    SEQLIB_CHECK(first.done == 2 && first.failed == 0 && first.status == "Done." && !first.running);
    SEQLIB_CHECK(first.files == 4 && first.files_done == 4);
    SEQLIB_CHECK(second.done == 2 && second.status == "Done.");
    SEQLIB_CHECK(FilesIn(folder.Path()) ==
                 (std::vector<std::string>{"SEQ_LOOP - Track 0 2.wav", "SEQ_LOOP - Track 0.wav", "SEQ_LOOP 2.wav",
                                           "SEQ_LOOP.wav", "SEQ_PAN - Track 0 2.wav", "SEQ_PAN - Track 0.wav",
                                           "SEQ_PAN 2.wav", "SEQ_PAN.wav"}));

    const TemporaryFolder midi_folder;
    job.setup.type = MixdownFileType::kMidiSoundFont;
    job.setup.folder = midi_folder.Path().string();
    hokuto::app::MixdownJobProgress midi;
    hokuto::app::RunMixdownJob(sources, job, midi, go);
    SEQLIB_CHECK(midi.done == 2 && midi.status == "Done." && midi.files == 2 && midi.files_done == 2);
    hokuto::app::MixdownJobProgress midi_again;
    hokuto::app::RunMixdownJob(sources, job, midi_again, go);
    SEQLIB_CHECK(midi_again.done == 2 && midi_again.status == "Done.");
    SEQLIB_CHECK(FilesIn(midi_folder.Path()) ==
                 (std::vector<std::string>{"SEQ_LOOP 2.mid", "SEQ_LOOP 2.sf2", "SEQ_LOOP.mid", "SEQ_LOOP.sf2",
                                           "SEQ_PAN 2.mid", "SEQ_PAN 2.sf2", "SEQ_PAN.mid", "SEQ_PAN.sf2"}));

    const TemporaryFolder cut_folder;
    MixdownJob cut{.setup = {.folder = cut_folder.Path().string()}};
    cut.setup.settings.tail_seconds = 0.001;
    cut.songs = {{.sound = 2, .name = "SEQ_RELEASE"}};
    hokuto::app::MixdownJobProgress tails;
    hokuto::app::RunMixdownJob(sources, cut, tails, go);
    SEQLIB_CHECK(tails.done == 1 && tails.status == "Done. Tails cut at the limit: SEQ_RELEASE.");

    const TemporaryFolder stopped_folder;
    job.setup.type = MixdownFileType::kWave;
    job.setup.folder = stopped_folder.Path().string();
    const std::atomic<bool> stop{true};
    hokuto::app::MixdownJobProgress stopped;
    hokuto::app::RunMixdownJob(sources, job, stopped, stop);
    SEQLIB_CHECK(stopped.done == 0 && stopped.status == "Stopped. 0 of 2 songs done.");

    // A folder that can't be created fails every song: the window then stays open.
    const TemporaryFolder blocked;
    const std::filesystem::path file = blocked.Path() / "file";
    std::ofstream(file) << "x";
    job.setup.folder = (file / "Mixdown").string();
    hokuto::app::MixdownJobProgress refused;
    hokuto::app::RunMixdownJob(sources, job, refused, go);
    SEQLIB_CHECK(refused.failed == 2 && refused.done == 0 && refused.status.starts_with("Can't create the folder"));
    SEQLIB_CHECK(FilesIn(stopped_folder.Path()).empty());
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("mixdown_window",
                                    []
                                    {
                                        TestPreview();
                                        TestTexts();
                                        TestFollowSongs();
                                        TestFilter();
                                        TestQueueActions();
                                        TestFrames();
                                        TestLongTexts();
                                        TestWaitsForMixdown();
                                        TestJobs();
                                    });
}
