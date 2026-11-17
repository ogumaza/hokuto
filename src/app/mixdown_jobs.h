// SPDX-License-Identifier: MIT

// The exports of the Export Audio Mixdown window's queue (mixdown_window.h), away from the program's thread. An export
// renders each chosen song through the playback engine, offline, on a few threads (edit::WriteMixdownSound), or
// converts it to MIDI (edit::WriteBatchMidi). Each song plays as the file sets it: its effects, its mixing mode and the
// FX channels' volumes. The shown song keeps the tracks that mute and solo silence. The whole mix goes through the
// Stereo Out fader.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

#include "app/mixdown_window.h"
#include "edit/sound_exports.h"
#include "song/mixdown.h"

namespace hokuto::app
{

// What every export of the window reads: what plays the songs and the file, the mix, the shown song with its muted
// tracks, and the name of the project for the Project token.
struct MixdownSources
{
    edit::BatchSource source;
    song::MixdownMix mix;          // the FX channels' volumes and the Stereo Out fader's gain
    std::optional<uint32_t> shown; // the shown song, with
    uint32_t shown_mutes = 0;      // the tracks that mute and solo silence there
    std::string project;
};

// How far a queued export has got: the songs done and failed, and the files to write and written. The export's thread
// writes it, and the program's thread reads it each frame.
struct MixdownJobProgress
{
    std::atomic<std::size_t> done{0};
    std::atomic<std::size_t> failed{0};
    std::atomic<std::size_t> files{0};
    std::atomic<std::size_t> files_done{0};
    std::atomic<bool> running{false};
    std::mutex mutex;
    std::string status; // set once the export has run
};

// The threads of an export: half the processor's threads, from 1 to 4. A render of 10 minutes uses about 230 MB at
// 48 kHz.
std::size_t MixdownThreads();

// Runs `job` with `sources`, and writes its progress to `progress`. A song that fails is counted, and the others go on.
// A song that plays no notes counts as failed in a MIDI export. `stop` ends the export after the frame that each render
// is on. A song stopped after some of its files keeps them, and the status counts it apart. The export creates its
// folder. Its files get unique names unless the job overwrites: a MIDI file's name is free only while its SoundFont 2
// file, or its folder of SFZ files, is free too. The status names the songs that fade at 10 minutes and the songs whose
// tails were cut.
void RunMixdownJob(const MixdownSources& sources, const MixdownJob& job, MixdownJobProgress& progress,
                   const std::atomic<bool>& stop);

} // namespace hokuto::app
