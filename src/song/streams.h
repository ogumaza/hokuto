// SPDX-License-Identifier: MIT

// The streams of a sound archive of the 3DS, the Wii U or the Switch. A game keeps its streams in files beside the
// archive, at the paths that the archive's file table gives, such as stream/BGM_TITLE.bfstm, or BGM_TITLE.bcstm on the
// 3DS. A stream sound plays one of them. The archive's tables give the sound's tracks: the stream's channels that each
// one plays, with its volume and pan. Before version 2.3.0.0 of a 3DS archive, the stream file gives them. A Hokuto
// file keeps each stream whole (format::NativeStream). The browser plays a stream sound's stream, and exports it as its
// file, as a file of another console or as a WAV file.
//
// A 3DS game can keep a stream as an AAC file in ADTS, such as BGM_TITLE.aac (song/aac.h). The stream sound's entry
// marks the stream as AAC and gives its loop (AacStreamOf). The functions below take such a file as they take a stream
// file, with the loop that the entry gives.
//
// The Wii keeps its streams the same way, as RSTM files. The archive's file table gives each one's path, from the
// archive's folder, such as stream/BGM_TITLE.brstm. A path that starts with a slash gives the file from the disc's
// root. Hokuto reads and writes no such stream. The stream file gives a stream sound's tracks. The functions below that
// take the Wii's archive work as those that take the later consoles' archives.

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/sar/decode.h"
#include "ntdlib/sar/formats.h"

