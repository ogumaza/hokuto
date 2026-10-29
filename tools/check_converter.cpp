// SPDX-License-Identifier: MIT

// Checks Hokuto's converter (src/edit/converter.h) on the sequences of sound archives of the Wii, the 3DS, the Wii U
// and the Switch. For each sequence that holds a song of NintendoWare's MIDI converter, it checks three things:
//
// - ReadConverted finds hints with which LayOutConverted gives the sequence back, with subroutines, item for item. The
//   encoder then gives the file's bytes.
// - LayOutConverted without subroutines writes every bar in its track. The layout holds no call.
// - That layout, laid out again with subroutines, gives the sequence back.
//
// A song that ReadConverted can't read counts apart, with the reason. The songs of a Wii archive decode by the rules of
// the build of NW4R SND that the program's import takes: the game's, from the main.dol of the extracted disc that holds
// the archive, or else the one that the archive's version gives. The tool prints a line for each song and for each
// problem, then a summary. Each line gives a hash of the sequence's bytes: a song that several archives hold counts
// once by its hash.
//
// The tool then imports each archive into a Hokuto file, and opens it as the program does (edit/timelines.h). The
// file's timelines, laid out with their subroutines again, have to export the archive byte for byte. With --play, each
// sound of a timeline also has to play the same notes from the archive that the timelines build without subroutines as
// from the game's archive. The program plays that archive.
//
// With --edits, the tool makes the edits of the arrangement on the first sound of each timeline, each in a version of
// its own: the busiest part of the busiest track moves by a beat, goes up a semitone and takes 10 less velocity, and
// the loop's start and end move. The program then settles each edit (SettleTimelines). The saved archive has to play
// the edit: the part's notes moved, transposed or softer, and every other note as before. A moved loop has to play from
// its new start to its new end, and every note before the earlier end as before. The saved file has to open again as
// the same timeline, with its hints and without them: the converter's rules then give the edit's layout back. ntdlib's
// performer can find the saved archive's loop a whole number of passes earlier or later. The tool counts such loops
// apart, and compares the notes up to the earlier loop's end.
//
// An instrument can lack a region for a note's new key or velocity, and the performer then leaves the note out. A note
// of a key group releases the group's other notes on its track, and a note released in the tick that it starts doesn't
// sound. A changed note can then cut other notes of its track in its tick, or stop cutting them. The tool counts apart
// the edits after which the track plays fewer notes, and those after which it plays more. A changed note can also take
// another region with a wave that plays once. Once the wave ends, the loop's passes sound nothing. The sequence keeps
// its loop, but ntdlib's performer finds no loop in a sound whose loop sounds nothing. The tool counts such edits apart
// too. It counts the edits that the program refuses by their messages, such as a loop without a note. It returns 1 for
// any problem.
//
// With --model, each archive goes into the model form after the import (format::InModelForm), and the archive that the
// model builds has to be the game's. The file that the opened timelines save has to save in both forms and open again
// as it was (tools/checked_archive.h). So do the first 64 settled edits and every 64th after them.
//
// usage: hokuto_check_converter [--model] [--play] [--edits] <archive>...

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/nwsnd/commands.h"
#include "ntdlib/nwsnd/console.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/sequence_file.h"
#include "ntdlib/sound_build.h"
#include "xsflib/console.h"
#include "xsflib/files.h"

#include "checked_archive.h"
#include "edit/converter.h"
#include "edit/notes.h"
#include "edit/timelines.h"
#include "format/archive.h"
#include "format/file.h"
#include "song/archive.h"
#include "song/sequence.h"

