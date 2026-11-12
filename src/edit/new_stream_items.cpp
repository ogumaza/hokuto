// SPDX-License-Identifier: MIT

// New stream sounds in the archives of the 3DS, the Wii U and the Switch (edit/new_items.h).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/sar/archive_model.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/stream_file.h"
#include "ntdlib/sar/tables.h"
#include "seqlib/file_name.h"
#include "xsflib/ascii.h"

#include "edit/files_internal.h"
#include "edit/new_items.h"
#include "edit/new_items_parts.h"
#include "format/archive.h"
#include "format/file.h"
#include "format/kinds.h"
#include "song/items.h"

namespace hokuto::edit
{

namespace
{

namespace sar = ntdlib::sar;
using new_items_parts::CheckName;
using new_items_parts::RebuiltArchive;
using new_items_parts::TablesOf;

// The most tracks and channels that the 3DS's player of stream sounds plays (Pokémon X's code.bin, 0x180898).
constexpr std::size_t kCtrMostTracks = 4;
constexpr std::size_t kCtrMostChannels = 8;

// Whether the file table of `archive` names `path` outside the archive, in any case, with either kind of slash.
bool PathTaken(const sar::SoundArchive& archive, const std::string& path)
{
    const auto plain = [](std::string text)
    {
        std::ranges::replace(text, '\\', '/');
        return xsflib::AsciiLower(text);
    };
    const std::string wanted = plain(path);
    for (uint32_t file = 0; file < archive.FileCount(); ++file)
    {
        if (!archive.ExternalPath(file).empty() && plain(archive.ExternalPath(file)) == wanted)
        {
            return true;
        }
    }

    return false;
}

// The path of a new stream file named `name` in `archive`: a number after the name while the file table names it. A
// stream of DSP-ADPCM gets its extension, and `ctr` selects the extension of the 3DS (MakeNewStream).
std::string StreamPath(const sar::SoundArchive& archive, const std::string& name, const sar::StreamFile& file, bool ctr)
{
    std::string path;
    const std::string stem = seqlib::SafeFileName(name.empty() ? std::string("STREAM") : name);
    const std::string extension =
        std::string(file.encoding == sar::WaveEncoding::kDspAdpcm ? ".dspadpcm" : "") + (ctr ? ".bcstm" : ".bfstm");
    path = "stream/" + stem + extension;
    for (int number = 2; PathTaken(archive, path); ++number)
    {
        path = "stream/" + stem + "_" + std::to_string(number) + extension;
    }

    return path;
}

// The tracks that a stream sound of `ctr` or another console can play from `file`: its tracks, or a track for each pair
// of its channels (MakeNewStream). `stream` notes a file with tracks. Throws std::invalid_argument with a message for
// the user for too many tracks or channels.
std::vector<sar::StreamFile::Track> StreamTracks(const sar::StreamFile& file, bool ctr, NewStream& stream)
{
    std::vector<sar::StreamFile::Track> tracks;
    if (file.tracks && !file.tracks->empty())
    {
        tracks = *file.tracks;
        stream.file_tracks = true;
    }
    else
    {
        for (std::size_t c = 0; c < file.channels.size(); c += 2)
        {
            sar::StreamFile::Track& track = tracks.emplace_back();
            track.channels.push_back(static_cast<uint8_t>(c));
            if (c + 1 < file.channels.size())
            {
                track.channels.push_back(static_cast<uint8_t>(c + 1));
            }
        }
    }

    const std::string held =
        std::to_string(file.channels.size()) + " channels" +
        (file.tracks ? " in " + std::to_string(tracks.size()) + (tracks.size() == 1 ? " track." : " tracks.") : ".");
    if (ctr && tracks.size() > kCtrMostTracks)
    {
        throw std::invalid_argument("A 3DS stream sound can have at most 4 tracks. The stream has " + held);
    }
    if (tracks.size() > 8 || file.channels.size() > 255)
    {
        throw std::invalid_argument("A stream sound can have at most 8 tracks. The stream has " + held);
    }

    for (std::size_t t = 0; ctr && t < tracks.size(); ++t)
    {
        if (tracks[t].channels.size() > 2)
        {
            throw std::invalid_argument("A track of a 3DS stream sound can have at most 2 channels. Track " +
                                        std::to_string(t) + " of the stream has " +
                                        std::to_string(tracks[t].channels.size()) + ".");
        }
    }

    if (ctr && file.channels.size() > kCtrMostChannels)
    {
        throw std::invalid_argument("A 3DS stream sound can have at most 8 channels. The stream has " + held);
    }

    return tracks;
}

// Fills `stream.details` with the settings of each of `tracks`: the track of sound `model` of `archive` in its place,
// and the file's volume, pan, span and flags (MakeNewStream).
void TrackSettings(const sar::SoundArchive& archive, uint32_t model, const std::vector<sar::StreamFile::Track>& tracks,
                   NewStream& stream)
{
    sar::StreamSoundInfo& details = stream.details;
    const sar::SoundInfo& copied = archive.Sounds()[model];
    const bool stream_model = copied.type == sar::SoundType::kStream;
    if (stream_model)
    {
        details.pitch = copied.stream.pitch;
        details.main_send = copied.stream.main_send;
        details.aux_sends = copied.stream.aux_sends;
    }

    for (std::size_t t = 0; t < tracks.size(); ++t)
    {
        sar::StreamTrackInfo track;
        if (stream_model && t < copied.stream.tracks.size())
        {
            track = copied.stream.tracks[t];
        }

        if (stream.file_tracks)
        {
            track.volume = tracks[t].volume;
            track.pan = tracks[t].pan;
            track.span = tracks[t].span;
            track.flags = tracks[t].flags;
        }

        track.channels = tracks[t].channels;
        details.tracks.push_back(std::move(track));
    }
}

} // namespace

NewStream MakeNewStream(const sar::SoundArchive& archive, uint32_t model, const std::string& name,
                        const IncomingStream& incoming)
{
    if (model >= archive.Sounds().size())
    {
        throw std::invalid_argument("The sound to copy doesn't exist in the archive.");
    }

    const bool ctr = archive.ReadOptions().flavor == sar::Flavor::kCtr;
    if (ctr && !sar::LaysOutStreamSounds(archive.Version()))
    {
        throw std::invalid_argument("Hokuto can't add a stream sound to a 3DS archive of version " +
                                    song::VersionText(sar::Flavor::kCtr, archive.Version()) +
                                    ". The layout of stream sounds in this version is unknown.");
    }

    sar::StreamFile file;
    try
    {
        file = sar::StreamFile::Read(incoming.bytes);
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Can't read the stream. ") + format::AsSentence(error.what()));
    }

    if (file.channels.empty())
    {
        throw std::invalid_argument("The stream has no channels.");
    }

    // The path: a number after the name while the file table names it.
    NewStream stream;
    stream.bytes = incoming.bytes;
    stream.path = StreamPath(archive, name, file, ctr);

    // The tracks: the file's, or a track for each pair of channels.
    const std::vector<sar::StreamFile::Track> tracks = StreamTracks(file, ctr, stream);

    // Where the sound plays the tracks of its stream file, the details give only the tracks that play and the count of
    // channels. A stream file without tracks takes those of the pairs of its channels.
    sar::StreamSoundInfo& details = stream.details;
    details.channel_count = static_cast<uint16_t>(file.channels.size());
    details.valid_tracks = static_cast<uint16_t>((1u << tracks.size()) - 1);
    if (TracksInFiles(archive))
    {
        stream.tracks_in_file = true;
        if (!stream.file_tracks)
        {
            file.tracks = tracks;
            try
            {
                stream.bytes = file.Write();
            }
            catch (const std::exception& error)
            {
                throw std::invalid_argument(std::string("Can't write the tracks into the stream file. ") +
                                            format::AsSentence(error.what()));
            }
        }

        return stream;
    }

    // Each track's settings: the model's track in its place, and the file's volume, pan, span and flags.
    TrackSettings(archive, model, tracks, stream);

    return stream;
}

format::File AddStreamSound(const format::File& file, uint32_t model, const std::string& name, const NewStream& stream)
{
    const format::NativeTables& tables = TablesOf(file, "sound");
    const sar::Flavor flavor = FlavorOf(*file.native);
    if (model >= tables.sounds.size())
    {
        throw std::invalid_argument("The sound to copy doesn't exist in the archive.");
    }

    // The archive that the file exports. Its names mustn't hold the new name, and its file table mustn't name the path.
    const std::vector<uint8_t> archive = format::ExportArchive(file);
    const sar::Tables blocks = sar::TablesOf(archive, flavor);
    CheckName(blocks.strings, flavor, name, "sound", true);
    if (PathTaken(sar::SoundArchive::Load(archive, StrictOptions(flavor)), stream.path))
    {
        throw std::invalid_argument("The archive's file table already contains " + stream.path + ".");
    }

    // The archive with the new file and the new sound, and the file that it makes. The file keeps the stream.
    format::File result;
    try
    {
        const sar::ArchiveWithFile with = sar::WithNewExternalFile(archive, flavor, stream.path);
        const sar::NewStreamSound sound{.model = model, .name = name, .file = with.file, .stream = stream.details};
        result = RebuiltArchive(file, sar::WithNewStreamSound(with.archive, flavor, sound));
        const auto keep_stream = [&stream, id = with.file](format::NativeArchive& native)
        {
            native.streams[id] = format::NativeStream{stream.bytes, false};
        };
        result.native.Edit(keep_stream);
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Can't add a stream sound to this archive. ") +
                                    format::AsSentence(error.what()));
    }

    if (!result.native->tables || result.native->tables->sounds.size() != tables.sounds.size() + 1)
    {
        throw std::invalid_argument("Can't read the archive tables after adding the sound.");
    }

    return result;
}

} // namespace hokuto::edit
