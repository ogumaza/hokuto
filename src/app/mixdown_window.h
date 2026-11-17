// SPDX-License-Identifier: MIT

// The Export Audio Mixdown window of File > Export > Audio Mixdown…: the songs of the file with check boxes, the whole
// mix or a stem for each track, the range of each song, the files' names and format, and a queue of exports. Each
// export renders offline through the playback engine. The same list of songs exports MIDI with SoundFont 2 or SFZ
// files too. app_mixdown.cpp and mixdown_jobs.cpp run the exports.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "song/batch.h"
#include "song/mixdown.h"
#include "song/mixdown_files.h"

namespace hokuto::app
{

// What the window exports: WAV files, or MIDI files with their instruments.
enum class MixdownFileType
{
    kWave,
    kMidiSoundFont,
    kMidiSfz,
};

// A song in the window's list: the sound by its number, its name, whether it's chosen, and the length of its file once
// the window has read the song. `loops` says whether the read found a loop. Loops and Fade Out change only such a
// song. `error` says why a song can't be read.
struct MixdownSong
{
    uint32_t sound = 0;
    std::string name;
    bool chosen = false;
    std::optional<double> seconds;
    bool loops = false;
    std::string error;
};

// What the window sets for an export. Add to Queue copies it into the queue.
struct MixdownSetup
{
    friend bool operator==(const MixdownSetup&, const MixdownSetup&) = default;

    MixdownFileType type = MixdownFileType::kWave;
    bool mix = true;
    bool stems = false;
    song::MixdownSettings settings;
    std::optional<song::Mixing> mixing; // none plays each song in its mode
    song::NamingScheme naming;
    std::string folder;
    bool overwrite = false; // Always Overwrite, or else Create Unique File Name
};

// A row of the export queue: the export's setup and songs, and how far it has got. `done` counts the songs written,
// and `failed` the songs that failed or stayed silent. `files` counts the files that the export writes once it has
// counted them, and `files_done` the files written. `running` marks the export that renders now. `status` is empty
// until the export has run, and then says what happened.
struct MixdownJob
{
    MixdownSetup setup;
    std::vector<song::BatchSound> songs;
    std::size_t done = 0;
    std::size_t failed = 0;
    std::size_t files = 0;
    std::size_t files_done = 0;
    bool running = false;
    std::string status;
};

// How far `job` has got, from 0 to 1: its files written, or its songs done before the export counts its files.
float MixdownJobDone(const MixdownJob& job);

// What the window shows. `filter` limits the list to the songs whose names contain it, ignoring case. `project` names
// the file for the Project token. `console_rate` is the rate of the console's
// mixer in the chosen mode, in hertz, or 0 while it isn't known or varies by song. `enhanced` says whether the file's
// console offers enhanced mixing. `sfz` says whether its converter writes SFZ files. `running` says that the queue's
// exports run. `settling` says that the last edit's archive is still being built: no export starts until it's done.
struct MixdownView
{
    std::vector<MixdownSong> songs;
    std::string filter;
    std::string project;
    int console_rate = 0;
    bool enhanced = true;
    bool sfz = true;
    MixdownSetup setup;
    std::vector<MixdownJob> queue;
    bool keep_open = false;
    bool running = false;
    bool settling = false;
};

// The songs of the window after the file changed: `sounds` in their order. A song that stays keeps its choice. A new
// song isn't chosen, apart from `shown` when the window had no songs: the window opens with the shown song chosen. The
// lengths wait for the next read.
void FollowSongs(MixdownView& view, const std::vector<song::BatchSound>& sounds,
                 std::optional<uint32_t> shown = std::nullopt);

// Whether song `song` matches the window's filter `filter`.
bool MatchesFilter(const MixdownSong& song, const std::string& filter);

// What the queue's buttons do now: Add to Queue, Remove All and the export's button. Nothing changes the queue while
// its exports run. An export requires chosen songs, a folder and, for WAV files, the mix or the stems. No export starts
// while the last edit's archive is being built.
struct MixdownActions
{
    bool add = false;
    bool remove = false;
    bool start = false;
    bool waiting = false; // an export of the queue waits: the button runs the queue
};
MixdownActions QueueActions(const MixdownView& view);

// What the user did in the window.
enum class MixdownChoice
{
    kNone,
    kClose,        // the window's close button, or Close
    kChooseFolder, // Choose… beside the path
    kChanged,      // a setting that changes the songs' lengths
    kAddToQueue,
    kRemoveAll, // every export of the queue
    kStart,     // Export Audio, or Start Queue Export with exports waiting in the queue
    kStop,
};

// The sample rates that the window offers after the console's rate.
inline constexpr int kMixdownRates[] = {44100, 48000, 88200, 96000};

// The names that the window shows as an example of the naming scheme, for the first chosen song of `view` or else the
// first song, with the file type's extension: the whole mix, and the stem of track 0 with stems chosen. Two names have
// a comma between them.
std::string MixdownPreview(const MixdownView& view);

// The text of the queue's Format column for `setup`: "Wave, 48 kHz, 24-bit", or the MIDI target. A rate of 0 shows
// `console_rate`, or "console rate" when that's 0 too.
std::string MixdownFormatText(const MixdownSetup& setup, int console_rate);

// The text of the queue's Channels column for `setup`: "Mix", "Stems (Dry)", "Mix and Stems (With Effects)" and the
// like.
std::string MixdownChannelsText(const MixdownSetup& setup);

// The text of a song's length in the list, in minutes, seconds and tenths: "2:31.4".
std::string MixdownLengthText(double seconds);

// Draws the window while `open` is true. The window closes when the user clicks its close button. While the queue's
// exports run (MixdownView::running), the settings and the queue stay disabled, and Stop replaces Close.
MixdownChoice DrawMixdownWindow(MixdownView& view, bool* open);

} // namespace hokuto::app
