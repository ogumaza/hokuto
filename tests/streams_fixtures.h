// SPDX-License-Identifier: MIT

// This header contains the streams, archives and helpers that the tests of the streams share.

#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "ntdlib/cafe/snd/engine.h"
#include "ntdlib/nx/atk/engine.h"
#include "ntdlib/rsar/archive_model.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/stream_file.h"
#include "ntdlib/rvl/snd/engine.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/archive_model.h"
#include "ntdlib/sar/decode.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/stream_file.h"
#include "ntdlib/sound_build.h"
#include "ntdlib/testing/rsar_streams.h"
#include "ntdlib/testing/sar_files.h"
#include "ntdlib/testing/sar_tools.h"
#include "ntdlib/wave/decode.h"
#include "ntdlib/wave/encode.h"
#include "seqlib/wav.h"
#include "xsflib/files.h"
#include "xsflib/testing/check.h"

#include "edit/new_items.h"
#include "edit/streams.h"
#include "format/archive.h"
#include "format/file.h"
#include "format/model.h"
#include "format/text.h"
#include "song/aac.h"
#include "song/archive.h"
#include "song/items.h"
#include "song/preview.h"
#include "song/streams.h"
#include "temporary_files.h"

using hokuto::edit::IncomingStream;
using hokuto::edit::NewStream;
using hokuto::edit::StreamChanges;
using hokuto::format::File;
using hokuto::testing::TemporaryFolder;
using xsflib::testing::ErrorOf;

namespace
{

namespace sar = ntdlib::sar;
namespace sar_files = ntdlib::testing::sar_files;
namespace sar_tools = ntdlib::testing::sar_tools;

// The archive's files and sounds.
constexpr uint32_t kSongFile = 0;
constexpr uint32_t kBackslashFile = 1;
constexpr uint32_t kOutsideFile = 2;
constexpr uint32_t kStereoSound = 0;
constexpr uint32_t kMonoSound = 1;
constexpr uint32_t kBackslashSound = 2;
constexpr uint32_t kOutsideSound = 3;
constexpr uint32_t kWaveSound = 4; // in the archive of the tools' layout

// The streams' rate, and the samples in each channel.
constexpr uint32_t kRate = 32000;
constexpr std::size_t kLength = 40000;

// The versions of the streams: 4.0.0, and 3.0.0 for most of the Wii U's games. On the 3DS, 2.0.0.0 and 2.3.1.0.
constexpr uint32_t kVersion = 0x00040000;
constexpr uint32_t kOldVersion = 0x00030000;
constexpr uint32_t kCtrOldestVersion = 0x02000000;
constexpr uint32_t kCtrNewestVersion = 0x02030100;

// Two channels: a ramp, and a sine of 440 Hz.
inline std::vector<std::vector<int16_t>> Channels(std::size_t length = kLength)
{
    std::vector<std::vector<int16_t>> channels(2, std::vector<int16_t>(length));
    for (std::size_t i = 0; i < length; i++)
    {
        channels[0][i] = static_cast<int16_t>(static_cast<int>(i % 4000) * 8 - 16000);
        channels[1][i] =
            static_cast<int16_t>(std::lround(12000.0 * std::sin(2.0 * 3.14159265358979 * 440.0 * i / kRate)));
    }

    return channels;
}

// A stream of `flavor` and `version` that holds Channels(), with a loop from sample 1000.
inline std::vector<uint8_t> StreamOf(sar::Flavor flavor, sar::WaveEncoding encoding = sar::WaveEncoding::kDspAdpcm,
                                     uint32_t version = kVersion)
{
    return sar::MakeStream(flavor, version, encoding, kRate, Channels(), 1000).Write();
}

// A stream sound's details: the channels that each track plays, and each track's pan. Each track's volume is 100. The
// sound's pitch is 1.5 and its main send 110.
inline sar::StreamSoundInfo Details(std::vector<std::vector<uint8_t>> tracks, std::vector<uint8_t> pans)
{
    sar::StreamSoundInfo info;
    info.channel_count = 2;
    info.pitch = 1.5f;
    info.main_send = 110;
    for (std::size_t t = 0; t < tracks.size(); t++)
    {
        info.valid_tracks = static_cast<uint16_t>(info.valid_tracks | 1u << t);
        sar::StreamTrackInfo track;
        track.channels = std::move(tracks[t]);
        track.pan = pans[t];
        track.volume = 100;
        info.tracks.push_back(std::move(track));
    }

    return info;
}

// The archive of `flavor`, laid out as ntdlib's tests lay it out or, with `tools`, as NintendoWare's tools do. That one
// holds a wave sound after the stream sounds.
inline std::vector<uint8_t> ArchiveOf(sar::Flavor flavor, bool tools = false)
{
    sar_files::ArchiveSpec spec;
    spec.files = {{{}, "stream/STRM_SONG.bfstm"}, {{}, "stream\\STRM_BACK.bfstm"}, {{}, "../STRM_OUT.bfstm"}};
    spec.players = {{"PLAYER", 1}};
    spec.sounds = {{.name = "STRM_SONG",
                    .kind = sar_files::SoundSpec::kStream,
                    .file = kSongFile,
                    .stream = Details({{0, 1}}, {64})},
                   {.name = "STRM_SONG_MONO",
                    .kind = sar_files::SoundSpec::kStream,
                    .file = kSongFile,
                    .stream = Details({{0}, {1}}, {0, 127})},
                   {.name = "STRM_BACK",
                    .kind = sar_files::SoundSpec::kStream,
                    .file = kBackslashFile,
                    .stream = Details({{0, 1}}, {64})},
                   {.name = "STRM_OUT",
                    .kind = sar_files::SoundSpec::kStream,
                    .file = kOutsideFile,
                    .stream = Details({{0, 1}}, {64})}};
    if (!tools)
    {
        return sar_files::Archive(flavor, spec);
    }

    spec.files.push_back({sar_files::WaveSoundFile(flavor, {}), "", false});
    spec.sounds.push_back({.name = "WSD_A", .kind = sar_files::SoundSpec::kWaveSound, .file = 3, .volume = 70});

    return sar_tools::Archive(flavor, spec);
}

// A WAV file of `count` channels: Channels() over and over.
inline std::vector<uint8_t> WavOf(std::size_t count)
{
    seqlib::Pcm pcm;
    const std::vector<std::vector<int16_t>> channels = Channels(20000);
    for (std::size_t c = 0; c < count; c++)
    {
        pcm.channels.push_back(channels[c % 2]);
    }

    return seqlib::WavFile(pcm, kRate);
}

// The first `frames` stereo frames of a preview, or fewer at its end, in blocks of 256.
inline std::vector<float> RenderPreview(hokuto::audio::Renderer& preview, std::size_t frames)
{
    constexpr std::size_t kBlock = 256;
    std::vector<float> samples(frames * 2);
    std::size_t done = 0;
    while (done < frames)
    {
        const std::size_t rendered = preview.Render(samples.data() + done * 2, std::min(kBlock, frames - done));
        done += rendered;
        if (rendered == 0)
        {
            break;
        }
    }
    samples.resize(done * 2);

    return samples;
}

} // namespace
