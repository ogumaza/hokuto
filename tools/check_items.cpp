// SPDX-License-Identifier: MIT

// Checks the exports of single items (src/song/items.h) against the archive that they come from. The tool opens a sound
// archive of the 3DS, the Wii U or the Switch twice:
//
// - as the program opens it, with the group files beside it, and
// - as the program imports it into a Hokuto file. The file then builds the archive again, and the exports read that.
//
// Each item exports from the built archive as the browser's Export… exports it: each sequence sound's sequence file,
// each wave sound file under the name of its sound group, each bank, each wave archive and each wave of each wave
// archive. The browser lists the same wave sound files, with the same wave sounds, for both archives. Each file has to
// hold the bytes that the opened archive gives for the item. A wave's file has to start with its format's signature,
// and its header has to give the file's length. Each wave also goes out as a WAV file. The tool reads the WAV file back
// by itself and compares it with ntdlib's decoding of the wave in the opened archive: the channels, the rate, every
// sample and the loop. A wave that both sides fail to decode counts apart, and so does an item whose file the archive
// lacks. Each wave sound goes out as a WAV file too. The tool reads the opened archive's wave sound file with ntdlib,
// finds the wave of the sound's first note, and compares the WAV file with ntdlib's decoding of that wave at the rate
// that the sound's pitch and the note's give. A wave sound that lacks its file, its note or its wave on both sides
// counts apart.
//
// Each bank's instruments also go out as SoundFont 2 files and as SFZ files. They have to be the files that ntdlib's
// converter makes of the bank in the opened archive, with the settings of the archive's console. Each SoundFont has to
// pass seqlib's check. Together, the SoundFonts have to hold a preset for each program that holds an instrument, in
// order, at bank p / 128 and program p % 128. A bank past the format's limits takes several SoundFonts, each with a run
// of programs. The SFZ files have to pass ntdlib's check of a conversion's SFZ files: each sample that they play is
// among the conversion's samples, and each loop lies inside its sample.
//
// Each sequence file also goes out as a file of each other console among the 3DS, the Wii U and the Switch. ntdlib's
// reader of that console has to read it, and that console's player has to reach the commands that the opened archive's
// file holds, at the same offsets. The biquad filters can take other types and values there (song::MapBiquads), and the
// tool counts the files whose filters do. Converted back, the file has to hold the same labels, and the player has to
// reach the same commands again, but for the biquad filters. The tool counts the files that come back with other biquad
// filters, those that come back with another version, and those that come back with other bytes. Otherwise, only bytes
// that no command reaches can differ. The decoder reads what it can of them as commands. The byte order changes what it
// can read there. The 3DS lacks a few commands of the Wii U and the Switch. A file that reaches one has to fail for the
// 3DS, and the tool counts it apart.
//
// Each bank, wave sound file and wave archive also goes out as a file of each other console, with the versions of
// Export…'s files for that console. ntdlib's reader of that console has to read the same values from it: a bank's waves
// and regions, a wave sound file's waves and wave sounds, and each wave's samples, rate, loop and encoding. A wave that
// goes out alone has to give the bytes that the converted wave archive holds for it. Converted back with the versions
// of the archive's files, a bank file and a wave sound file have to give the archive's bytes. So does each wave archive
// whose waves come back whole. A wave can come back with another original loop start: the other console's version may
// hold none. A wave can also come back without the bytes that follow its last samples. The tool counts those apart, and
// checks that nothing else differs.
//
// The import also reads the streams beside the archive, at the paths of its file table. Each stream sound's stream goes
// out once for each file: as the file, as the file of the other console, and as a WAV file. The tool reads the file
// beside the archive by itself. The export has to give its bytes. ntdlib's reader of the other console has to read the
// other console's file in the version that goes to that console (song::ExportVersions), and its decoding has to give
// the same samples. Converted back in the stream's version, the file has to give the bytes beside the archive. A stream
// that leaves the 3DS drops the samples past its end, and an older version holds no original loop. A stream that goes
// to the 3DS or leaves it therefore has to give the same samples instead. Converted back in the version that goes to
// the stream's console, the file has to give the same samples. The WAV file has to hold ntdlib's decoding of the
// stream: every channel, the rate, every sample and the loop. A stream that the folder lacks counts apart. Hokuto's
// export has to fail for it. An AAC stream of the 3DS counts apart too. The system's decoder decodes it, with the loop
// that ntdlib's model reads from the sound's entry. The WAV file has to hold those samples within 2 steps, and the
// loop. The decoder can move a sample by a step from one decoding to the next. Each other console's file has to hold
// DSP-ADPCM at the stream's rate, with the loop and the length. The loop may move forward to a block, and the samples
// up to there then follow its end. Up to the loop's end, the error of the file's decoding has to lie 15 dB or more
// below the samples.
//
// An archive of the Wii takes a check of its own. As in an import, the Hokuto file takes the game's build of NW4R SND
// from the main.dol of the extracted disc that holds the archive. Each item exports from the built archive as the
// browser exports it: each sequence file and each wave sound file once, each bank, and each wave archive that is an
// RWAR, with each of its waves (song::WaveArchivesOf). Each file has to hold the bytes that ntdlib alone gives for the
// item in the opened archive, and a wave's file has to give its length. Wave data that a WAVE block of a bank or a wave
// sound file describes goes out as an RWAR of its waves, and each of its waves as an RWAV file (song::ItemBytes). The
// RWAR has to hold the RWAV files. Each RWAV file has to hold the encoding of the WAVE block's wave in the opened
// archive, and decode to ntdlib's decoding of that wave. The browser lists the same wave sound files and wave archives
// for both archives. Each wave of each wave archive goes out as a WAV file, those that the WAVE block of a bank or a
// wave sound file describes included. The tool compares it with ntdlib's decoding of the wave in the opened archive.
// Each wave sound goes out as a WAV file too. The tool reads the sound's file in the opened archive, with its wave
// data, and finds the wave of the sound's first note. The WAV file has to hold ntdlib's decoding of that wave at the
// rate that the sound's pitch gives: NW4R SND leaves the note's pitch unread. Each bank's instruments go out as
// SoundFont 2 and SFZ files. BankProblem checks them. The reference converts the bank of the opened archive with the
// game's build, and with the pan mode of the first sequence sound that plays the bank. Each program also plays a note
// as the browser plays it (song::NotePreview), on the built archive. Its output has to be the output of ntdlib's model
// of NW4R SND and AX run directly on the opened archive with the same note and the game's build, up to the preview's
// last frame. A preview that reaches its limit of 10 s fades out in that frame. The tool counts the notes that stay
// silent, and those that reach the limit. The tool leaves out the streams beside a Wii archive, and the Wii's items as
// files of the later consoles. It checks no item of a later console as a file of the Wii either.
// tests/other_console_test.cpp checks those conversions.
//
// The tool prints a line for each item that differs, a line for each warning of the converter but the attenuation that
// it counts, and a summary. It returns 1 when an item differs, and 2 when the archive can't be read.
//
// usage: hokuto_check_items <archive>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/nwsnd/commands.h"
#include "ntdlib/rsar/decode.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rvl/snd/engine.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/archive_model.h"
#include "ntdlib/sar/decode.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/sequence_file.h"
#include "ntdlib/sar/stream_file.h"
#include "ntdlib/sar/wave_file.h"
#include "ntdlib/sequence_export/consoles.h"
#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/sequence_export/sfz.h"
#include "ntdlib/testing/sequence_export_check.h"
#include "seqlib/sf2/reader.h"
#include "xsflib/ascii.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/console.h"
#include "xsflib/files.h"

#include "format/archive.h"
#include "format/file.h"
#include "song/aac.h"
#include "song/archive.h"
#include "song/biquads.h"
#include "song/items.h"
#include "song/midi_export.h"
#include "song/preview.h"
#include "song/streams.h"