namespace hokuto::song
{

// The path that the file table of `archive` gives file `file` outside the archive, relative to the archive's folder.
// Nothing for a file inside the archive, and for a path that leads out of the folder: an absolute path, or one with a
// ".." part. Hokuto reads and writes no stream outside the folder.
std::optional<std::filesystem::path> StreamPath(const ntdlib::sar::SoundArchive& archive, uint32_t file);

// The paths beside the archive at which an export writes the streams of `files`, by their file IDs: the paths that the
// file table of `archive` gives them (StreamPath). Throws std::runtime_error with a message for the user for a file
// that the table doesn't name outside the archive, or names outside the archive's folder.
std::vector<std::filesystem::path> StreamPaths(const ntdlib::sar::SoundArchive& archive,
                                               std::span<const uint32_t> files);

// Writes `bytes` at `file`, with the folders that lead to it. Gives a message for the user when the file can't be
// written, and nothing otherwise.
std::optional<std::string> WriteStream(const std::filesystem::path& file, std::span<const uint8_t> bytes);

// The streams beside the archive at `path`, by their file IDs: each file that the file table of `archive` names outside
// the archive (StreamPath), that the archive's folder holds, and that starts as a stream file of the archive's console.
// On the 3DS, an AAC file in ADTS counts too.
std::map<uint32_t, std::vector<uint8_t>> ReadStreams(const ntdlib::sar::SoundArchive& archive,
                                                     const std::filesystem::path& path);

// The ID of the file that sound `sound` of `archive` plays, when the sound is a stream sound and the table names the
// file beside the archive, in its folder (StreamPath). Nothing otherwise.
std::optional<uint32_t> StreamFileOf(const ntdlib::sar::SoundArchive& archive, uint32_t sound);

// The name of the stream file of stream sound `sound`: the last part of its path, such as BGM_TITLE.bfstm, or the
// sound's name and the console's extension without a path: .bcstm on the 3DS and .bfstm on the others. The name suits a
// file on every system.
std::string StreamFileName(const ntdlib::sar::SoundArchive& archive, uint32_t sound);

// The same for the Wii's `archive`. A path from the disc's root gives nothing.
std::optional<std::filesystem::path> StreamPath(const ntdlib::rsar::SoundArchive& archive, uint32_t file);
std::vector<std::filesystem::path> StreamPaths(const ntdlib::rsar::SoundArchive& archive,
                                               std::span<const uint32_t> files);

// The streams beside the Wii's archive at `path`, by their file IDs: each file that the file table of `archive` names
// beside the archive (StreamPath), that the archive's folder holds, and that starts as a stream file of the Wii (RSTM).
std::map<uint32_t, std::vector<uint8_t>> ReadStreams(const ntdlib::rsar::SoundArchive& archive,
                                                     const std::filesystem::path& path);

// The ID of the file that sound `sound` of the Wii's `archive` plays. StreamFileOf gives the same for the later
// consoles.
std::optional<uint32_t> StreamFileOf(const ntdlib::rsar::SoundArchive& archive, uint32_t sound);

// The name of the stream file of the Wii's stream sound `sound`: the last part of its path, or the sound's name and
// .brstm.
std::string StreamFileName(const ntdlib::rsar::SoundArchive& archive, uint32_t sound);

// The loop of an AAC stream, in samples: from `start` up to `end`, the first sample that the stream doesn't play.
struct AacLoop
{
    uint32_t start = 0;
    uint32_t end = 0;
};

// A stream that a stream sound's entry marks as AAC, with the loop that the entry gives, or nothing for a stream
// without a loop.
struct AacStream
{
    std::optional<AacLoop> loop;
};

// Whether the entry of stream sound `sound` of the 3DS's `archive` marks its stream as AAC. The entry's extension holds
// the stream's type: 2 for AAC, with bit 8 for a loop. Nothing for another sound, and for an archive whose entries
// ntdlib's model can't read.
std::optional<AacStream> AacStreamOf(const ntdlib::sar::SoundArchive& archive, uint32_t sound);

// `stream`, a stream file, decoded as the console decodes it: every channel, from the start to the end, with the rate
// and the loop. A stream of the Wii decodes as NW4R SND plays it from its start (ntdlib::rsar::DecodeStream). An AAC
// file in ADTS decodes on the system's decoder (song::DecodeAdts). It takes the loop of `aac`, and then ends at the
// loop's end. Throws std::runtime_error with a message for the user when the file isn't a stream file or can't be
// decoded.
ntdlib::sar::Pcm DecodeStreamFile(std::span<const uint8_t> stream, const AacStream& aac = {});

// `stream`, a stream file or an AAC file, as a WAV file: 16-bit PCM with every channel, the rate, and the loop in a
// smpl chunk. Throws std::runtime_error with a message for the user when DecodeStreamFile does, and when a WAV file
// can't hold the stream.
std::vector<uint8_t> StreamAsWav(std::span<const uint8_t> stream, const AacStream& aac = {});

// `stream`, a stream file of the Wii, the 3DS, the Wii U or the Switch, as a file of the console of `to` in `version`.
// Its channels keep their samples (ntdlib::sar::ConvertStream, and ntdlib::rsar::StreamFileOf for the Wii's). A stream
// that leaves the 3DS drops the samples that the 3DS's tools encode past its end. An AAC file goes in DSP-ADPCM, with
// the samples and the loop that DecodeStreamFile gives. ExportVersions (song/items.h) gives the version that goes to
// each console. Throws std::runtime_error with a message for the user when the file isn't a stream file, or when ntdlib
// can't write it in that version.
std::vector<uint8_t> ConvertStreamFile(std::span<const uint8_t> stream, ntdlib::sar::Flavor to, uint32_t version,
                                       const AacStream& aac = {});

// `stream`, a stream file of the 3DS, the Wii U or the Switch, or an AAC file, as a stream file of the Wii (BRSTM): its
// encoding, its rate, its loop, its tracks and every channel (ntdlib::rsar::WiiStreamOf). An AAC file goes in
// DSP-ADPCM, with the samples and the loop that DecodeStreamFile gives (ntdlib::rsar::MakeStream). Throws
// std::runtime_error with a message for the user when the file isn't such a stream file, and when ntdlib can't convert
// it, such as a stream in IMA-ADPCM.
std::vector<uint8_t> WiiStreamFile(std::span<const uint8_t> stream, const AacStream& aac = {});

// `stream` as ntdlib's players of the 3DS take it: a stream file as it is, and an AAC file as a stream file of the 3DS
// in 16-bit PCM, with the samples and the loop that DecodeStreamFile gives. Throws std::runtime_error with a message
// for the user when DecodeStreamFile does.
std::vector<uint8_t> PlayableStream(std::span<const uint8_t> stream, const AacStream& aac = {});

} // namespace hokuto::song
