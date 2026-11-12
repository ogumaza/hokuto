// SPDX-License-Identifier: MIT

#include "edit/midi_songs.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/units.h"
#include "ntdlib/sequence_export/filter.h"
#include "ntdlib/sequence_export/settings.h"
#include "seqlib/midi/controllers.h"
#include "seqlib/midi/performance.h"
#include "seqlib/midi/reader.h"

#include "edit/midi_events.h"
#include "edit/midi_songs_parts.h"

namespace hokuto::edit
{
namespace
{

using midi_songs_parts::MidiTrack;
using midi_songs_parts::Placed;
using midi_songs_parts::Setting;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;

// MIDI's drum channel, 10.
constexpr uint8_t kDrumChannel = seqlib::midi::kDrumChannel;

// The controllers that the song keeps. Registered parameter 0 is the bend range.
constexpr uint8_t kDataEntry = seqlib::midi::cc::kDataEntry;
constexpr uint8_t kVolumeController = seqlib::midi::cc::kVolume;
constexpr uint8_t kPanController = seqlib::midi::cc::kPan;
constexpr uint8_t kExpression = seqlib::midi::cc::kExpression;
constexpr uint8_t kDataEntryFine = seqlib::midi::cc::kDataEntryLsb;
constexpr uint8_t kDamperController = seqlib::midi::cc::kSustain;
constexpr uint8_t kSendA = seqlib::midi::cc::kReverb;
constexpr uint8_t kSendB = seqlib::midi::cc::kChorus;
constexpr uint8_t kParameterFine = seqlib::midi::cc::kRpnLsb;
constexpr uint8_t kParameter = seqlib::midi::cc::kRpnMsb;
constexpr uint8_t kBankSelect = seqlib::midi::cc::kBankSelect;        // with `selects_banks`
constexpr uint8_t kBankSelectFine = seqlib::midi::cc::kBankSelectLsb; // with `selects_banks`, at 0

// The controllers that select a non-registered parameter. Data entry after them sets no registered parameter, as after
// the null parameter, 127 and 127.
constexpr uint8_t kNonRegisteredFine = seqlib::midi::cc::kNrpnLsb;
constexpr uint8_t kNonRegistered = seqlib::midi::cc::kNrpnMsb;
constexpr uint8_t kNullParameter = seqlib::midi::cc::kNullRpn;

// The further controllers that an import keeps (`imports`). Registered parameter 2 is the transposition.
constexpr uint8_t kModulation = seqlib::midi::cc::kModulation;
constexpr uint8_t kPortamentoTime = seqlib::midi::cc::kPortamentoTime;
constexpr uint8_t kPortamentoSwitch = seqlib::midi::cc::kPortamento;
constexpr uint8_t kRelease = seqlib::midi::cc::kReleaseTime;
constexpr uint8_t kAttack = seqlib::midi::cc::kAttackTime;
constexpr uint8_t kBrightness = seqlib::midi::cc::kBrightness;
constexpr uint8_t kDecay = seqlib::midi::cc::kDecayTime;
constexpr uint8_t kPortamentoKey = seqlib::midi::cc::kPortamentoControl;
constexpr uint8_t kMonoOn = seqlib::midi::cc::kMonoOn;
constexpr uint8_t kPolyOn = seqlib::midi::cc::kPolyOn;
constexpr uint8_t kTransposeParameter = 2;

// A controller's values: 0 to 127. The middle value means no pan and no transposition, and turns a switch on.
constexpr int64_t kControllerValues = 128;
constexpr int64_t kControllerMiddle = 64;

// The programs of a bank of MIDI.
constexpr int kBankPrograms = 128;

// The value of a low-pass command at no filter.
constexpr int64_t kLowPassOff = 64;

// The bank slots of a sound of the later consoles.
constexpr uint8_t kBankSlots = 4;

// The low-pass value whose cutoff is nearest the cutoff that control change 102 of the converters sets with `value`:
// the Switch's table of cutoffs. A value of 0 leaves the filter open.
int64_t LowPassOfFilterController(uint8_t value)
{
    namespace sequence_export = ntdlib::sequence_export;
    if (value == 0)
    {
        return kLowPassOff;
    }

    const int cutoff = sequence_export::kFilterOpen - sequence_export::kFilterControllerRange * value / 128;
    sequence_export::Settings settings;
    settings.engine = sequence_export::Engine::kSwitch;
    int64_t nearest = kLowPassOff;
    int distance = INT_MAX;
    for (int low_pass = 0; low_pass < kLowPassOff; ++low_pass)
    {
        const int step = sequence_export::TrackFilters(low_pass, 0, 0, settings).low_pass;
        if (step < 0)
        {
            continue;
        }

        const int off =
            std::abs(sequence_export::kSwitchFilters.low_pass_cutoff[static_cast<std::size_t>(step)] - cutoff);
        if (off < distance)
        {
            distance = off;
            nearest = low_pass;
        }
    }

    return nearest;
}

// The setting of control change `controller` with `value` that an import keeps and the song's other rules leave out.
std::optional<Command> ImportedSetting(uint8_t controller, uint8_t value)
{
    const int64_t raw = int64_t{value} * kOne;
    const auto low_pass = [](int64_t held)
    {
        return Setting(Kind::kLowPass, (held - kLowPassOff) * (kOne / kControllerMiddle));
    };
    switch (controller)
    {
    case kBankSelect:
        if (value >= kBankSlots)
        {
            return std::nullopt;
        }

        return Setting(Kind::kBankSelect, raw);

    case kModulation:
        return Setting(Kind::kLfoDepth, int64_t{value} * (kOne / kControllerValues));

    case kPortamentoTime:
        return Setting(Kind::kPortamentoTime, raw);

    case kPortamentoSwitch:
        return Setting(Kind::kPortamento, value >= kControllerMiddle ? kOne : 0);

    case kRelease:
        return Setting(Kind::kRelease, raw);

    case kAttack:
        return Setting(Kind::kAttack, raw);

    case kBrightness:
        return low_pass(value);

    case kDecay:
        return Setting(Kind::kDecay, raw);

    case kPortamentoKey:
        return Setting(Kind::kPortamentoFrom, raw);

    case ntdlib::sequence_export::kFilterController:
        return low_pass(LowPassOfFilterController(value));

    case ntdlib::sequence_export::kSfzLowPassController:
        return low_pass(kLowPassOff - std::min<int64_t>(value, kLowPassOff));

    case ntdlib::sequence_export::kSfzBiquadController:
        return Setting(Kind::kBiquadValue, raw);

    case kMonoOn:
        return Setting(Kind::kMonophonic, kOne);

    case kPolyOn:
        return Setting(Kind::kMonophonic, 0);

    default:
        return std::nullopt;
    }
}

// A bend range of `cents` as NintendoWare's bend range stores it: the nearest whole number of semitones.
int64_t HeldBendRange(int64_t cents)
{
    return std::clamp<int64_t>((cents + 50) / 100, 0, 127) * 100 * kOne;
}

// Adds the data entry of control change `data` at `value` at `tick` to `track`: the bend range, or a transpose that
// a converter's MIDI file sets (SongOfMidi). `left_out` counts the data entries of other parameters.
void PlaceDataEntry(MidiTrack& track, uint64_t tick, uint8_t data, uint8_t value, const MidiOptions& options,
                    std::map<std::string, std::size_t>& left_out)
{
    if (options.imports && track.parameter == 0 && track.parameter_fine == kTransposeParameter)
    {
        if (data == kDataEntry)
        {
            track.Add(tick, Setting(Kind::kTranspose, (int64_t{value} - kControllerMiddle) * 100 * kOne));
        }

        return;
    }

    if (track.parameter != 0 || track.parameter_fine != 0)
    {
        ++left_out["data entry for other parameters"];
        return;
    }

    // The bend range in cents. A fine data entry right after the coarse one changes its setting. NintendoWare's bend
    // range is in whole semitones. The song rounds to the nearest.
    if (data == kDataEntry)
    {
        track.bend_cents = int64_t{value} * 100 + track.bend_cents % 100;
        track.Add(tick, Setting(Kind::kBendRange, HeldBendRange(track.bend_cents)));
    }
    else
    {
        track.bend_cents = track.bend_cents / 100 * 100 + value;
        Placed* last = track.placed.empty() ? nullptr : &track.placed.back();
        if (last != nullptr && last->tick == tick && last->command.kind == Kind::kBendRange)
        {
            last->command.value.value = HeldBendRange(track.bend_cents);
        }
        else
        {
            track.Add(tick, Setting(Kind::kBendRange, HeldBendRange(track.bend_cents)));
        }
    }
}

} // namespace

namespace midi_songs_parts
{

Command Setting(Kind kind, int64_t value, uint8_t index)
{
    Command command;
    command.kind = kind;
    command.index = index;
    command.value.value = value;

    return command;
}

std::vector<Placed> TimeKeepingTempos(const std::vector<std::pair<uint64_t, uint32_t>>& tempos,
                                      const std::vector<uint64_t>& lines, uint64_t start,
                                      std::optional<uint64_t> loop_start, std::optional<uint64_t> loop_end,
                                      uint64_t file_end, uint16_t division, uint32_t timebase)
{
    // The stretches' ends, in MIDI ticks.
    std::vector<uint64_t> ends;
    for (const auto& tempo : tempos)
    {
        ends.push_back(tempo.first);
    }

    ends.insert(ends.end(), lines.begin(), lines.end());
    for (const std::optional<uint64_t>& end : {loop_start, loop_end})
    {
        if (end)
        {
            ends.push_back(*end);
        }
    }

    ends.push_back(file_end);
    std::erase_if(ends, [start, file_end](uint64_t tick) { return tick < start || tick > file_end; });
    ends.push_back(start);
    std::ranges::sort(ends);
    const auto [same, last] = std::ranges::unique(ends);
    ends.erase(same, last);

    const auto song_tick = [&](uint64_t tick)
    {
        return static_cast<double>((tick - start) * timebase / division);
    };
    std::vector<Placed> commands;
    std::size_t next = 0;
    uint32_t micros = tempos.front().second;
    double off = 0.0; // the time of the song less the time of the MIDI file, in seconds
    std::optional<int64_t> current;
    std::optional<int64_t> loop_first;
    std::optional<int64_t> loop_last;
    for (std::size_t i = 0; i + 1 < ends.size(); ++i)
    {
        while (next < tempos.size() && tempos[next].first <= ends[i])
        {
            micros = std::max<uint32_t>(tempos[next].second, 1);
            ++next;
        }

        const bool loops_here = loop_start && ends[i] == *loop_start;
        if (loops_here)
        {
            off = 0.0;
        }

        // The MIDI file's time over the stretch, and the song's at each whole tempo.
        const double midi = static_cast<double>(ends[i + 1] - ends[i]) * micros / 1e6 / division;
        const double bpm = 60e6 / micros;
        const double ticks = song_tick(ends[i + 1]) - song_tick(ends[i]);
        const auto lower = static_cast<int64_t>(std::max(std::floor(bpm), 1.0));
        const int64_t upper = std::max<int64_t>(static_cast<int64_t>(std::ceil(bpm)), 1);
        const auto seconds = [ticks, timebase](int64_t whole)
        {
            return ticks * 60.0 / (static_cast<double>(whole) * timebase);
        };
        const int64_t chosen =
            std::abs(off + seconds(lower) - midi) <= std::abs(off + seconds(upper) - midi) ? lower : upper;
        off += seconds(chosen) - midi;
        if (loops_here)
        {
            loop_first = chosen;
        }

        if (loop_end && ends[i + 1] == *loop_end)
        {
            loop_last = chosen;
        }

        if (chosen != current || loops_here)
        {
            commands.push_back({ends[i], Setting(Kind::kTempo, chosen * kOne), std::nullopt});
            current = chosen;
        }
    }

    // A loop whose first tempo is the tempo at its end requires no command at its start.
    if (loop_first && loop_start && loop_last.value_or(*loop_first) == *loop_first)
    {
        const auto at_start = std::ranges::find(commands, *loop_start, &Placed::tick);
        if (at_start != commands.end() && at_start != commands.begin() &&
            std::prev(at_start)->command.value.value == at_start->command.value.value)
        {
            commands.erase(at_start);
        }
    }

    return commands;
}

bool Holds(uint8_t status, uint8_t data, uint8_t value, const MidiOptions& options)
{
    switch (status & 0xF0)
    {
    case 0x90:
    case 0xC0:
    case 0xE0:
        return true;
    case 0xB0:
        return data == kVolumeController || data == kExpression || data == kPanController || data == kSendA ||
               data == kSendB || data == kDamperController || data == kParameter || data == kParameterFine ||
               data == kDataEntry || data == kDataEntryFine ||
               (options.imports && ImportedSetting(data, value).has_value()) ||
               ((options.selects_banks || options.imports) && data == kBankSelectFine && value == 0) ||
               (options.selects_banks && data == kBankSelect);
    default:
        return false;
    }
}

void PlaceNoteOn(MidiTrack& track, uint64_t tick, uint8_t channel, uint8_t data, uint8_t value,
                 const MidiOptions& options)
{
    Command note;
    note.kind = Kind::kNote;
    note.key = data;
    note.velocity = int64_t{value} * kOne;
    if (channel == kDrumChannel && options.drum_notes != nullptr && !options.drum_notes->empty())
    {
        // A pairing sets the program and the key of the note. The program stays selected for the next note. A note
        // without a pairing selects the file's program again.
        const auto paired = options.drum_notes->find(data);
        const bool found_pair = paired != options.drum_notes->end();
        const int program = found_pair ? paired->second.program : track.file_program;
        if (program != track.played_program)
        {
            track.Add(tick, Setting(Kind::kProgram, int64_t{program} * kOne));
            track.played_program = program;
        }

        note.key = found_pair ? static_cast<uint8_t>(paired->second.key) : data;
    }

    track.sounding[data].push_back(track.placed.size());
    track.Add(tick, note);
}

void PlaceNoteOff(MidiTrack& track, uint64_t tick, uint8_t data)
{
    if (auto sounding = track.sounding.find(data); sounding != track.sounding.end() && !sounding->second.empty())
    {
        track.placed[sounding->second.front()].off = tick;
        sounding->second.pop_front();
    }
}

void PlaceControlChange(MidiTrack& track, uint64_t tick, uint8_t status, uint8_t data, uint8_t value,
                        const MidiOptions& options, bool exact_low_pass, std::map<std::string, std::size_t>& left_out,
                        std::size_t& biquad_values)
{
    if (options.imports)
    {
        // The MIDI file of a converter stores the low-pass in control change 103. Its 102 then stores what the filter
        // of a SoundFont makes of the low-pass and the biquad filter together.
        if (data == ntdlib::sequence_export::kFilterController && exact_low_pass)
        {
            return;
        }

        if (const std::optional<Command> setting = ImportedSetting(data, value))
        {
            biquad_values += setting->kind == Kind::kBiquadValue ? 1 : 0;
            track.Add(tick, *setting);
            return;
        }
    }

    switch (data)
    {
    case kVolumeController:
        track.Add(tick, Setting(Kind::kVolume, int64_t{value} * kOne));
        break;

    case kExpression:
        track.Add(tick, Setting(Kind::kVolume2, int64_t{value} * kOne));
        break;

    case kPanController:
        track.Add(tick, Setting(Kind::kPan, (int64_t{value} - kControllerMiddle) * kOne));
        break;

    case kSendA:
    case kSendB:
        track.Add(tick, Setting(Kind::kSend, int64_t{value} * kOne, data == kSendA ? 0 : 1));
        break;

    case kDamperController:
        track.Add(tick, Setting(Kind::kDamper, value >= kControllerMiddle ? kOne : 0));
        break;

    case kParameter:
        track.parameter = value;
        break;

    case kBankSelect:
    case kBankSelectFine:
        if (!Holds(status, data, value, options))
        {
            ++left_out[LeftOut(status, data)];
        }
        else if (data == kBankSelect)
        {
            track.bank = value;
        }
        break;

    case kParameterFine:
        track.parameter_fine = value;
        break;

    case kNonRegistered:
    case kNonRegisteredFine:
        track.parameter = kNullParameter;
        track.parameter_fine = kNullParameter;
        ++left_out[LeftOut(status, data)];
        break;

    case kDataEntry:
    case kDataEntryFine:
        PlaceDataEntry(track, tick, data, value, options, left_out);
        break;

    default:
        ++left_out[LeftOut(status, data)];
        break;
    }
}

void PlaceProgramChange(MidiTrack& track, uint64_t tick, uint8_t data)
{
    track.Add(tick, Setting(Kind::kProgram, (int64_t{track.bank} * kBankPrograms + data) * kOne));
    track.file_program = track.bank * kBankPrograms + data;
    track.played_program = track.file_program;
}

void PlacePitchBend(MidiTrack& track, uint64_t tick, uint8_t data, uint8_t value)
{
    // A fraction of the bend range: 8192 steps on each side of the centre. NintendoWare's pitch bend has 128 steps on
    // each side. The song rounds to the nearest step.
    const int bend = (int{value} << 7 | data) - 8192;
    const auto steps = std::clamp<int64_t>(std::lround(bend / 64.0), -128, 127);
    track.Add(tick, Setting(Kind::kPitchBend, steps * (kOne / 128)));
}

} // namespace midi_songs_parts

MidiDrums DrumNotesOf(std::span<const uint8_t> midi)
{
    seqlib::midi::File file;
    try
    {
        file = seqlib::midi::Read(midi);
    }
    catch (const std::exception&)
    {
        return {};
    }

    MidiDrums drums;
    std::set<int> notes;
    for (const seqlib::midi::Track& track : file.tracks)
    {
        for (const seqlib::midi::Event& event : track.events)
        {
            if (event.bytes.size() < 3)
            {
                continue;
            }

            if (event.bytes[0] == 0x90 + kDrumChannel && event.bytes[2] > 0)
            {
                notes.insert(event.bytes[1]);
            }
            else if (notes.empty() && event.bytes[0] == 0xB0 + kDrumChannel && event.bytes[1] == kBankSelect &&
                     event.bytes[2] < kBankSlots)
            {
                drums.slot = event.bytes[2];
            }
        }
    }

    drums.notes.assign(notes.begin(), notes.end());

    return drums;
}

} // namespace hokuto::edit