namespace
{

namespace export_ = ntdlib::sequence_export;
namespace rsar = ntdlib::rsar;
namespace sar = ntdlib::sar;
namespace song = hokuto::song;

// A WAV file as the tool reads it: the format, the samples by channel, and the smpl chunk's first loop.
struct WavRead
{
    uint16_t channels = 0;
    uint32_t rate = 0;
    std::vector<std::vector<int16_t>> samples;
    bool loop = false;
    uint32_t loop_start = 0;
    uint32_t loop_last = 0; // the last sample that the loop plays
};

// The little-endian 16-bit number at `at` of `bytes`.
uint16_t Le16(std::span<const uint8_t> bytes, std::size_t at)
{
    return static_cast<uint16_t>(bytes[at] | bytes[at + 1] << 8);
}

// The little-endian 32-bit number at `at` of `bytes`.
uint32_t Le32(std::span<const uint8_t> bytes, std::size_t at)
{
    return static_cast<uint32_t>(Le16(bytes, at)) | static_cast<uint32_t>(Le16(bytes, at + 2)) << 16;
}

// Reads a 16-bit PCM WAV file by the RIFF specification: a fmt chunk of PCM or of WAVE_FORMAT_EXTENSIBLE, a data chunk
// and an optional smpl chunk. Returns nothing for a file that breaks a rule.
std::optional<WavRead> ReadWav(std::span<const uint8_t> file)
{
    if (file.size() < 12 || std::string(file.begin(), file.begin() + 4) != "RIFF" ||
        std::string(file.begin() + 8, file.begin() + 12) != "WAVE" || Le32(file, 4) != file.size() - 8)
    {
        return std::nullopt;
    }

    // The chunks, each padded to an even size.
    WavRead wav;
    std::span<const uint8_t> data;
    bool format = false;
    for (std::size_t at = 12; at + 8 <= file.size();)
    {
        const std::string id(file.begin() + static_cast<std::ptrdiff_t>(at),
                             file.begin() + static_cast<std::ptrdiff_t>(at + 4));
        const uint32_t size = Le32(file, at + 4);
        if (at + 8 + size > file.size())
        {
            return std::nullopt;
        }
        const std::span<const uint8_t> body = file.subspan(at + 8, size);
        if (id == "fmt ")
        {
            const uint16_t tag = Le16(body, 0);
            const bool pcm = tag == 1 && size == 16;
            const bool extensible = tag == 0xFFFE && size == 40 && Le16(body, 16) == 22 && Le16(body, 18) == 16 &&
                                    Le16(body, 24) == 1 && Le16(body, 26) == 0;
            wav.channels = Le16(body, 2);
            wav.rate = Le32(body, 4);
            if ((!pcm && !extensible) || extensible != (wav.channels > 2) || Le16(body, 14) != 16 ||
                Le16(body, 12) != wav.channels * 2 || Le32(body, 8) != wav.rate * wav.channels * 2)
            {
                return std::nullopt;
            }
            format = true;
        }
        else if (id == "data")
        {
            data = body;
        }
        else if (id == "smpl")
        {
            if (size != 60 || Le32(body, 28) != 1 || Le32(body, 40) != 0 || Le32(body, 56) != 0)
            {
                return std::nullopt;
            }
            wav.loop = true;
            wav.loop_start = Le32(body, 44);
            wav.loop_last = Le32(body, 48);
        }
        at += 8 + size + (size & 1);
    }
    if (!format || wav.channels == 0 || data.size() % (wav.channels * 2u) != 0)
    {
        return std::nullopt;
    }

    // The samples, interleaved.
    const std::size_t frames = data.size() / (wav.channels * 2u);
    wav.samples.assign(wav.channels, std::vector<int16_t>(frames));
    for (std::size_t frame = 0; frame < frames; frame++)
    {
        for (std::size_t channel = 0; channel < wav.channels; channel++)
        {
            wav.samples[channel][frame] = static_cast<int16_t>(Le16(data, (frame * wav.channels + channel) * 2));
        }
    }

    return wav;
}

// Whether two decodings hold the same samples, at the same rate, with the same loop.
bool SamePcm(const sar::Pcm& a, const sar::Pcm& b)
{
    return a.sample_rate == b.sample_rate && a.loop == b.loop && a.loop_start == b.loop_start &&
           a.loop_end == b.loop_end && a.channels == b.channels;
}

// Whether `wav` holds `pcm` at its rate, with its loop, each sample within `tolerance` steps.
bool Holds(const WavRead& wav, const sar::Pcm& pcm, int tolerance = 0)
{
    if (wav.channels != pcm.channels.size() || wav.rate != pcm.sample_rate ||
        wav.samples.size() != pcm.channels.size() || wav.loop != pcm.loop)
    {
        return false;
    }

    for (std::size_t channel = 0; channel < pcm.channels.size(); channel++)
    {
        const std::vector<int16_t>& ours = wav.samples[channel];
        const std::vector<int16_t>& theirs = pcm.channels[channel];
        if (ours.size() != theirs.size() ||
            !std::equal(ours.begin(), ours.end(), theirs.begin(),
                        [tolerance](int16_t a, int16_t b) { return std::abs(int{a} - int{b}) <= tolerance; }))
        {
            return false;
        }
    }

    return !pcm.loop || (wav.loop_start == pcm.loop_start && wav.loop_last + 1 == pcm.loop_end);
}

// What the check counted.
struct Totals
{
    std::size_t items = 0;
    std::size_t waves = 0;
    std::size_t undecoded = 0;
    std::size_t wide = 0;
    std::size_t clashes = 0;
    std::size_t missing = 0;
    std::size_t differ = 0;
    std::size_t banks = 0;
    std::size_t instruments = 0;
    std::size_t empty_banks = 0;
    std::size_t unread_banks = 0;
    std::size_t split_banks = 0;
    std::size_t louder_banks = 0;
    std::size_t bank_warnings = 0;
    std::size_t converted = 0;
    std::size_t refused = 0;      // sequence files that reach a command that the 3DS lacks
    std::size_t overlapping = 0;  // sequence files whose commands overlap: no other console takes them
    std::size_t biquads = 0;      // sequence files whose biquad filters take other types or values there
    std::size_t biquads_back = 0; // sequence files whose biquad filters come back otherwise
    std::size_t versions = 0;
    std::size_t unreached = 0;
    std::size_t converted_files = 0; // banks, wave sound files and wave archives for the other consoles
    std::size_t converted_waves = 0;
    std::size_t file_versions = 0; // files that take another version for another console
    std::size_t wave_versions = 0;
    std::size_t loop_starts = 0; // waves that come back with another original loop start
    std::size_t trimmed = 0;     // waves that come back without the bytes after their last sample
    std::size_t wave_sounds = 0;
    std::size_t pitched = 0;
    std::size_t silent = 0;
    std::size_t streams = 0;
    std::size_t stream_channels = 0;
    std::size_t stream_conversions = 0; // a stream's conversions to another console and back
    std::size_t aac_streams = 0;
    std::size_t lacked_streams = 0;
    std::size_t wave_sound_files = 0;
    std::size_t named_files = 0; // wave sound files under the names of their sound groups
    std::size_t notes = 0;       // the Wii's notes as the browser plays them
    std::size_t silent_notes = 0;
    std::size_t endless_notes = 0; // notes that reach the preview's limit
};

// The stream file that the file table of `archive` names for file `file`, read by the tool from the folder of the
// archive at `path`: the table's path with slashes, inside the folder. Nothing when the table names no such path, and
// when the folder lacks the file.
std::optional<std::vector<uint8_t>> StreamBeside(const sar::SoundArchive& archive, uint32_t file,
                                                 const std::filesystem::path& path)
{
    std::string text = archive.ExternalPath(file);
    std::replace(text.begin(), text.end(), '\\', '/');
    if (text.empty() || text.starts_with('/') || text.find("..") != std::string::npos)
    {
        return std::nullopt;
    }
    std::vector<uint8_t> bytes;
    if (!xsflib::ReadFile(path.parent_path() / xsflib::PathFromUtf8(text), bytes))
    {
        return std::nullopt;
    }

    return bytes;
}

// How far the system's decoder of AAC can move a sample between two decodings: Audio Toolbox's samples moved by a step.
constexpr int kAacSteps = 2;

// What is wrong with the exports of the AAC stream `ours` of stream sound `sound`, or nothing. `model` is ntdlib's
// model of the archive.
std::string AacStreamProblem(const std::vector<uint8_t>& ours, const sar::ArchiveModel& model, uint32_t sound,
                             Totals& totals)
{
    // The entry's loop. The stream ends at the loop's end.
    const std::optional<sar::StreamExtension>& extension = model.sounds.at(sound).stream_extension;
    if (!extension || (extension->stream_type & 0xFF) != 2)
    {
        return "'s entry doesn't mark an AAC stream";
    }
    sar::Pcm decoded = song::DecodeAdts(ours);
    song::AacStream aac;
    if ((extension->stream_type & 0x100) != 0 && extension->loop_start < extension->loop_end &&
        extension->loop_start < decoded.loop_end)
    {
        aac.loop = song::AacLoop{extension->loop_start, extension->loop_end};
        decoded.loop = true;
        decoded.loop_start = extension->loop_start;
        decoded.loop_end = std::min(extension->loop_end, decoded.loop_end);
        for (std::vector<int16_t>& channel : decoded.channels)
        {
            channel.resize(decoded.loop_end);
        }
    }

    const std::optional<WavRead> wav = ReadWav(song::StreamAsWav(ours, aac));
    if (!wav || !Holds(*wav, decoded, kAacSteps))
    {
        return "'s WAV file differs";
    }
    for (const sar::Flavor other_console : song::OtherConsoles(sar::Flavor::kCtr))
    {
        const std::string console = song::ConsoleName(other_console);
        const uint32_t version = song::ExportVersions(song::ItemKind::kStream, other_console).file;
        const sar::StreamFile converted =
            sar::StreamFile::Read(song::ConvertStreamFile(ours, other_console, version, aac));
        const sar::Pcm samples = sar::DecodeStream(converted);

        // The loop moves forward to the first block at or after its start, and the samples up to there follow its end
        // (ntdlib's MakeStream).
        const uint32_t moved = samples.loop_start - std::min(samples.loop_start, decoded.loop_start);
        const bool same_loop = samples.loop == decoded.loop && samples.loop_start >= decoded.loop_start &&
                               moved < sar::StreamBlockSamples(sar::WaveEncoding::kDspAdpcm) &&
                               samples.loop_end == decoded.loop_end + moved;

        double error = 0.0;
        double signal = 0.0;
        for (std::size_t channel = 0; channel < decoded.channels.size() && channel < samples.channels.size(); channel++)
        {
            for (uint32_t i = 0; i < decoded.loop_end && i < samples.channels[channel].size(); i++)
            {
                const double expected = decoded.channels[channel][i];
                const double difference = samples.channels[channel][i] - expected;
                error += difference * difference;
                signal += expected * expected;
            }
        }
        if (converted.flavor != other_console || converted.version != version ||
            converted.encoding != sar::WaveEncoding::kDspAdpcm || converted.sample_rate != decoded.sample_rate ||
            !same_loop || samples.channels.size() != decoded.channels.size() ||
            (signal > 0.0 && 10.0 * std::log10(error / signal) > -15.0))
        {
            return "'s file for the " + console + " differs";
        }
        totals.stream_conversions++;
    }

    totals.stream_channels += decoded.channels.size();
    totals.aac_streams++;

    return {};
}

// What is wrong with the exports of the stream `ours` that the Hokuto file holds, or nothing. `theirs` holds the file
// beside the archive.
std::string StreamProblem(const std::vector<uint8_t>& ours, const std::vector<uint8_t>& theirs, Totals& totals)
{
    if (ours != theirs)
    {
        return "'s file differs";
    }

    // The file of each other console, and back. Between the Wii U and the Switch, the stream comes back with its bytes.
    // A stream that leaves the 3DS drops the samples past its end, and an older version holds no original loop: a
    // stream that goes to the 3DS or leaves it comes back with its samples.
    const sar::StreamFile read = sar::StreamFile::Read(theirs);
    const sar::Pcm decoded = sar::DecodeStream(read);
    for (const sar::Flavor other_console : song::OtherConsoles(read.flavor))
    {
        const std::string console = song::ConsoleName(other_console);
        const uint32_t other_version = song::ExportVersions(song::ItemKind::kStream, other_console).file;
        const std::vector<uint8_t> other = song::ConvertStreamFile(ours, other_console, other_version);
        const sar::StreamFile converted = sar::StreamFile::Read(other);
        if (converted.flavor != other_console || converted.version != other_version ||
            sar::DecodeStream(converted).channels != decoded.channels)
        {
            return "'s file for the " + console + " differs";
        }
        const std::vector<uint8_t> again = song::ConvertStreamFile(other, read.flavor, read.version);
        const bool ctr = read.flavor == sar::Flavor::kCtr || other_console == sar::Flavor::kCtr;
        if (ctr ? sar::DecodeStream(sar::StreamFile::Read(again)).channels != decoded.channels : again != theirs)
        {
            return " comes back from the " + console + (ctr ? " with other samples" : " with other bytes");
        }
        const uint32_t version = song::ExportVersions(song::ItemKind::kStream, read.flavor).file;
        const sar::StreamFile back = sar::StreamFile::Read(song::ConvertStreamFile(other, read.flavor, version));
        if (back.version != version || sar::DecodeStream(back).channels != decoded.channels)
        {
            return " comes back from the " + console + " with other samples in the version that goes to its console";
        }
        totals.stream_conversions++;
    }

    // The WAV file.
    const std::optional<WavRead> wav = ReadWav(song::StreamAsWav(ours));
    if (!wav || !Holds(*wav, decoded))
    {
        return "'s WAV file differs";
    }
    totals.stream_channels += decoded.channels.size();

    return {};
}

// The wave that wave sound `sound` of `archive` plays. The tool reads it with ntdlib alone: the wave of the sound's
// note 0, decoded, at the wave's rate times the sound's pitch and the note's, rounded to whole hertz. The consoles play
// note 0, and read none of the note events of the sound's tracks. Nothing when the archive lacks the sound's file, its
// note or its wave, and when one of them can't be read.
std::optional<sar::Pcm> WaveSoundPcm(const sar::SoundArchive& archive, const sar::SoundInfo& sound,
                                     const sar::Options& options)
{
    try
    {
        const sar::WaveSoundFile file = sar::WaveSoundFile::Parse(archive.FileData(sound.file_id), options);
        const sar::WaveSoundFile::WaveSound& wave_sound = file.sounds.at(sound.wave_sound_index);
        const sar::VelocityRegion& note = wave_sound.notes.at(0);
        const sar::WaveId& wave = file.waves.at(note.wave_id_index);
        const std::optional<std::size_t> wave_archive =
            sar::ItemIndex(wave.wave_archive_item, sar::kWaveArchiveItem, archive.WaveArchives().size());
        if (!wave_archive)
        {
            return std::nullopt;
        }
        const std::vector<std::span<const uint8_t>> waves =
            sar::WaveArchive::Parse(archive.FileData(archive.WaveArchives()[*wave_archive].file_id), options).waves;
        sar::Pcm pcm = sar::DecodeWave(waves.at(wave.index), options);
        const float pitch = wave_sound.pitch * note.pitch;
        pcm.sample_rate =
            static_cast<uint32_t>(std::lround(static_cast<double>(pcm.sample_rate) * static_cast<double>(pitch)));

        return pcm;
    }
    catch (const std::exception&)
    {
        // The archive lacks the file, the note or the wave, or one of them can't be read.
    }

    return std::nullopt;
}

// The wave that the Wii's wave sound `sound` of `archive` plays. The tool reads it with ntdlib alone: the wave of the
// sound's first note, among the waves of its file's wave data, decoded, at the wave's rate times the sound's pitch,
// rounded to whole hertz. Nothing when the archive lacks the sound's file, its note or its wave, and when one of them
// can't be read.
std::optional<rsar::Pcm> WiiWaveSoundPcm(const rsar::SoundArchive& archive, const rsar::SoundInfo& sound)
{
    try
    {
        const rsar::WaveSoundFile file = rsar::WaveSoundFile::Parse(
            archive.FileData(sound.file_id), archive.WaveData(sound.file_id), ntdlib::rvl::snd::kRsarOptions);
        const rsar::WaveSoundFile::WaveSound& wave_sound = file.Sounds().at(static_cast<std::size_t>(sound.sub_index));
        const int32_t wave = wave_sound.notes.at(0).wave_index;
        rsar::Pcm pcm = rsar::DecodeWave(file.Waves().at(static_cast<std::size_t>(wave)));
        pcm.sample_rate = static_cast<uint32_t>(
            std::lround(static_cast<double>(pcm.sample_rate) * static_cast<double>(wave_sound.pitch)));

        return pcm;
    }
    catch (const std::exception&)
    {
        // The archive lacks the file, the note or the wave, or one of them can't be read.
    }

    return std::nullopt;
}

// What is wrong with the export of a bank's instruments, or nothing: Hokuto's files against those that ntdlib's
// converter makes of the opened archive, the SoundFont by seqlib's check and its presets, and the SFZ files by ntdlib's
// check of a conversion's SFZ files.
std::string BankProblem(const export_::BankConversion& ours, const export_::BankConversion& theirs,
                        const std::vector<uint32_t>& programs)
{
    bool same = ours.warnings == theirs.warnings && ours.sound_fonts.size() == theirs.sound_fonts.size() &&
                ours.sfz.files.size() == theirs.sfz.files.size() &&
                ours.sfz.samples.size() == theirs.sfz.samples.size();
    for (std::size_t i = 0; same && i < ours.sound_fonts.size(); i++)
    {
        same = ours.sound_fonts[i].sf2 == theirs.sound_fonts[i].sf2;
    }
    for (std::size_t i = 0; same && i < ours.sfz.files.size(); i++)
    {
        same = ours.sfz.files[i].name == theirs.sfz.files[i].name && ours.sfz.files[i].text == theirs.sfz.files[i].text;
    }
    for (std::size_t i = 0; same && i < ours.sfz.samples.size(); i++)
    {
        same = ours.sfz.samples[i].name == theirs.sfz.samples[i].name &&
               ours.sfz.samples[i].wav == theirs.sfz.samples[i].wav;
    }
    if (!same)
    {
        return "differs from the converter's files";
    }

    // The SoundFonts hold the programs in order, each SoundFont a run from its first program to its last. Program p's
    // preset sits at bank p / 128, program p % 128. The last record ends the list.
    if (ours.sfz.files.size() != programs.size())
    {
        return "has " + std::to_string(ours.sfz.files.size()) + " SFZ files for " + std::to_string(programs.size()) +
               " instruments";
    }
    std::size_t next = 0;
    for (const export_::BankSoundFont& sound_font : ours.sound_fonts)
    {
        const std::vector<std::string> problems = seqlib::sf2::Check(sound_font.sf2);
        if (!problems.empty())
        {
            return "has a broken SoundFont: " + problems[0];
        }

        const seqlib::sf2::Records records = seqlib::sf2::Read(sound_font.sf2);
        const std::size_t presets = records.Count("phdr", 38) - 1;
        if (presets == 0 || next + presets > programs.size() || programs[next] != sound_font.first ||
            programs[next + presets - 1] != sound_font.last)
        {
            return "has a SoundFont of programs " + std::to_string(sound_font.first) + " to " +
                   std::to_string(sound_font.last) + " with " + std::to_string(presets) + " presets";
        }
        for (std::size_t i = 0; i < presets; i++)
        {
            const uint32_t program = programs[next + i];
            const uint8_t* preset = records.Record("phdr", 38, i);
            if (Le16({preset + 20, 2}, 0) != program % 128 || Le16({preset + 22, 2}, 0) != program / 128)
            {
                return "puts program " + std::to_string(program) + " in the wrong place";
            }
        }
        next += presets;
    }
    if (next != programs.size())
    {
        return "leaves " + std::to_string(programs.size() - next) + " instruments out of its SoundFonts";
    }

    // ntdlib's check pairs each SFZ file with a MIDI track of the same name. The first name is the conductor track's.
    std::vector<std::string> names = {""};
    for (const export_::SfzFile& file : ours.sfz.files)
    {
        names.push_back(file.name.substr(0, file.name.size() - 4));
    }
    const std::string sfz = ntdlib::testing::SfzProblem(ours.sfz, names);
    return sfz.empty() ? "" : "has broken SFZ files: " + sfz;
}

// A command that a player reaches, at its byte offset. The command gives its target as an offset too.
struct Reached
{
    friend bool operator==(const Reached&, const Reached&) = default;

