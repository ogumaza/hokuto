// SPDX-License-Identifier: MIT

// The import of a MIDI file as a song (SongOfMidi in edit/midi_songs.h): the MidiImport class and its steps, the
// joins of layers, and the writer of the song's code.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/sequence_export/filter.h"
#include "seqlib/midi/controllers.h"
#include "seqlib/midi/reader.h"

#include "edit/converter.h"
#include "edit/midi_events.h"
#include "edit/midi_songs.h"
#include "edit/midi_songs_parts.h"
#include "format/file.h"
#include "format/kinds.h"

namespace hokuto::edit
{
namespace
{

using midi_songs_parts::Holds;
using midi_songs_parts::MidiTrack;
using midi_songs_parts::PlaceControlChange;
using midi_songs_parts::Placed;
using midi_songs_parts::PlaceNoteOff;
using midi_songs_parts::PlaceNoteOn;
using midi_songs_parts::PlacePitchBend;
using midi_songs_parts::PlaceProgramChange;
using midi_songs_parts::Setting;
using midi_songs_parts::TimeKeepingTempos;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Sequence;

// The most tracks that a song of NintendoWare's engines has.
constexpr std::size_t kMostTracks = 16;

// The timebase that a song without a timebase command plays at: NintendoWare's default.
constexpr uint32_t kDefaultTimebase = 48;

// The MIDI file's microseconds a quarter note before its first tempo event: 120 beats a minute.
constexpr uint32_t kDefaultTempo = seqlib::midi::kDefaultMicrosPerQuarter;

// The beats of a held note: ntdlib's kHeldTicks (nwsnd/sequence.h) at the default timebase. Such a note sounds until
// the game stops the sound.
constexpr uint32_t kHeldBeats = 200;

// The controller that RPG Maker reads as the start of the loop without a marker.
constexpr uint8_t kLoopStart = seqlib::midi::cc::kLoopStart;

// Joins the layers of `tracks`, channels of the MIDI file's tracks with `names`: a MIDI track named after an earlier
// one and " (layer n)" goes into the track of the earlier one. The joined track's commands keep the order of their
// ticks, the earlier track's first. A setting of a layer that only repeats the value that its track has there is left
// out. Returns the number of layers that joined.
std::size_t JoinLayers(std::vector<MidiTrack>& tracks, const std::vector<std::string>& names)
{
    std::size_t joined = 0;
    for (std::size_t t = 0; t < tracks.size();)
    {
        // The name of the track that the layer joins.
        const std::string& name = names[tracks[t].midi_track];
        const std::size_t open = name.rfind(" (layer ");
        const bool layer = open != std::string::npos && name.ends_with(')') && open + 9 < name.size() &&
                           std::all_of(name.begin() + static_cast<std::ptrdiff_t>(open + 8), name.end() - 1,
                                       [](char c) { return c >= '0' && c <= '9'; });
        const std::string first = layer ? name.substr(0, open) : std::string();
        const auto end = tracks.begin() + static_cast<std::ptrdiff_t>(t);
        const auto into = std::find_if(
            tracks.begin(), end, [&](const MidiTrack& track) { return layer && names[track.midi_track] == first; });
        if (into == end)
        {
            ++t;
            continue;
        }

        std::vector<Placed> placed = std::move(into->placed);
        for (Placed command : tracks[t].placed)
        {
            command.layer = true;
            placed.push_back(std::move(command));
        }

        std::stable_sort(placed.begin(), placed.end(),
                         [](const Placed& a, const Placed& b) { return a.tick < b.tick; });
        std::map<std::pair<Kind, uint8_t>, int64_t> values;
        into->placed.clear();
        for (Placed& command : placed)
        {
            if (command.command.kind != Kind::kNote)
            {
                const auto [value, added] =
                    values.try_emplace(std::pair(command.command.kind, command.command.index), 0);
                if (!added && command.layer && value->second == command.command.value.value)
                {
                    continue;
                }

                value->second = command.command.value.value;
            }

            into->placed.push_back(std::move(command));
        }

        into->end = std::max(into->end, tracks[t].end);
        tracks.erase(end);
        ++joined;
    }

    return joined;
}

// Whether `track` plays a note.
bool PlaysNotes(const MidiTrack& track)
{
    return std::ranges::any_of(track.placed, [](const Placed& p) { return p.command.kind == Kind::kNote; });
}

// Writes the waits and the loop of one track's code (SongOfMidi). `loop` contains the start and the end of the loop in
// ticks of the song, and `label` the label of the track.
class TrackCode
{
public:
    TrackCode(LabelWriter& writer, const std::optional<std::pair<uint32_t, uint32_t>>& loop, const std::string& label)
        : writer_(writer), loop_(loop), label_(label)
    {
    }

