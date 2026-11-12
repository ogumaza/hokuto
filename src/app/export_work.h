// SPDX-License-Identifier: MIT

// The bytes of a stream sound's stream, for the exports, the previews and the properties.

#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "app/item_views.h"
#include "format/file.h"
#include "song/streams.h"

namespace hokuto::app
{

// The bytes of the stream that stream sound `sound` of `archive` plays, from `file`, the file that builds the archive.
// `archive` is an archive of the Wii or of a later console. Throws std::runtime_error with a message for the user that
// names the stream's file when the file holds no such stream.
template <class Archive>
const std::vector<uint8_t>& StreamBytes(const format::File& file, const Archive& archive, uint32_t sound)
{
    const std::optional<uint32_t> id = song::StreamFileOf(archive, sound);
    if (id && file.native)
    {
        const auto found = file.native->streams.find(*id);
        if (found != file.native->streams.end())
        {
            return *found->second.bytes;
        }
    }

    if (sound >= archive.Sounds().size())
    {
        throw std::runtime_error("The archive has no sound " + std::to_string(sound) + ".");
    }

    throw std::runtime_error(MissingStreamText(archive.Sounds()[sound].name, song::StreamFileName(archive, sound)));
}

} // namespace hokuto::app