    uint32_t offset = 0;
    ntdlib::commands::Command command;
};

// The commands of sequence file `file` of the console of `flavor` that its player can reach from the labels and from
// `starts`, in the order of their bytes. The player goes on after each command but an end, a return and a jump that
// aren't conditional. A jump, a call and a track start reach their targets too. The decoder reads the bytes that no
// command reaches as commands where it can. The byte order can change what it reads there. The list leaves those bytes
// out.
std::vector<Reached> ReachedCommands(std::span<const uint8_t> file, sar::Flavor flavor,
                                     std::span<const uint32_t> starts)
{
    using ntdlib::commands::Kind;
    const sar::SequenceFile read = sar::SequenceFile::Read(file);
    std::vector<ntdlib::nwsnd::LabelOffset> labels;
    for (const sar::Sequence::Label& label : read.labels)
    {
        labels.push_back({label.name, label.offset});
    }
    const ntdlib::nwsnd::Grammar grammar = song::GrammarOf(flavor);
    const ntdlib::nwsnd::Decoded decoded = ntdlib::nwsnd::DecodeSequence(read.data, labels, grammar, starts);
    const std::vector<ntdlib::commands::Command>& items = decoded.sequence.items;
    const std::vector<uint32_t> offsets = ntdlib::nwsnd::EncodeSequence(decoded.sequence, grammar).offsets;

    // The walk from the labels and the starts.
    std::vector<bool> reached(items.size(), false);
    std::vector<uint32_t> next(decoded.entries.begin(), decoded.entries.end());
    for (const ntdlib::commands::Label& label : decoded.sequence.labels)
    {
        next.push_back(label.item);
    }
    while (!next.empty())
    {
        const uint32_t item = next.back();
        next.pop_back();
        if (item >= items.size() || reached[item])
        {
            continue;
        }

        reached[item] = true;
        const ntdlib::commands::Command& command = items[item];
        if (ntdlib::commands::HasTarget(command.kind) && !command.target_outside)
        {
            next.push_back(command.target);
        }
        const bool stops = command.kind == Kind::kEnd || command.kind == Kind::kReturn || command.kind == Kind::kJump;
        if (!stops || command.conditional)
        {
            next.push_back(item + 1);
        }
    }

    std::vector<Reached> commands;
    for (std::size_t i = 0; i < items.size(); i++)
    {
        if (!reached[i])
        {
            continue;
        }

        Reached command{offsets[i], items[i]};
        if (ntdlib::commands::HasTarget(command.command.kind) && !command.command.target_outside)
        {
            command.command.target = offsets[command.command.target];
        }
        commands.push_back(command);
    }

    return commands;
}

// Whether two sequence files hold the same labels at the same offsets.
bool SameLabels(std::span<const uint8_t> a, std::span<const uint8_t> b)
{
    const std::vector<sar::Sequence::Label> first = sar::SequenceFile::Read(a).labels;
    const std::vector<sar::Sequence::Label> second = sar::SequenceFile::Read(b).labels;
    const auto same = [](const sar::Sequence::Label& x, const sar::Sequence::Label& y)
    {
        return x.name == y.name && x.offset == y.offset;
    };

    return std::equal(first.begin(), first.end(), second.begin(), second.end(), same);
}

// Whether `a` and `b` hold the same commands at the same offsets, but for the types and values of biquad filters
// (song::SameButBiquads).
bool SameButBiquads(std::span<const Reached> a, std::span<const Reached> b)
{
    const auto same = [](const Reached& x, const Reached& y)
    {
        return x.offset == y.offset && song::SameButBiquads(x.command, y.command);
    };

    return std::ranges::equal(a, b, same);
}

// Whether the commands of `file`, a sequence file of the console of `flavor`, overlap or jump into one another as its
// player reads them from its labels and `starts`. ConvertSequenceFile refuses such a file.
bool Overlapping(std::span<const uint8_t> file, sar::Flavor flavor, std::span<const uint32_t> starts)
{
    const sar::SequenceFile read = sar::SequenceFile::Read(file);
    std::vector<ntdlib::nwsnd::LabelOffset> labels;
    for (const sar::Sequence::Label& label : read.labels)
    {
        labels.push_back({label.name, label.offset});
    }

    return ntdlib::nwsnd::DecodeSequence(read.data, labels, song::GrammarOf(flavor), starts).opaque;
}

// Whether the commands of `file`, a sequence file of the console of `flavor`, that its player can reach from `starts`
// include one that the 3DS lacks: each such command alone fails the 3DS's encoder. Commands with targets leave out.
bool ReachesWhatTheCtrLacks(std::span<const uint8_t> file, sar::Flavor flavor, std::span<const uint32_t> starts)
{
    const ntdlib::nwsnd::Grammar grammar = song::GrammarOf(sar::Flavor::kCtr);
    for (const Reached& reached : ReachedCommands(file, flavor, starts))
    {
        if (ntdlib::commands::HasTarget(reached.command.kind))
        {
            continue;
        }

        ntdlib::commands::Sequence alone;
        alone.items = {reached.command};
        try
        {
            ntdlib::nwsnd::EncodeSequence(alone, grammar);
        }
        catch (const std::invalid_argument&)
        {
            return true;
        }
    }

    return false;
}

// What is wrong with the file of sequence sound `sound` for the console of `other`, or nothing. The file comes from
// `built`. `opened` holds the file as the archive gives it. ntdlib's reader of that console has to read the new file,
// and its player has to reach the commands that the archive's file holds (ReachedCommands), but for the biquad filters.
// Converted back, the file has to hold the labels and reach the commands of the archive's file again, but for the
// biquad filters. `totals` counts the files whose filters change there, and those whose filters come back otherwise. It
// also counts the files that come back with another version, and those that come back with other bytes. A file that
// reaches a command that the 3DS lacks has to fail for the 3DS, and `totals` counts it apart. So does a file whose
// commands overlap: it has to fail for every console.
std::string OtherConsoleProblem(const sar::SoundArchive& built, const sar::SoundArchive& opened, uint32_t sound,
                                sar::Flavor other, Totals& totals)
{
    // The starts of the sounds that play the file.
    const sar::Flavor flavor = opened.ReadOptions().flavor;
    const std::span<const uint8_t> theirs = opened.FileData(opened.Sounds()[sound].file_id);
    std::vector<uint32_t> starts = {opened.Sounds()[sound].start_offset};
    for (const uint32_t sharing : song::SoundsSharingFile(opened, sound))
    {
        starts.push_back(opened.Sounds()[sharing].start_offset);
    }

    if (Overlapping(theirs, flavor, starts))
    {
        try
        {
            song::OtherConsoleSequenceFile(built, sound, other);
        }
        catch (const std::runtime_error&)
        {
            totals.overlapping++;
            return {};
        }

        return "goes to another console with commands that overlap";
    }
    if (other == sar::Flavor::kCtr && ReachesWhatTheCtrLacks(theirs, flavor, starts))
    {
        try
        {
            song::OtherConsoleSequenceFile(built, sound, other);
        }
        catch (const std::runtime_error&)
        {
            totals.refused++;
            return {};
        }

        return "goes to the 3DS with a command that the 3DS lacks";
    }

    try
    {
        const std::vector<uint8_t> ours = song::OtherConsoleSequenceFile(built, sound, other).file;
        sar::Options options = song::ItemOptions(opened);
        options.flavor = other;
        sar::Sequence::Parse(ours, options);
        const std::vector<Reached> reached = ReachedCommands(theirs, flavor, starts);
        const std::vector<Reached> there = ReachedCommands(ours, other, starts);
        if (!SameButBiquads(there, reached))
        {
            return "holds other commands";
        }
        totals.biquads += there == reached ? 0 : 1;

        // Bytes that no command reaches can come back otherwise. The decoder reads what it can of them as commands. The
        // byte order changes what it can read. Bytes 8 to 11 hold the version. A Wii U file of version 2.0.0 comes back
        // as 1.0.0.
        std::vector<uint8_t> back = song::ConvertSequenceFile(ours, other, flavor, starts).file;
        const std::vector<Reached> reached_back = ReachedCommands(back, flavor, starts);
        if (!SameLabels(back, theirs) || !SameButBiquads(reached_back, reached))
        {
            return "doesn't convert back to its commands";
        }
        if (back.size() >= 12 && theirs.size() >= 12 &&
            !std::equal(back.begin() + 8, back.begin() + 12, theirs.begin() + 8))
        {
            std::copy(theirs.begin() + 8, theirs.begin() + 12, back.begin() + 8);
            totals.versions++;
        }
        if (reached_back != reached)
        {
            totals.biquads_back++;
        }
        else if (!std::equal(back.begin(), back.end(), theirs.begin(), theirs.end()))
        {
            totals.unreached++;
        }
    }
    catch (const std::exception& error)
    {
        return std::string("fails: ") + error.what();
    }

    return {};
}

// The version in the header of `file`, a file of the 3DS, the Wii U or the Switch, in the byte order that its mark
// gives.
uint32_t VersionOf(std::span<const uint8_t> file)
{
    return xsflib::Reader(file, file.size() > 5 && file[4] == 0xFE).U32(0x08);
}

// Whether two regions hold the same wave, key, volume, pan, pitch, flags and envelope.
bool SameRegion(const sar::VelocityRegion& a, const sar::VelocityRegion& b)
{
    return a.wave_id_index == b.wave_id_index && a.original_key == b.original_key && a.volume == b.volume &&
           a.pan == b.pan && a.pitch == b.pitch && a.ignore_note_off == b.ignore_note_off &&
           a.key_group == b.key_group && a.interpolation == b.interpolation && a.adshr.attack == b.adshr.attack &&
           a.adshr.decay == b.adshr.decay && a.adshr.sustain == b.adshr.sustain && a.adshr.hold == b.adshr.hold &&
           a.adshr.release == b.adshr.release;
}

// Whether two tables of waves name the same waves.
bool SameWaves(const std::vector<sar::WaveId>& a, const std::vector<sar::WaveId>& b)
{
    const auto same = [](const sar::WaveId& x, const sar::WaveId& y)
    {
        return x.wave_archive_item == y.wave_archive_item && x.index == y.index;
    };

    return std::equal(a.begin(), a.end(), b.begin(), b.end(), same);
}

// Whether two banks hold the same waves, and the same regions over the same keys and velocities.
bool SameBank(const sar::Bank& a, const sar::Bank& b)
{
    if (!SameWaves(a.waves, b.waves) || a.instruments.size() != b.instruments.size())
    {
        return false;
    }

    for (std::size_t i = 0; i < a.instruments.size(); i++)
    {
        if (a.instruments[i].has_value() != b.instruments[i].has_value() ||
            (a.instruments[i] && a.instruments[i]->keys.size() != b.instruments[i]->keys.size()))
        {
            return false;
        }

        for (std::size_t k = 0; a.instruments[i] && k < a.instruments[i]->keys.size(); k++)
        {
            const sar::KeyRegion& x = a.instruments[i]->keys[k];
            const sar::KeyRegion& y = b.instruments[i]->keys[k];
            if (x.lo != y.lo || x.hi != y.hi || x.velocities.size() != y.velocities.size())
            {
                return false;
            }
            for (std::size_t v = 0; v < x.velocities.size(); v++)
            {
                const sar::KeyRegion::Vel& p = x.velocities[v];
                const sar::KeyRegion::Vel& q = y.velocities[v];
                if (p.lo != q.lo || p.hi != q.hi || p.region.has_value() != q.region.has_value() ||
                    (p.region && !SameRegion(*p.region, *q.region)))
                {
                    return false;
                }
            }
        }
    }

    return true;
}

// Whether two wave sound files hold the same waves, and the same wave sounds with the same note events and notes.
bool SameWaveSounds(const sar::WaveSoundFile& a, const sar::WaveSoundFile& b)
{
    if (!SameWaves(a.waves, b.waves) || a.sounds.size() != b.sounds.size())
    {
        return false;
    }

    const auto same_event = [](const sar::WaveSoundFile::NoteEvent& x, const sar::WaveSoundFile::NoteEvent& y)
    {
        return x.position == y.position && x.length == y.length && x.note == y.note;
    };
    for (std::size_t s = 0; s < a.sounds.size(); s++)
    {
        const sar::WaveSoundFile::WaveSound& x = a.sounds[s];
        const sar::WaveSoundFile::WaveSound& y = b.sounds[s];
        const sar::VelocityRegion envelope_x{.adshr = x.adshr};
        const sar::VelocityRegion envelope_y{.adshr = y.adshr};
        if (x.pan != y.pan || x.pitch != y.pitch || !SameRegion(envelope_x, envelope_y) ||
            x.tracks.size() != y.tracks.size() ||
            !std::equal(x.notes.begin(), x.notes.end(), y.notes.begin(), y.notes.end(), SameRegion))
        {
            return false;
        }
        for (std::size_t k = 0; k < x.tracks.size(); k++)
        {
            if (!std::equal(x.tracks[k].begin(), x.tracks[k].end(), y.tracks[k].begin(), y.tracks[k].end(), same_event))
            {
                return false;
            }
        }
    }

    return true;
}

// Whether two waves, each read with its options, decode to the same samples at the same rate, with the same loop and
// encoding.
bool SameWave(std::span<const uint8_t> a, const sar::Options& a_options, std::span<const uint8_t> b,
              const sar::Options& b_options)
{
    const sar::Wave x = sar::Wave::Parse(a, a_options);
    const sar::Wave y = sar::Wave::Parse(b, b_options);
    const sar::Pcm p = sar::DecodeWave(a, a_options);
    const sar::Pcm q = sar::DecodeWave(b, b_options);
    return x.encoding == y.encoding && x.loop == y.loop && x.loop_start == y.loop_start && x.loop_end == y.loop_end &&
           p.sample_rate == q.sample_rate && p.loop == q.loop && p.channels == q.channels;
}

// The bytes of each channel of `wave` up to its last sample: whole frames of DSP-ADPCM.
std::size_t SampleBytes(const sar::WaveFile& wave)
{
    switch (wave.encoding)
    {
    case sar::WaveEncoding::kPcm8:
        return wave.loop_end;
    case sar::WaveEncoding::kPcm16:
        return std::size_t{wave.loop_end} * 2;
    default:
        break;
    }

    return (std::size_t{wave.loop_end} + 13) / 14 * 8;
}

// What differs between a wave that went to the other console and back and the archive's wave.
struct WaveBack
{
    bool loop_start = false;
    bool trimmed = false;
    bool other = false;
};

// What differs between `back`, a wave that went to the other console and back, and the archive's wave `theirs`: the
// original loop start, the bytes after the channels' last samples, or anything else.
WaveBack CompareBack(std::span<const uint8_t> back, std::span<const uint8_t> theirs)
{
    WaveBack difference;
    if (std::equal(back.begin(), back.end(), theirs.begin(), theirs.end()))
    {
        return difference;
    }

    // The archive's original loop start and bytes after the last samples put back have to give the archive's bytes. A
    // channel's samples run to the next channel's, and the writer pads them to a boundary with zeros.
    sar::WaveFile patched = sar::WaveFile::Read(back);
    const sar::WaveFile original = sar::WaveFile::Read(theirs);
    difference.loop_start = patched.original_loop_start != original.original_loop_start;
    patched.original_loop_start = original.original_loop_start;
    const auto held = static_cast<std::ptrdiff_t>(SampleBytes(original));
    const auto padding = [](uint8_t byte)
    {
        return byte == 0;
    };
    for (std::size_t c = 0; c < patched.channels.size() && c < original.channels.size(); c++)
    {
        std::vector<uint8_t>& ours = patched.channels[c].samples;
        const std::vector<uint8_t>& archive = original.channels[c].samples;
        if (ours != archive && std::ssize(ours) >= held && std::ssize(archive) >= held &&
            std::equal(ours.begin(), ours.begin() + held, archive.begin()) &&
            std::all_of(ours.begin() + held, ours.end(), padding))
        {
            ours = archive;
            difference.trimmed = true;
        }
    }
    const std::vector<uint8_t> written = patched.Write();
    difference.other = !std::equal(written.begin(), written.end(), theirs.begin(), theirs.end());

    return difference;
}

// What is wrong with the file of `item` for the console of `other`, or nothing: a bank's, a wave sound's or a wave
// archive's. The file comes from `built`. `opened` holds the file as the archive gives it. ntdlib's reader of that
// console has to read the same values from the new file: a bank's waves and regions, a wave sound file's waves and wave
// sounds, and each wave's samples, rate, loop and encoding. The new file and its waves have to hold the versions of
// Export…'s files for that console. A wave that goes out alone has to give the bytes that the new wave archive holds
// for it. Converted back with the versions of the archive's file, a bank file and a wave sound file have to give the
// archive's bytes. A wave can come back with another original loop start, or without bytes after its last samples.
// `totals` counts those apart, with the files and waves that take another version.
std::string OtherConsoleFileProblem(const sar::SoundArchive& built, const sar::SoundArchive& opened,
                                    const song::Item& item, sar::Flavor other, Totals& totals)
{
    const sar::Flavor flavor = opened.ReadOptions().flavor;
    const sar::Options options = song::ItemOptions(opened);
    sar::Options other_options = options;
    other_options.flavor = other;
    const song::FileVersions versions = song::ExportVersions(item.kind, other);
    try
    {
        // The new file's version, and the values that the other console's reader reads.
        const std::span<const uint8_t> theirs = song::ItemFile(opened, item);
        const std::vector<uint8_t> ours = song::OtherConsoleFile(built, item, other);
        if (VersionOf(ours) != versions.file)
        {
            return "takes another version";
        }
        totals.file_versions += VersionOf(ours) != VersionOf(theirs) ? 1 : 0;
        totals.converted_files++;
        if (item.kind == song::ItemKind::kBank &&
            !SameBank(sar::Bank::Parse(ours, other_options), sar::Bank::Parse(theirs, options)))
        {
            return "reads otherwise";
        }
        if (item.kind == song::ItemKind::kWaveSound &&
            !SameWaveSounds(sar::WaveSoundFile::Parse(ours, other_options), sar::WaveSoundFile::Parse(theirs, options)))
        {
            return "reads otherwise";
        }
        if (item.kind != song::ItemKind::kWaveArchive)
        {
            const std::vector<uint8_t> back =
                song::ConvertItemFile(ours, item.kind, other, flavor, {VersionOf(theirs), 0});
            return std::equal(back.begin(), back.end(), theirs.begin(), theirs.end())
                       ? ""
                       : "doesn't convert back to the archive's bytes";
        }

        // Each wave, in the wave archive and alone, then back with the version of the archive's wave.
        const std::vector<std::span<const uint8_t>> our_waves = sar::WaveArchive::Parse(ours, other_options).waves;
        const std::vector<std::span<const uint8_t>> their_waves = sar::WaveArchive::Parse(theirs, options).waves;
        if (our_waves.size() != their_waves.size())
        {
            return "holds another number of waves";
        }
        bool whole = true;
        sar::WaveArchiveFile back_archive;
        back_archive.flavor = flavor;
        back_archive.version = VersionOf(theirs);
        for (uint32_t w = 0; w < our_waves.size(); w++)
        {
            const std::vector<uint8_t> alone =
                song::OtherConsoleFile(built, {song::ItemKind::kWave, item.index, w}, other);
            if (!SameWave(our_waves[w], other_options, their_waves[w], options) ||
                VersionOf(our_waves[w]) != versions.waves ||
                !std::equal(alone.begin(), alone.end(), our_waves[w].begin(), our_waves[w].end()))
            {
                return "plays wave " + std::to_string(w) + " otherwise";
            }

            const std::vector<uint8_t> back = song::ConvertItemFile(our_waves[w], song::ItemKind::kWave, other, flavor,
                                                                    {VersionOf(their_waves[w]), 0});
            const WaveBack difference = CompareBack(back, their_waves[w]);
            if (difference.other)
            {
                return "doesn't convert wave " + std::to_string(w) + " back to the archive's bytes";
            }
            totals.wave_versions += VersionOf(our_waves[w]) != VersionOf(their_waves[w]) ? 1 : 0;
            totals.loop_starts += difference.loop_start ? 1 : 0;
            totals.trimmed += difference.trimmed ? 1 : 0;
            whole = whole && !difference.loop_start && !difference.trimmed;
            back_archive.waves.push_back(back);
            totals.converted_waves++;
        }

        // With every wave back as it was, the wave archive has to come back as it was too.
        const std::vector<uint8_t> written = back_archive.Write();
        if (whole && !std::equal(written.begin(), written.end(), theirs.begin(), theirs.end()))
        {
            return "doesn't convert back to the archive's bytes";
        }
    }
    catch (const std::exception& error)
    {
        return std::string("fails: ") + error.what();
    }

    return {};
}

// Whether the note that the browser plays of program `program` of bank `bank` of the Wii's `built` archive, on `build`
// of NW4R SND, gives the output of ntdlib's model run directly on `opened` with the same note, up to the preview's last
// frame. Counts the note in `totals`.
bool NotePlays(const std::shared_ptr<const rsar::SoundArchive>& built, const rsar::SoundArchive& opened, uint32_t bank,
               uint32_t program, const ntdlib::SndBuild& build, Totals& totals)
{
    song::NotePreview preview(built, bank, program, build);
    const int key = song::NotePreview::KeyOf(rsar::Bank::Parse(opened.FileData(opened.Banks()[bank].file_id)), program);
    std::vector<float> played(static_cast<std::size_t>(ntdlib::rvl::ax::kSamplesPerFrame) * 2);
    std::vector<float> samples;
    for (std::size_t got = 0; (got = preview.Render(played.data(), played.size() / 2)) > 0;)
    {
        samples.insert(samples.end(), played.begin(), played.begin() + static_cast<std::ptrdiff_t>(got * 2));
    }

    // The model run directly, with the preview's note-off and limit.
    ntdlib::rvl::snd::ArchiveModel model;
    model.build = build;
    model.StartWithRenderer(opened);
    ntdlib::rvl::snd::NoteOnInfo info;
    info.prg_no = program;
    info.key = key;
    info.velocity = song::NotePreview::kVelocity;
    std::vector<float> expected;
    if (key >= 0 && model.engine->NoteOn({bank}, 0, info) != nullptr)
    {
        std::vector<int16_t> output;
        for (uint64_t frame = 0; frame < preview.LongestFrames() && model.engine->IsBusy(); frame++)
        {
            if (frame == preview.HoldFrames())
            {
                ntdlib::rvl::snd::ChannelManager& channels = model.engine->Channels();
                const std::vector<ntdlib::nwsnd::ChannelId> places(channels.InUse().begin(), channels.InUse().end());
                for (const ntdlib::nwsnd::ChannelId place : places)
                {
                    channels.At(place).NoteOff(*model.engine);
                }
            }
            output.clear();
            if (!model.RunFrame(output))
            {
                break;
            }
            for (const int16_t sample : output)
            {
                expected.push_back(static_cast<float>(sample) / 32768.0f);
            }
        }
    }

    totals.notes++;
    const std::size_t frame = played.size();
    const bool endless = samples.size() == preview.LongestFrames() * frame;
    totals.endless_notes += endless ? 1 : 0;
    totals.silent_notes += std::ranges::all_of(samples, [](float sample) { return sample == 0.0f; }) ? 1 : 0;
    const std::size_t compared = endless ? samples.size() - frame : samples.size();
    return samples.size() == expected.size() &&
           std::equal(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(compared), expected.begin());
}

// The check of the Wii's archive at `path` with the bytes `bytes` (see the top of the file).
int CheckWii(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
{
    // The opened archive, and the one that the Hokuto file builds, both as the program reads them.
    const std::string name = xsflib::Utf8(path.filename());
    std::shared_ptr<const rsar::SoundArchive> opened;
    std::shared_ptr<const rsar::SoundArchive> built;
    std::optional<ntdlib::SndBuild> build;
    try
    {
        std::vector<uint8_t> dol;
        const std::optional<std::filesystem::path> found = song::FindMainDol(path);
        build = found && xsflib::ReadFile(*found, dol) ? song::ReadWiiBuild(dol) : std::nullopt;
        const hokuto::format::File file = hokuto::format::ImportWiiArchive(bytes, build);
        opened = song::LoadWiiArchive(bytes, name, ntdlib::rvl::snd::kRsarOptions);
        built = song::LoadWiiArchive(hokuto::format::ExportArchive(file), name, ntdlib::rvl::snd::kRsarOptions);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
        return 2;
    }

    // Each item's file from the built archive against the bytes that ntdlib alone gives for it in the opened archive.
    // Each file starts with its format's signature, the format's name without its first letter. An item whose file the
    // opened archive lacks has to fail on Hokuto's side too.
    Totals totals;
    std::set<std::string> names;
    const auto check = [&](const song::Item& item, std::span<const uint8_t> theirs, std::size_t waves)
    {
        const std::string label = song::ItemName(*opened, item);
        if (theirs.empty())
        {
            bool failed = false;
            try
            {
                song::ItemBytes(*built, item);
            }
            catch (const std::runtime_error&)
            {
                failed = true;
            }
            if (!failed)
            {
                std::printf("%s: %s exports without a file\n", name.c_str(), label.c_str());
                totals.differ++;
            }
            totals.missing++;
            return;
        }

        const std::string signature = song::WiiItemFormat(item.kind).substr(1);
        try
        {
            const std::vector<uint8_t> ours = song::ItemBytes(*built, item);
            totals.items++;
            if (!std::equal(ours.begin(), ours.end(), theirs.begin(), theirs.end()) || ours.size() < 4 ||
                std::string(ours.begin(), ours.begin() + 4) != signature)
            {
                std::printf("%s: %s differs\n", name.c_str(), label.c_str());
                totals.differ++;
            }
            if (!names.insert(xsflib::AsciiLower(song::ItemFileName(*built, item, waves))).second)
            {
                totals.clashes++;
            }
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s can't be exported: %s\n", name.c_str(), label.c_str(), error.what());
            totals.differ++;
        }
    };

    std::set<uint32_t> files;
    for (std::size_t i = 0; i < opened->Sounds().size(); i++)
    {
        const rsar::SoundInfo& sound = opened->Sounds()[i];
        if (sound.type == rsar::SoundType::kSequence && files.insert(sound.file_id).second)
        {
            check({song::ItemKind::kSequence, static_cast<uint32_t>(i), 0}, opened->FileData(sound.file_id), 0);
        }
    }

    const std::vector<song::WaveSoundSet> sets = song::WaveSoundSets(*built);
    const auto same_set = [](const song::WaveSoundSet& a, const song::WaveSoundSet& b)
    {
        return a.file_id == b.file_id && a.name == b.name && a.sounds == b.sounds;
    };
    if (!std::ranges::equal(sets, song::WaveSoundSets(*opened), same_set))
    {
        std::printf("%s: the wave sound files differ between the archives\n", name.c_str());
        totals.differ++;
    }
    for (const song::WaveSoundSet& set : sets)
    {
        check({song::ItemKind::kWaveSoundFile, set.sounds.front(), 0}, opened->FileData(set.file_id), 0);
        totals.wave_sound_files++;
    }

    for (std::size_t i = 0; i < opened->Banks().size(); i++)
    {
        check({song::ItemKind::kBank, static_cast<uint32_t>(i), 0}, opened->FileData(opened->Banks()[i].file_id), 0);
    }

    // Each bank's instruments as SoundFont 2 and SFZ files (BankProblem). A bank that ntdlib can't read, or one without
    // instruments, has to stop Hokuto's export. A bank whose file the archive lacks counts above.
    for (std::size_t i = 0; i < opened->Banks().size(); i++)
    {
        const auto bank = static_cast<uint32_t>(i);
        const std::string& label = opened->Banks()[i].name;
        const std::span<const uint8_t> bank_file = opened->FileData(opened->Banks()[i].file_id);
        if (bank_file.empty())
        {
            continue;
        }

        std::vector<uint32_t> programs;
        bool unread = false;
        try
        {
            const rsar::Bank parsed = rsar::Bank::Parse(bank_file);
            for (uint32_t program = 0; program < parsed.InstrumentCount(); program++)
            {
                if (parsed.Plays(program))
                {
                    programs.push_back(program);
                }
            }
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s can't be read: %s\n", name.c_str(), label.c_str(), error.what());
            unread = true;
        }
        try
        {
            if (programs.empty())
            {
                bool stopped = false;
                try
                {
                    song::ConvertBank(*built, bank, "Hokuto", build);
                }
                catch (const std::runtime_error&)
                {
                    stopped = true;
                }
                if (!stopped)
                {
                    std::printf("%s: %s exports without instruments\n", name.c_str(), label.c_str());
                    totals.differ++;
                }
                (unread ? totals.unread_banks : totals.empty_banks)++;
                continue;
            }

            // The pan mode of the first sequence sound that plays the bank, or balance mode.
            uint8_t pan_mode = 1;
            for (const rsar::SoundInfo& sound : opened->Sounds())
            {
                if (sound.type == rsar::SoundType::kSequence && sound.bank == bank)
                {
                    pan_mode = sound.pan_mode;
                    break;
                }
            }
            const export_::BankConversion ours = song::ConvertBank(*built, bank, "Hokuto", build);
            const export_::BankConversion theirs =
                export_::ConvertBank(*opened, i, pan_mode, song::SettingsOf(*opened, build), "Hokuto");
            const std::string problem = BankProblem(ours, theirs, programs);
            if (!problem.empty())
            {
                std::printf("%s: %s %s\n", name.c_str(), label.c_str(), problem.c_str());
                totals.differ++;
            }

            for (const std::string& warning : ours.warnings)
            {
                if (warning.starts_with("region volume exceeds 127"))
                {
                    totals.louder_banks++;
                    continue;
                }

                std::printf("%s: %s: %s\n", name.c_str(), label.c_str(), warning.c_str());
                totals.bank_warnings++;
            }

            totals.banks++;
            totals.instruments += programs.size();
            if (ours.sound_fonts.size() > 1)
            {
                totals.split_banks++;
            }
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s's instruments can't be exported: %s\n", name.c_str(), label.c_str(), error.what());
            totals.differ++;
        }

        // A note of each program as the browser plays it.
        for (const uint32_t program : programs)
        {
            try
            {
                if (!NotePlays(built, *opened, bank, program,
                               build.value_or(export_::BuildOfRsarVersion(opened->Version())), totals))
                {
                    std::printf("%s: %s's note of program %u plays otherwise\n", name.c_str(), label.c_str(), program);
                    totals.differ++;
                }
            }
            catch (const std::exception& error)
            {
                std::printf("%s: %s's note of program %u fails: %s\n", name.c_str(), label.c_str(), program,
                            error.what());
                totals.differ++;
            }
        }
    }

    // Each wave archive, then each of its waves: an RWAR's file and its RWAV files, and each wave's WAV file against
    // ntdlib's decoding.
    const std::vector<song::WiiWaveArchive> wave_archives = song::WaveArchivesOf(*built);
    const auto same_wave_archive = [](const song::WiiWaveArchive& a, const song::WiiWaveArchive& b)
    {
        return a.file_id == b.file_id && a.name == b.name && a.rwar == b.rwar;
    };
    if (!std::ranges::equal(wave_archives, song::WaveArchivesOf(*opened), same_wave_archive))
    {
        std::printf("%s: the wave archives differ between the archives\n", name.c_str());
        totals.differ++;
    }
    std::size_t rwars = 0;
    std::size_t blocks = 0;
    for (const song::WiiWaveArchive& wave_archive : wave_archives)
    {
        const uint32_t file_id = wave_archive.file_id;
        const std::span<const uint8_t> wave_data = opened->WaveData(file_id);
        std::vector<std::span<const uint8_t>> rwavs;
        std::vector<rsar::Wave> waves;
        try
        {
            if (wave_archive.rwar)
            {
                check({song::ItemKind::kWaveArchive, file_id, 0}, wave_data, 0);
                rwavs = rsar::WaveArchive::Parse(wave_data).waves;
                rwars++;
            }
            else if (const std::span<const uint8_t> file = opened->FileData(file_id);
                     file.size() >= 4 && std::string(file.begin(), file.begin() + 4) == "RWSD")
            {
                waves = rsar::WaveSoundFile::Parse(file, wave_data).Waves();
            }
            else
            {
                waves = rsar::Bank::Parse(file, wave_data).Waves();
            }
        }
        catch (const std::exception& error)
        {
            std::printf("%s: the waves of %s can't be read: %s\n", name.c_str(), wave_archive.name.c_str(),
                        error.what());
            totals.differ++;
        }

        // Wave data that a WAVE block describes goes out as an RWAR of its waves (song::ItemBytes).
        std::vector<uint8_t> block_rwar;
        std::vector<std::span<const uint8_t>> block_rwavs;
        if (!wave_archive.rwar && !waves.empty())
        {
            const song::Item item{song::ItemKind::kWaveArchive, file_id, 0};
            try
            {
                block_rwar = song::ItemBytes(*built, item);
                block_rwavs = rsar::WaveArchive::Parse(block_rwar).waves;
                totals.items++;
                blocks++;
                if (block_rwavs.size() != waves.size())
                {
                    std::printf("%s: %s's RWAR holds %zu waves, and its WAVE block %zu\n", name.c_str(),
                                wave_archive.name.c_str(), block_rwavs.size(), waves.size());
                    totals.differ++;
                }
                if (!names.insert(xsflib::AsciiLower(song::ItemFileName(*built, item, 0))).second)
                {
                    totals.clashes++;
                }
            }
            catch (const std::exception& error)
            {
                std::printf("%s: %s can't be exported: %s\n", name.c_str(), wave_archive.name.c_str(), error.what());
                totals.differ++;
            }
        }
        const std::size_t count = wave_archive.rwar ? rwavs.size() : waves.size();
        for (uint32_t wave = 0; wave < count; wave++)
        {
            // An RWAV file gives its length at 8, after its signature, its byte order mark and its version.
            const song::Item item{song::ItemKind::kWave, file_id, wave};
            const std::string label = song::ItemName(*opened, item);
            if (wave_archive.rwar)
            {
                const std::span<const uint8_t> rwav = rwavs[wave];
                check(item, rwav, count);
                if (rwav.size() < 16 || std::string(rwav.begin(), rwav.begin() + 4) != "RWAV" ||
                    xsflib::LoadBe32(rwav.data() + 8) != rwav.size())
                {
                    std::printf("%s: %s isn't a whole wave file\n", name.c_str(), label.c_str());
                    totals.differ++;
                }
            }
            else if (!block_rwar.empty())
            {
                // The wave's RWAV file has to be the RWAR's, with the encoding and the samples of the WAVE block's
                // wave.
                try
                {
                    const std::vector<uint8_t> rwav = song::ItemBytes(*built, item);
                    totals.items++;
                    const rsar::Wave read = rsar::Wave::ParseFile(rwav);
                    if (wave >= block_rwavs.size() || !std::ranges::equal(rwav, block_rwavs[wave]) ||
                        read.encoding != waves[wave].encoding ||
                        !SamePcm(rsar::DecodeWave(read), rsar::DecodeWave(waves[wave])))
                    {
                        std::printf("%s: %s's wave file differs\n", name.c_str(), label.c_str());
                        totals.differ++;
                    }
                    if (!names.insert(xsflib::AsciiLower(song::ItemFileName(*built, item, count))).second)
                    {
                        totals.clashes++;
                    }
                }
                catch (const std::exception& error)
                {
                    std::printf("%s: %s's wave file can't be exported: %s\n", name.c_str(), label.c_str(),
                                error.what());
                    totals.differ++;
                }
            }

            // The WAV file, read back, against the wave that ntdlib decodes from the opened archive.
            totals.waves++;
            std::optional<rsar::Pcm> decoded;
            try
            {
                decoded = rsar::DecodeWave(wave_archive.rwar ? rsar::Wave::ParseFile(rwavs[wave]) : waves[wave]);
            }
            catch (const std::exception&)
            {
                // Hokuto's side has to fail too.
            }
            try
            {
                const std::vector<uint8_t> wav = song::WaveAsWav(*built, file_id, wave);
                const std::optional<WavRead> read = ReadWav(wav);
                if (!decoded || !read || !Holds(*read, *decoded))
                {
                    std::printf("%s: %s's WAV file differs\n", name.c_str(), label.c_str());
                    totals.differ++;
                }
                if (song::WaveChannels(*built, file_id, wave) > 2)
                {
                    totals.wide++;
                }
            }
            catch (const std::exception& error)
            {
                if (decoded)
                {
                    std::printf("%s: %s's WAV file fails: %s\n", name.c_str(), label.c_str(), error.what());
                    totals.differ++;
                }
                totals.undecoded++;
            }
        }
    }

    // Each wave sound as a WAV file, read back, against the wave that the tool reads for it from the opened archive
    // (WiiWaveSoundPcm). A wave sound without a file, a note or a wave there has to fail on Hokuto's side too.
    for (std::size_t i = 0; i < opened->Sounds().size(); i++)
    {
        const rsar::SoundInfo& sound = opened->Sounds()[i];
        if (sound.type != rsar::SoundType::kWaveSound)
        {
            continue;
        }

        totals.wave_sounds++;
        const std::optional<rsar::Pcm> expected = WiiWaveSoundPcm(*opened, sound);
        try
        {
            const rsar::Pcm ours = song::DecodeWaveSound(*built, static_cast<uint32_t>(i));
            const std::optional<WavRead> read = ReadWav(song::WaveSoundAsWav(*built, static_cast<uint32_t>(i)));
            if (!expected || !read || !Holds(*read, *expected))
            {
                std::printf("%s: %s's WAV file differs\n", name.c_str(), sound.name.c_str());
                totals.differ++;
            }
            const song::WaveSoundNote note = song::WaveSoundNoteOf(*built, static_cast<uint32_t>(i));
            if (ours.sample_rate != song::DecodeItemWave(*built, note.wave_archive, note.wave).sample_rate)
            {
                totals.pitched++;
            }
        }
        catch (const std::exception& error)
        {
            if (expected)
            {
                std::printf("%s: %s's WAV file fails: %s\n", name.c_str(), sound.name.c_str(), error.what());
                totals.differ++;
            }
            totals.silent++;
        }
    }

    std::printf(
        "%s: %zu items checked, and %zu waves as WAV files. %zu differ. %zu lack their files on both sides. %zu "
        "waves can't be decoded, and %zu have more than two channels. %zu names clash.\n",
        name.c_str(), totals.items, totals.waves, totals.differ, totals.missing, totals.undecoded, totals.wide,
        totals.clashes);
    std::printf(
        "%s: %zu wave sounds as WAV files, %zu of them at another rate than their wave's. %zu lack their file, their "
        "note or their wave on both sides.\n",
        name.c_str(), totals.wave_sounds, totals.pitched, totals.silent);
    std::printf(
        "%s: %zu wave sound files, and %zu wave archives, %zu of them RWARs. %zu WAVE blocks went out as RWARs "
        "of their waves.\n",
        name.c_str(), totals.wave_sound_files, wave_archives.size(), rwars, blocks);
    const std::string build_text = !build                 ? "the build that the archive's version gives"
                                   : build->date > 0      ? std::to_string(build->date)
                                   : build->October2006() ? "2006"
                                                          : "a build without a banner";
    std::printf(
        "%s: %zu banks' instruments checked, %zu instruments in all, by NW4R SND of %s. %zu banks hold no "
        "instruments, and %zu can't be read. %zu need several SoundFonts. %zu take an attenuation in their SoundFont "
        "for a region above volume 127. The converter warned %zu times more.\n",
        name.c_str(), totals.banks, totals.instruments, build_text.c_str(), totals.empty_banks, totals.unread_banks,
        totals.split_banks, totals.louder_banks, totals.bank_warnings);
    std::printf("%s: %zu notes played as the browser plays them. %zu stay silent, and %zu reach the limit of 10 s.\n",
                name.c_str(), totals.notes, totals.silent_notes, totals.endless_notes);

    return totals.differ > 0 ? 1 : 0;
}

} // namespace

int main(int argc, char** argv)
{
    const std::vector<std::string> arguments = xsflib::console::Arguments(argc, argv);
    if (arguments.size() != 2)
    {
        std::fprintf(stderr, "usage: hokuto_check_items <archive>\n");
        return 2;
    }

    // The opened archive, and the one that the Hokuto file builds. Both have the group files beside the archive. An
    // archive of the Wii takes a check of its own.
    const std::filesystem::path path = xsflib::PathFromUtf8(arguments[1]);
    const std::string name = xsflib::Utf8(path.filename());
    std::shared_ptr<const sar::SoundArchive> opened;
    std::shared_ptr<const sar::SoundArchive> built;
    hokuto::format::File file;
    try
    {
        std::vector<uint8_t> bytes;
        if (!xsflib::ReadFile(path, bytes))
        {
            std::fprintf(stderr, "%s can't be read\n", name.c_str());
            return 2;
        }
        if (bytes.size() >= 4 && std::string(bytes.begin(), bytes.begin() + 4) == "RSAR")
        {
            return CheckWii(path, bytes);
        }
        opened = song::OpenArchive(path);
        file = hokuto::format::ImportArchive(bytes, opened->ReadOptions().flavor, opened->GroupFiles(),
                                             song::ReadStreams(*opened, path), opened->GroupFileGroups());
        built = song::LoadArchive(hokuto::format::ExportArchive(file), name, hokuto::format::ExportGroupFiles(file));
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
        return 2;
    }

    // Each item's file from the built archive against the bytes that the opened archive's tables give it, read with
    // ntdlib alone: the file of the item's file ID, or the wave that the wave archive's file holds. Each file starts
    // with its format's signature. A sequence file and a wave sound file count once, under the first sound that plays
    // them. The hok tool writes them that way too. Names that differ only in case would share a file on macOS and
    // Windows.
    Totals totals;
    std::set<std::string> names;
    const sar::Options options = song::ItemOptions(*opened);
    const auto check = [&](const song::Item& item, std::span<const uint8_t> theirs, std::size_t waves)
    {
        // An item whose file the archive lacks has to fail on Hokuto's side too. A group file that the game keeps
        // beside the archive can hold the file, and the archive can come without it.
        const std::string label = song::ItemName(*opened, item);
        if (theirs.empty())
        {
            bool failed = false;
            try
            {
                song::ItemFile(*built, item);
            }
            catch (const std::runtime_error&)
            {
                failed = true;
            }
            if (!failed)
            {
                std::printf("%s: %s exports without a file\n", name.c_str(), label.c_str());
                totals.differ++;
            }
            totals.missing++;
            return;
        }

        const std::string signature = song::ItemFormat(item.kind, options.flavor).substr(1);
        try
        {
            const std::span<const uint8_t> ours = song::ItemFile(*built, item);
            totals.items++;
            if (!std::equal(ours.begin(), ours.end(), theirs.begin(), theirs.end()) || ours.size() < 4 ||
                std::string(ours.begin(), ours.begin() + 4) != signature)
            {
                std::printf("%s: %s differs\n", name.c_str(), label.c_str());
                totals.differ++;
            }
            if (!names.insert(xsflib::AsciiLower(song::ItemFileName(*built, item, waves))).second)
            {
                totals.clashes++;
            }
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s can't be exported: %s\n", name.c_str(), label.c_str(), error.what());
            totals.differ++;
        }
    };

    std::set<uint32_t> files;
    for (std::size_t i = 0; i < opened->Sounds().size(); i++)
    {
        const sar::SoundInfo& sound = opened->Sounds()[i];
        if (sound.type == sar::SoundType::kSequence && files.insert(sound.file_id).second)
        {
            check({song::ItemKind::kSequence, static_cast<uint32_t>(i), 0}, opened->FileData(sound.file_id), 0);
        }
    }

    const std::vector<song::WaveSoundSet> sets = song::WaveSoundSets(*built);
    const std::vector<song::WaveSoundSet> opened_sets = song::WaveSoundSets(*opened);
    const auto same_set = [](const song::WaveSoundSet& a, const song::WaveSoundSet& b)
    {
        return a.file_id == b.file_id && a.name == b.name && a.sounds == b.sounds;
    };
    if (!std::ranges::equal(sets, opened_sets, same_set))
    {
        std::printf("%s: the wave sound files differ between the archives\n", name.c_str());
        totals.differ++;
    }
    for (const song::WaveSoundSet& set : sets)
    {
        check({song::ItemKind::kWaveSoundFile, set.sounds.front(), 0}, opened->FileData(set.file_id), 0);
        const auto named = [&set](const sar::SoundGroupInfo& group)
        {
            return group.name == set.name;
        };
        totals.named_files += std::ranges::any_of(opened->SoundGroups(), named) ? 1 : 0;
        totals.wave_sound_files++;
    }

    for (std::size_t i = 0; i < opened->Banks().size(); i++)
    {
        check({song::ItemKind::kBank, static_cast<uint32_t>(i), 0}, opened->FileData(opened->Banks()[i].file_id), 0);
    }

    // Each sequence file for each other console, once, under the first sound that plays it (OtherConsoleProblem).
    const std::vector<sar::Flavor> consoles = song::OtherConsoles(options.flavor);
    std::set<uint32_t> converted;
    for (std::size_t i = 0; i < opened->Sounds().size(); i++)
    {
        const sar::SoundInfo& sound = opened->Sounds()[i];
        if (sound.type != sar::SoundType::kSequence || opened->FileData(sound.file_id).empty() ||
            !converted.insert(sound.file_id).second)
        {
            continue;
        }

        for (const sar::Flavor other : consoles)
        {
            const std::string problem = OtherConsoleProblem(*built, *opened, static_cast<uint32_t>(i), other, totals);
            if (!problem.empty())
            {
                std::printf("%s: %s %s for the %s\n", name.c_str(), sound.name.c_str(), problem.c_str(),
                            song::ConsoleName(other).c_str());
                totals.differ++;
            }
            totals.converted++;
        }
    }

    // Each bank, wave sound file and wave archive for each other console, once (OtherConsoleFileProblem). An item whose
    // file the archive lacks counts above.
    std::vector<song::Item> others;
    std::set<uint32_t> wave_sound_files;
    for (std::size_t i = 0; i < opened->Sounds().size(); i++)
    {
        const sar::SoundInfo& sound = opened->Sounds()[i];
        if (sound.type == sar::SoundType::kWaveSound && wave_sound_files.insert(sound.file_id).second)
        {
            others.push_back({song::ItemKind::kWaveSound, static_cast<uint32_t>(i), 0});
        }
    }
    for (std::size_t i = 0; i < opened->Banks().size(); i++)
    {
        others.push_back({song::ItemKind::kBank, static_cast<uint32_t>(i), 0});
    }
    for (std::size_t i = 0; i < opened->WaveArchives().size(); i++)
    {
        others.push_back({song::ItemKind::kWaveArchive, static_cast<uint32_t>(i), 0});
    }
    for (const song::Item& item : others)
    {
        bool lacked = false;
        try
        {
            song::ItemFile(*opened, item);
        }
        catch (const std::runtime_error&)
        {
            lacked = true;
        }
        for (const sar::Flavor other : consoles)
        {
            const std::string problem = lacked ? "" : OtherConsoleFileProblem(*built, *opened, item, other, totals);
            if (!problem.empty())
            {
                std::printf("%s: %s %s for the %s\n", name.c_str(), song::ItemName(*opened, item).c_str(),
                            problem.c_str(), song::ConsoleName(other).c_str());
                totals.differ++;
            }
        }
    }

    // Each bank's instruments as SoundFont 2 and SFZ files (BankProblem). A bank that ntdlib can't read, or one without
    // instruments, has to stop Hokuto's export. A bank whose file the archive lacks counts above.
    for (std::size_t i = 0; i < opened->Banks().size(); i++)
    {
        const auto bank = static_cast<uint32_t>(i);
        const std::string& label = opened->Banks()[i].name;
        if (opened->FileData(opened->Banks()[i].file_id).empty())
        {
            continue;
        }

        std::vector<uint32_t> programs;
        bool unread = false;
        try
        {
            programs = song::ProgramsOf(*opened, i);
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s can't be read: %s\n", name.c_str(), label.c_str(), error.what());
            unread = true;
        }
        try
        {
            if (programs.empty())
            {
                bool stopped = false;
                try
                {
                    song::ConvertBank(*built, bank, "Hokuto");
                }
                catch (const std::runtime_error&)
                {
                    stopped = true;
                }
                if (!stopped)
                {
                    std::printf("%s: %s exports without instruments\n", name.c_str(), label.c_str());
                    totals.differ++;
                }
                (unread ? totals.unread_banks : totals.empty_banks)++;
                continue;
            }

            const export_::BankConversion ours = song::ConvertBank(*built, bank, "Hokuto");
            const export_::BankConversion theirs =
                export_::ConvertBank(*opened, i, song::SettingsOf(*opened), "Hokuto");
            const std::string problem = BankProblem(ours, theirs, programs);
            if (!problem.empty())
            {
                std::printf("%s: %s %s\n", name.c_str(), label.c_str(), problem.c_str());
                totals.differ++;
            }

            // The converter's warnings: the attenuation of a bank above volume 127, and what it can't read.
            for (const std::string& warning : ours.warnings)
            {
                if (warning.starts_with("region volume exceeds 127"))
                {
                    totals.louder_banks++;
                    continue;
                }

                std::printf("%s: %s: %s\n", name.c_str(), label.c_str(), warning.c_str());
                totals.bank_warnings++;
            }

            totals.banks++;
            totals.instruments += programs.size();
            if (ours.sound_fonts.size() > 1)
            {
                totals.split_banks++;
            }
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s's instruments can't be exported: %s\n", name.c_str(), label.c_str(), error.what());
            totals.differ++;
        }
    }

    // Each wave archive, then each of its waves: its file, and its WAV file against ntdlib's decoding.
    for (std::size_t i = 0; i < opened->WaveArchives().size(); i++)
    {
        const auto archive = static_cast<uint32_t>(i);
        const std::span<const uint8_t> archive_file = opened->FileData(opened->WaveArchives()[i].file_id);
        check({song::ItemKind::kWaveArchive, archive, 0}, archive_file, 0);
        if (archive_file.empty())
        {
            continue;
        }
        std::vector<std::span<const uint8_t>> waves;
        try
        {
            waves = sar::WaveArchive::Parse(archive_file, options).waves;
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s can't be read: %s\n", name.c_str(), opened->WaveArchives()[i].name.c_str(),
                        error.what());
            totals.differ++;
        }
        for (uint32_t wave = 0; wave < waves.size(); wave++)
        {
            const song::Item item{song::ItemKind::kWave, archive, wave};
            check(item, waves[wave], waves.size());
            const std::string label = song::ItemName(*opened, item);

            // The wave's file gives its length at 0x0C, after its signature, in the console's byte order.
            const std::span<const uint8_t> wave_file = waves[wave];
            const std::string signature = song::ItemFormat(song::ItemKind::kWave, options.flavor).substr(1);
            uint32_t length = 0;
            if (wave_file.size() >= 16)
            {
                length = options.flavor == sar::Flavor::kCafe
                             ? static_cast<uint32_t>(wave_file[12]) << 24 | static_cast<uint32_t>(wave_file[13]) << 16 |
                                   static_cast<uint32_t>(wave_file[14]) << 8 | wave_file[15]
                             : Le32(wave_file, 12);
            }
            if (wave_file.size() < 16 || std::string(wave_file.begin(), wave_file.begin() + 4) != signature ||
                length != wave_file.size())
            {
                std::printf("%s: %s isn't a whole wave file\n", name.c_str(), label.c_str());
                totals.differ++;
            }

            // The WAV file, read back, against the wave that ntdlib decodes from the opened archive.
            totals.waves++;
            std::optional<sar::Pcm> decoded;
            try
            {
                decoded = sar::DecodeWave(wave_file, options);
            }
            catch (const std::exception&)
            {
                // Hokuto's side has to fail too.
            }
            try
            {
                const std::vector<uint8_t> wav = song::WaveAsWav(*built, archive, wave);
                const std::optional<WavRead> read = ReadWav(wav);
                if (!decoded || !read || !Holds(*read, *decoded))
                {
                    std::printf("%s: %s's WAV file differs\n", name.c_str(), label.c_str());
                    totals.differ++;
                }
                if (song::WaveChannels(*built, archive, wave) > 2)
                {
                    totals.wide++;
                }
            }
            catch (const std::exception& error)
            {
                if (decoded)
                {
                    std::printf("%s: %s's WAV file fails: %s\n", name.c_str(), label.c_str(), error.what());
                    totals.differ++;
                }
                totals.undecoded++;
            }
        }
    }