    // Writes a wait from the current tick up to `to`, when `to` is later.
    void WaitTo(uint32_t to)
    {
        if (to > tick_)
        {
            writer_.Item(Setting(Kind::kWait, int64_t{to - tick_} * kOne));
            tick_ = to;
        }
    }

    // Writes the loop's start label and then its jump, each once, when it comes at or before `to`.
    void LoopTo(uint32_t to)
    {
        if (loop_ && !looped_ && loop_->first <= to)
        {
            WaitTo(loop_->first);
            writer_.Label(label_ + "_LoopStart");
            looped_ = true;
        }

        if (loop_ && !jumped_ && loop_->second <= to)
        {
            WaitTo(loop_->second);
            writer_.Item(Setting(Kind::kJump, 0), label_ + "_LoopStart");
            jumped_ = true;
        }
    }

    bool Jumped() const
    {
        return jumped_;
    }

private:
    LabelWriter& writer_;
    const std::optional<std::pair<uint32_t, uint32_t>>& loop_;
    const std::string& label_;
    uint32_t tick_ = 0;
    bool looped_ = false;
    bool jumped_ = false;
};

// Reads a MIDI file into a song (SongOfMidi). Import runs the steps in order, and each step uses the members that the
// steps before it set.
class MidiImport
{
public:
    MidiImport(const seqlib::midi::File& file, const MidiOptions& options) : file_(file), options_(options)
    {
    }

    // The song, with `name` in its labels.
    MidiSong Import(const std::string& name);

private:
    // The tracks of the song that hold low-pass control changes of a converter's MIDI file, the file's end, the loop's
    // start and end, and the tracks' names.
    void ScanFile();

    // Places the events of each channel in its track.
    void PlaceEvents();

    // Places `event` of track `t` of the MIDI file in the track of its channel.
    void PlaceEvent(std::size_t t, const seqlib::midi::Event& event);

    // Places the channel message `event` in `track` by its status: a note, a control change, a program change or a
    // pitch bend. `data` and `value` are the message's data bytes, and `channel` its channel.
    void PlaceChannelMessage(MidiTrack& track, const seqlib::midi::Event& event, uint8_t status, uint8_t channel,
                             uint8_t data, uint8_t value);

    // Keeps the tempo, the time signature or the marker of `event`, a meta event.
    void PlaceMeta(const seqlib::midi::Event& event);

    // Whether `event` comes at or after the loop's end, with `ends_at_loop`.
    bool PastLoop(const seqlib::midi::Event& event) const;

    // Joins layers or leaves out channels where the file has more than 16.
    void LimitTracks();

    // Marks the notes that hold past the MIDI file's end.
    void MarkHeldNotes();

    // Leaves out the notes that never end.
    void DropEndlessNotes();

    // Sets the song's first MIDI tick.
    void FindStart();

    // MIDI tick `tick` in the song's ticks.
    uint32_t Ticks(uint64_t tick) const;

    // Counts the events between two of the song's ticks, with `imports`.
    void CountMoved();

