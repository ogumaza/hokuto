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

#include "check_converter.h"

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
#include <type_traits>
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
#include "checked_saves.h"
#include "edit/converter.h"
#include "edit/timelines.h"
#include "format/archive.h"
#include "format/file.h"
#include "song/archive.h"
#include "song/sequence.h"

namespace hokuto::tools::converter
{

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

void Problem(Totals& totals, const std::string& where, const std::string& what)
{
    std::printf("%s: %s\n", where.c_str(), what.c_str());
    ++totals.problems;
}

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

} // namespace hokuto::tools::converter

namespace
{

namespace commands = ntdlib::commands;
namespace nwsnd = ntdlib::nwsnd;
using hokuto::tools::CheckedArchive;
using hokuto::tools::converter::CheckEdits;
using hokuto::tools::converter::NotesOf;
using hokuto::tools::converter::Problem;
using hokuto::tools::converter::SequenceOfSound;
using hokuto::tools::converter::Totals;
using hokuto::tools::converter::WithoutNumbers;

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

// Checks the converted songs of the sequence files of `archive`, a sound archive from the file at `path`, by `grammar`.
// `read` reads a sequence file into its data and its labels. A file that `read` can't read counts as a problem.
template <typename Archive, typename Read>
void CheckSequenceFiles(const std::string& path, const Archive& archive, const nwsnd::Grammar& grammar,
                        const Read& read, Totals& totals)
{
    // The starts of each sequence file's sounds.
    std::map<uint32_t, std::set<uint32_t>> starts;
    for (const auto& sound : archive.Sounds())
    {
        if (sound.type == std::remove_cvref_t<decltype(sound.type)>::kSequence)
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
            const auto sequence = read(file);
            std::vector<nwsnd::LabelOffset> labels;
            for (const auto& label : sequence.labels)
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
    CheckSequenceFiles(
        path, archive, grammar, [](std::span<const uint8_t> file) { return ntdlib::sar::SequenceFile::Read(file); },
        totals);
}

// Checks the converted songs of the sequence files of a sound archive of the Wii, `bytes`, from the file at `path`. The
// songs decode by the rules of `build` of NW4R SND.
void CheckRsar(const std::string& path, std::vector<uint8_t> bytes, const ntdlib::SndBuild& build, Totals& totals)
{
    ntdlib::rsar::ReadOptions options;
    options.playback_fields = true;
    const ntdlib::rsar::SoundArchive archive = ntdlib::rsar::SoundArchive::Load(std::move(bytes), options);
    const nwsnd::Grammar grammar = nwsnd::GrammarOf<nwsnd::RevolutionPolicy>(build);
    CheckSequenceFiles(
        path, archive, grammar,
        [&](std::span<const uint8_t> file) { return ntdlib::rsar::Sequence::Parse(file, options); }, totals);
}

// The tool's options: --play and --edits.
struct Options
{
    bool play = false;
    bool edits = false;
};

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
        if (hokuto::tools::IsWiiArchive(bytes))
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