    // Each wave sound as a WAV file, read back, against the wave that the tool reads for it from the opened archive
    // (WaveSoundPcm). A wave sound without a file, a note or a wave there has to fail on Hokuto's side too.
    for (std::size_t i = 0; i < opened->Sounds().size(); i++)
    {
        const sar::SoundInfo& sound = opened->Sounds()[i];
        if (sound.type != sar::SoundType::kWaveSound)
        {
            continue;
        }

        totals.wave_sounds++;
        const std::optional<sar::Pcm> expected = WaveSoundPcm(*opened, sound, options);
        try
        {
            const sar::Pcm ours = song::DecodeWaveSound(*built, static_cast<uint32_t>(i));
            const std::optional<WavRead> read = ReadWav(song::WaveSoundAsWav(*built, static_cast<uint32_t>(i)));
            if (!expected || !read || !Holds(*read, *expected))
            {
                std::printf("%s: %s's WAV file differs\n", name.c_str(), sound.name.c_str());
                totals.differ++;
            }
            const song::WaveSoundNote note = song::WaveSoundNoteOf(*built, static_cast<uint32_t>(i));
            if (ours.sample_rate != song::DecodeItemWave(*built, note.wave_archive, note.wave).sample_rate)
            {
                totals.pitched++;
            }
        }
        catch (const std::exception& error)
        {
            if (expected)
            {
                std::printf("%s: %s's WAV file fails: %s\n", name.c_str(), sound.name.c_str(), error.what());
                totals.differ++;
            }
            totals.silent++;
        }
    }