    // Sets the bar lines of the MIDI file.
    void FindBarLines();

    // Sets the bars of the song's hints.
    void AddBars();

    // Sets the loop in the song's ticks.
    void FindLoop();

    // Puts the tempo commands into the first track.
    void AddTempos();

    // The song's code: the header, then each track.
    Sequence WriteCode(const std::string& name);

    // The hints of track `t`'s blocks before the song's start.
    format::TrackHints OpeningHints(std::size_t t) const;

    // Writes the code of `track`, with `label`.
    void WriteTrack(LabelWriter& writer, const MidiTrack& track, const std::string& label);

    // Adds the warnings that count what the song changed or left out.
    void AddWarnings();

    const seqlib::midi::File& file_;
    const MidiOptions& options_;
    MidiSong song_;
    std::map<std::string, std::size_t> left_out_;

    // The tracks, the tempo, the time signatures and the markers, in the order of the file.
    std::vector<MidiTrack> tracks_;
    std::map<std::pair<std::size_t, uint8_t>, std::size_t> track_of_;
    std::vector<std::pair<uint64_t, uint32_t>> tempos_;
    std::vector<std::pair<uint64_t, std::pair<uint8_t, uint8_t>>> signatures_;
    std::vector<uint64_t> markers_;
    std::vector<std::string> names_;
    std::optional<uint64_t> loop_start_;
    std::optional<uint64_t> loop_end_;
    uint64_t file_end_ = 0;
    uint64_t midi_end_ = 0;
    bool exact_low_pass_ = false;

    // The song's first MIDI tick, the MIDI file's bar lines and the loop in the song's ticks.
    uint64_t start_ = UINT64_MAX;
    std::vector<uint64_t> lines_;
    std::optional<std::pair<uint32_t, uint32_t>> loop_;