namespace
{

namespace commands = ntdlib::commands;
namespace nwsnd = ntdlib::nwsnd;
using hokuto::tools::CheckedArchive;

// What the check counted, for the summary.
struct Totals
{
    int archives = 0;
    int problems = 0;
    int sequences = 0;
    int songs = 0;
    int read = 0;
    int with_calls = 0;
    int whole = 0;
    int opened = 0;
    int excepted = 0;
    int kept_inline = 0;
    int called = 0;
    int files = 0;
    int timelines = 0;
    int played = 0;
    int edited = 0;
    int edits = 0;
    int loops_apart = 0;
    int silenced = 0;
    int fewer = 0;
    int more = 0;
    std::map<std::string, int> refused;
    std::set<uint64_t> unique_songs;
    std::set<uint64_t> unique_read;
    std::map<std::string, std::set<uint64_t>> reasons;
};

// FNV-1a over `bytes`.
uint64_t HashOf(std::span<const uint8_t> bytes)
{
    uint64_t hash = 14695981039346656037ull;
    for (const uint8_t byte : bytes)
    {
        hash = (hash ^ byte) * 1099511628211ull;
    }

    return hash;
}

// The name of the converted song that `sequence` holds, from its Begin label, or an empty one.
std::string SongName(const commands::Sequence& sequence)
{
    for (const commands::Label& label : sequence.labels)
    {
        if (label.item == 0 && label.name.starts_with("SMF_") && label.name.ends_with("_Begin"))
        {
            return label.name.substr(4, label.name.size() - 10);
        }
    }

    return {};
}

// The timebase of `sequence`, in ticks a quarter note: its first timebase command's, or else NintendoWare's default.
uint32_t TimebaseOf(const commands::Sequence& sequence)
{
    for (const commands::Command& command : sequence.items)
    {
        if (command.kind == commands::Kind::kTimebase && command.value.source == commands::Source::kValue &&
            command.value.value >= commands::kOne && command.value.value % commands::kOne == 0)
        {
            return static_cast<uint32_t>(command.value.value / commands::kOne);
        }
    }

    return 48;
}

// `text` with a # for each run of digits. Reasons of the same kind then count together.
std::string WithoutNumbers(const std::string& text)
{
    std::string result;
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const bool digit = text[i] >= '0' && text[i] <= '9';
        if (!digit)
        {
            result += text[i];
        }
        else if (i == 0 || text[i - 1] < '0' || text[i - 1] > '9')
        {
            result += '#';
        }
    }

    return result;
}

// The items of the tracks of the song `name` in `layout`: from the first track's label to the End label. A song edited
// by hand can hold code of its own before them.
std::pair<std::size_t, std::size_t> TracksOf(const commands::Sequence& layout, const std::string& name)
{
    const std::string track = "SMF_" + name + "_Track_";
    std::size_t first = layout.items.size();
    std::size_t end = layout.items.size();
    for (const commands::Label& label : layout.labels)
    {
        const std::string_view rest = std::string_view(label.name).substr(std::min(label.name.size(), track.size()));
        const auto digit = [](char c)
        {
            return c >= '0' && c <= '9';
        };
        if (label.name.starts_with(track) && !rest.empty() && std::all_of(rest.begin(), rest.end(), digit))
        {
            first = std::min<std::size_t>(first, label.item);
        }
        if (label.name == "SMF_" + name + "_End")
        {
            end = label.item;
        }
    }

    return {std::min(first, end), end};
}

// Prints problem `what` at `where`, and counts it.
void Problem(Totals& totals, const std::string& where, const std::string& what)
{
    std::printf("%s: %s\n", where.c_str(), what.c_str());
    ++totals.problems;
}

// Checks the sequence in `data`, a sequence file's bytecode, with `labels` and the sounds' starts in `entries`.
void CheckSequence(const std::string& where, std::span<const uint8_t> data,
                   const std::vector<nwsnd::LabelOffset>& labels, const nwsnd::Grammar& grammar,
                   const std::vector<uint32_t>& entries, Totals& totals)
{
    const nwsnd::Decoded decoded = nwsnd::DecodeSequence(data, labels, grammar, entries);
    const commands::Sequence& sequence = decoded.sequence;
    ++totals.sequences;
    const std::string name = SongName(sequence);
    if (name.empty())
    {
        return;
    }

    // ReadConverted gives the hints, or the reason why it can't.
    const uint64_t hash = HashOf(data);
    std::array<char, 17> digits{};
    std::snprintf(digits.data(), digits.size(), "%016llx", static_cast<unsigned long long>(hash));
    const std::string song = where + " " + name + " (" + digits.data() + ")";
    ++totals.songs;
    totals.unique_songs.insert(hash);
    const hokuto::edit::ConvertedSong converted = hokuto::edit::ReadConverted(sequence);
    if (!converted.hints)
    {
        std::printf("%s: not read: %s\n", song.c_str(), converted.problem.c_str());
        totals.reasons[WithoutNumbers(converted.problem)].insert(hash);
        return;
    }

    // The layout with subroutines gives the sequence and the file's bytes back.
    const hokuto::edit::ConverterHints& hints = *converted.hints;
    ++totals.read;
    totals.unique_read.insert(hash);
    const commands::Sequence folded = hokuto::edit::LayOutConverted(sequence, hints, true);
    if (folded != sequence)
    {
        Problem(totals, song, "the layout with subroutines differs from the sequence");
    }
    const nwsnd::Encoded encoded = nwsnd::EncodeSequence(folded, grammar);
    if (!decoded.opaque && !std::equal(encoded.bytes.begin(), encoded.bytes.end(), data.begin(), data.end()))
    {
        Problem(totals, song, "the layout's bytes differ from the file's");
    }

    // The tracks of the layout without subroutines hold no call. The layout gives the sequence back with them.
    const commands::Sequence unfolded = hokuto::edit::LayOutConverted(sequence, hints, false);
    const auto is_call = [](const commands::Command& command)
    {
        return command.kind == commands::Kind::kCall;
    };
    const auto [first, end] = TracksOf(unfolded, name);
    if (std::any_of(unfolded.items.begin() + first, unfolded.items.begin() + end, is_call))
    {
        Problem(totals, song, "the tracks of the layout without subroutines hold a call");
    }
    if (hokuto::edit::LayOutConverted(unfolded, hints, true) != sequence)
    {
        Problem(totals, song, "the layout without subroutines doesn't give the sequence back");
    }

    // What the hints hold.
    const bool has_calls = std::any_of(sequence.items.begin(), sequence.items.end(), is_call);
    totals.with_calls += has_calls ? 1 : 0;

    // For a song without calls, the candidates are bars of 1 to 8 quarter notes. Bars of another length span the whole
    // song.
    const uint32_t timebase = TimebaseOf(sequence);
    const uint32_t length = hints.bars.empty() ? 0 : hints.bars.front().length;
    const bool whole = !has_calls && hints.bars.size() == 1 && (length % timebase != 0 || length / timebase > 8);
    totals.whole += whole ? 1 : 0;
    int kept_inline = 0;
    int called = 0;
    bool opened = false;
    for (const hokuto::edit::TrackHints& track : hints.tracks)
    {
        kept_inline += static_cast<int>(track.kept_inline.size());
        called += static_cast<int>(track.called.size());
        opened = opened || !track.opening.empty();
    }
    totals.opened += opened ? 1 : 0;
    totals.kept_inline += kept_inline;
    totals.called += called;
    totals.excepted += kept_inline + called > 0 ? 1 : 0;
    std::printf("%s: read%s%s, %d bars kept in their tracks, %d called against the rules\n", song.c_str(),
                has_calls ? " with calls" : "", whole ? ", one bar for the whole song" : "", kept_inline, called);
}