    // Each stream sound's stream, once for each file (StreamProblem). The Hokuto file keeps the stream by its file ID.
    // An AAC stream's loop lies in the sound's entry. ntdlib's model reads it there (AacStreamProblem).
    std::set<uint32_t> stream_files;
    std::optional<sar::ArchiveModel> model;
    for (std::size_t i = 0; i < opened->Sounds().size(); i++)
    {
        const sar::SoundInfo& sound = opened->Sounds()[i];
        if (sound.type != sar::SoundType::kStream || !stream_files.insert(sound.file_id).second)
        {
            continue;
        }

        const std::optional<std::vector<uint8_t>> theirs = StreamBeside(*opened, sound.file_id, path);
        const auto kept = file.native->streams.find(sound.file_id);
        if (!theirs)
        {
            if (kept != file.native->streams.end())
            {
                std::printf("%s: %s's stream exports without a file\n", name.c_str(), sound.name.c_str());
                totals.differ++;
            }
            totals.lacked_streams++;
            continue;
        }
        if (kept == file.native->streams.end())
        {
            std::printf("%s: %s's stream is missing from the file\n", name.c_str(), sound.name.c_str());
            totals.differ++;
            continue;
        }

        try
        {
            if (song::IsAdts(*theirs) && !model)
            {
                model = sar::ReadArchiveModel(opened->Bytes(), sar::Flavor::kCtr);
            }
            const std::string problem =
                song::IsAdts(*theirs)
                    ? (*kept->second.bytes != *theirs
                           ? std::string("'s file differs")
                           : AacStreamProblem(*kept->second.bytes, *model, static_cast<uint32_t>(i), totals))
                    : StreamProblem(*kept->second.bytes, *theirs, totals);
            if (!problem.empty())
            {
                std::printf("%s: %s%s\n", name.c_str(), sound.name.c_str(), problem.c_str());
                totals.differ++;
            }
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s's stream fails: %s\n", name.c_str(), sound.name.c_str(), error.what());
            totals.differ++;
        }
        totals.streams++;
    }