    std::size_t endless_ = 0;
    std::size_t biquad_values_ = 0;
    std::size_t outlasting_ = 0;
};

MidiSong MidiImport::Import(const std::string& name)
{
    ScanFile();
    PlaceEvents();
    LimitTracks();
    MarkHeldNotes();
    DropEndlessNotes();
    FindStart();
    CountMoved();
    FindBarLines();
    AddBars();
    FindLoop();
    AddTempos();

    // The converter's layout puts the song's labels in order, and checks that the song is one that it lays out.
    song_.timeline = LayOutConverted(WriteCode(name), song_.hints, false);
    AddWarnings();

    return std::move(song_);
}

void MidiImport::ScanFile()
{
    const auto low_pass_track = [](const seqlib::midi::Track& track)
    {
        const auto sets_low_pass = [](const seqlib::midi::Event& event)
        {
            return event.bytes.size() > 2 && (event.bytes[0] & 0xF0) == 0xB0 &&
                   event.bytes[1] == ntdlib::sequence_export::kSfzLowPassController;
        };
        return std::ranges::any_of(track.events, sets_low_pass);
    };
    exact_low_pass_ = options_.imports && std::ranges::any_of(file_.tracks, low_pass_track);

    // The file's end, the first markers of the loop's start and end, the first control change 111, and the tracks'
    // names.
    std::optional<uint64_t> loop_controller;
    names_.assign(file_.tracks.size(), std::string());
    for (std::size_t t = 0; t < file_.tracks.size(); ++t)
    {
        const seqlib::midi::Track& track = file_.tracks[t];
        file_end_ = std::max(file_end_, track.end_tick);
        for (const seqlib::midi::Event& event : track.events)
        {
            if (event.bytes.size() >= 3 && event.bytes[0] == 0xFF)
            {
                const auto [type, data] = MetaOf(event);
                if (type == 0x06 && NamesLoop(data, false) && !loop_start_)
                {
                    loop_start_ = event.tick;
                }
                else if (type == 0x06 && NamesLoop(data, true) && !loop_end_)
                {
                    loop_end_ = event.tick;
                }
                else if (type == 0x03 && names_[t].empty())
                {
                    names_[t].assign(data.begin(), data.end());
                }
            }
            else if (event.bytes.size() >= 3 && (event.bytes[0] & 0xF0) == 0xB0 && event.bytes[1] == kLoopStart &&
                     (!loop_controller || event.tick < *loop_controller))
            {
                loop_controller = event.tick;
            }
        }
    }
    if (!loop_start_)
    {
        loop_start_ = loop_controller;
    }
}

void MidiImport::PlaceEvents()
{
    // With `ends_at_loop`, the song ends at the loop's end, and the events from there on stay out. A note-off there
    // still ends its note.
    midi_end_ = file_end_;
    if (options_.ends_at_loop && loop_end_)
    {
        file_end_ = std::min(file_end_, *loop_end_);
    }

    for (std::size_t t = 0; t < file_.tracks.size(); ++t)
    {
        for (const seqlib::midi::Event& event : file_.tracks[t].events)
        {
            PlaceEvent(t, event);
        }
    }
}

void MidiImport::PlaceMeta(const seqlib::midi::Event& event)
{
    const auto [type, data] = MetaOf(event);
    if (type == 0x51 && data.size() == 3)
    {
        tempos_.emplace_back(event.tick, (uint32_t{data[0]} << 16) | (uint32_t{data[1]} << 8) | data[2]);
    }
    else if (type == 0x58 && data.size() >= 2)
    {
        signatures_.emplace_back(event.tick, std::pair(data[0], data[1]));
    }
    else if (type == 0x06)
    {
        markers_.push_back(event.tick);
    }
}

bool MidiImport::PastLoop(const seqlib::midi::Event& event) const
{
    return options_.ends_at_loop && loop_end_ && event.tick >= *loop_end_;
}

void MidiImport::PlaceEvent(std::size_t t, const seqlib::midi::Event& event)
{
    const uint8_t status = event.bytes.empty() ? 0 : event.bytes[0];
    const bool note_off =
        event.bytes.size() > 2 && ((status & 0xF0) == 0x80 || ((status & 0xF0) == 0x90 && event.bytes[2] == 0));
    if (PastLoop(event) && !(note_off && track_of_.contains(std::pair(t, static_cast<uint8_t>(status & 0x0F)))))
    {
        return;
    }

    if (status == 0xFF && event.bytes.size() >= 3)
    {
        PlaceMeta(event);
        return;
    }

    if (status == 0xF0 || status == 0xF7)
    {
        ++left_out_["system exclusive"];
        return;
    }

    if (status < 0x80 || status >= 0xF0 || event.bytes.size() < 2)
    {
        return;
    }

    // A channel gets a track at the first of its events that the song keeps. Control change 111 starts the loop where
    // no marker does.
    const auto channel = static_cast<uint8_t>(status & 0x0F);
    const uint8_t data = event.bytes[1];
    const uint8_t value = event.bytes.size() > 2 ? event.bytes[2] : 0;
    if ((status & 0xF0) == 0xB0 && data == kLoopStart)
    {
        return;
    }

    auto found = track_of_.find(std::pair(t, channel));
    if (found == track_of_.end() && note_off)
    {
        return;
    }

    if (found == track_of_.end() && !Holds(status, data, value, options_))
    {
        ++left_out_[LeftOut(status, data)];
        return;
    }

    if (found == track_of_.end())
    {
        found = track_of_.emplace(std::pair(t, channel), tracks_.size()).first;
        tracks_.emplace_back().midi_track = t;
    }

    MidiTrack& track = tracks_[found->second];
    track.end = std::max(track.end, event.tick);
    PlaceChannelMessage(track, event, status, channel, data, value);
}

void MidiImport::PlaceChannelMessage(MidiTrack& track, const seqlib::midi::Event& event, uint8_t status,
                                     uint8_t channel, uint8_t data, uint8_t value)
{
    switch (status & 0xF0)
    {
    case 0x90:
        if (value > 0)
        {
            PlaceNoteOn(track, event.tick, channel, data, value, options_);
            break;
        }
        [[fallthrough]];

    case 0x80:
        PlaceNoteOff(track, event.tick, data);
        break;

    case 0xB0:
        PlaceControlChange(track, event.tick, status, data, value, options_, exact_low_pass_, left_out_,
                           biquad_values_);
        break;

    case 0xC0:
        PlaceProgramChange(track, event.tick, data);
        break;

    case 0xE0:
        PlacePitchBend(track, event.tick, data, value);
        break;

    default:
        ++left_out_[LeftOut(status, data)];
        break;
    }
}

void MidiImport::LimitTracks()
{
    // A song has 16 tracks at most. With `joins_layers`, a file of more channels joins its layers.
    if (tracks_.size() > kMostTracks && options_.joins_layers)
    {
        const std::size_t channels = tracks_.size();
        const std::size_t joined = JoinLayers(tracks_, names_);
        if (joined > 0)
        {
            song_.warnings.push_back(
                "The MIDI file uses " + std::to_string(channels) +
                " channels, but NintendoWare sequences have at most 16 tracks. " +
                (joined == 1 ? std::string("1 layer was merged back into the track it came from.")
                             : std::to_string(joined) + " layers were merged back into the tracks they came from."));
        }
    }

    if (tracks_.size() > kMostTracks && options_.imports)
    {
        // The first 16 channels that play notes stay. The warnings name the others that do.
        const std::size_t channels = tracks_.size();
        std::erase_if(tracks_, std::not_fn(PlaysNotes));
        std::vector<std::string> others;
        for (std::size_t t = kMostTracks; t < tracks_.size(); ++t)
        {
            const std::string& track_name = names_[tracks_[t].midi_track];
            others.push_back(track_name.empty() ? "track " + std::to_string(tracks_[t].midi_track + 1) : track_name);
        }

        if (tracks_.size() > kMostTracks)
        {
            tracks_.resize(kMostTracks);
        }

        if (!others.empty())
        {
            std::string list;
            for (std::size_t other = 0; other < others.size(); ++other)
            {
                list += (other == 0 ? "" : ", ") + others[other];
            }

            song_.warnings.push_back("The MIDI file uses " + std::to_string(channels) +
                                     " channels, but a sequence has at most 16 tracks. The first 16 channels with "
                                     "notes are kept. Left out: " +
                                     list + ".");
        }
    }
    if (tracks_.size() > kMostTracks)
    {
        throw std::invalid_argument(
            "The MIDI file has events for more than 16 tracks. NintendoWare sequences have at most 16 tracks.");
    }
}

void MidiImport::MarkHeldNotes()
{
    // With `ends_at_loop`, a note before the loop's start whose note-off comes at the MIDI file's end sounds on past
    // it: the file's end cut it. The note holds.
    for (MidiTrack& track : tracks_)
    {
        for (Placed& placed : track.placed)
        {
            placed.held = options_.ends_at_loop && loop_start_ && loop_end_ && placed.command.kind == Kind::kNote &&
                          placed.tick < *loop_start_ && placed.off && *placed.off >= midi_end_ &&
                          *placed.off > *loop_end_;
        }
    }
}

void MidiImport::DropEndlessNotes()
{
    // The notes that never end stay out.
    for (MidiTrack& track : tracks_)
    {
        const auto ends = [](const Placed& placed)
        {
            return placed.command.kind != Kind::kNote || placed.off;
        };
        endless_ += static_cast<std::size_t>(std::ranges::count_if(track.placed, std::not_fn(ends)));
        std::erase_if(track.placed, std::not_fn(ends));
    }

    if (std::ranges::none_of(tracks_, PlaysNotes))
    {
        throw std::invalid_argument("The MIDI file has no notes.");
    }
}

void MidiImport::FindStart()
{
    // The song starts at its first note, or at a marker before it. The ticks then count from there.
    for (const MidiTrack& track : tracks_)
    {
        for (const Placed& placed : track.placed)
        {
            if (placed.command.kind == Kind::kNote)
            {
                start_ = std::min(start_, placed.tick);
                break;
            }
        }
    }

    for (const uint64_t marker : markers_)
    {
        start_ = std::min(start_, marker);
    }

    if (options_.from_tick_0)
    {
        start_ = 0;
    }
}

uint32_t MidiImport::Ticks(uint64_t tick) const
{
    const uint64_t half = options_.imports ? file_.division / 2 : 0;

    return tick <= start_ ? 0u : static_cast<uint32_t>(((tick - start_) * options_.timebase + half) / file_.division);
}

void MidiImport::CountMoved()
{
    // The events between two of the song's ticks move to the nearer one. The tempos follow the bars.
    if (!options_.imports)
    {
        return;
    }

    const auto between = [&](uint64_t tick)
    {
        return tick > start_ && (tick - start_) * options_.timebase % file_.division != 0;
    };
    for (const MidiTrack& track : tracks_)
    {
        for (const Placed& placed : track.placed)
        {
            song_.moved += between(placed.tick) ? 1 : 0;
            song_.moved += placed.off && between(*placed.off) ? 1 : 0;
        }
    }
    if (song_.moved > 0)
    {
        song_.warnings.push_back(
            std::to_string(song_.moved) +
            (song_.moved == 1 ? " event falls between ticks and is" : " events fall between ticks and are") +
            " moved to the nearest tick. The song uses " + std::to_string(options_.timebase) +
            " ticks per quarter note.");
    }
}

void MidiImport::FindBarLines()
{
    // The bar lines of the MIDI file, from tick 0 to past its end. A time signature starts a bar.
    std::stable_sort(signatures_.begin(), signatures_.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    uint64_t tick = 0;
    uint64_t length = uint64_t{file_.division} * 4;
    std::size_t next = 0;
    while (tick <= file_end_ || lines_.empty())
    {
        while (next < signatures_.size() && signatures_[next].first <= tick)
        {
            const auto [numerator, power] = signatures_[next].second;
            const uint64_t bar = uint64_t{numerator} * file_.division * 4 >> std::min<uint8_t>(power, 31);
            length = bar > 0 ? bar : length;
            ++next;
        }

        lines_.push_back(tick);
        const uint64_t following = next < signatures_.size() ? signatures_[next].first : UINT64_MAX;
        tick = std::min(tick + length, following);
    }

    lines_.push_back(tick);
}

void MidiImport::AddBars()
{
    // The bars from the song's start in runs of one length. A start between bar lines makes the first bar short.
    std::vector<uint32_t> starts = {0};
    for (const uint64_t line : lines_)
    {
        if (line > start_ && Ticks(line) > starts.back())
        {
            starts.push_back(Ticks(line));
        }
    }

    for (std::size_t bar = 0; bar + 1 < starts.size(); ++bar)
    {
        const uint32_t length = starts[bar + 1] - starts[bar];
        if (!song_.hints.bars.empty() && song_.hints.bars.back().length == length)
        {
            ++song_.hints.bars.back().count;
        }
        else
        {
            song_.hints.bars.push_back({length, 1});
        }
    }
    if (song_.hints.bars.empty())
    {
        song_.hints.bars.push_back({options_.timebase * 4, 1});
    }
}

void MidiImport::FindLoop()
{
    // The loop, in the song's ticks.
    if (!loop_start_ && !loop_end_)
    {
        return;
    }

    uint64_t song_end = 0;
    for (const MidiTrack& track : tracks_)
    {
        song_end = std::max(song_end, track.end);
    }

    const uint32_t from = Ticks(loop_start_.value_or(0));
    const uint32_t to = Ticks(loop_end_.value_or(std::max(song_end, file_end_)));
    if (to <= from)
    {
        throw std::invalid_argument("The loop end in the MIDI file doesn't come after the loop start.");
    }

    loop_ = std::pair(from, to);
}

void MidiImport::AddTempos()
{
    // The tempo goes into the first track.
    std::stable_sort(tempos_.begin(), tempos_.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    if (tempos_.empty() || tempos_.front().first > 0)
    {
        tempos_.insert(tempos_.begin(), {0, kDefaultTempo});
    }

    // The tempo commands of NITRO-Composer and DSE store a whole number of beats a minute too.
    const std::string whole_tempos = std::string(options_.engine) + " tempos must be whole BPM values.";
    std::vector<Placed> tempo_commands;
    if (options_.keeps_time)
    {
        tempo_commands = TimeKeepingTempos(tempos_, lines_, start_, loop_start_, loop_end_, file_end_, file_.division,
                                           options_.timebase);
        const auto whole = [](const std::pair<uint64_t, uint32_t>& tempo)
        {
            return 60'000'000 % std::max<uint32_t>(tempo.second, 1) == 0;
        };
        if (!std::ranges::all_of(tempos_, whole))
        {
            song_.warnings.push_back(whole_tempos +
                                     " From bar to bar, the song alternates between the whole tempos below and above "
                                     "each MIDI tempo, and keeps the timing.");
        }
    }
    else
    {
        bool rounded = false;
        for (const auto& [tick, micros] : tempos_)
        {
            // The nearest whole number of beats a minute.
            const uint32_t quarter = std::max<uint32_t>(micros, 1);
            const int64_t bpm = std::clamp<int64_t>((60'000'000 + quarter / 2) / quarter, 1, 0xFFFF);
            rounded = rounded || 60'000'000 % quarter != 0;
            tempo_commands.push_back({tick, Setting(Kind::kTempo, bpm * kOne), std::nullopt});
        }
        if (rounded)
        {
            song_.warnings.push_back(whole_tempos + " The MIDI tempos are rounded to the nearest whole BPM.");
        }
    }

    tracks_[0].placed.insert(tracks_[0].placed.begin(), tempo_commands.begin(), tempo_commands.end());
    std::stable_sort(tracks_[0].placed.begin(), tracks_[0].placed.end(),
                     [](const Placed& a, const Placed& b) { return a.tick < b.tick; });
}

Sequence MidiImport::WriteCode(const std::string& name)
{
    const std::string prefix = SongLabelPrefix(name);
    const auto track_label = [&prefix](std::size_t track)
    {
        return prefix + "Track_" + std::to_string(track);
    };
    Sequence code;
    LabelWriter writer(code);
    writer.Label(prefix + "Begin");
    writer.Item(Setting(Kind::kAllocateTracks, int64_t{(1 << tracks_.size()) - 1} * kOne));
    writer.Label(prefix + "Start");
    if (options_.timebase != kDefaultTimebase)
    {
        writer.Item(Setting(Kind::kTimebase, int64_t{options_.timebase} * kOne));
    }

    for (std::size_t t = 1; t < tracks_.size(); ++t)
    {
        Command open;
        open.kind = Kind::kOpenTrack;
        open.index = static_cast<uint8_t>(t);
        writer.Item(open, track_label(t));
    }

    for (std::size_t t = 0; t < tracks_.size(); ++t)
    {
        song_.hints.tracks.push_back(OpeningHints(t));
        WriteTrack(writer, tracks_[t], track_label(t));
    }

    writer.Label(prefix + "End");
    writer.Item(Setting(Kind::kEnd, 0));
    writer.Finish();

    return code;
}

format::TrackHints MidiImport::OpeningHints(std::size_t t) const
{
    format::TrackHints hints;
    hints.track = static_cast<uint8_t>(t);

    // The blocks before the song's start: a block for each bar of the MIDI file.
    std::optional<std::size_t> block_bar;
    for (const Placed& placed : tracks_[t].placed)
    {
        if (placed.tick >= start_)
        {
            break;
        }

        const auto bar =
            static_cast<std::size_t>(std::upper_bound(lines_.begin(), lines_.end(), placed.tick) - lines_.begin());
        if (bar != block_bar)
        {
            hints.opening.push_back(0);
            block_bar = bar;
        }

        ++hints.opening.back();
    }

    return hints;
}

void MidiImport::WriteTrack(LabelWriter& writer, const MidiTrack& track, const std::string& label)
{
    writer.Label(label);
    writer.Item(Setting(Kind::kNoteWait, 0));
    TrackCode code(writer, loop_, label);
    uint32_t end = Ticks(track.end);
    bool after_jump = false;
    for (const Placed& placed : track.placed)
    {
        // An event that moves to the nearer tick stays on its side of the loop's start and end.
        uint32_t at = Ticks(placed.tick);
        if (options_.imports && loop_ && loop_start_ && placed.tick < *loop_start_ && loop_->first > 0)
        {
            at = std::min(at, loop_->first - 1);
        }

        if (options_.imports && loop_ && loop_end_ && placed.tick < *loop_end_ && loop_->second > 0)
        {
            at = std::min(at, loop_->second - 1);
        }

        code.LoopTo(at);
        code.WaitTo(at);
        Command command = placed.command;
        if (command.kind == Kind::kNote && placed.held)
        {
            command.value.value = int64_t{kHeldBeats} * options_.timebase * kOne;
        }
        else if (command.kind == Kind::kNote)
        {
            const uint32_t off = Ticks(*placed.off);
            uint32_t length = std::max(off, at + 1) - at;
            // Such a note would still sound when its next pass starts, and the voices would pile up pass by pass.
            if (options_.imports && loop_ && at >= loop_->first && length > loop_->second - loop_->first)
            {
                length = loop_->second - loop_->first;
                outlasting_++;
            }

            command.value.value = int64_t{length} * kOne;
            end = std::max(end, std::max(off, at + 1));
        }

        writer.Item(command);
        after_jump = code.Jumped();
    }

    if (loop_)
    {
        code.LoopTo(UINT32_MAX);
        if (after_jump)
        {
            // Music after the loop's end: the track ends where the MIDI file's track ends.
            code.WaitTo(end);
            writer.Item(Setting(Kind::kEnd, 0));
        }
    }
    else
    {
        code.WaitTo(end);
        writer.Item(Setting(Kind::kEnd, 0));
    }
}

void MidiImport::AddWarnings()
{
    if (biquad_values_ > 0)
    {
        song_.warnings.push_back(
            std::to_string(biquad_values_) +
            (biquad_values_ == 1 ? " control change 104 event sets" : " control change 104 events set") +
            " the biquad filter value. The MIDI file sets no filter type. The filter stays off "
            "until you set a type in the List Editor.");
    }

    if (outlasting_ > 0)
    {
        song_.warnings.push_back(std::to_string(outlasting_) +
                                 (outlasting_ == 1 ? " note lasts longer than one pass of the loop. It ends"
                                                   : " notes last longer than one pass of the loop. They end") +
                                 " where the next pass starts.");
    }

    if (endless_ > 0)
    {
        song_.warnings.push_back(std::to_string(endless_) +
                                 (endless_ == 1 ? " note in the MIDI file never ends. It's left out."
                                                : " notes in the MIDI file never end. They're left out."));
    }

    AddLeftOutWarnings(left_out_, song_.warnings);
}

} // namespace

MidiSong SongOfMidi(std::span<const uint8_t> midi, const std::string& name, const MidiOptions& options)
{
    const seqlib::midi::File file = ReadMidiToImport(midi);
    return MidiImport(file, options).Import(name);
}

} // namespace hokuto::edit