// Checks the converted songs of the sequence files of a sound archive of the 3DS, the Wii U or the Switch, `bytes`, of
// `flavor`, from the file at `path`.
void CheckSar(const std::string& path, std::vector<uint8_t> bytes, ntdlib::sar::Flavor flavor, Totals& totals)
{
    ntdlib::sar::Options options;
    options.flavor = flavor;
    const ntdlib::sar::SoundArchive archive = ntdlib::sar::SoundArchive::Load(std::move(bytes), options);
    const nwsnd::Grammar grammar = flavor == ntdlib::sar::Flavor::kCtr    ? nwsnd::GrammarOf<nwsnd::CtrPolicy>()
                                   : flavor == ntdlib::sar::Flavor::kCafe ? nwsnd::GrammarOf<nwsnd::CafePolicy>()
                                                                          : nwsnd::GrammarOf<nwsnd::NxPolicy>();

    // The starts of each sequence file's sounds.
    std::map<uint32_t, std::set<uint32_t>> starts;
    for (const ntdlib::sar::SoundInfo& sound : archive.Sounds())
    {
        if (sound.type == ntdlib::sar::SoundType::kSequence)
        {
            starts[sound.file_id].insert(sound.start_offset);
        }
    }

    for (const auto& [file_id, offsets] : starts)
    {
        const std::span<const uint8_t> file = archive.FileData(file_id);
        if (file.empty())
        {
            continue;
        }

        const std::string where = path + "#" + std::to_string(file_id);
        try
        {
            const ntdlib::sar::SequenceFile sequence = ntdlib::sar::SequenceFile::Read(file);
            std::vector<nwsnd::LabelOffset> labels;
            for (const ntdlib::sar::Sequence::Label& label : sequence.labels)
            {
                labels.push_back({label.name, label.offset});
            }
            CheckSequence(where, sequence.data, labels, grammar, std::vector<uint32_t>(offsets.begin(), offsets.end()),
                          totals);
        }
        catch (const std::exception& error)
        {
            Problem(totals, where, error.what());
        }
    }
}

// Checks the converted songs of the sequence files of a sound archive of the Wii, `bytes`, from the file at `path`. The
// songs decode by the rules of `build` of NW4R SND.
void CheckRsar(const std::string& path, std::vector<uint8_t> bytes, const ntdlib::SndBuild& build, Totals& totals)
{
    ntdlib::rsar::ReadOptions options;
    options.playback_fields = true;
    const ntdlib::rsar::SoundArchive archive = ntdlib::rsar::SoundArchive::Load(std::move(bytes), options);
    const nwsnd::Grammar grammar = nwsnd::GrammarOf<nwsnd::RevolutionPolicy>(build);

    std::map<uint32_t, std::set<uint32_t>> starts;
    for (const ntdlib::rsar::SoundInfo& sound : archive.Sounds())
    {
        if (sound.type == ntdlib::rsar::SoundType::kSequence)
        {
            starts[sound.file_id].insert(sound.start_offset);
        }
    }

    for (const auto& [file_id, offsets] : starts)
    {
        const std::span<const uint8_t> file = archive.FileData(file_id);
        if (file.empty())
        {
            continue;
        }

        const std::string where = path + "#" + std::to_string(file_id);
        try
        {
            const ntdlib::rsar::Sequence sequence = ntdlib::rsar::Sequence::Parse(file, options);
            std::vector<nwsnd::LabelOffset> labels;
            for (const ntdlib::rsar::Sequence::Label& label : sequence.labels)
            {
                labels.push_back({label.name, label.offset});
            }
            CheckSequence(where, sequence.data, labels, grammar, std::vector<uint32_t>(offsets.begin(), offsets.end()),
                          totals);
        }
        catch (const std::exception& error)
        {
            Problem(totals, where, error.what());
        }
    }
}

