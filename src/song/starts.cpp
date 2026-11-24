// SPDX-License-Identifier: MIT

#include "song/starts.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <vector>

namespace hokuto::song
{
namespace
{

namespace export_ = ntdlib::sequence_export;

// The tracks of a sequence.
constexpr uint8_t kSequenceTracks = 16;

// Whether `variable` is a variable of a single track.
bool IsSingleTrack(const format::StartVariable& variable)
{
    return variable.track.has_value();
}

} // namespace

StartValues StartValuesOf(const format::SoundStart& start)
{
    StartValues values;
    for (const format::StartVariable& variable : start.variables)
    {
        if (variable.time == 0 && !IsSingleTrack(variable))
        {
            values[variable.variable] = variable.value;
        }
    }

    return values;
}

Changes ChangesOf(const format::SoundStart& start)
{
    Changes changes;
    for (const format::StartVariable& variable : start.variables)
    {
        if (variable.time != 0 && !IsSingleTrack(variable))
        {
            changes.push_back({variable.time, variable.variable, variable.value});
        }
    }

    return changes;
}

format::SoundStart WithVariables(format::SoundStart start, const StartValues& values, const Changes& changes)
{
    // A change at time 0 comes before the first tick, after the values, and replaces a variable's value.
    StartValues first = values;
    Changes later;
    for (const VariableChange& change : changes)
    {
        if (change.time == 0)
        {
            first[change.variable] = change.value;
        }
        else
        {
            later.push_back(change);
        }
    }
    std::ranges::stable_sort(later, {}, &VariableChange::time);

    // The values come first, then the variables of single tracks, then the changes in the order of their times.
    std::vector<format::StartVariable> single;
    std::copy_if(start.variables.begin(), start.variables.end(), std::back_inserter(single), IsSingleTrack);
    start.variables.clear();
    for (const auto& [number, value] : first)
    {
        if (number >= 0 && number < format::kStartVariables)
        {
            start.variables.push_back({.variable = static_cast<uint8_t>(number), .value = value});
        }
    }

    start.variables.insert(start.variables.end(), single.begin(), single.end());
    for (const VariableChange& change : later)
    {
        if (change.variable >= 0 && change.variable < format::kStartVariables)
        {
            start.variables.push_back(
                {.time = change.time, .variable = static_cast<uint8_t>(change.variable), .value = change.value});
        }
    }

    return start;
}

uint16_t QuietedSequenceTracks(const format::SoundStart& start)
{
    return static_cast<uint16_t>(format::QuietedTracks(start) & 0xFFFF);
}

void ApplyStart(export_::PerformOptions& options, export_::Settings& settings, const format::SoundStart& start)
{
    options.tracks = static_cast<uint16_t>(options.tracks & ~QuietedSequenceTracks(start));
    for (const auto& [track, volume] : start.track_volumes)
    {
        if (track < kSequenceTracks)
        {
            options.track_volumes[track] = volume;
        }
    }

    options.tempo_ratio = start.tempo_ratio.value_or(options.tempo_ratio);
    options.pitch = start.pitch.value_or(options.pitch);
    options.volume = start.volume.value_or(options.volume);
    options.lpf = start.low_pass.value_or(options.lpf);
    if (start.sends)
    {
        options.fx_sends = *start.sends;
    }
    if (start.banks)
    {
        options.banks = *start.banks;
    }
    if (start.seed)
    {
        options.seed = *start.seed;
        settings.random_steps_before_start = 0;
    }

    // The performer sets the variables of single tracks before those of every track. Where both set a variable, the
    // value of every track goes to each track first, among the variables of single tracks.
    std::vector<export_::TrackVariable> every;
    std::vector<export_::TrackVariable> single;
    for (const format::StartVariable& variable : start.variables)
    {
        if (!IsSingleTrack(variable) || variable.time != 0 || *variable.track >= kSequenceTracks)
        {
            continue;
        }

        const auto index = static_cast<uint8_t>(variable.variable - format::kFirstTrackVariable);
        if (const auto found = options.variables.find(variable.variable); found != options.variables.end())
        {
            for (uint8_t track = 0; track < kSequenceTracks; ++track)
            {
                every.push_back({.track = track, .index = index, .value = found->second});
            }
            options.variables.erase(found);
        }

        single.push_back({.track = *variable.track, .index = index, .value = variable.value});
    }

    options.track_variables.insert(options.track_variables.end(), every.begin(), every.end());
    options.track_variables.insert(options.track_variables.end(), single.begin(), single.end());
}

} // namespace hokuto::song
