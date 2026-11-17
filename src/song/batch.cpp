// SPDX-License-Identifier: MIT

#include "song/batch.h"

#include <algorithm>
#include <cstddef>
#include <set>
#include <utility>

#include "seqlib/file_name.h"

#include "song/driver.h"
#include "song/ds.h"
#include "song/dse.h"
#include "song/gba.h"
#include "song/midi_export.h"

namespace hokuto::song
{

std::vector<BatchSound> BatchSounds(const Played& played)
{
    std::vector<BatchSound> sounds;
    const auto add = [&sounds](uint32_t sound, std::string name)
    {
        sounds.push_back({.sound = sound, .name = std::move(name)});
    };

    if (played.Sar())
    {
        const auto& listed = played.Sar()->Sounds();
        for (std::size_t i = 0; i < listed.size(); i++)
        {
            if (listed[i].type == ntdlib::sar::SoundType::kSequence)
            {
                add(static_cast<uint32_t>(i), listed[i].name);
            }
        }
    }
    else if (played.Wii())
    {
        const auto& listed = played.Wii()->Sounds();
        for (std::size_t i = 0; i < listed.size(); i++)
        {
            if (listed[i].type == ntdlib::rsar::SoundType::kSequence)
            {
                add(static_cast<uint32_t>(i), listed[i].name);
            }
        }
    }
    else if (played.Gba())
    {
        for (const int song : GbaSongs(*played.Gba()))
        {
            add(static_cast<uint32_t>(song), GbaSongName(*played.Gba(), song));
        }
    }
    else if (played.Ds())
    {
        for (const uint32_t sound : DsSounds(*played.Ds()))
        {
            add(sound, DsSoundName(*played.Ds(), sound));
        }
    }
    else if (played.Dse())
    {
        for (const uint32_t sound : DseSounds(*played.Dse()))
        {
            add(sound, DseSoundName(*played.Dse(), sound));
        }
    }
    else if (played.Driver())
    {
        for (const uint32_t sound : played.Driver()->Sounds())
        {
            add(sound, played.Driver()->SoundName(sound));
        }
    }

    return sounds;
}

Sequence ReadBatchSequence(const Played& played, uint32_t sound, const std::optional<ntdlib::SndBuild>& wii_build,
                           const StartValues& values, const Changes& changes)
{
    if (played.Wii())
    {
        return ReadSequence(*played.Wii(), sound, values, changes, wii_build);
    }
    if (played.Gba())
    {
        return ReadSequence(*played.Gba(), static_cast<int>(sound));
    }
    if (played.Ds())
    {
        return ReadSequence(*played.Ds(), sound, values, changes);
    }
    if (played.Dse())
    {
        return ReadSequence(*played.Dse(), sound);
    }
    if (played.Driver())
    {
        return played.Driver()->ReadSequence(sound);
    }

    return ReadSequence(*played.Sar(), sound, values, changes);
}

ntdlib::sequence_export::Conversion ConvertBatchSound(const Played& played, uint32_t sound,
                                                      const std::optional<ntdlib::SndBuild>& wii_build,
                                                      const std::string& program, const StartValues& values,
                                                      const Changes& changes)
{
    if (played.Wii())
    {
        return ConvertToMidi(*played.Wii(), sound, values, changes, program, wii_build);
    }
    if (played.Gba())
    {
        return ConvertToMidi(*played.Gba(), static_cast<int>(sound), program);
    }
    if (played.Ds())
    {
        return ConvertToMidi(*played.Ds(), sound, values, program);
    }
    if (played.Dse())
    {
        return ConvertToMidi(*played.Dse(), sound, program);
    }
    if (played.Driver())
    {
        return played.Driver()->ConvertToMidi(sound, program);
    }

    return ConvertToMidi(*played.Sar(), sound, values, changes, program);
}

std::string SchemeName(const NamingScheme& scheme, const NameParts& parts)
{
    std::vector<NameToken> tokens = scheme.tokens;
    if (!parts.track.empty() && std::ranges::find(tokens, NameToken::kTrack) == tokens.end())
    {
        tokens.push_back(NameToken::kTrack);
    }

    std::string name;
    for (const NameToken token : tokens)
    {
        std::string part;
        switch (token)
        {
        case NameToken::kProject:
            part = parts.project;
            break;
        case NameToken::kSong:
            part = parts.song;
            break;
        case NameToken::kNumber:
            part = std::to_string(parts.number);
            part.insert(0, part.size() < 3 ? 3 - part.size() : 0, '0');
            break;
        case NameToken::kTrack:
            part = parts.track;
            break;
        case NameToken::kText:
            part = scheme.text;
            break;
        }
        if (!part.empty())
        {
            name += (name.empty() ? "" : scheme.separator) + part;
        }
    }

    return name;
}

std::vector<std::string> BatchFileNames(std::span<const std::string> names, std::string_view extension,
                                        const std::function<bool(const std::string&)>& taken)
{
    // Two names that differ only in case name one file on the Mac and on Windows.
    const auto folded = [](std::string name)
    {
        std::ranges::transform(name, name.begin(),
                               [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
        return name;
    };

    std::vector<std::string> files;
    std::set<std::string> used;
    for (std::size_t i = 0; i < names.size(); i++)
    {
        const std::string stem = seqlib::SafeFileName(names[i].empty() ? "Sound " + std::to_string(i + 1) : names[i]);
        std::string file = stem + std::string(extension);
        for (int copy = 2; used.contains(folded(file)) || (taken && taken(file)); copy++)
        {
            file = stem + " " + std::to_string(copy) + std::string(extension);
        }

        used.insert(folded(file));
        files.push_back(std::move(file));
    }

    return files;
}

} // namespace hokuto::song
