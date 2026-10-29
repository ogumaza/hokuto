// SPDX-License-Identifier: MIT

#include "app/sequence_view.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app/key_edits.h"
#include "app/theme.h"
#include "app/transport.h"
#include "app/widgets.h"
#include "edit/mix.h"

namespace hokuto::app
{
namespace
{

// Parts start and end on beats, or on bars while bars show. Eight beats without a note, two bars of 4/4, start a new
// part.
constexpr int kPartGap = 8;

// The start that every label of a sequence shares, up to and with its last underscore. NintendoWare's converter names
// the labels after the sequence, such as SMF_BGM_TITLE_Track_1_0. A sequence with one label shares nothing.
std::size_t SharedStart(const std::map<uint32_t, std::string>& labels)
{
    if (labels.size() < 2)
    {
        return 0;
    }

    std::string_view shared = labels.begin()->second;
    for (const auto& [offset, name] : labels)
    {
        const auto [end, other] = std::mismatch(shared.begin(), shared.end(), name.begin(), name.end());
        shared = shared.substr(0, static_cast<std::size_t>(end - shared.begin()));
    }

    const std::size_t underscore = shared.rfind('_');
    return underscore == std::string_view::npos ? 0 : underscore + 1;
}

// The name of the subroutine at `offset`: its label without the start that every label shares, such as Track_1_0.
std::string SubroutineName(const std::map<uint32_t, std::string>& labels, std::size_t shared, uint32_t offset)
{
    const auto label = labels.find(offset);
    if (label == labels.end())
    {
        return "Subroutine";
    }

    return label->second.size() > shared ? label->second.substr(shared) : label->second;
}

// The level in dB of a track's send, from 0 to 255. nn::atk takes the send's fraction of 127 as its gain. The level is
// therefore 20 log10(send / 127).
float SendLevel(int send)
{
    if (send <= 0)
    {
        return kSilence;
    }

    return 20.0f * std::log10(static_cast<float>(send) / 127.0f);
}

} // namespace

SongView MakeSongView(const song::Sequence& sequence)
{
    SongView view;
    view.name = sequence.name;
    view.ticks_per_quarter = sequence.timebase;
    view.tempo = song::TempoAt(sequence, 0);
    view.beats_per_bar = kBeatsPerBar;
    view.length = static_cast<int32_t>(std::min<uint32_t>(sequence.length, std::numeric_limits<int32_t>::max()));
    if (sequence.loop)
    {
        view.loop_start = static_cast<int32_t>(std::min<uint32_t>(sequence.loop->start, view.length));
        view.loop_end = static_cast<int32_t>(std::min<uint32_t>(sequence.loop->end, view.length));
    }
    for (const song::Tempo& tempo : sequence.tempo)
    {
        const auto tick = static_cast<int32_t>(std::min<uint32_t>(tempo.tick, std::numeric_limits<int32_t>::max()));
        view.tempo_changes.push_back({tick, tempo.bpm});
    }

    // Each track's parts, and how many places play each subroutine: the calls in the file that a sound reaches, or the
    // parts that show it, whichever is more. A call that runs more than once makes a part each time.
    const int grid = kShowBars ? view.beats_per_bar : 1;
    std::vector<std::vector<song::Part>> parts;
    std::map<uint32_t, int> plays = sequence.calls;
    std::map<uint32_t, int> shown;
    for (const song::Track& track : sequence.tracks)
    {
        parts.push_back(song::SplitParts(sequence, track, grid, kPartGap));
        for (const song::Part& part : parts.back())
        {
            if (part.call)
            {
                shown[*part.call]++;
            }
        }
    }
    for (const auto& [offset, count] : shown)
    {
        plays[offset] = std::max(plays[offset], count);
    }

    // Each track takes the colour of its number. A track that comes or goes leaves the others' colours as they were.
    // The track's notes go into parts that the arrangement draws. A call's part takes the subroutine's name, and it's
    // linked when the subroutine plays from more than one place.
    const std::size_t shared = SharedStart(sequence.labels);
    for (std::size_t i = 0; i < sequence.tracks.size(); i++)
    {
        const song::Track& track = sequence.tracks[i];
        TrackView track_view;
        track_view.name = track.name;
        track_view.bank = track.bank;
        track_view.program = track.program;
        track_view.slot = track.program >= 0 && !track.notes.empty() ? track.notes.front().bank_slot : -1;
        track_view.rainbow_colour =
            theme::kTrackColours[static_cast<std::size_t>(track.number) % std::size(theme::kTrackColours)];
        track_view.colour = track_view.rainbow_colour;
        for (const song::Part& part : parts[i])
        {
            PartView part_view;
            part_view.start = static_cast<int32_t>(part.start);
            part_view.length = static_cast<int32_t>(part.end - part.start);
            part_view.first = part.first;
            for (std::size_t n = part.first; n < part.first + part.count; n++)
            {
                const song::Note& note = track.notes[n];
                part_view.notes.push_back(
                    {static_cast<int32_t>(note.start - part.start), static_cast<int32_t>(note.length), note.key});
            }
            if (part.call)
            {
                part_view.name = SubroutineName(sequence.labels, shared, *part.call);
                part_view.linked = plays[*part.call] > 1;
            }
            track_view.parts.push_back(std::move(part_view));
        }
        view.tracks.push_back(std::move(track_view));
    }
    ShowMixerAt(view, sequence, 0.0);

    // The variables that a game can set, at the values that the sequence starts with.
    for (const song::Variable& variable : sequence.variables)
    {
        const auto start = sequence.start_values.find(variable.number);
        const int initial = song::StartingValue(sequence, variable.number);
        view.variables.push_back({.number = variable.number,
                                  .choices = variable.choices,
                                  .written = variable.written,
                                  .start = start != sequence.start_values.end() ? start->second : initial,
                                  .initial = initial});
    }

    // The changes, each at its place in the arrangement, and the variables that they can set.
    for (const song::VariableChange& change : sequence.changes)
    {
        view.changes.push_back({change.time, change.variable, change.value, song::TickOfMs(sequence, change.time)});
    }
    for (const song::Variable& variable : sequence.read_variables)
    {
        view.read_variables.push_back({.number = variable.number,
                                       .choices = variable.choices,
                                       .initial = song::StartingValue(sequence, variable.number)});
    }

    return view;
}

void ShowMixerAt(SongView& view, const song::Sequence& sequence, double ticks)
{
    const auto tick = static_cast<uint32_t>(std::clamp(ticks, 0.0, static_cast<double>(sequence.length)));
    for (std::size_t i = 0; i < view.tracks.size() && i < sequence.tracks.size(); i++)
    {
        TrackView& shown = view.tracks[i];
        const auto show = [&](MixControl control, float& value)
        {
            if (!shown.held || shown.held->control != control)
            {
                value = ShownValue(control, song::MixAt(sequence.tracks[i], MixOf(control), tick));
            }
        };
        show(MixControl::kVolume, shown.volume);
        show(MixControl::kPan, shown.pan);
        show(MixControl::kSendA, shown.sends[0]);
        show(MixControl::kSendB, shown.sends[1]);
        show(MixControl::kSendC, shown.sends[2]);
    }
}

song::Mix MixOf(MixControl control)
{
    switch (control)
    {
    case MixControl::kVolume:
        return song::Mix::kVolume;
    case MixControl::kPan:
        return song::Mix::kPan;
    case MixControl::kSendA:
        return song::Mix::kSendA;
    case MixControl::kSendB:
        return song::Mix::kSendB;
    case MixControl::kSendC:
        break;
    }

    return song::Mix::kSendC;
}

int MixValue(MixControl control, float shown)
{
    // A level of minus infinity is a value of 0.
    double value = 0.0;
    if (control == MixControl::kPan)
    {
        value = static_cast<double>(shown) * (shown < 0.0f ? 64.0 : 63.0);
    }
    else if (shown > kSilence)
    {
        value = 127.0 * std::pow(10.0, static_cast<double>(shown) / (control == MixControl::kVolume ? 40.0 : 20.0));
    }

    const song::Mix setting = MixOf(control);
    return static_cast<int>(std::clamp<double>(std::round(value), edit::MixLowest(setting), edit::MixHighest(setting)));
}

float ShownValue(MixControl control, int value)
{
    switch (control)
    {
    case MixControl::kVolume:
        return VolumeLevel(value);
    case MixControl::kPan:
        return std::clamp(static_cast<float>(value) / (value < 0 ? 64.0f : 63.0f), -1.0f, 1.0f);
    default:
        return SendLevel(value);
    }
}

float VolumeLevel(int volume)
{
    if (volume <= 0)
    {
        return kSilence;
    }

    return 40.0f * std::log10(static_cast<float>(volume) / 127.0f);
}

} // namespace hokuto::app
