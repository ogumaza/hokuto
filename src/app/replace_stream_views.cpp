// SPDX-License-Identifier: MIT

// The views of a stream or a WAV file that replaces a stream sound's stream or comes in as a new stream, on the
// 3DS, the Wii U, the Switch and the Wii.

#include "app/replace_stream_views.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/stream_file.h"
#include "ntdlib/rvl/snd/stream_sound.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/stream_file.h"
#include "ntdlib/sound_build.h"
#include "xsflib/files.h"

#include "app/export_warnings.h"
#include "app/item_views.h"
#include "app/replace_common.h"
#include "app/replace_view.h"
#include "edit/files.h"
#include "edit/new_items.h"
#include "edit/streams.h"
#include "format/archive.h"
#include "format/file.h"
#include "song/aac.h"
#include "song/items.h"
#include "song/streams.h"

namespace hokuto::app
{
namespace
{

using replace_common::ChannelsText;
using replace_common::EncodingText;
using replace_common::FillImportModels;
using replace_common::HoldsNames;
using replace_common::LengthText;
using replace_common::LoopText;
using replace_common::NameRefusal;
using replace_common::NameTaken;
using replace_common::Placed;
using replace_common::RateChangeText;
using replace_common::Refused;
using replace_common::SamplesText;
using replace_common::TakenNames;
using replace_common::WavWarnings;
using replace_common::WiiVersionText;

// A count of tracks in words: "1 track", "4 tracks".
std::string TracksText(std::size_t count)
{
    return std::to_string(count) + (count == 1 ? " track" : " tracks");
}

// The line that names `file_name`, a WAV file or a stream file (`wav`) for an archive of `flavor`, and gives what its
// stream `incoming` holds.
std::string StreamSource(const std::string& file_name, const edit::IncomingStream& incoming, bool wav,
                         ntdlib::sar::Flavor flavor)
{
    const std::optional<edit::ConvertedFile>& converted = incoming.converted;
    const std::string format = wav ? SamplesText(incoming)
                                   : song::ConsoleName(converted ? converted->flavor : flavor) + " stream, version " +
                                         song::VersionText(converted ? converted->flavor : flavor,
                                                           converted ? converted->version : incoming.version) +
                                         ", " + EncodingText(incoming.encoding);
    std::string text = file_name + ": " + format + ", " + ChannelsText(incoming.channels) + ", " +
                       Grouped(incoming.rate) + " Hz, " +
                       LengthText(incoming.frames + incoming.after_loop, incoming.rate);
    if (incoming.loop)
    {
        text += LoopText(incoming.loop_start, incoming.frames);
    }

    return text;
}

// Why a stream of `flavor` takes version `version` (edit::IncomingStreamVersion), in a sentence for the warnings.
std::string StreamVersionReason(uint32_t version, ntdlib::sar::Flavor flavor)
{
    if (flavor == ntdlib::sar::Flavor::kCtr)
    {
        return version == song::ExportVersions(song::ItemKind::kStream, flavor).file
                   ? "It's the oldest version used by 3DS games, or the version of the other streams in this file."
                   : "The other streams in this file use that version.";
    }
    if (flavor != ntdlib::sar::Flavor::kCafe)
    {
        return "Hokuto hasn't been tested with Switch streams.";
    }

    return version < 0x00040000 ? "Most Wii U games read stream files only up to that version."
                                : "The other streams in this file use that version or a later one.";
}

// The warnings of a WAV file's samples (`wav`), or of a stream file that goes in converted or written again, for a
// stream of `flavor`.
void StreamFormatWarnings(const edit::IncomingStream& incoming, bool wav, ntdlib::sar::Flavor flavor,
                          std::vector<std::string>& warnings)
{
    const std::string version = song::VersionText(flavor, incoming.version);
    const std::string reason = StreamVersionReason(incoming.version, flavor);
    if (wav)
    {
        WavWarnings(incoming, warnings, true);
        warnings.push_back("The stream is saved as version " + version + " in the NintendoWare layout. " + reason);
    }
    else if (incoming.converted && incoming.converted->flavor == flavor)
    {
        warnings.push_back("The stream is saved as version " + version + " with the same samples. " + reason);
    }
    else if (incoming.converted)
    {
        warnings.push_back("This is a " + song::ConsoleName(incoming.converted->flavor) + " file. It's converted to " +
                           song::ConsoleName(flavor) + " byte order (version " + version + ") with the same samples. " +
                           reason);
    }
}

// Adds the warnings about the other sounds that play the stream, and about where an export writes it. `play_end` ends
// the first warning and `path_end` the second.
template <typename Archive, typename Changes>
void SharedStreamWarnings(const Archive& archive, const Changes& changes, const std::string& play_end,
                          const std::string& path_end, std::vector<std::string>& warnings)
{
    if (!changes.sounds.empty())
    {
        std::vector<std::string> names;
        for (const uint32_t other : changes.sounds)
        {
            names.push_back(archive.Sounds()[other].name);
        }
        const bool one = names.size() == 1;
        warnings.push_back(NameList(names) + (one ? " uses" : " use") +
                           " the same stream and will also play the new one" + play_end);
    }
    if (const std::optional<std::filesystem::path> path = song::StreamPath(archive, changes.file))
    {
        warnings.push_back("Export writes the new stream next to the archive, at " + xsflib::Utf8(*path) + path_end);
    }
}

// Adds the warnings about the tracks of the sound and the channels that they use.
void WarnStreamTracks(const edit::StreamChanges& changes, const edit::IncomingStream& incoming,
                      const std::optional<std::vector<ntdlib::sar::StreamFile::Track>>& given,
                      std::vector<std::string>& warnings)
{
    std::size_t needed = 0;
    std::vector<std::string> tracks;
    for (const std::vector<uint8_t>& track : changes.tracks)
    {
        std::vector<std::string> channels;
        for (const uint8_t channel : track)
        {
            channels.push_back(std::to_string(channel + 1));
            needed = std::max<std::size_t>(needed, channel + 1u);
        }
        tracks.push_back(channels.empty()       ? std::string("no channels")
                         : channels.size() == 1 ? "channel " + channels.front()
                                                : "channels " + NameList(channels));
    }
    if (changes.tracks_in_file)
    {
        // Before version 2.3.0.0 of a 3DS archive, the stream file gives the tracks.
        const bool same = given && *given == changes.file_tracks;
        warnings.push_back(
            std::string("Sounds in this archive play the tracks in their stream files. ") +
            (!given ? "The tracks of the new stream are used."
             : same ? "The new stream keeps the " + TracksText(given->size()) + " of the current stream."
                    : "The new stream gets one track per channel pair: " + TracksText(given->size()) + "."));
        needed = 0;
    }
    else if (!tracks.empty())
    {
        std::string list;
        for (std::size_t t = 0; t < tracks.size(); t++)
        {
            list += (t == 0 ? "" : "; ") + ("track " + std::to_string(t + 1) + ": ") + tracks[t];
        }
        warnings.push_back("The sound keeps its " + TracksText(tracks.size()) + " (" + list + ").");
    }
    if (incoming.channels < needed)
    {
        warnings.push_back("The tracks of the sound use channels up to channel " + std::to_string(needed) +
                           ", but the new stream has only " + std::to_string(incoming.channels) +
                           ". Tracks with missing channels may fail in the game.");
    }
    if (incoming.channels > changes.table_channels)
    {
        warnings.push_back("The sound entry specifies " + std::to_string(changes.table_channels) +
                           (changes.table_channels == 1 ? " channel" : " channels") + ", but the new stream has " +
                           std::to_string(incoming.channels) +
                           ". The game allocates buffers only for the channels in the entry. Extra channels may "
                           "fail in the game.");
    }
}

// Adds the warnings that compare the new stream with the current one.
void PresentStreamWarnings(const edit::StreamChanges& changes, const edit::IncomingStream& incoming,
                           std::vector<std::string>& warnings)
{
    if (!changes.present)
    {
        warnings.push_back(
            "The project has no current stream for this sound. None was found next to the archive when it was "
            "imported.");
    }
    else
    {
        if (changes.loop && !incoming.loop)
        {
            warnings.push_back(
                "The current stream loops, but the new one doesn't. The sound stops at the end of the stream.");
        }
        else if (!changes.loop && incoming.loop)
        {
            warnings.push_back(
                "The new stream loops, but the current one doesn't. The sound plays until the game stops it.");
        }
        if (changes.channels != incoming.channels)
        {
            warnings.push_back("The current stream is " + ChannelsText(changes.channels) + ". The new one is " +
                               ChannelsText(incoming.channels) + ".");
        }
        if (changes.rate != incoming.rate)
        {
            warnings.push_back(RateChangeText(changes.rate, incoming.rate));
        }
    }
}

// The warning with the sizes of the current stream and the new one.
std::string StreamSizeWarning(const edit::StreamChanges& changes, const edit::IncomingStream& incoming)
{
    const std::string size = "The new stream is " + std::string(EncodingText(incoming.encoding)) + " (" +
                             SizeText(incoming.bytes.size()) + ").";
    return ((changes.present ? std::string("The current stream is ") + EncodingText(changes.encoding) + " (" +
                                   SizeText(changes.size) + "). "
                             : std::string()) +
            size + " Streams load from disc in blocks and don't count against the sound heap.");
}

// Adds the warnings about the encoding of the new AAC stream.
void AacEncodingWarnings(const edit::AacChanges& changes, const edit::IncomingAac& incoming,
                         std::vector<std::string>& warnings)
{
    if (incoming.encoded)
    {
        // Audio Toolbox keeps the rate constant. Media Foundation's encoder aims at it (song::EncodesConstantRate).
        warnings.push_back(
            std::string("The system encoder encodes the new stream as AAC-LC at ") +
            (song::EncodesConstantRate() ? "a constant " : "a target of ") + std::to_string(changes.bit_rate / 1000) +
            (changes.present ? " kbit/s (closest to the current stream)." : " kbit/s (the rate the game uses).") +
            " AAC is lossy.");
        if (incoming.rate != incoming.stream_rate)
        {
            warnings.push_back("The file is " + Grouped(incoming.rate) + " Hz. The new stream uses " +
                               Grouped(incoming.stream_rate) +
                               (changes.present ? " Hz to match the current stream."
                                                : " Hz because the encoder supports only 44,100 and 48,000 Hz."));
        }
        if (incoming.channels != incoming.stream_channels)
        {
            warnings.push_back(
                incoming.stream_channels == 2
                    ? std::string("The file is mono. It's copied to both channels of the new stream.")
                    : std::string("The file is stereo. It's mixed down to mono because ") +
                          (changes.present ? "the current stream is mono." : "the sound entry specifies one channel."));
        }
        warnings.push_back("The encoder adds " + Grouped(incoming.delay) + " samples of silence at the start." +
                           (incoming.loop ? " The loop points move to match." : ""));
        if (incoming.after_loop > 0)
        {
            warnings.push_back((incoming.after_loop == 1
                                    ? std::string("The sample after the loop end is")
                                    : "The " + Grouped(incoming.after_loop) + " samples after the loop end are") +
                               " dropped because the stream plays only up to its loop end.");
        }
    }
    else if (incoming.present_loop)
    {
        warnings.push_back(changes.present ? "AAC has no loop points. The stream keeps its current loop."
                                           : "AAC has no loop points. The stream uses the loop in the sound entry.");
    }
}

// Adds the warnings about the lengths and the loops of the current AAC stream and the new one.
void AacLoopWarnings(const edit::AacChanges& changes, const edit::IncomingAac& incoming,
                     std::vector<std::string>& warnings)
{
    const auto loop_text = [](const std::optional<song::AacLoop>& loop)
    {
        return loop ? "a loop from sample " + Grouped(loop->start) + " to " + Grouped(loop->end)
                    : std::string("no loop");
    };
    warnings.push_back("The new stream has " + LengthText(incoming.length, incoming.stream_rate) + " with " +
                       loop_text(incoming.loop) + "." +
                       (changes.present ? " The current one has " + LengthText(changes.length, changes.rate) +
                                              " with " + loop_text(changes.loop) + "."
                                        : " The sound entry currently specifies " + loop_text(changes.loop) + "."));
    if (changes.loop && !incoming.loop)
    {
        warnings.push_back(std::string(changes.present
                                           ? "The current stream loops, but the new one doesn't."
                                           : "The sound entry specifies a loop, but the new stream doesn't loop.") +
                           " The sound stops at the end of the stream.");
    }
    else if (!changes.loop && incoming.loop)
    {
        warnings.push_back(changes.present
                               ? "The new stream loops, but the current one doesn't. The sound plays until the "
                                 "game stops it."
                               : "The new stream loops, and its loop is written to the sound entry. The sound "
                                 "plays until the game stops it.");
    }
}

// The warning that lists the tracks of the new sound with their channels.
std::string NewStreamTracksWarning(const edit::NewStream& made)
{
    // The tracks, numbered from 1 as the stream's properties number them, with their channels. Before version 2.3.0.0
    // of a 3DS archive, the stream file holds them.
    std::vector<ntdlib::sar::StreamTrackInfo> tracks = made.details.tracks;
    if (made.tracks_in_file)
    {
        const ntdlib::sar::StreamFile stream = ntdlib::sar::StreamFile::Read(made.bytes);
        for (const ntdlib::sar::StreamFile::Track& track :
             stream.tracks.value_or(std::vector<ntdlib::sar::StreamFile::Track>()))
        {
            tracks.push_back({.channels = track.channels, .volume = track.volume, .pan = track.pan});
        }
    }
    std::string text = "The new sound plays " + TracksText(tracks.size()) +
                       (made.file_tracks ? std::string(" from the stream file.") : std::string(".")) +
                       (made.tracks_in_file && !made.file_tracks
                            ? std::string(" Sounds in this archive play the tracks in their stream files. The new "
                                          "stream gets one track per channel pair.")
                            : std::string());
    for (std::size_t t = 0; t < tracks.size(); t++)
    {
        text += " Track " + std::to_string(t + 1) + ": " + ChannelsOf(tracks[t].channels) + ", volume " +
                std::to_string(tracks[t].volume) + ", pan " + PanName(tracks[t].pan) + ".";
    }

    return text;
}

} // namespace

ReplaceView MakeStreamReplaceView(const ReplaceSource& source, const edit::IncomingStream& incoming, bool wav,
                                  const edit::StreamChanges& changes,
                                  const std::optional<std::vector<ntdlib::sar::StreamFile::Track>>& given,
                                  std::optional<format::File> result, std::string error)
{
    // The sound, and the WAV file or the stream file that goes in the place of its stream.
    const ntdlib::sar::SoundArchive& archive = *source.played.Sar();
    const ntdlib::sar::Flavor flavor = archive.ReadOptions().flavor;
    const std::string sound = archive.Sounds().at(source.sound).name;
    ReplaceView view;
    view.sequence = false;
    view.title = "Replace " + sound;
    view.heading = wav ? "WAV file" : "Stream file";
    view.source = StreamSource(source.file_name, incoming, wav, flavor);
    if (wav)
    {
        view.encoding = incoming.encoding;
    }
    view.wave = incoming.bytes;
    view.result = std::move(result);
    view.error = std::move(error);

    // The other sounds that play the stream, and where an export writes it.
    SharedStreamWarnings(archive, changes, ".", ". The game looks for it there.", view.warnings);

    // The sound plays the stream's channels as tracks. The game sets up buffers for the channels that the sound's entry
    // gives. The lines count the channels from 1. The stream's properties count them the same way.
    WarnStreamTracks(changes, incoming, given, view.warnings);

    // The present stream.
    PresentStreamWarnings(changes, incoming, view.warnings);

    // A WAV file's samples, or a stream file of the other console.
    StreamFormatWarnings(incoming, wav, flavor, view.warnings);

    // The sizes. A stream plays from the disc, a block at a time.
    view.warnings.push_back(StreamSizeWarning(changes, incoming));

    return view;
}

ReplaceView MakeAacStreamReplaceView(const ReplaceSource& source, const edit::IncomingAac& incoming,
                                     const edit::AacChanges& changes, std::optional<format::File> result,
                                     std::string error)
{
    const ntdlib::sar::SoundArchive& archive = *source.played.Sar();
    const std::string sound = archive.Sounds().at(source.sound).name;

    ReplaceView view;
    view.sequence = false;
    view.title = "Replace " + sound;
    view.heading = incoming.encoded ? "File" : "AAC file";
    view.source = source.file_name + ": " + ChannelsText(incoming.channels) + ", " + Grouped(incoming.rate) + " Hz" +
                  (incoming.encoded ? "" : ", AAC");
    view.result = std::move(result);
    view.error = std::move(error);

    // What the new stream is, and how it came to be.
    if (!changes.present)
    {
        view.warnings.push_back(
            "The sound entry specifies an AAC stream that's missing from the project. The new stream is encoded as "
            "AAC.");
    }
    AacEncodingWarnings(changes, incoming, view.warnings);
    AacLoopWarnings(changes, incoming, view.warnings);

    // The sound's entry marks the stream as AAC, and gives the loop or none. Its new values require the model form. The
    // warnings above already say that neither stream loops.
    const bool loops = changes.loop || incoming.loop;
    view.warnings.push_back(std::string(loops ? "The loop is stored in the sound entry of the archive. " : "") +
                            "To store the new values, the project is converted to the model form. Export then lays "
                            "out the archive the way NintendoWare's tools do.");
    SharedStreamWarnings(archive, changes, incoming.loop ? " with its loop." : ".", ". The game looks for it there.",
                         view.warnings);
    if (incoming.encoded)
    {
        view.warnings.push_back(
            "Game AAC files can have a CRC in each frame header. The system encoder doesn't write one. Streams like "
            "this haven't been tested on a 3DS.");
    }
    const auto [present, added] = SizeTexts(changes.size, incoming.bytes.size());
    view.warnings.push_back((changes.present ? "The current stream is " + present + ". The new one is "
                                             : std::string("The new stream is ")) +
                            added + ". Streams load from disc in blocks and don't count against the sound heap.");

    return view;
}

ReplaceView MakeStreamImportView(const ReplaceSource& source, const edit::IncomingStream& incoming, bool wav,
                                 const std::string& name, const std::optional<edit::NewStream>& made,
                                 const std::string& error)
{
    // The WAV file or the stream file, the name and the sounds to copy.
    const ntdlib::sar::SoundArchive& archive = *source.played.Sar();
    const ntdlib::sar::Flavor flavor = archive.ReadOptions().flavor;
    ReplaceView view;
    view.title = "Import Stream";
    view.action = "Import";
    view.import = true;
    view.sequence = false;
    view.heading = wav ? "WAV file" : "Stream file";
    view.source = StreamSource(source.file_name, incoming, wav, flavor);
    FillImportModels(view, source, archive, name);
    if (wav)
    {
        view.encoding = incoming.encoding;
    }
    view.wave = made ? made->bytes : incoming.bytes;

    // The name, and the stream. The import makes the same checks.
    const format::NativeTables& tables = *source.file->native->tables;
    const auto taken = [&tables, &archive](const std::string& each)
    {
        return NameTaken(TakenNames(tables, &archive), each);
    };
    if (!HoldsNames(source))
    {
        view.error = edit::NoNamesReason("sound");
    }
    else if (const std::optional<std::string> refusal = NameRefusal(name, "sound", taken))
    {
        view.error = *refusal;
    }
    else if (!made)
    {
        view.error = error;
    }
    view.ready = view.error.empty();
    if (!view.ready)
    {
        return view;
    }

    // The new sound's number, and what it copies.
    const ntdlib::sar::SoundInfo& model = archive.Sounds()[source.sound];
    view.warnings.push_back("The new sound is number " + std::to_string(archive.Sounds().size()) +
                            Placed(archive.Sounds().size(), "sounds") +
                            ". The game plays it only if the game code requests that number or name.");
    if (model.type != ntdlib::sar::SoundType::kStream)
    {
        view.warnings.push_back(model.name +
                                " isn't a stream sound. The new sound uses its player, volume, pan, priorities and "
                                "3D settings. Its tracks use full volume, centre pan and full main send.");
    }

    view.warnings.push_back(NewStreamTracksWarning(*made));
    view.warnings.push_back("Export writes the new stream next to the archive, at " + made->path +
                            ". The game looks for it there.");

    // A WAV file's samples, or a stream file of the other console, and the size.
    StreamFormatWarnings(incoming, wav, flavor, view.warnings);
    view.warnings.push_back("The new stream is " + std::string(EncodingText(incoming.encoding)) + " (" +
                            SizeText(incoming.bytes.size()) +
                            "). Streams load from disc in blocks and don't count against the sound heap.");

    return view;
}

bool WiiNameTaken(const ntdlib::rsar::SoundArchive& archive, const std::string& name)
{
    const auto holds = [&name](const auto& items)
    {
        return std::ranges::any_of(items, [&name](const auto& each) { return each.name == name; });
    };
    return holds(archive.Sounds()) || holds(archive.Banks()) || holds(archive.Players());
}

namespace
{

// The line that names `file_name`, a WAV file or a stream file (`wav`) for an archive of the Wii, and gives what its
// stream `incoming` holds (StreamSource).
std::string WiiStreamSource(const std::string& file_name, const edit::IncomingStream& incoming, bool wav)
{
    const std::optional<edit::ConvertedFile>& converted = incoming.converted;
    std::string format;
    if (wav)
    {
        format = SamplesText(incoming);
    }
    else if (converted)
    {
        format = song::ConsoleName(converted->flavor) + " stream, version " +
                 song::VersionText(converted->flavor, converted->version) + ", " + EncodingText(incoming.encoding);
    }
    else
    {
        format = "Wii stream, version " + WiiVersionText(incoming.version) + ", " + EncodingText(incoming.encoding);
    }
    std::string text = file_name + ": " + format + ", " + ChannelsText(incoming.channels) + ", " +
                       Grouped(incoming.rate) + " Hz, " +
                       LengthText(incoming.frames + incoming.after_loop, incoming.rate);
    if (incoming.loop)
    {
        text += LoopText(incoming.loop_start, incoming.frames);
    }

    return text;
}

// The warnings of the tracks that stream sound `info` of the Wii's `archive` plays of `stream` on `build` of NW4R SND
// (ntdlib::rvl::snd::StrmPlayer::TracksOf). The tracks count from 0. So do the channels.
void WiiStreamTrackWarnings(const ntdlib::rsar::SoundArchive& archive, const ntdlib::rsar::SoundInfo& info,
                            const ntdlib::SndBuild& build, const ntdlib::rsar::StreamFile& stream,
                            std::vector<std::string>& warnings)
{
    using ntdlib::rvl::snd::StrmPlayer;
    if (!build.SinceMarch2008())
    {
        warnings.emplace_back(
            "The NW4R SND build of this game plays only the first two channels of a stream, at full volume and "
            "centred. It ignores the tracks.");
        return;
    }
    const std::optional<std::vector<StrmPlayer::Track>> played =
        StrmPlayer::TracksOf(build, info, archive.Version(), stream);
    if (!played)
    {
        warnings.push_back("The sound entry of " + info.name +
                           " uses a track or channel that's missing from the new stream. The game won't play any of "
                           "its tracks.");
        return;
    }

    // The tracks that play, and the channels that the player keeps.
    std::string text;
    std::size_t playing = 0;
    for (std::size_t t = 0; t < played->size(); t++)
    {
        const StrmPlayer::Track& track = (*played)[t];
        if (!track.active)
        {
            continue;
        }
        text += " Track " + std::to_string(t + 1) + ": " + ChannelsOf(track.channels) + ", volume " +
                std::to_string(track.volume) + ", pan " + PanName(track.pan) + ".";
        playing++;
    }
    warnings.push_back(info.name + " plays " + TracksText(playing) + "." + text);
    const int kept = StrmPlayer::ChannelsKept(build, info, archive.Version(), stream);
    if (kept >= 0 && static_cast<std::size_t>(kept) < stream.channels.size())
    {
        warnings.push_back("The sound entry specifies " + std::to_string(kept) +
                           (kept == 1 ? " channel" : " channels") + ". The game uses only the first " +
                           std::to_string(kept) + " of the " + std::to_string(stream.channels.size()) +
                           " channels in the new stream.");
    }
}

// The warnings of a WAV file's samples (`wav`), or of a stream file of a later console that goes in converted, for a
// stream of the Wii.
void WiiStreamFormatWarnings(const edit::IncomingStream& incoming, bool wav, std::vector<std::string>& warnings)
{
    if (wav)
    {
        WavWarnings(incoming, warnings, true);
        warnings.emplace_back("The stream is saved as version 1.0 in the NW4R layout.");
    }
    else if (incoming.converted)
    {
        warnings.push_back("This is a " + song::ConsoleName(incoming.converted->flavor) +
                           " file. It's converted to a Wii stream file (version 1.0) with the same samples.");
    }
}

} // namespace

ReplaceView MakeWiiStreamReplaceView(const ReplaceSource& source, const edit::IncomingStream& incoming, bool wav,
                                     const edit::StreamChanges& changes, std::optional<format::File> result,
                                     std::string error)
{
    // The sound, and the WAV file or the stream file that goes in the place of its stream.
    const ntdlib::rsar::SoundArchive& archive = *source.played.Wii();
    const ntdlib::rsar::SoundInfo& info = archive.Sounds().at(source.sound);
    ReplaceView view;
    view.sequence = false;
    view.title = "Replace " + info.name;
    view.heading = wav ? "WAV file" : "Stream file";
    view.source = WiiStreamSource(source.file_name, incoming, wav);
    if (wav)
    {
        view.encoding = incoming.encoding;
    }
    view.wave = incoming.bytes;
    view.result = std::move(result);
    view.error = std::move(error);

    // The other sounds that play the stream, and where an export writes it.
    SharedStreamWarnings(archive, changes, ".",
                         ". The game looks for it there. Its size is written to the file table of the archive.",
                         view.warnings);

    // The sound plays the tracks of its stream file. A WAV file, or a stream file without tracks, takes the present
    // stream's tracks where its channels allow.
    ntdlib::rsar::StreamFile stream;
    try
    {
        stream = ntdlib::rsar::StreamFile::Read(incoming.bytes);
    }
    catch (const std::exception&)
    {
        return view;
    }
    std::string tracks = "The sound plays the tracks in its stream file. ";
    if (changes.present && stream.tracks == changes.wii_tracks)
    {
        tracks += "The new stream keeps the " + TracksText(stream.tracks.size()) + " of the current stream.";
    }
    else
    {
        tracks += "The new stream has " + TracksText(stream.tracks.size()) + ".";
        if (changes.present)
        {
            tracks += " The current one has " + TracksText(changes.wii_tracks.size()) + ".";
        }
    }
    view.warnings.push_back(tracks);
    WiiStreamTrackWarnings(archive, info, format::BuildOfWiiArchive(*source.file->native), stream, view.warnings);

    // The present stream.
    PresentStreamWarnings(changes, incoming, view.warnings);

    // A WAV file's samples, or a stream file of a later console, and the sizes. A stream plays from the disc.
    WiiStreamFormatWarnings(incoming, wav, view.warnings);
    view.warnings.push_back(StreamSizeWarning(changes, incoming));

    return view;
}

ReplaceView MakeWiiStreamImportView(const ReplaceSource& source, const edit::IncomingStream& incoming, bool wav,
                                    const std::string& name, const std::optional<edit::NewStream>& made,
                                    const std::string& error)
{
    // The WAV file or the stream file, the name and the sounds to copy.
    const ntdlib::rsar::SoundArchive& archive = *source.played.Wii();
    ReplaceView view;
    view.title = "Import Stream";
    view.action = "Import";
    view.import = true;
    view.sequence = false;
    view.heading = wav ? "WAV file" : "Stream file";
    view.source = WiiStreamSource(source.file_name, incoming, wav);
    FillImportModels(view, source, archive, name);
    if (wav)
    {
        view.encoding = incoming.encoding;
    }
    view.wave = made ? made->bytes : incoming.bytes;

    // The name, and the stream. No two items of a Wii archive share a name. The import checks the groups' names too:
    // ntdlib's reader of the archive doesn't give them.
    const auto taken = [&archive](const std::string& each)
    {
        return WiiNameTaken(archive, each);
    };
    if (const std::optional<std::string> refusal = NameRefusal(name, "sound", taken))
    {
        view.error = *refusal;
    }
    else if (!made || !made->wii_sound)
    {
        view.error = error;
    }
    view.ready = view.error.empty();
    if (!view.ready)
    {
        return Refused(source, std::move(view));
    }

    // The new sound's number, and what it copies.
    const ntdlib::rsar::SoundInfo& model = archive.Sounds()[source.sound];
    view.warnings.push_back("The new sound is number " + std::to_string(archive.Sounds().size()) +
                            Placed(archive.Sounds().size(), "sounds") +
                            ". The game plays it only if the game code requests that number or name.");
    if (model.type != ntdlib::rsar::SoundType::kStream)
    {
        view.warnings.push_back(model.name +
                                " isn't a stream sound. The new sound uses its player, volume, priority, pan and 3D "
                                "settings.");
    }

    // The tracks of the stream file and the tracks that the game plays. The new sound's entry names every track of the
    // stream file.
    const ntdlib::rsar::StreamFile stream = ntdlib::rsar::StreamFile::Read(made->bytes);
    const std::size_t channels = stream.channels.size();
    view.warnings.push_back("The sound entry of the new sound lists all " + std::to_string(channels) +
                            (channels == 1 ? " channel and " : " channels and ") + TracksText(stream.tracks.size()) +
                            " of the stream." +
                            (made->file_tracks ? std::string(" The tracks come from the stream file.")
                                               : std::string(" The new stream gets one track per channel pair.")));
    WiiStreamTrackWarnings(archive, *made->wii_sound, format::BuildOfWiiArchive(*source.file->native), stream,
                           view.warnings);
    view.warnings.push_back("Export writes the new stream next to the archive, at " + made->path +
                            ". The game looks for it there. The stream isn't in any group.");

    // A WAV file's samples, or a stream file of a later console, and the size.
    WiiStreamFormatWarnings(incoming, wav, view.warnings);
    view.warnings.push_back("The new stream is " + std::string(EncodingText(incoming.encoding)) + " (" +
                            SizeText(incoming.bytes.size()) +
                            "). Streams load from disc in blocks and don't count against the sound heap.");

    return Refused(source, std::move(view));
}

} // namespace hokuto::app
