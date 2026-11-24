// SPDX-License-Identifier: MIT

// The starts of a file's sounds: their checks, their edits and the STRT chunk.

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <utility>

#include "format/file_parts.h"

namespace hokuto::format
{
namespace
{

using file_parts::Damaged;
using file_parts::In;
using file_parts::Newer;
using file_parts::Out;

// The flags of a start in STRT: which fields follow, in the order of their bits.
constexpr uint32_t kMutedField = 1 << 0;
constexpr uint32_t kSilencedField = 1 << 1;
constexpr uint32_t kTrackVolumesField = 1 << 2;
constexpr uint32_t kTempoRatioField = 1 << 3;
constexpr uint32_t kPitchField = 1 << 4;
constexpr uint32_t kVolumeField = 1 << 5;
constexpr uint32_t kSendsField = 1 << 6;
constexpr uint32_t kBanksField = 1 << 7;
constexpr uint32_t kSeedField = 1 << 8;
constexpr uint32_t kLowPassField = 1 << 9;
constexpr uint32_t kDseLevelField = 1 << 10;
constexpr uint32_t kStartFields = (kDseLevelField << 1) - 1;

// The byte of a variable's track in STRT: none for a player's or a global variable, 1 + N for track N, and every
// track.
constexpr uint8_t kNoTrackByte = 0;

constexpr uint8_t kEveryTrackByte = 0xFF;

// Whether `value` is a finite number of 0 or more.
bool IsAmount(float value)
{
    return std::isfinite(value) && value >= 0.0f;
}

// The field of `start` that isn't a finite number of 0 or more, a send above 1, a low-pass outside its range, or
// nothing.
std::optional<std::string> NumbersProblem(const SoundStart& start)
{
    for (const auto& [track, volume] : start.track_volumes)
    {
        if (track >= kStartTracks)
        {
            return "gives track " + std::to_string(track) + " a volume, and a start has tracks 0 to " +
                   std::to_string(kStartTracks - 1) + " only";
        }
        if (!IsAmount(volume))
        {
            return "gives track " + std::to_string(track) + " a volume that isn't a number of 0 or more";
        }
    }

    const std::pair<const std::optional<float>*, const char*> kRatios[] = {
        {&start.tempo_ratio, "tempo ratio"}, {&start.pitch, "pitch"}, {&start.volume, "volume"}};
    for (const auto& [ratio, what] : kRatios)
    {
        if (*ratio && !IsAmount(**ratio))
        {
            return std::string("has a ") + what + " that isn't a number of 0 or more";
        }
    }

    if (start.sends && std::any_of(start.sends->begin(), start.sends->end(),
                                   [](float send) { return !IsAmount(send) || send > 1.0f; }))
    {
        return std::string("has a send that isn't a number from 0 to 1");
    }
    if (start.low_pass &&
        !(std::isfinite(*start.low_pass) && *start.low_pass >= kLowestStartLowPass && *start.low_pass <= 0.0f))
    {
        return std::string("has a low-pass that isn't a number from -1 to 0");
    }
    if (start.dse_level && (*start.dse_level < 0 || *start.dse_level > kFullDseLevel))
    {
        return "has a level of DSE that isn't a whole number from 0 to " + std::to_string(kFullDseLevel);
    }

    return std::nullopt;
}

// The variable of `start` that it can't set, or sets out of order, or nothing.
std::optional<std::string> VariablesProblem(const SoundStart& start)
{
    std::set<std::pair<uint8_t, int>> at_start;
    uint32_t time = 0;
    for (const StartVariable& variable : start.variables)
    {
        const std::string which = "variable " + std::to_string(variable.variable);
        if (variable.variable >= kStartVariables)
        {
            return "sets " + which + ", and a start sets variables 0 to " + std::to_string(kStartVariables - 1) +
                   " only";
        }
        if (variable.track && variable.variable < kFirstTrackVariable)
        {
            return "gives " + which + " a track, and only a track variable has one";
        }
        if (variable.track && *variable.track >= kStartTracks)
        {
            return "sets " + which + " of track " + std::to_string(*variable.track) + ", and a start has tracks 0 to " +
                   std::to_string(kStartTracks - 1) + " only";
        }
        if (variable.track && variable.time != 0)
        {
            return "changes " + which + " of a single track after the start";
        }
        if (variable.time < time)
        {
            return std::string("sets its variables out of the order of their times");
        }

        time = variable.time;
        const int track = variable.track ? *variable.track : -1;
        if (variable.time == 0 && !at_start.emplace(variable.variable, track).second)
        {
            return "sets " + which + (variable.track ? " of track " + std::to_string(track) : std::string()) +
                   " twice at the start";
        }
    }

    return std::nullopt;
}

// The record of `start` in STRT, without its sound. Only a file of the 3DS keeps the banks, and only a file of DSE the
// level.
void WriteStart(Out& out, const SoundStart& start, bool banks, bool dse)
{
    out.Name(start.name, "a start's name");
    const uint32_t flags = (start.muted != 0 ? kMutedField : 0) | (start.silenced != 0 ? kSilencedField : 0) |
                           (!start.track_volumes.empty() ? kTrackVolumesField : 0) |
                           (start.tempo_ratio ? kTempoRatioField : 0) | (start.pitch ? kPitchField : 0) |
                           (start.volume ? kVolumeField : 0) | (start.sends ? kSendsField : 0) |
                           (banks && start.banks ? kBanksField : 0) | (start.seed ? kSeedField : 0) |
                           (start.low_pass ? kLowPassField : 0) | (dse && start.dse_level ? kDseLevelField : 0);
    out.U32(flags);
    if (start.muted != 0)
    {
        out.U32(start.muted);
    }
    if (start.silenced != 0)
    {
        out.U32(start.silenced);
    }

    if (!start.track_volumes.empty())
    {
        uint32_t tracks = 0;
        for (const auto& [track, volume] : start.track_volumes)
        {
            tracks |= 1u << track;
        }
        out.U32(tracks);
        for (const auto& [track, volume] : start.track_volumes)
        {
            out.F32(volume);
        }
    }

    for (const std::optional<float>* ratio : {&start.tempo_ratio, &start.pitch, &start.volume})
    {
        if (*ratio)
        {
            out.F32(**ratio);
        }
    }

    if (start.sends)
    {
        for (const float send : *start.sends)
        {
            out.F32(send);
        }
    }
    if (banks && start.banks)
    {
        for (const uint32_t bank : *start.banks)
        {
            out.U32(bank);
        }
    }
    if (start.seed)
    {
        out.U32(*start.seed);
    }
    if (start.low_pass)
    {
        out.F32(*start.low_pass);
    }
    if (dse && start.dse_level)
    {
        out.U32(static_cast<uint32_t>(*start.dse_level));
    }

    out.Count(start.variables.size(), "a start's variables");
    for (const StartVariable& variable : start.variables)
    {
        const bool track_variable = variable.variable >= kFirstTrackVariable;
        out.U32(variable.time);
        out.U8(static_cast<uint8_t>(track_variable ? variable.variable - kFirstTrackVariable : variable.variable));
        out.U8(!track_variable ? kNoTrackByte
                               : (variable.track ? static_cast<uint8_t>(1 + *variable.track) : kEveryTrackByte));
        out.U16(static_cast<uint16_t>(variable.value));
    }
}

// A start's variable in STRT: its time, its number, its track and its value.
StartVariable ReadStartVariable(In& in, const std::string& whose)
{
    StartVariable variable;
    variable.time = in.U32();
    const uint8_t number = in.U8();
    const uint8_t track = in.U8();
    variable.value = static_cast<int16_t>(in.U16());
    if (track == kNoTrackByte)
    {
        if (number >= kFirstTrackVariable)
        {
            Damaged(whose + " sets variable " + std::to_string(number) + " without a track");
        }

        variable.variable = number;
        return variable;
    }

    if (number >= kStartVariables - kFirstTrackVariable)
    {
        Damaged(whose + " sets track variable " + std::to_string(number));
    }
    if (track != kEveryTrackByte && track > kStartTracks)
    {
        Damaged(whose + " sets a variable of track " + std::to_string(track - 1));
    }

    variable.variable = static_cast<uint8_t>(kFirstTrackVariable + number);
    if (track != kEveryTrackByte)
    {
        variable.track = static_cast<uint8_t>(track - 1);
    }

    return variable;
}

// The record of a start in STRT, after its sound.
SoundStart ReadStart(In& in, uint32_t sound)
{
    SoundStart start;
    start.sound = sound;
    start.name = in.Name();
    const std::string whose = "sound " + std::to_string(sound) + "'s start";
    const uint32_t flags = in.U32();
    if ((flags & ~kStartFields) != 0)
    {
        Newer("a sound's start with a field");
    }

    start.muted = (flags & kMutedField) != 0 ? in.U32() : 0;
    start.silenced = (flags & kSilencedField) != 0 ? in.U32() : 0;
    if ((flags & kTrackVolumesField) != 0)
    {
        const uint32_t tracks = in.U32();
        for (uint8_t track = 0; track < kStartTracks; ++track)
        {
            if ((tracks >> track & 1) != 0)
            {
                start.track_volumes.emplace(track, in.F32());
            }
        }
    }

    const std::pair<uint32_t, std::optional<float>*> kRatios[] = {
        {kTempoRatioField, &start.tempo_ratio}, {kPitchField, &start.pitch}, {kVolumeField, &start.volume}};
    for (const auto& [field, ratio] : kRatios)
    {
        if ((flags & field) != 0)
        {
            *ratio = in.F32();
        }
    }

    if ((flags & kSendsField) != 0)
    {
        start.sends = StartSends{in.F32(), in.F32(), in.F32()};
    }
    if ((flags & kBanksField) != 0)
    {
        start.banks = StartBanks{in.U32(), in.U32(), in.U32(), in.U32()};
    }
    if ((flags & kSeedField) != 0)
    {
        start.seed = in.U32();
    }
    if ((flags & kLowPassField) != 0)
    {
        start.low_pass = in.F32();
    }
    if ((flags & kDseLevelField) != 0)
    {
        const uint32_t level = in.U32();
        if (level > static_cast<uint32_t>(kFullDseLevel))
        {
            Damaged(whose + " has a level of DSE of " + std::to_string(level));
        }

        start.dse_level = static_cast<int>(level);
    }

    const uint32_t count = in.U32();
    for (uint32_t i = 0; i < count; ++i)
    {
        start.variables.push_back(ReadStartVariable(in, whose));
    }
    if (HasTimedTrackVariable(start))
    {
        Newer("a change of a single track's variable after a sound's start");
    }
    if (const std::optional<std::string> problem = StartProblem(start))
    {
        Damaged(whose + " " + *problem);
    }

    return start;
}

} // namespace

std::optional<std::string> StartProblem(const SoundStart& start)
{
    if (std::optional<std::string> problem = NumbersProblem(start))
    {
        return problem;
    }

    return VariablesProblem(start);
}

bool HasTimedTrackVariable(const SoundStart& start)
{
    return std::any_of(start.variables.begin(), start.variables.end(),
                       [](const StartVariable& variable) { return variable.track && variable.time != 0; });
}

SoundStart StartOf(const File& file, uint32_t sound)
{
    const auto played = [sound](const SoundStart& start)
    {
        return start.sound == sound && start.name.empty();
    };
    const auto found = std::find_if(file.starts.begin(), file.starts.end(), played);
    return found != file.starts.end() ? *found : SoundStart{.sound = sound};
}

File WithStart(File file, SoundStart start)
{
    const auto named_alike = [&start](const SoundStart& kept)
    {
        return kept.sound == start.sound && kept.name == start.name;
    };
    const auto same = std::find_if(file.starts.begin(), file.starts.end(), named_alike);
    const bool dropped = start.name.empty() && IsEmpty(start);
    if (same != file.starts.end())
    {
        if (dropped)
        {
            file.starts.erase(same);
        }
        else
        {
            *same = std::move(start);
        }
        return file;
    }
    if (dropped)
    {
        return file;
    }

    // A start without a name goes before the sound's states, and a state after them.
    const auto after = [&start](const SoundStart& kept)
    {
        return start.name.empty() ? kept.sound >= start.sound : kept.sound > start.sound;
    };
    file.starts.insert(std::find_if(file.starts.begin(), file.starts.end(), after), std::move(start));

    return file;
}

namespace file_parts
{

// STRT: the starts. Their number, then each one: its sound, its name, its flags, the fields that the flags give, the
// number of its variables and each one. A file whose engine isn't the 3DS's keeps no banks, and one whose engine isn't
// DSE keeps no level.
std::vector<uint8_t> StartsChunk(const File& file)
{
    Out out;
    out.Count(file.starts.size(), "sounds' starts");
    std::set<std::pair<uint32_t, std::string>> written;
    for (const SoundStart& start : file.starts)
    {
        if (std::optional<std::string> problem = StartProblem(start))
        {
            throw std::invalid_argument("sound " + std::to_string(start.sound) + "'s start " + *problem);
        }
        if (!written.emplace(start.sound, start.name).second)
        {
            throw std::invalid_argument("sound " + std::to_string(start.sound) + " has two starts named \"" +
                                        start.name + "\"");
        }

        out.U32(start.sound);
        WriteStart(out, start, file.engine == Engine::k3ds, file.engine == Engine::kDse);
    }

    return out.Take();
}

void ReadStarts(std::span<const uint8_t> chunk, File& file)
{
    In in(chunk, "the starts");
    const uint32_t count = in.U32();
    std::set<std::pair<uint32_t, std::string>> read;
    for (uint32_t i = 0; i < count; ++i)
    {
        SoundStart start = ReadStart(in, in.U32());
        if (!read.emplace(start.sound, start.name).second)
        {
            Damaged("it gives sound " + std::to_string(start.sound) + " two starts named \"" + start.name + "\"");
        }

        file.starts.push_back(std::move(start));
    }
}

} // namespace file_parts

} // namespace hokuto::format