// The notes of `song` without the places of their commands. The layouts move those places.
std::vector<hokuto::song::Note> NotesOf(const hokuto::song::Sequence& song)
{
    std::vector<hokuto::song::Note> notes;
    for (const hokuto::song::Track& track : song.tracks)
    {
        for (hokuto::song::Note note : track.notes)
        {
            note.command = 0;
            note.stretch = 0;
            notes.push_back(note);
        }
    }

    return notes;
}

// The tool's options: --play and --edits.
struct Options
{
    bool play = false;
    bool edits = false;
};

// A note as an edit leaves it: its track, start, key and velocity. The engine sets the length of many notes, such as a
// note of a one-shot wave, and an edit can change it.
struct HeardNote
{
    friend auto operator<=>(const HeardNote&, const HeardNote&) = default;

    int track = 0;
    uint32_t start = 0;
    int key = 0;
    int velocity = 0;
    uint32_t command = 0; // the note's command, in the archive that the program plays
};

// The notes of `song` that start before `cut`, in order, without their commands.
std::vector<HeardNote> HeardNotes(const hokuto::song::Sequence& song, uint32_t cut, bool commands = false)
{
    std::vector<HeardNote> notes;
    for (const hokuto::song::Track& track : song.tracks)
    {
        for (const hokuto::song::Note& note : track.notes)
        {
            if (note.start < cut)
            {
                notes.push_back({track.number, note.start, note.key, note.velocity, commands ? note.command : 0});
            }
        }
    }
    std::sort(notes.begin(), notes.end());

    return notes;
}

// An edit of the arrangement: its name, the version that it makes, and the notes that the saved archive has to play
// before `cut`. A moved loop has to play from its new start to its new end. An edit of a part changes the notes of
// `changed`, in order, on track `track`. A moved loop changes no note, and its track is -1.
struct ArrangementEdit
{
    std::string name;
    hokuto::format::File file;
    std::vector<HeardNote> expected;
    uint32_t cut = 0;
    std::optional<hokuto::song::Loop> loop;
    std::vector<HeardNote> changed;
    int track = -1;
};

