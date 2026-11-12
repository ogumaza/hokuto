// SPDX-License-Identifier: MIT

// The import of a MIDI file as a GBA song (GbaSongOfMidi in edit/gba_midi.h): the steps of GbaMidiReader that read the
// file's events onto the tracks, and the controllers and text events that the import reads. gba_midi_tracks.cpp
// contains the steps that write the tracks.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ntdlib/commands/units.h"
#include "ntdlib/mp2k/commands.h"
#include "seqlib/midi/controllers.h"
#include "seqlib/midi/performance.h"
#include "seqlib/midi/reader.h"

#include "edit/gba_midi.h"
#include "edit/gba_midi_reader_parts.h"
#include "edit/midi_events.h"
#include "edit/mp2k.h"

namespace hokuto::edit
{
namespace
{

using gba_midi_reader_parts::ChannelEvent;
using gba_midi_reader_parts::GbaMidiReader;
using ntdlib::commands::kOne;
using ntdlib::commands::Source;

// MIDI's drum channel, 10, counted from 0.
constexpr uint8_t kDrumChannel = seqlib::midi::kDrumChannel;

// The tempo of a MIDI file before its first tempo event, in microseconds a quarter note: 120 beats a minute.
constexpr uint32_t kMidiTempo = seqlib::midi::kDefaultMicrosPerQuarter;

// The control changes that the import reads.
constexpr uint8_t kModulation = seqlib::midi::cc::kModulation;
constexpr uint8_t kDataEntry = seqlib::midi::cc::kDataEntry;
constexpr uint8_t kVolume = seqlib::midi::cc::kVolume;
constexpr uint8_t kPan = seqlib::midi::cc::kPan;
constexpr uint8_t kExpression = seqlib::midi::cc::kExpression;
constexpr uint8_t kMemoryAccess = 12;
constexpr uint8_t kMemoryOperation = 13;
constexpr uint8_t kMemoryByte = 14;
constexpr uint8_t kMemoryLabel = 15;
constexpr uint8_t kMemoryAccess2 = 16;
constexpr uint8_t kLabel = 17;
constexpr uint8_t kBendRange = 20;
constexpr uint8_t kLfoSpeed = 21;
constexpr uint8_t kLfoType = 22;
constexpr uint8_t kTune = 24;
constexpr uint8_t kLfoDelay = 26;
constexpr uint8_t kExtendedValue = 29;
constexpr uint8_t kExtendedCommand = 30;
constexpr uint8_t kExtendedValue2 = 31;
constexpr uint8_t kPriority = 33;
constexpr uint8_t kPriority2 = 39;
constexpr uint8_t kDataEntryFine = seqlib::midi::cc::kDataEntryLsb;
constexpr uint8_t kParameterFine = seqlib::midi::cc::kRpnLsb;
constexpr uint8_t kParameter = seqlib::midi::cc::kRpnMsb;
constexpr uint8_t kLoopStart = seqlib::midi::cc::kLoopStart;

// The controllers that select a non-registered parameter. Data entry after them sets no registered parameter, as after
// the null parameter, 127 and 127.
constexpr uint8_t kNonRegisteredFine = seqlib::midi::cc::kNrpnLsb;
constexpr uint8_t kNonRegistered = seqlib::midi::cc::kNrpnMsb;
constexpr int kNullParameter = seqlib::midi::cc::kNullRpn;

// The extended commands that control change 30 chooses: the echo's level and its length.
constexpr uint8_t kEchoVolume = 8;
constexpr uint8_t kEchoLength = 9;

// The memory accesses that control change 13 chooses. 0 to 2 set, add and subtract a value, and 3 to 5 the value of
// another byte. 6 to 11 jump to a label when the byte compares with a value, and 12 to 17 when it compares with
// another byte: equal, not equal, higher, higher or the same, lower or the same, and lower.
constexpr int kMemoryOperations = 18;
constexpr int kFirstJump = 6;
constexpr int kFirstByteJump = 12;

// The value of a byte from the number of a text event, such as "lfos=22": the number clamped to `low` and `high`, or
// nothing for text that contains no number.
std::optional<int> NumberAfter(const std::string& text, std::size_t at, int low, int high)
{
    const std::string rest = text.substr(at);
    char* end = nullptr;
    const long value = std::strtol(rest.c_str(), &end, 10);
    if (end == rest.c_str())
    {
        return std::nullopt;
    }

    return static_cast<int>(std::clamp<long>(value, low, high));
}

// A level after the natural curve that "nat=1" chooses: the level's fraction of 127 to the power 10/6.
double Natural(double fraction)
{
    return 127.0 * std::pow(std::clamp(fraction, 0.0, 1.0), 10.0 / 6.0);
}

// The text of a text-like meta event: types 1 to 7.
std::optional<std::string> TextOf(const seqlib::midi::Event& event)
{
    if (event.bytes.size() < 3 || event.bytes[0] != 0xFF || event.bytes[1] < 0x01 || event.bytes[1] > 0x07)
    {
        return std::nullopt;
    }

    const auto [type, data] = MetaOf(event);
    return std::string(data.begin(), data.end());
}

// Whether `a` comes before `b` in the order of the MIDI file.
bool InFileOrder(const ChannelEvent& a, const ChannelEvent& b)
{
    return std::tie(a.tick, a.chunk, a.index) < std::tie(b.tick, b.chunk, b.index);
}

// Adds `entry` to the events of each channel of `named` that has notes.
void AddToNamed(std::map<uint8_t, std::vector<ChannelEvent>>& channels, const std::set<uint8_t>& note_channels,
                const std::set<uint8_t>& named, const ChannelEvent& entry)
{
    for (const uint8_t channel : named)
    {
        if (note_channels.contains(channel))
        {
            channels[channel].push_back(entry);
        }
    }
}

// The events of each channel with notes, in the order of the file, with the text events of the track chunks that name
// the channel, or of the chunks that name none. `note_channels` contains the channels with notes, and `chunk_channels`
// the channels that each track chunk names. `left_out` counts the events that stay out.
std::map<uint8_t, std::vector<ChannelEvent>> ChannelEventsOf(const seqlib::midi::File& file,
                                                             const std::set<uint8_t>& note_channels,
                                                             std::span<const std::set<uint8_t>> chunk_channels,
                                                             std::map<std::string, std::size_t>& left_out)
{
    std::map<uint8_t, std::vector<ChannelEvent>> channels;
    for (std::size_t t = 0; t < file.tracks.size(); ++t)
    {
        for (std::size_t i = 0; i < file.tracks[t].events.size(); ++i)
        {
            const seqlib::midi::Event& event = file.tracks[t].events[i];
            const uint8_t status = event.bytes.empty() ? 0 : event.bytes[0];
            if (status >= 0x80 && status < 0xF0)
            {
                const auto channel = static_cast<uint8_t>(status & 0x0F);
                if (note_channels.contains(channel))
                {
                    channels[channel].push_back({event.tick, t, i, &event});
                }
                else
                {
                    ++left_out["channels without notes"];
                }

                continue;
            }

            if (status == 0xF0 || status == 0xF7)
            {
                ++left_out["system exclusive"];
                continue;
            }

            if (TextOf(event))
            {
                AddToNamed(channels, note_channels, chunk_channels[t].empty() ? note_channels : chunk_channels[t],
                           {event.tick, t, i, &event});
            }
        }
    }

    for (auto& [channel, events] : channels)
    {
        std::stable_sort(events.begin(), events.end(), InFileOrder);
    }

    return channels;
}

// The tempo events, in the order of the file.
std::vector<ChannelEvent> TempoEventsOf(const seqlib::midi::File& file)
{
    std::vector<ChannelEvent> tempos;
    for (std::size_t t = 0; t < file.tracks.size(); ++t)
    {
        for (std::size_t i = 0; i < file.tracks[t].events.size(); ++i)
        {
            const seqlib::midi::Event& event = file.tracks[t].events[i];
            if (event.bytes.size() >= 3 && event.bytes[0] == 0xFF && event.bytes[1] == 0x51)
            {
                tempos.push_back({event.tick, t, i, &event});
            }
        }
    }

    std::stable_sort(tempos.begin(), tempos.end(), InFileOrder);

    return tempos;
}

} // namespace

namespace gba_midi_reader_parts
{

Placed Place(uint64_t tick, Order order, uint64_t serial, std::optional<Command> command, int label,
             std::optional<GbaMidiInstrument> instrument)
{
    return {tick, order, serial, std::move(command), label, instrument};
}

Command Setting(Kind kind, int64_t value, uint8_t index)
{
    Command command;
    command.kind = kind;
    command.index = index;
    command.value.value = ntdlib::commands::ToUnit(value, ntdlib::mp2k::ScaleOf(kind));
    return command;
}

GbaMidiSong GbaMidiReader::Read()
{
    ScanTracks();
    FindLoop();

    const std::map<uint8_t, std::vector<ChannelEvent>> channels =
        ChannelEventsOf(file_, note_channels_, chunk_channels_, left_out_);
    const std::vector<ChannelEvent> tempos = TempoEventsOf(file_);
    for (const auto& [channel, events] : channels)
    {
        ChannelCommands(channel, events, tempos);
    }

    if (tempo_steps_ > 0)
    {
        song_.warnings.push_back(std::to_string(tempo_steps_) + (tempo_steps_ == 1 ? " tempo is" : " tempos are") +
                                 " rounded to the nearest even BPM. GBA tempos must be even.");
    }

    for (std::size_t t = 0; t < tracks_.size(); t++)
    {
        TrackCommands(t);
    }

    AddWarnings();

    return std::move(song_);
}

uint64_t GbaMidiReader::TickOf(uint64_t midi_tick) const
{
    return (midi_tick * kGbaTimebase * 2 + division_) / (division_ * 2);
}

bool GbaMidiReader::Moves(uint64_t midi_tick) const
{
    return midi_tick * kGbaTimebase % division_ != 0;
}

bool GbaMidiReader::PastLoop(uint64_t tick) const
{
    return loop_end_ && tick >= *loop_end_;
}

void GbaMidiReader::ScanTracks()
{
    for (std::size_t t = 0; t < file_.tracks.size(); ++t)
    {
        file_end_ = std::max(file_end_, file_.tracks[t].end_tick);
        for (const seqlib::midi::Event& event : file_.tracks[t].events)
        {
            const uint8_t status = event.bytes.empty() ? 0 : event.bytes[0];
            if (status >= 0x80 && status < 0xF0)
            {
                chunk_channels_[t].insert(static_cast<uint8_t>(status & 0x0F));
                if ((status & 0xF0) == 0x90 && event.bytes.size() > 2 && event.bytes[2] != 0)
                {
                    note_channels_.insert(static_cast<uint8_t>(status & 0x0F));
                }

                if ((status & 0xF0) == 0xB0 && event.bytes.size() > 2 && event.bytes[1] == kLoopStart &&
                    !global_.loop_cc)
                {
                    global_.loop_cc = event.tick;
                }

                continue;
            }

            const std::optional<std::string> text = TextOf(event);
            if (text)
            {
                ScanText(*text, event.tick);
            }
        }
    }
    if (note_channels_.empty())
    {
        throw std::invalid_argument("The MIDI file has no notes.");
    }

    song_.priority = global_.priority;
    song_.reverb = global_.reverb;
}

void GbaMidiReader::ScanText(const std::string& text, uint64_t tick)
{
    const std::vector<uint8_t> data(text.begin(), text.end());
    if ((text == "[" || NamesLoop(data, false)) && !global_.loop_start)
    {
        global_.loop_start = tick;
    }
    else if ((text == "]" || NamesLoop(data, true)) && !global_.loop_end)
    {
        global_.loop_end = tick;
    }
    else if (text.starts_with("mvl="))
    {
        global_.master_volume = NumberAfter(text, 4, 0, 128).value_or(global_.master_volume);
    }
    else if (text.starts_with("nat="))
    {
        global_.natural = NumberAfter(text, 4, 0, 1).value_or(0) == 1;
    }
    else if (text.starts_with("modscale_global="))
    {
        global_.mod_scale = std::clamp(std::strtod(text.c_str() + 16, nullptr), 0.0, 16.0);
    }
    else if (text.starts_with("pri="))
    {
        if (const std::optional<int> value = NumberAfter(text, 4, 0, 127))
        {
            global_.priority = static_cast<uint8_t>(*value);
        }
    }
    else if (text.starts_with("rev="))
    {
        if (const std::optional<int> value = NumberAfter(text, 4, 0, 127))
        {
            global_.reverb = static_cast<uint8_t>(*value > 0 ? 0x80 + *value : 0);
        }
    }
    else if (text.starts_with("modt_global="))
    {
        global_.lfo_type = NumberAfter(text, 12, 0, 255);
    }
    else if (text.starts_with("lfos_global="))
    {
        global_.lfo_speed = NumberAfter(text, 12, 0, 255);
    }
    else if (text.starts_with("lfodl_global="))
    {
        global_.lfo_delay = NumberAfter(text, 13, 0, 255);
    }
    else if (text == "][")
    {
        ++left_out_["extra loop markers"];
    }
    else if (text.starts_with("vgr="))
    {
        ++left_out_["voice group names (vgr=)"];
    }
}

void GbaMidiReader::FindLoop()
{
    // The loop's start and end, in the song's ticks. Without a marker of its start, control change 111 starts it. A
    // loop without an end ends at the file's end.
    loop_start_ = global_.loop_start ? global_.loop_start : global_.loop_cc;
    if (loop_start_)
    {
        loop_end_ = TickOf(global_.loop_end.value_or(file_end_));
        loop_start_ = TickOf(*loop_start_);
        if (*loop_end_ <= *loop_start_)
        {
            song_.warnings.push_back(
                "The loop end in the MIDI file doesn't come after the loop start. The song plays once.");
            loop_start_.reset();
            loop_end_.reset();
        }
    }
}

int GbaMidiReader::VoiceOf(const GbaMidiInstrument& instrument) const
{
    const auto found = voices_.find(instrument);
    return found != voices_.end() ? found->second : instrument.program;
}

int GbaMidiReader::VolumeOf(int volume, int expression) const
{
    const double fraction = volume * expression * global_.master_volume / (127.0 * 127.0 * 128.0);
    const double level = global_.natural ? Natural(fraction) : 127.0 * fraction;
    return static_cast<int>(std::clamp<long>(std::lround(level), 0, 127));
}

int GbaMidiReader::VelocityOf(int velocity) const
{
    const double level = global_.natural ? Natural(velocity / 127.0) : velocity;
    return static_cast<int>(std::clamp<long>(std::lround(level), 1, 127));
}

void GbaMidiReader::ChannelCommands(uint8_t channel, const std::vector<ChannelEvent>& events,
                                    const std::vector<ChannelEvent>& tempos)
{
    const bool drums = channel == kDrumChannel;
    drum_tracks_.push_back(drums);
    uint64_t channel_end = 0;
    for (std::size_t t = 0; t < file_.tracks.size(); ++t)
    {
        if (chunk_channels_[t].contains(channel))
        {
            channel_end = std::max(channel_end, file_.tracks[t].end_tick);
        }
    }

    track_ends_.push_back(TickOf(channel_end));

    ChannelState state;
    state.instrument = GbaMidiInstrument{drums, 0};
    if (tracks_.empty())
    {
        AddTempos(state, tempos);
    }

    AddGlobalSettings(state);
    for (const ChannelEvent& channel_event : events)
    {
        PlaceEvent(state, *channel_event.event);
    }

    tracks_.push_back(std::move(state.placed));
}

void GbaMidiReader::Add(ChannelState& channel, uint64_t midi_tick, Order order, Command command)
{
    if (Moves(midi_tick))
    {
        ++moved_;
    }

    channel.placed.push_back(Place(TickOf(midi_tick), order, serial_++, std::move(command)));
}

void GbaMidiReader::AddTempos(ChannelState& channel, const std::vector<ChannelEvent>& tempos)
{
    // The tempos go into the first track. A file without a tempo at tick 0 starts at MIDI's tempo.
    if (tempos.empty() || tempos.front().tick != 0)
    {
        channel.placed.push_back(Place(0, kSetting, serial_++, Setting(Kind::kTempo, 60000000 / kMidiTempo / 2)));
    }

    for (const ChannelEvent& tempo : tempos)
    {
        const auto [type, data] = MetaOf(*tempo.event);
        if (data.size() < 3)
        {
            continue;
        }

        const uint32_t micros = (uint32_t{data[0]} << 16) | (uint32_t{data[1]} << 8) | data[2];
        const double bpm = micros == 0 ? 510.0 : 60000000.0 / micros;
        const auto half = std::clamp<long>(std::lround(bpm / 2.0), 1, 255);
        if (std::abs(bpm - 2.0 * static_cast<double>(half)) > 0.005)
        {
            ++tempo_steps_;
        }

        Add(channel, tempo.tick, kSetting, Setting(Kind::kTempo, half));
    }
}

void GbaMidiReader::AddGlobalSettings(ChannelState& channel)
{
    // The settings for the whole song open each track.
    if (global_.lfo_type)
    {
        channel.placed.push_back(Place(0, kSetting, serial_++, Setting(Kind::kLfoType, *global_.lfo_type)));
    }

    if (global_.lfo_speed)
    {
        channel.placed.push_back(Place(0, kSetting, serial_++, Setting(Kind::kLfoSpeed, *global_.lfo_speed)));
    }

    if (global_.lfo_delay)
    {
        channel.placed.push_back(Place(0, kSetting, serial_++, Setting(Kind::kLfoDelay, *global_.lfo_delay)));
    }
}

void GbaMidiReader::PlaceEvent(ChannelState& channel, const seqlib::midi::Event& event)
{
    const uint8_t status = event.bytes[0];
    const uint8_t data = event.bytes.size() > 1 ? event.bytes[1] : 0;
    const uint8_t value = event.bytes.size() > 2 ? event.bytes[2] : 0;
    const bool note_off =
        ((status & 0xF0) == 0x80 || ((status & 0xF0) == 0x90 && value == 0)) && event.bytes.size() > 2;

    // A note-off ends its note wherever the note-off falls: past the loop's end too.
    if (note_off)
    {
        EndNote(channel, event, data);
        return;
    }

    if (PastLoop(TickOf(event.tick)))
    {
        if (status != 0xFF)
        {
            ++left_out_["after the loop end"];
        }

        return;
    }

    if (status == 0xFF)
    {
        PlaceText(channel, event);
        return;
    }

    switch (status & 0xF0)
    {
    case 0x90:
        {
            // A note: its length comes with its note-off. A note without one ends at the file's end.
            Command note;
            note.kind = Kind::kNote;
            note.key = data;
            note.velocity = int64_t{VelocityOf(value)} * kOne;
            const uint64_t tick = TickOf(event.tick);
            note.value.value = static_cast<int64_t>(std::max<uint64_t>(TickOf(file_end_), tick + 1) - tick) * kOne;
            channel.open[data].push_back(channel.placed.size());
            Add(channel, event.tick, kNote, std::move(note));
            channel.placed.back().instrument = channel.instrument;
            break;
        }

    case 0xB0:
        PlaceControlChange(channel, event, status, data, value);
        break;

    case 0xC0:
        channel.instrument.program = data;
        Add(channel, event.tick, kSetting, Setting(Kind::kProgram, VoiceOf(channel.instrument)));
        channel.placed.back().instrument = channel.instrument;
        break;

    case 0xE0:
        {
            // The bend in the GBA's 64 steps on each side of the centre.
            const int bend = int{value} << 7 | data;
            Add(channel, event.tick, kSetting, Setting(Kind::kPitchBend, std::min(127, (bend + 64) >> 7)));
            break;
        }

    default:
        ++left_out_[LeftOut(status, data)];
        break;
    }
}

void GbaMidiReader::EndNote(ChannelState& channel, const seqlib::midi::Event& event, uint8_t data)
{
    auto found = channel.open.find(data);
    if (found == channel.open.end() || found->second.empty())
    {
        return;
    }

    Placed& note = channel.placed[found->second.front()];
    found->second.pop_front();
    if (Moves(event.tick))
    {
        ++moved_;
    }

    note.command->value.value =
        static_cast<int64_t>(std::max<uint64_t>(TickOf(event.tick), note.tick + 1) - note.tick) * kOne;
}

void GbaMidiReader::PlaceText(ChannelState& channel, const seqlib::midi::Event& event)
{
    // A text event of the tracks: the LFO's settings, the fine tune and the track's priority.
    const std::string text = *TextOf(event);
    if (text.starts_with("modt="))
    {
        Add(channel, event.tick, kSetting, Setting(Kind::kLfoType, NumberAfter(text, 5, 0, 255).value_or(0)));
    }
    else if (text.starts_with("lfos="))
    {
        Add(channel, event.tick, kSetting, Setting(Kind::kLfoSpeed, NumberAfter(text, 5, 0, 255).value_or(0)));
    }
    else if (text.starts_with("lfodl="))
    {
        Add(channel, event.tick, kSetting, Setting(Kind::kLfoDelay, NumberAfter(text, 6, 0, 255).value_or(0)));
    }
    else if (text.starts_with("tune="))
    {
        Add(channel, event.tick, kSetting, Setting(Kind::kTune, NumberAfter(text, 5, -64, 63).value_or(0) + 64));
    }
    else if (text.starts_with("prio="))
    {
        Add(channel, event.tick, kSetting, Setting(Kind::kPriority, NumberAfter(text, 5, 0, 255).value_or(0)));
    }
}

void GbaMidiReader::PlaceControlChange(ChannelState& channel, const seqlib::midi::Event& event, uint8_t status,
                                       uint8_t data, uint8_t value)
{
    switch (data)
    {
    case kVolume:
        channel.volume = value;
        Add(channel, event.tick, kSetting, Setting(Kind::kVolume, VolumeOf(channel.volume, channel.expression)));
        break;

    case kExpression:
        channel.expression = value;
        Add(channel, event.tick, kSetting, Setting(Kind::kVolume, VolumeOf(channel.volume, channel.expression)));
        break;

    case kPan:
        Add(channel, event.tick, kSetting, Setting(Kind::kPan, value));
        break;

    case kModulation:
        Add(channel, event.tick, kSetting,
            Setting(Kind::kLfoDepth, std::clamp<long>(std::lround(value * global_.mod_scale), 0, 127)));
        break;

    case kBendRange:
        Add(channel, event.tick, kSetting, Setting(Kind::kBendRange, value));
        break;

    case kLfoSpeed:
        Add(channel, event.tick, kSetting, Setting(Kind::kLfoSpeed, value));
        break;

    case kLfoType:
        Add(channel, event.tick, kSetting, Setting(Kind::kLfoType, value));
        break;

    case kTune:
        Add(channel, event.tick, kSetting, Setting(Kind::kTune, value));
        break;

    case kLfoDelay:
        Add(channel, event.tick, kSetting, Setting(Kind::kLfoDelay, value));
        break;

    case kPriority:
    case kPriority2:
        Add(channel, event.tick, kSetting, Setting(Kind::kPriority, value));
        break;

    case kParameter:
    case kParameterFine:
    case kNonRegistered:
    case kNonRegisteredFine:
    case kDataEntry:
    case kDataEntryFine:
        PlaceParameter(channel, event, status, data, value);
        break;

    case kMemoryOperation:
        channel.operation = value;
        break;

    case kMemoryByte:
        channel.memory_byte = value;
        break;

    case kMemoryLabel:
        channel.memory_label = value;
        break;

    case kMemoryAccess:
    case kMemoryAccess2:
        PlaceMemoryAccess(channel, event, value);
        break;

    case kLabel:
        if (Moves(event.tick))
        {
            ++moved_;
        }

        channel.placed.push_back(Place(TickOf(event.tick), kSetting, serial_++, std::nullopt, value));
        break;

    case kExtendedCommand:
        channel.extended = value;
        break;

    case kExtendedValue:
    case kExtendedValue2:
        if (channel.extended == kEchoVolume || channel.extended == kEchoLength)
        {
            Add(channel, event.tick, kSetting,
                Setting(channel.extended == kEchoVolume ? Kind::kEchoVolume : Kind::kEchoLength, value));
        }
        else
        {
            ++left_out_["extended commands other than echo"];
        }
        break;

    case kLoopStart:
        break;

    default:
        ++left_out_[LeftOut(status, data)];
        break;
    }
}

void GbaMidiReader::PlaceParameter(ChannelState& channel, const seqlib::midi::Event& event, uint8_t status,
                                   uint8_t data, uint8_t value)
{
    switch (data)
    {
    case kParameter:
        channel.parameter = value;
        break;

    case kParameterFine:
        channel.parameter_fine = value;
        break;

    case kNonRegistered:
    case kNonRegisteredFine:
        channel.parameter = kNullParameter;
        channel.parameter_fine = kNullParameter;
        ++left_out_[LeftOut(status, data)];
        break;

    case kDataEntry:
        if (channel.parameter == 0 && channel.parameter_fine == 0)
        {
            Add(channel, event.tick, kSetting, Setting(Kind::kBendRange, value));
        }
        else
        {
            ++left_out_["data entry for other parameters"];
        }
        break;

    case kDataEntryFine:
        if (channel.parameter != 0 || channel.parameter_fine != 0)
        {
            ++left_out_["data entry for other parameters"];
        }
        else if (value != 0)
        {
            ++left_out_["fine bend range (cents)"];
        }
        break;
    }
}

void GbaMidiReader::PlaceMemoryAccess(ChannelState& channel, const seqlib::midi::Event& event, uint8_t value)
{
    if (channel.operation >= kMemoryOperations)
    {
        ++left_out_["memory access with other operations"];
        return;
    }

    // 0 to 5 change the byte, and 6 to 17 jump. A second form uses the value of another byte.
    static constexpr Kind kChanges[] = {Kind::kSetVariable, Kind::kAddVariable, Kind::kSubtractVariable};
    static constexpr Kind kJumps[] = {Kind::kJumpIfEqual,          Kind::kJumpIfNotEqual,    Kind::kJumpIfGreater,
                                      Kind::kJumpIfGreaterOrEqual, Kind::kJumpIfLessOrEqual, Kind::kJumpIfLess};
    Command access;
    access.index = static_cast<uint8_t>(channel.memory_byte);
    const bool jumps = channel.operation >= kFirstJump;
    const int form = jumps ? channel.operation - kFirstJump : channel.operation;
    access.kind = jumps ? kJumps[form % 6] : kChanges[form % 3];
    access.value.source =
        (jumps ? channel.operation >= kFirstByteJump : channel.operation >= 3) ? Source::kVariable : Source::kValue;
    access.value.value = access.value.source == Source::kVariable ? value : int64_t{value} * kOne;
    Add(channel, event.tick, kSetting, std::move(access));
    if (jumps)
    {
        channel.placed.back().label = channel.memory_label;
    }
}

} // namespace gba_midi_reader_parts

GbaMidiSong GbaSongOfMidi(std::span<const uint8_t> midi, const GbaVoiceMap& voices, int note_arguments)
{
    const seqlib::midi::File file = ReadMidiToImport(midi);
    return GbaMidiReader(file, voices, note_arguments).Read();
}

} // namespace hokuto::edit
