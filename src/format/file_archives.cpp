// SPDX-License-Identifier: MIT

// The binary form of a file with more than one archive (format/project.h). The file's ENGN chunk gives engine 0, the
// mixing mode and the archive in front. MIXR, TAGS with the file's tags and the chunks that Hokuto doesn't read stand
// beside an ARCH chunk for each archive. Each ARCH chunk contains its archive as a file in the binary form.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/binary.h"

#include "format/codes.h"
#include "format/file.h"
#include "format/file_parts.h"
#include "format/model.h"
#include "format/project.h"
#include "format/text.h"

namespace hokuto::format
{
namespace
{

using file_parts::ChunksOf;
using file_parts::Damaged;
using file_parts::In;
using file_parts::kArchivesEngine;
using file_parts::kEngineChunk;
using file_parts::kMixerChunk;
using file_parts::kTagsChunk;
using file_parts::Out;

// Refuses the chunks of `bytes`, the body of an ARCH chunk, that a file keeps outside its archives: a file of more than
// one archive, MIXR and the model form.
void CheckArchiveChunks(std::span<const uint8_t> bytes)
{
    for (const auto& [id, chunk] : ChunksOf(bytes))
    {
        if (id == kEngineChunk && !chunk.empty() && chunk[0] == kArchivesEngine)
        {
            Damaged("an archive contains archives");
        }
        if (id == kMixerChunk)
        {
            Damaged("an archive contains the effects or the volumes of the buses");
        }
        if (id == kSoundsChunk || id == kItemsChunk)
        {
            Damaged("an archive is in the model form");
        }
    }
}

} // namespace

std::vector<uint8_t> WriteProject(const Project& project)
{
    if (const std::optional<std::string> problem = ProjectProblem(project))
    {
        throw std::invalid_argument("the project " + *problem);
    }
    if (project.archives.size() == 1)
    {
        return Write(project.archives.front());
    }
    if (project.front > UINT16_MAX)
    {
        throw std::invalid_argument("the file can't name archive " + std::to_string(project.front) + " in front");
    }

    // The values of the project: the engine of more than one archive, the mixing mode and the archive in front, then
    // the effects, the volumes of the buses and the file's tags.
    const File& first = project.archives.front();
    std::vector<std::pair<std::string, std::vector<uint8_t>>> chunks;
    Out engine;
    engine.U8(kArchivesEngine);
    engine.U8(CodeOf(first.mixing));
    engine.U16(static_cast<uint16_t>(project.front));
    chunks.emplace_back(kEngineChunk, engine.Take());

    const auto has_effects = [](const std::vector<Effect>& effects)
    {
        return !effects.empty();
    };
    if (std::ranges::any_of(first.aux_effects, has_effects) ||
        std::ranges::any_of(first.aux_volumes, file_parts::NotUnity))
    {
        chunks.emplace_back(kMixerChunk, file_parts::MixerChunk(first));
    }

    if (!first.tags.empty())
    {
        File tags;
        tags.tags = first.tags;
        chunks.emplace_back(kTagsChunk, file_parts::TagsChunk(tags));
    }

    // Each archive, then the chunks that Hokuto doesn't read.
    for (const File& archive : project.archives)
    {
        chunks.emplace_back(kArchiveChunk, Write(WithoutProjectValues(archive)));
    }

    for (const OtherChunk& chunk : project.other_chunks)
    {
        if (chunk.id.size() != 4 || IsHokutoChunk(chunk.id))
        {
            throw std::invalid_argument("a chunk that Hokuto doesn't read can't have the ID " + chunk.id);
        }

        chunks.emplace_back(chunk.id, chunk.bytes);
    }

    return file_parts::LaidOut(chunks);
}

Project ReadBinaryProject(std::span<const uint8_t> bytes)
{
    // A file of one archive.
    const std::vector<std::pair<std::string, std::span<const uint8_t>>> chunks = ChunksOf(bytes);
    const auto is_engine = [](const auto& chunk)
    {
        return chunk.first == kEngineChunk;
    };
    const auto engine = std::ranges::find_if(chunks, is_engine);
    if (engine == chunks.end() || engine->second.empty() || engine->second[0] != kArchivesEngine)
    {
        return ProjectOf(Read(bytes));
    }

    // The values of the project. Version 1 of the format has no file with more than one archive.
    if (MajorVersionOf(bytes).value_or(0) < 2)
    {
        Damaged("a file of version 1 contains more than one archive");
    }

    In in(engine->second, "the engine");
    in.U8();
    const uint8_t mixing_code = in.U8();
    Project project;
    project.front = in.U16();
    const std::optional<Mixing> mixing = MixingOfCode(mixing_code);
    if (!mixing)
    {
        file_parts::Newer("a mixing mode (" + std::to_string(mixing_code) + ")");
    }

    if (in.Left() != 0)
    {
        Damaged("the engine of a file with more than one archive runs past the archive in front");
    }

    // The chunks beside the archives, and the archives.
    File values;
    values.mixing = *mixing;
    bool engine_read = false;
    bool mixer_read = false;
    bool tags_read = false;
    for (const auto& [id, chunk] : chunks)
    {
        bool* read = id == kEngineChunk  ? &engine_read
                     : id == kMixerChunk ? &mixer_read
                     : id == kTagsChunk  ? &tags_read
                                         : nullptr;
        if (read && *read)
        {
            Damaged("it contains two " + id + " chunks");
        }

        if (read)
        {
            *read = true;
        }

        if (id == kMixerChunk)
        {
            file_parts::ReadMixer(chunk, values);
        }
        else if (id == kTagsChunk)
        {
            file_parts::ReadTags(chunk, values);
            if (!values.sound_tags.empty())
            {
                Damaged("the tags beside its archives name sounds");
            }
        }
        else if (id == kArchiveChunk)
        {
            CheckArchiveChunks(chunk);
            project.archives.push_back(Read(chunk));
        }
        else if (id != kEngineChunk && IsHokutoChunk(id))
        {
            Damaged("chunk " + id + " stands outside its archives");
        }
        else if (id != kEngineChunk)
        {
            project.other_chunks.push_back({id, std::vector<uint8_t>(chunk.begin(), chunk.end())});
        }
    }

    // Each archive takes the values of the project.
    if (project.archives.size() < 2)
    {
        Damaged("the engine of a file with more than one archive comes with " +
                std::to_string(project.archives.size()) + " archive");
    }

    for (File& archive : project.archives)
    {
        if (archive.mixing != values.mixing)
        {
            Damaged("an archive's mixing mode isn't the file's");
        }
        if (!archive.tags.empty())
        {
            Damaged("an archive contains the file's tags");
        }

        archive.aux_effects = values.aux_effects;
        archive.aux_volumes = values.aux_volumes;
        archive.tags = values.tags;
    }

    if (const std::optional<std::string> problem = ProjectProblem(project))
    {
        Damaged("the project " + *problem);
    }

    return WithFreeFloors(std::move(project));
}

} // namespace hokuto::format