// The edits that the arrangement's info line makes of the busiest part of the busiest track of `shown`, the first sound
// of timeline `sequence` of `opened`, and the moves of the loop's edges. `before` is the sound as the game's archive
// plays it. An edit that the sequence refuses counts apart, by its reason.
std::vector<ArrangementEdit> ArrangementEdits(const hokuto::format::File& opened, uint32_t sequence,
                                              const hokuto::song::Sequence& shown, const hokuto::song::Sequence& before,
                                              Totals& totals)
{
    // A note at the sequence's end starts in the tick where the sound ends, and doesn't sound.
    std::vector<ArrangementEdit> edits;
    const uint32_t cut = before.loop ? before.loop->end : before.length;

    const auto refuse = [&totals](const std::string& name, const std::exception& error)
    {
        ++totals.refused[name + ": " + WithoutNumbers(error.what())];
    };

    // The busiest part. The arrangement splits parts on beats, at gaps of two bars of 4/4.
    const auto busiest = std::max_element(shown.tracks.begin(), shown.tracks.end(),
                                          [](const auto& a, const auto& b) { return a.notes.size() < b.notes.size(); });
    if (busiest != shown.tracks.end() && !busiest->notes.empty())
    {
        const std::vector<hokuto::song::Part> parts = hokuto::song::SplitParts(shown, *busiest, 1, 8);
        const auto part = std::max_element(parts.begin(), parts.end(),
                                           [](const auto& a, const auto& b) { return a.count < b.count; });
        std::set<uint32_t> commands;
        for (std::size_t n = part->first; n < part->first + part->count; ++n)
        {
            commands.insert(busiest->notes[n].command);
        }

        // Each command takes the edit once. Every note that the command plays changes the same way, in the part and
        // elsewhere, such as in a pass of a loop inside the part.
        const auto edit_part = [&](const std::string& name, auto change, auto expect)
        {
            std::vector<hokuto::edit::NoteEdit> note_edits;
            std::set<uint32_t> done;
            for (std::size_t n = part->first; n < part->first + part->count; ++n)
            {
                const hokuto::song::Note& note = busiest->notes[n];
                if (done.insert(note.command).second)
                {
                    hokuto::edit::NoteEdit& note_edit = note_edits.emplace_back(
                        hokuto::edit::NoteEdit{.command = note.command, .stretch = busiest->stretches[note.stretch]});
                    change(note_edit, note);
                }
            }
            try
            {
                ArrangementEdit made{.name = name,
                                     .file = hokuto::edit::EditNotes(opened, sequence, note_edits, {}).file,
                                     .expected = HeardNotes(shown, cut, true),
                                     .cut = cut,
                                     .loop = before.loop,
                                     .track = busiest->number};
                for (HeardNote& note : made.expected)
                {
                    const bool changed = note.track == busiest->number && commands.contains(note.command);
                    note.command = 0;
                    if (changed)
                    {
                        expect(note);
                        made.changed.push_back(note);
                    }
                }
                for (std::vector<HeardNote>* notes : {&made.expected, &made.changed})
                {
                    std::erase_if(*notes, [cut](const HeardNote& note) { return note.start >= cut; });
                    std::sort(notes->begin(), notes->end());
                }
                edits.push_back(std::move(made));
            }
            catch (const std::invalid_argument& error)
            {
                refuse(name, error);
            }
        };

        const uint32_t beat = static_cast<uint32_t>(std::max(shown.timebase, 1));
        const auto shift = [beat](hokuto::edit::NoteEdit& note_edit, const auto&)
        {
            note_edit.shift = static_cast<int32_t>(beat);
        };
        edit_part("Move", shift, [beat](HeardNote& note) { note.start += beat; });
        const auto transpose = [](hokuto::edit::NoteEdit& note_edit, const auto&)
        {
            note_edit.transpose = 1;
        };
        edit_part("Transpose", transpose, [](HeardNote& note) { ++note.key; });
        const auto soften = [](hokuto::edit::NoteEdit& note_edit, const hokuto::song::Note& note)
        {
            note_edit.velocity = std::max(note.velocity - 10, 1);
        };
        const auto softer = [](HeardNote& note)
        {
            note.velocity = std::max(note.velocity - 10, 1);
        };
        edit_part("Change Part Velocity", soften, softer);
    }

    // The loop's edges, a beat in and a bar out.
    const std::optional<hokuto::edit::ConvertedLoop> loop = hokuto::edit::TimelineLoop(opened, sequence);
    if (loop && before.loop && before.loop->start == loop->start && before.loop->end == loop->end)
    {
        const uint32_t beat = static_cast<uint32_t>(std::max(shown.timebase, 1));
        std::vector<std::pair<std::string, hokuto::edit::ConvertedLoop>> moves;
        if (loop->start + beat < loop->end)
        {
            moves.push_back({"Size: start later", {loop->start + beat, loop->end}});
            moves.push_back({"Size: end earlier", {loop->start, loop->end - beat}});
        }
        if (loop->start >= beat)
        {
            moves.push_back({"Size: start earlier", {loop->start - beat, loop->end}});
        }
        moves.push_back({"Size: end later", {loop->start, loop->end + 4 * beat}});
        for (const auto& [name, moved] : moves)
        {
            try
            {
                const uint32_t moved_cut = std::min(loop->end, moved.end);
                edits.push_back({.name = name,
                                 .file = hokuto::edit::MoveLoop(opened, sequence, moved).file,
                                 .expected = HeardNotes(before, moved_cut),
                                 .cut = moved_cut,
                                 .loop = hokuto::song::Loop{moved.start, moved.end}});
            }
            catch (const std::invalid_argument& error)
            {
                refuse(name, error);
            }
        }
    }

    return edits;
}

// The sequence of `file` that sound `sound` of `archive` plays, or nothing for a sound of another kind. `sequences`
// gives each file's sequence.
std::optional<uint32_t> SequenceOfSound(const CheckedArchive& archive, uint32_t sound,
                                        const std::vector<std::optional<uint32_t>>& sequences)
{
    const uint32_t file_id = archive.FileOf(sound);
    if (!archive.IsSequence(sound) || file_id >= sequences.size())
    {
        return std::nullopt;
    }

    return sequences[file_id];
}

