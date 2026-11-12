// SPDX-License-Identifier: MIT

// The views of the Replace window for a stream or a WAV file on the 3DS, the Wii U, the Switch and the Wii.

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/sar/stream_file.h"

#include "app/replace_view.h"
#include "edit/new_items.h"
#include "edit/streams.h"
#include "format/file.h"

namespace hokuto::app
{

// The view of the stream of a stream file or a WAV file (`wav`) in the place of the stream of stream sound
// `source.sound`. `incoming` gives the new stream, in a WAV file's encoding chosen in the window's menu. `changes`
// gives the present stream and what plays it. `given` holds the tracks that the new stream took for the sound
// (edit::SoundTracksFor), if any. The window's Play button plays the new stream. Without a result, `error` says why the
// stream can't go in.
ReplaceView MakeStreamReplaceView(const ReplaceSource& source, const edit::IncomingStream& incoming, bool wav,
                                  const edit::StreamChanges& changes,
                                  const std::optional<std::vector<ntdlib::sar::StreamFile::Track>>& given,
                                  std::optional<format::File> result, std::string error);

// The view of `incoming`, an AAC stream, in the place of the AAC stream of 3DS stream sound `source.sound`. `changes`
// gives the present stream and what plays it. The window's Play button plays the new stream. Without a result, `error`
// says why the stream can't go in.
ReplaceView MakeAacStreamReplaceView(const ReplaceSource& source, const edit::IncomingAac& incoming,
                                     const edit::AacChanges& changes, std::optional<format::File> result,
                                     std::string error);

// The view of the stream of a stream file or a WAV file (`wav`) as new stream sound `name`, a copy of sound
// `source.sound`, one of `source.models` (File > Import > Stream…). `incoming` gives the stream, in a WAV file's
// encoding chosen in the window's menu. `made` gives the new sound's stream, its path and its details
// (edit::MakeNewStream). Without it, `error` says why the stream can't go in. The window's Play button plays the stream
// as the new sound mixes its tracks. The view holds no result.
ReplaceView MakeStreamImportView(const ReplaceSource& source, const edit::IncomingStream& incoming, bool wav,
                                 const std::string& name, const std::optional<edit::NewStream>& made,
                                 const std::string& error);

// Whether a sound, a bank or a player of the Wii's `archive` holds `name`. ntdlib's reader of the archive doesn't give
// the groups' names.
bool WiiNameTaken(const ntdlib::rsar::SoundArchive& archive, const std::string& name);

// The same views for a stream sound of the Wii, `source.sound` of `source.played.Wii()`. The sound plays the tracks of
// its stream file that its entry names, on the file's build of NW4R SND. A WAV file, or a stream file of a later
// console without tracks, takes the present stream's tracks where its channels allow (edit::ReadIncomingWiiStreamFile
// and edit::ReadIncomingWiiStreamWav). The views say which tracks the game plays. An export writes the stream beside
// the archive, and the archive's file table takes its size.
ReplaceView MakeWiiStreamReplaceView(const ReplaceSource& source, const edit::IncomingStream& incoming, bool wav,
                                     const edit::StreamChanges& changes, std::optional<format::File> result,
                                     std::string error);
ReplaceView MakeWiiStreamImportView(const ReplaceSource& source, const edit::IncomingStream& incoming, bool wav,
                                    const std::string& name, const std::optional<edit::NewStream>& made,
                                    const std::string& error);

} // namespace hokuto::app