    std::printf(
        "%s: %zu items checked, and %zu waves as WAV files. %zu differ. %zu lack their files on both sides. %zu "
        "waves can't be decoded, and %zu have more than two channels. %zu names clash.\n",
        name.c_str(), totals.items, totals.waves, totals.differ, totals.missing, totals.undecoded, totals.wide,
        totals.clashes);
    std::printf(
        "%s: %zu wave sounds as WAV files, %zu of them at another rate than their wave's. %zu lack their file, their "
        "note or their wave on both sides.\n",
        name.c_str(), totals.wave_sounds, totals.pitched, totals.silent);
    std::printf("%s: %zu wave sound files, %zu of them under the names of their sound groups.\n", name.c_str(),
                totals.wave_sound_files, totals.named_files);
    std::printf(
        "%s: %zu banks' instruments checked, %zu instruments in all. %zu banks hold no instruments, and %zu can't be "
        "read. %zu need several SoundFonts. %zu take an attenuation in their SoundFont for a region above volume 127. "
        "The converter warned %zu times more.\n",
        name.c_str(), totals.banks, totals.instruments, totals.empty_banks, totals.unread_banks, totals.split_banks,
        totals.louder_banks, totals.bank_warnings);
    std::printf(
        "%s: %zu conversions of sequence files for another console and back. %zu take other biquad filters there, and "
        "%zu come back with other biquad filters. %zu come back with another version. %zu come back with other bytes "
        "where no command reaches. %zu reach a command that the 3DS lacks, and fail for it. %zu hold commands that "
        "overlap, and fail for every console.\n",
        name.c_str(), totals.converted, totals.biquads, totals.biquads_back, totals.versions, totals.unreached,
        totals.refused, totals.overlapping);
    std::printf(
        "%s: %zu conversions of banks, wave sound files and wave archives for another console and back, with %zu "
        "waves. %zu files and %zu waves take another version there. %zu waves come back with another original loop "
        "start, and %zu without the bytes after their last samples.\n",
        name.c_str(), totals.converted_files, totals.converted_waves, totals.file_versions, totals.wave_versions,
        totals.loop_starts, totals.trimmed);
    std::printf(
        "%s: %zu streams checked as files, as files of the other consoles and as WAV files, with %zu channels and %zu "
        "conversions. %zu of them are AAC. %zu lie outside the archive's folder or are missing from it.\n",
        name.c_str(), totals.streams, totals.stream_channels, totals.stream_conversions, totals.aac_streams,
        totals.lacked_streams);

    return totals.differ > 0 ? 1 : 0;
}