// Settles each of the arrangement's edits of the first sound of each timeline of `opened`, and checks the saved archive
// and file (see the top of the file). `unfolded` is the archive that the program plays, and `game` the game's.
void CheckEdits(const std::string& name, const hokuto::format::File& opened, const CheckedArchive& game,
                const CheckedArchive& unfolded, const std::vector<std::optional<uint32_t>>& sequences, Totals& totals)
{
    std::set<uint32_t> done;
    int edited = 0;
    int edits = 0;
    for (uint32_t sound = 0; sound < game.SoundCount(); ++sound)
    {
        const std::optional<uint32_t> sequence = SequenceOfSound(game, sound, sequences);
        if (!sequence || !hokuto::edit::IsTimeline(opened, *sequence) || !done.insert(*sequence).second)
        {
            continue;
        }

        const std::string where = name + " " + game.NameOf(sound);
        try
        {
            const hokuto::song::Sequence before = game.Read(sound);
            const hokuto::song::Sequence shown = unfolded.Read(sound);
            for (const ArrangementEdit& edit : ArrangementEdits(opened, *sequence, shown, before, totals))
            {
                // The program settles the edit, and saves it with the timeline's subroutines.
                const hokuto::edit::Settled settled = hokuto::edit::SettleTimelines(opened, edit.file);
                if (!hokuto::edit::IsTimeline(settled.file, *sequence))
                {
                    Problem(totals, where, edit.name + ": the edit leaves no timeline");
                    continue;
                }
                const hokuto::format::File folded = hokuto::edit::FoldTimelines(settled.file);
                hokuto::tools::CheckSaved(folded, true);
                const std::vector<uint8_t> saved = hokuto::format::ExportArchive(folded);
                const std::vector<std::vector<uint8_t>> group_files = hokuto::format::ExportGroupFiles(folded);
                const CheckedArchive archive = hokuto::tools::BuildChecked(folded, name);
                const hokuto::song::Sequence after = archive.Read(sound);

                // The settled edit plays as the edit plays before the program settles it.
                const hokuto::song::Sequence unsettled = hokuto::tools::BuildChecked(edit.file, name).Read(sound);
                if (NotesOf(after) != NotesOf(unsettled) || after.length != unsettled.length ||
                    after.loop != unsettled.loop)
                {
                    Problem(totals, where, edit.name + ": the settled edit plays otherwise");
                }

                // An instrument can lack a region for a changed note's new key or velocity. A changed note can cut
                // other notes of its track in its tick through a key group, or stop cutting them. The performer leaves
                // out a note that doesn't sound. ntdlib's performer can find a loop a whole number of passes earlier or
                // later. Every track's state repeats there. The performer stops at the end of the loop that it finds.
                const uint32_t length = edit.loop ? edit.loop->end - edit.loop->start : 0;
                const bool apart =
                    edit.loop && after.loop && after.loop != edit.loop && length > 0 &&
                    after.loop->end - after.loop->start == length &&
                    (std::max(after.loop->start, edit.loop->start) - std::min(after.loop->start, edit.loop->start)) %
                            length ==
                        0;
                const uint32_t cut = apart ? std::min(edit.cut, after.loop->end) : edit.cut;
                std::vector<HeardNote> expected = edit.expected;
                std::vector<HeardNote> changed = edit.changed;
                for (std::vector<HeardNote>* notes : {&expected, &changed})
                {
                    std::erase_if(*notes, [cut](const HeardNote& note) { return note.start >= cut; });
                }
                const std::vector<HeardNote> heard = HeardNotes(after, cut);
                std::set<uint32_t> ticks;
                for (const HeardNote& note : changed)
                {
                    ticks.insert(note.start);
                }
                std::vector<HeardNote> fewer;
                std::vector<HeardNote> more;
                std::ranges::set_difference(expected, heard, std::back_inserter(fewer));
                std::ranges::set_difference(heard, expected, std::back_inserter(more));
                const auto left_out = [&edit, &changed, &ticks](const HeardNote& note)
                {
                    return std::ranges::binary_search(changed, note) ||
                           (note.track == edit.track && ticks.contains(note.start));
                };
                const auto added = [&edit](const HeardNote& note)
                {
                    return note.track == edit.track;
                };
                const bool allowed = std::ranges::all_of(fewer, left_out) && std::ranges::all_of(more, added);
                totals.fewer += allowed && !fewer.empty() ? 1 : 0;
                totals.more += allowed && !more.empty() ? 1 : 0;
                if (!allowed)
                {
                    // The first note that differs.
                    const auto [played, wanted] =
                        std::mismatch(heard.begin(), heard.end(), expected.begin(), expected.end());

                    const auto text = [](const std::vector<HeardNote>& notes, auto note)
                    {
                        return note == notes.end()
                                   ? std::string("none")
                                   : "track " + std::to_string(note->track) + " at " + std::to_string(note->start) +
                                         ", key " + std::to_string(note->key) + ", velocity " +
                                         std::to_string(note->velocity);
                    };
                    Problem(totals, where,
                            edit.name + ": the saved archive plays other notes: " + text(heard, played) +
                                " in the place of " + text(expected, wanted) + ", of " + std::to_string(heard.size()) +
                                " and " + std::to_string(expected.size()));
                }
                totals.loops_apart += apart ? 1 : 0;

                // The changed notes can leave the loop silent once their waves end. The sequence keeps its loop.
                const std::optional<hokuto::edit::ConvertedLoop> kept =
                    hokuto::edit::TimelineLoop(settled.file, *sequence);
                const bool silenced = edit.track >= 0 && edit.loop && !after.loop && kept &&
                                      kept->start == edit.loop->start && kept->end == edit.loop->end;
                totals.silenced += silenced ? 1 : 0;
                if (edit.loop && after.loop != edit.loop && !apart && !silenced)
                {
                    const auto text = [](const std::optional<hokuto::song::Loop>& loop)
                    {
                        return loop ? std::to_string(loop->start) + " to " + std::to_string(loop->end)
                                    : std::string("none");
                    };
                    Problem(totals, where,
                            edit.name + ": the saved archive plays another loop: " + text(after.loop) +
                                " in the place of " + text(edit.loop));
                }

                // The saved file opens again as the same timeline, with its hints and without them. Without them, the
                // converter's rules find the hints again.
                hokuto::format::File imported = hokuto::tools::ImportChecked(saved, archive, group_files);
                const hokuto::edit::ConverterHints& hints = settled.file.timelines->at(*sequence);
                const hokuto::edit::ConvertedSong found = hokuto::edit::ReadConverted(*imported.sequences[*sequence]);
                imported.timelines = std::map<uint32_t, hokuto::edit::ConverterHints>{{*sequence, hints}};
                const hokuto::format::File decided = hokuto::edit::OpenTimelines(imported);
                if (!hokuto::edit::IsTimeline(decided, *sequence))
                {
                    Problem(totals, where, edit.name + ": the saved file doesn't open as a timeline");
                }
                else if (*decided.sequences[*sequence] != *settled.file.sequences[*sequence])
                {
                    Problem(totals, where, edit.name + ": the saved file opens as another timeline");
                }
                if (!found.hints || hokuto::edit::LayOutConverted(*imported.sequences[*sequence], *found.hints,
                                                                  false) != *settled.file.sequences[*sequence])
                {
                    Problem(totals, where,
                            edit.name + ": the saved file without its hints opens as another layout" +
                                (found.hints ? std::string() : ": " + found.problem));
                }
                ++edits;
            }
            ++edited;
        }
        catch (const std::exception& error)
        {
            Problem(totals, where, error.what());
        }
    }
    totals.edited += edited;
    totals.edits += edits;
    std::printf("%s: %d timelines took %d edits\n", name.c_str(), edited, edits);
}

// Imports the archive at `path` into a file, opens its timelines, and checks that they give the archive back. With
// `play`, checks that each sound of a timeline plays the same notes without subroutines. With `edits`, checks the
// arrangement's edits.
void CheckFile(const std::filesystem::path& path, const std::vector<uint8_t>& bytes, const Options& options,
               Totals& totals)
{
    const std::string name = xsflib::Utf8(path);
    const CheckedArchive archive = hokuto::tools::OpenChecked(path, bytes);
    const hokuto::format::File file = hokuto::tools::CheckedForm(hokuto::tools::ImportChecked(bytes, archive), bytes);
    const hokuto::format::File opened = hokuto::edit::OpenTimelines(file);
    const std::size_t timelines = opened.timelines ? opened.timelines->size() : 0;
    ++totals.files;
    totals.timelines += static_cast<int>(timelines);
    const hokuto::format::File saved = hokuto::edit::FoldTimelines(opened);
    if (hokuto::format::ExportArchive(saved) != bytes)
    {
        Problem(totals, name, "the timelines with their subroutines don't give the archive back");
    }
    hokuto::tools::CheckSaved(saved);
    if ((!options.play && !options.edits) || timelines == 0)
    {
        std::printf("%s: %zu timelines of %zu sequences\n", name.c_str(), timelines, file.sequences.size());
        return;
    }

    // The archive that the program plays.
    const CheckedArchive unfolded = hokuto::tools::BuildChecked(opened, name);
    const std::vector<std::optional<uint32_t>> sequences = hokuto::tools::SequencesOfChecked(file, bytes, archive);
    if (options.edits)
    {
        CheckEdits(name, opened, archive, unfolded, sequences, totals);
    }
    if (!options.play)
    {
        return;
    }

    // Each sound of a timeline plays the same notes from the archive that the program plays.
    int played = 0;
    for (uint32_t sound = 0; sound < archive.SoundCount(); ++sound)
    {
        const std::optional<uint32_t> sequence = SequenceOfSound(archive, sound, sequences);
        if (!sequence || !hokuto::edit::IsTimeline(opened, *sequence))
        {
            continue;
        }

        try
        {
            const hokuto::song::Sequence before = archive.Read(sound);
            const hokuto::song::Sequence after = unfolded.Read(sound);
            if (NotesOf(before) != NotesOf(after) || before.length != after.length || before.loop != after.loop)
            {
                Problem(totals, name + " " + archive.NameOf(sound), "plays other notes without subroutines");
            }
            ++played;
        }
        catch (const std::exception& error)
        {
            Problem(totals, name + " " + archive.NameOf(sound), error.what());
        }
    }
    totals.played += played;
    std::printf("%s: %zu timelines of %zu sequences, %d sounds played the same\n", name.c_str(), timelines,
                file.sequences.size(), played);
}

// Checks the archive at `path` (see the top of the file).
void Check(const std::filesystem::path& path, const Options& options, Totals& totals)
{
    const std::string name = xsflib::Utf8(path);
    std::vector<uint8_t> bytes;
    if (!xsflib::ReadFile(path, bytes) || bytes.size() < 8)
    {
        Problem(totals, name, "can't read the archive");
        return;
    }

    ++totals.archives;
    try
    {
        if (std::memcmp(bytes.data(), "RSAR", 4) == 0)
        {
            // The build of the program's import: the game's, or else the one that the archive's version gives
            // (format::BuildOfWiiArchive).
            const auto version = static_cast<uint16_t>(bytes[6] << 8 | bytes[7]);
            const ntdlib::SndBuild build =
                hokuto::tools::GameBuildOf(path).value_or(hokuto::format::BuildOfWiiVersion(version));
            CheckRsar(name, bytes, build, totals);
            CheckFile(path, bytes, options, totals);
        }
        else if (std::memcmp(bytes.data(), "CSAR", 4) == 0)
        {
            CheckSar(name, bytes, ntdlib::sar::Flavor::kCtr, totals);
            CheckFile(path, bytes, options, totals);
        }
        else if (std::memcmp(bytes.data(), "FSAR", 4) == 0)
        {
            const bool big_endian = bytes[4] == 0xFE && bytes[5] == 0xFF;
            CheckSar(name, bytes, big_endian ? ntdlib::sar::Flavor::kCafe : ntdlib::sar::Flavor::kNx, totals);
            CheckFile(path, bytes, options, totals);
        }
        else
        {
            Problem(totals, name, "not a sound archive");
        }
    }
    catch (const std::exception& error)
    {
        Problem(totals, name, error.what());
    }
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> arguments = xsflib::console::Arguments(argc, argv);
    hokuto::tools::TakeModelOption(arguments);
    if (arguments.size() < 2)
    {
        std::fprintf(stderr, "usage: hokuto_check_converter [--model] [--play] [--edits] <archive>...\n");
        return 2;
    }

    Totals totals;
    Options options;
    std::size_t first = 1;
    for (; first < arguments.size() && arguments[first].starts_with("--"); ++first)
    {
        options.play = options.play || arguments[first] == "--play";
        options.edits = options.edits || arguments[first] == "--edits";
    }
    for (std::size_t i = first; i < arguments.size(); ++i)
    {
        Check(xsflib::PathFromUtf8(arguments[i]), options, totals);
    }

    std::printf(
        "%d archives: %d sequences, %d converted songs (%zu different). Read %d songs (%zu different), %d of "
        "them with calls. %d songs without calls took one bar for the whole song. %d songs have blocks before "
        "their start. %d songs need exceptions: %d bars kept in their tracks, %d called against the rules.\n",
        totals.archives, totals.sequences, totals.songs, totals.unique_songs.size(), totals.read,
        totals.unique_read.size(), totals.with_calls, totals.whole, totals.opened, totals.excepted, totals.kept_inline,
        totals.called);
    for (const auto& [reason, songs] : totals.reasons)
    {
        std::printf("  not read, %zu different songs: %s\n", songs.size(), reason.c_str());
    }
    std::printf("%d files with %d timelines gave their archives back, and %d sounds of timelines played the same.\n",
                totals.files, totals.timelines, totals.played);
    if (options.edits)
    {
        std::printf(
            "%d timelines took %d edits of the arrangement. The performer found %d loops a whole number of passes "
            "away, and no loop after %d edits that left the loop silent. %d edits played fewer notes, and %d more.\n",
            totals.edited, totals.edits, totals.loops_apart, totals.silenced, totals.fewer, totals.more);
        for (const auto& [reason, count] : totals.refused)
        {
            std::printf("  refused %d times: %s\n", count, reason.c_str());
        }
    }
    hokuto::tools::PrintSaves(std::to_string(totals.archives) + " archives");
    std::printf("%d problems.\n", totals.problems);

    return totals.problems == 0 ? 0 : 1;
}
