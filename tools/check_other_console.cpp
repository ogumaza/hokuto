// SPDX-License-Identifier: MIT

// Checks the songs that go to another console (src/edit/other_console.h) on a sound archive of the 3DS, the Wii U or
// the Switch, with the group files and the streams beside it, or on a sound archive of the Wii. The tool imports the
// archive as a .hok file. A Wii archive takes the build of NW4R SND from the main.dol of the extracted disc that holds
// it. The program's import takes the build from there too. Each checked sequence sound then goes to the console that
// --to names as a new file: 3ds, wii, wii_u or switch. Without the option, a sound of the Switch, the 3DS or the Wii
// goes to the Wii U, and one of the Wii U to the Switch. A sound of the Wii goes to another console only. The new file
// has to come back from its binary form and its text form. The sound plays dry for `seconds` seconds on ntdlib's model
// of each console: from the game's archive, and from the archive that the new file exports (song::RenderDry). The Wii's
// model mixes at 32 kHz, by the rules of the file's build of NW4R SND. The 3DS's model runs on ntdlib's model of the
// DSP's audio firmware. The render at the lower rate goes to the higher (song::AtRate). The audibility method's level
// measure compares the two renders (song::CompareLevels). A span of 1 s more than 1 dB off and a run of windows more
// than 6 dB off are its findings. The renders' gain counts apart: a listener sets the volume. A sound whose gain lies
// more than 3 dB either way still gets a line. A conversion that plays silent takes a gain of 0 dB instead. Every
// window where the game's sound plays then counts against the conversion.
//
// The tool checks up to `most` sequence sounds, spread evenly over the sound table, or every one with 0. It prints a
// line for each sound that doesn't convert or whose render has findings, then a summary: the sounds that convert, the
// reasons of those that don't, the findings by their worst kind, the gains, the sounds that end elsewhere, and the
// sounds with each kind of warning. A sound ends after its last window above the floor of the windows that the measure
// counts: 50 dB below the loudest window of either render, and -70 dBFS. A model can run on in silence after that. A
// sound that the program refuses with a message for the user counts apart from one that fails. It also counts the
// findings of the sounds whose commands draw random numbers, and of the other sounds that start other notes on the
// other console, such as a sound that waits for a voice to end. The engines' generator steps once a frame, a voice ends
// in a frame, and the consoles' frames differ. On the Wii, a note of another bank slot than the first starts the same
// note where it plays its program in its slot's range of the sound's one bank (edit::OtherConsoleSong). It counts the
// sounds whose conversions play their ticks at other times than the game's sound (song::SameTiming). On another
// console's engine, the program's playhead follows the conversion's ticks over the sound's arrangement. The tool
// returns 1 when a sound fails to convert or a file doesn't come back, and 2 when the archive can't be read. It makes
// no sound.
//
// With `--part k/n`, the tool checks only part k of n of the chosen sounds: those whose places among them leave k - 1
// over when divided by n. n runs side by side cover a large archive faster than one run. With `--mono on`, the stereo
// waves of every sound for the 3DS go in as mono, and with `--mono off`, none do. Without the option, Hokuto turns mono
// on or off as the Export window does (edit::MonoWaves). With `--sound <name>`, the tool checks only the sequence sound
// of that name, and `most` counts for nothing. The option can come more than once. With `--biquads`, the tool checks
// only the sequence sounds whose commands set a biquad filter's type or value where the player reaches them, and prints
// a line for each. It counts apart the sounds whose commands it can't read, such as those of an archive that is cut
// short.
//
// usage: hokuto_check_other_console [--part <k>/<n>] [--to <console>] [--mono <on|off>] [--sound <name>]...
//        [--biquads] <archive> [most] [seconds]

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sound_build.h"
#include "xsflib/console.h"
#include "xsflib/files.h"

#include "edit/bytecode.h"
#include "edit/other_console.h"
#include "edit/replace.h"
#include "format/archive.h"
#include "format/codes.h"
#include "format/file.h"
#include "format/text.h"
#include "song/archive.h"
#include "song/audibility.h"
#include "song/dry_render.h"
#include "song/items.h"
#include "song/sequence.h"
#include "song/streams.h"

namespace
{

namespace edit = hokuto::edit;
namespace format = hokuto::format;
namespace sar = ntdlib::sar;
namespace song = hokuto::song;

// The kinds of warning that the tool counts, by the start of their text.
struct WarningKind
{
    const char* name;
    const char* start;
    const char* within;
};

constexpr std::array<WarningKind, 34> kWarningKinds = {{
    {"banks left behind", "Bank ", "stays behind"},
    {"banks with only the instruments that the sound selects", "The banks go in with only", ""},
    {"banks of an older version", "Bank ", "goes in as version"},
    {"wave archives of an older version", "Wave archive ", "goes in as version"},
    {"waves without their loops' original starts", "The waves of ", ""},
    {"8-bit waves in 16-bit PCM", "", "in as 16-bit PCM"},
    {"sequence files of an older version", "The sequence file goes in as version", ""},
    {"sequence files with other sounds' commands", "The sequence file holds the commands", ""},
    {"sequence files whose commands keep their places", "The sequence file's commands have to keep", ""},
    {"commands left out", "The new file leaves out ", ""},
    {"commands in their places that the 3DS skips", "", "The 3DS's player skip"},
    {"settings that stay behind", "The sound's 3D settings", ""},
    {"values that stay behind", "The sound's ", " stays behind. "},
    {"strict mixing in place of the enhanced mode", "The sound plays in strict mixing", ""},
    {"more notes at once than the 3DS plays", "The 3DS plays ", " notes and "},
    {"stereo waves in as mono", "", " in as mono. "},
    {"volumes above 127 in as 127", "The sound's volume of ", ""},
    {"pans of the build of October 2006", "The sound takes the dual pan mode", ""},
    {"remote filters and user parameters that stay behind", "The sound's remote filter", ""},
    {"effects of the Wii that stay behind", "The Wii's ", ""},
    {"global variables given a starting value at the start", "The sound reads global variable", ""},
    {"waves that the Wii doesn't play", "", " that the Wii doesn't play, such as"},
    {"banks in other slots that the notes may play", "The bank", "Hokuto can't follow the sequence's bank selects"},
    {"banks in slots that only a variable or a random number selects", "The bank", "Only a variable or a random"},
    {"banks in other slots that stay behind", "The bank", " other slot"},
    {"banks of several slots in one bank", "A sound of the Wii plays one bank", ""},
    {"notes that may play another instrument on the Wii", "", " of the sequence's note commands may play "},
    {"Wii banks with only the instruments that the sound selects", "The bank goes in with only", ""},
    {"empty first bank slots", "The sound's first bank slot is empty", ""},
    {"settings that lose their times", "NW4R SND sets volume2", ""},
    {"settings of the 3DS that lose their times", "The 3DS sets volume2", " behind."},
    {"settings of the 3DS that keep their times", "The 3DS sets volume2", "keep their places"},
    {"effects that stay behind for the Wii's AXFX", "The ", "The Wii runs the effects of its SDK's AXFX"},
    {"biquad filters at the other console's rate", "The Switch plays the biquad filters", ""},
}};

// The kind of `warning` among kWarningKinds, or kWarningKinds.size() for another warning, such as an effect's.
std::size_t KindOf(const std::string& warning)
{
    for (std::size_t k = 0; k < kWarningKinds.size(); k++)
    {
        const WarningKind& kind = kWarningKinds[k];
        if (warning.starts_with(kind.start) && warning.find(kind.within) != std::string::npos)
        {
            return k;
        }
    }

    return kWarningKinds.size();
}

// The worst kind of finding of a comparison.
enum class Worst
{
    kSilent, // no counted window
    kNone,
    kSpans,  // spans from 1 to 3 dB off
    kBeyond, // a span more than 3 dB off
    kRun,    // a run of windows more than 6 dB off
};

// The worst kind of finding of `comparison`.
Worst WorstOf(const song::LevelComparison& comparison)
{
    if (comparison.windows == 0)
    {
        return Worst::kSilent;
    }

    Worst worst = Worst::kNone;
    for (const song::LevelPlace& place : comparison.places)
    {
        const Worst kind = place.run ? Worst::kRun : std::abs(place.db) > 3.0 ? Worst::kBeyond : Worst::kSpans;
        worst = std::max(worst, kind);
    }

    return worst;
}

// The levels of `render`'s windows of 50 ms in dBFS: the mean power of both channels.
std::vector<double> WindowLevels(const song::DryRender& render)
{
    const auto window = static_cast<std::size_t>(render.rate * song::kWindowSeconds);
    const std::size_t frames = render.samples.size() / 2;
    std::vector<double> levels;
    for (std::size_t at = 0; at < frames; at += window)
    {
        const std::size_t end = std::min(at + window, frames);
        double power = 0.0;
        for (std::size_t i = 2 * at; i < 2 * end; i++)
        {
            power += static_cast<double>(render.samples[i]) * render.samples[i];
        }
        levels.push_back(10.0 * std::log10(std::max(power / static_cast<double>(2 * (end - at)), 1e-20)));
    }

    return levels;
}

// The end in seconds of the last of `levels` at `floor` or above, or 0.
double AudibleEnd(const std::vector<double>& levels, double floor)
{
    for (std::size_t i = levels.size(); i > 0; i--)
    {
        if (levels[i - 1] >= floor)
        {
            return static_cast<double>(i) * song::kWindowSeconds;
        }
    }

    return 0.0;
}

// What ntdlib's performer reads of a sound, and the sound's render.
struct Played
{
    std::shared_ptr<const song::Sequence> sequence;
    song::DryRender render;
};

// Sound `sound` of `archive`, played dry for `seconds` seconds.
Played Play(const std::shared_ptr<const sar::SoundArchive>& archive, uint32_t sound, double seconds)
{
    const std::atomic<bool> kCancel = false;
    Played played{std::make_shared<const song::Sequence>(song::ReadSequence(*archive, sound)), {}};
    played.render = song::RenderDry(archive, played.sequence, song::Mixing::kStrict, seconds, kCancel);

    return played;
}

// Sound `sound` of the Wii's `archive`, played dry for `seconds` seconds by the rules of `build` of NW4R SND.
Played PlayWii(const std::shared_ptr<const ntdlib::rsar::SoundArchive>& archive, uint32_t sound, double seconds,
               const ntdlib::SndBuild& build)
{
    const std::atomic<bool> kCancel = false;
    Played played{std::make_shared<const song::Sequence>(song::ReadSequence(*archive, sound, {}, {}, build)), {}};
    played.render = song::RenderDry(archive, played.sequence, seconds, kCancel);

    return played;
}

// Whether `a` and `b` start the same notes: each track's notes at their ticks, with their keys, velocities and
// instruments. The notes' lengths don't count: a wave that ends sooner than its note ends the note in another frame.
bool SameNotes(const song::Sequence& a, const song::Sequence& b)
{
    const auto starts = [](const song::Sequence& sequence)
    {
        std::vector<std::vector<std::array<uint32_t, 5>>> tracks;
        for (const song::Track& track : sequence.tracks)
        {
            std::vector<std::array<uint32_t, 5>>& notes = tracks.emplace_back();
            for (const song::Note& note : track.notes)
            {
                notes.push_back({note.start, note.key, note.velocity, note.bank_slot, note.program});
            }
        }

        return tracks;
    };

    return starts(a) == starts(b);
}

// `sequence` with the banks of several slots in one bank. The Wii plays a sound with `slot_programs` this way
// (edit::OtherConsoleSong): a note of slot s and program p plays program slot_programs[s] + p of slot 0. The notes of a
// slot without a range stay as they are.
song::Sequence InOneBank(song::Sequence sequence, const std::map<int, uint32_t>& slot_programs)
{
    for (song::Track& track : sequence.tracks)
    {
        for (song::Note& note : track.notes)
        {
            if (const auto range = slot_programs.find(note.bank_slot); range != slot_programs.end())
            {
                note.bank_slot = 0;
                note.program += range->second;
            }
        }
    }

    return sequence;
}

// What the tool searches for in a sound's commands.
using Test = std::function<bool(const ntdlib::commands::Command&)>;

// Whether `command` draws a random number.
bool Draws(const ntdlib::commands::Command& command)
{
    using ntdlib::commands::Kind;
    using ntdlib::commands::Source;

    return command.kind == Kind::kRandomVariable || command.value.source == Source::kRandom ||
           (command.time && command.time->source == Source::kRandom);
}

// Whether `command` sets a biquad filter's type or value.
bool SetsBiquad(const ntdlib::commands::Command& command)
{
    using ntdlib::commands::Kind;

    return command.kind == Kind::kBiquadType || command.kind == Kind::kBiquadValue;
}

// Whether a command that the player reaches from item `start` of `sequence` passes `test`. The player's paths lead
// through the track starts too.
bool Reaches(const ntdlib::commands::Sequence& sequence, uint32_t start, const Test& test)
{
    const std::vector<bool> reached = edit::Reached(sequence, start, true);
    for (std::size_t item = 0; item < sequence.items.size(); item++)
    {
        if (reached[item] && test(sequence.items[item]))
        {
            return true;
        }
    }

    return false;
}

// Whether a command that sound `sound` of `game` reaches passes `test`. `where` is the sound's place in `file`. A sound
// whose file stays in the archive's bytes has no place. Its commands then come from the file's bytes. Replace… reads
// them the same way. A sound of the Wii without a place reaches nothing: `game` is null then.
bool SoundReaches(const sar::SoundArchive* game, const format::File& file, const std::optional<edit::SoundPlace>& where,
                  uint32_t sound, const Test& test)
{
    if (where)
    {
        const uint32_t start = where->start_field ? file.native->fields.at(*where->start_field).to.offset : 0;
        return Reaches(*file.sequences.at(where->sequence), start, test);
    }
    if (!game)
    {
        return false;
    }

    const edit::Incoming incoming =
        edit::ReadIncoming(song::ItemFile(*game, {.kind = song::ItemKind::kSequence, .index = sound}));
    const std::optional<uint32_t> start = edit::ItemAtOffset(
        edit::Offsets(incoming.sequence, song::Console{.flavor = incoming.flavor}), game->Sounds()[sound].start_offset);
    return start && Reaches(incoming.sequence, *start, test);
}

// The sequence sounds of `file` that the tool checks: those of `names`, or else up to `most`, spread evenly, or all of
// them with 0. Only the sounds that `keep` keeps count, when it's given.
std::vector<uint32_t> Chosen(const format::File& file, std::size_t most, const std::set<std::string>& names,
                             const std::function<bool(uint32_t)>& keep)
{
    std::vector<uint32_t> sequences;
    for (uint32_t sound = 0; sound < file.native->tables->sounds.size(); sound++)
    {
        const auto& entry = file.native->tables->sounds[sound];
        if (entry.kind == sar::SoundType::kSequence && (names.empty() || names.contains(entry.name)) &&
            (!keep || keep(sound)))
        {
            sequences.push_back(sound);
        }
    }
    if (!names.empty() || most == 0 || sequences.size() <= most)
    {
        return sequences;
    }

    std::vector<uint32_t> chosen;
    for (std::size_t k = 0; k < most; k++)
    {
        chosen.push_back(sequences[k * sequences.size() / most]);
    }

    return chosen;
}

// The sum of the counts of `reasons`.
std::size_t Total(const std::map<std::string, std::size_t>& reasons)
{
    std::size_t total = 0;
    for (const auto& [reason, count] : reasons)
    {
        total += count;
    }

    return total;
}

// Part k of n of the chosen sounds.
struct Part
{
    uint32_t k = 1;
    uint32_t n = 1;
};

// The part that `text` names, such as "3/8", or nothing for text that names no part.
std::optional<Part> PartOf(std::string_view text)
{
    const std::size_t slash = text.find('/');
    if (slash == std::string_view::npos)
    {
        return std::nullopt;
    }

    Part part;
    const std::string_view k = text.substr(0, slash);
    const std::string_view n = text.substr(slash + 1);
    const std::from_chars_result k_read = std::from_chars(k.data(), k.data() + k.size(), part.k);
    const std::from_chars_result n_read = std::from_chars(n.data(), n.data() + n.size(), part.n);
    if (k_read.ec != std::errc() || n_read.ec != std::errc() || k_read.ptr != k.data() + k.size() ||
        n_read.ptr != n.data() + n.size() || part.k == 0 || part.k > part.n)
    {
        return std::nullopt;
    }

    return part;
}

// The tool, with the command line's `arguments`. Returns the tool's exit status.
int Run(const std::vector<std::string>& arguments)
{
    // The options, then the archive and the numbers.
    std::optional<Part> part = Part{};
    std::string console;
    std::optional<edit::MonoWaves> mono = edit::MonoWaves::kAuto;
    std::set<std::string> names;
    bool biquads = false;
    std::size_t at = 1;
    while (at + 1 < arguments.size() && arguments[at].starts_with("--"))
    {
        if (arguments[at] == "--biquads")
        {
            biquads = true;
            at++;
            continue;
        }
        if (arguments[at] == "--part")
        {
            part = PartOf(arguments[at + 1]);
        }
        else if (arguments[at] == "--to")
        {
            console = arguments[at + 1];
        }
        else if (arguments[at] == "--mono")
        {
            mono = arguments[at + 1] == "on"    ? std::optional(edit::MonoWaves::kOn)
                   : arguments[at + 1] == "off" ? std::optional(edit::MonoWaves::kOff)
                                                : std::nullopt;
        }
        else if (arguments[at] == "--sound")
        {
            names.insert(arguments[at + 1]);
        }
        else
        {
            part.reset();
        }
        at += 2;
    }

    const bool to_wii = console == format::kWiiArchiveName;
    const std::optional<sar::Flavor> named = console.empty() ? std::nullopt : format::FlavorNamed(console);
    if (arguments.size() < at + 1 || arguments.size() > at + 3 || !part || !mono ||
        (!console.empty() && !named && !to_wii))
    {
        std::fprintf(stderr,
                     "usage: hokuto_check_other_console [--part <k>/<n>] [--to <console>] [--mono <on|off>] "
                     "[--sound <name>]... [--biquads] <archive> [most] [seconds]\n");
        return 2;
    }
    FILE* const kOut = stdout;
    const std::filesystem::path path = xsflib::PathFromUtf8(arguments[at]);
    const std::size_t most = arguments.size() > at + 1 ? std::stoul(arguments[at + 1]) : 64;
    const double seconds = arguments.size() > at + 2 ? std::stod(arguments[at + 2]) : 10.0;
    const std::string name = xsflib::Utf8(path.filename());
    const auto began = std::chrono::steady_clock::now();

    // The game's archive, and the file that its import makes. A Wii archive takes its game's build of NW4R SND.
    std::shared_ptr<const sar::SoundArchive> game;
    std::shared_ptr<const ntdlib::rsar::SoundArchive> wii;
    ntdlib::SndBuild build;
    format::File file;
    std::vector<std::optional<edit::SoundPlace>> sound_places;
    sar::Flavor to = sar::Flavor::kCafe;
    try
    {
        std::vector<uint8_t> bytes;
        xsflib::ReadFile(path, bytes);
        if (bytes.size() >= 4 && std::string_view(reinterpret_cast<const char*>(bytes.data()), 4) == "RSAR")
        {
            std::vector<uint8_t> dol;
            const std::optional<std::filesystem::path> found = song::FindMainDol(path);
            const std::optional<ntdlib::SndBuild> read =
                found && xsflib::ReadFile(*found, dol) ? song::ReadWiiBuild(dol) : std::nullopt;
            wii = song::LoadWiiArchive(bytes, name, ntdlib::rvl::snd::kRsarOptions);
            file = format::ImportWiiArchive(bytes, read);
            build = format::BuildOfWiiArchive(*file.native);
            sound_places = edit::SoundPlaces(*wii, format::SequencesOfFiles(file, bytes, *wii),
                                             format::StartFieldsOfSounds(file, bytes));
            to = named.value_or(sar::Flavor::kCafe);
        }
        else
        {
            game = song::LoadArchive(bytes, name, path);
            const sar::Flavor flavor = game->ReadOptions().flavor;
            file = format::ImportArchive(bytes, flavor, game->GroupFiles(), song::ReadStreams(*game, path),
                                         game->GroupFileGroups());
            sound_places = edit::SoundPlaces(*game, format::SequencesOfFiles(file, bytes, *game),
                                             format::StartFieldsOfSounds(file, bytes));
            to = named.value_or(song::OtherConsole(flavor));
        }
        if (!file.native->tables)
        {
            throw std::runtime_error("Hokuto can't read the archive's tables.");
        }
        if (wii && to_wii)
        {
            throw std::runtime_error("A sound of the Wii goes to another console only.");
        }
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
        return 2;
    }
    const std::string target = to_wii ? "Wii" : song::ConsoleName(to);

    // Whether a command that a sound reaches passes a test.
    const auto reaches = [&](uint32_t sound, const Test& test)
    {
        return SoundReaches(game.get(), file, sound_places.at(sound), sound, test);
    };

    int result = 0;
    std::vector<uint32_t> chosen;
    std::size_t unread = 0;
    const auto sets_biquads = [&](uint32_t sound)
    {
        try
        {
            return reaches(sound, SetsBiquad);
        }
        catch (const std::exception&)
        {
            unread++;
            return false;
        }
    };
    const std::vector<uint32_t> all =
        Chosen(file, most, names, biquads ? std::function<bool(uint32_t)>(sets_biquads) : nullptr);
    if (unread > 0)
    {
        std::fprintf(kOut, "%s: the tool can't read the commands of %zu sequence sounds, and leaves them out.\n",
                     name.c_str(), unread);
    }
    if (all.size() < names.size())
    {
        std::fprintf(stderr, "%s: %zu of the %zu names name no sequence sound.\n", name.c_str(),
                     names.size() - all.size(), names.size());
        return 2;
    }
    for (std::size_t place = 0; place < all.size(); place++)
    {
        if (place % part->n == part->k - 1)
        {
            chosen.push_back(all[place]);
        }
    }
    std::size_t converted = 0;
    std::map<std::string, std::size_t> refusals;
    std::map<std::string, std::size_t> failures;
    std::map<Worst, std::size_t> worst;
    std::vector<double> gains;
    std::size_t ends_elsewhere = 0;
    std::size_t large_gains = 0;
    std::size_t random = 0;
    std::map<Worst, std::size_t> worst_random;
    std::size_t other_notes = 0;
    std::map<Worst, std::size_t> worst_other_notes;
    std::size_t other_timing = 0;
    std::size_t lost = 0;
    std::array<std::size_t, kWarningKinds.size() + 1> warned{};
    double slowest = 0.0;
    for (const uint32_t sound : chosen)
    {
        const std::string& sound_name = file.native->tables->sounds[sound].name;
        edit::OtherConsoleSong other;
        std::shared_ptr<const sar::SoundArchive> built;
        std::shared_ptr<const ntdlib::rsar::SoundArchive> built_wii;
        try
        {
            const auto start = std::chrono::steady_clock::now();
            other = to_wii ? edit::ForWii(file, sound) : edit::ForOtherConsole(file, sound, to, *mono);
            slowest =
                std::max(slowest, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
            if (to_wii)
            {
                built_wii =
                    song::LoadWiiArchive(format::ExportArchive(other.file), sound_name, ntdlib::rvl::snd::kRsarOptions);
            }
            else
            {
                built = song::LoadArchive(format::ExportArchive(other.file), sound_name,
                                          format::ExportGroupFiles(other.file));
            }
        }
        catch (const std::invalid_argument& error)
        {
            std::fprintf(kOut, "%s: %s doesn't go to the %s. %s\n", name.c_str(), sound_name.c_str(), target.c_str(),
                         error.what());
            refusals[error.what()]++;
            continue;
        }
        catch (const std::exception& error)
        {
            std::fprintf(kOut, "%s: %s fails to go to the %s. %s\n", name.c_str(), sound_name.c_str(), target.c_str(),
                         error.what());
            failures[error.what()]++;
            result = 1;
            continue;
        }
        converted++;
        std::array<bool, kWarningKinds.size() + 1> kinds{};
        for (const std::string& warning : other.warnings)
        {
            kinds[KindOf(warning)] = true;
        }
        for (std::size_t k = 0; k < kinds.size(); k++)
        {
            warned[k] += kinds[k] ? 1 : 0;
        }
        if (!(format::Read(format::Write(other.file)) == other.file) ||
            !(format::ReadText(format::WriteText(other.file)) == other.file))
        {
            std::fprintf(kOut, "%s: %s's new file doesn't come back from both forms.\n", name.c_str(),
                         sound_name.c_str());
            lost++;
            result = 1;
        }

        // The renders and their levels, at the higher of the two rates.
        const Played played_before = wii ? PlayWii(wii, sound, seconds, build) : Play(game, sound, seconds);
        const Played played_after = to_wii
                                        ? PlayWii(built_wii, 0, seconds, format::BuildOfWiiArchive(*other.file.native))
                                        : Play(built, 0, seconds);
        const int rate = std::max(played_before.render.rate, played_after.render.rate);
        const song::DryRender before = song::AtRate(played_before.render, rate);
        const song::DryRender after = song::AtRate(played_after.render, rate);
        const bool draws = reaches(sound, Draws);
        const bool same_notes =
            SameNotes(InOneBank(*played_before.sequence, other.slot_programs), *played_after.sequence);
        const char* mark = draws ? ", with random numbers" : same_notes ? "" : ", with other notes";
        random += draws ? 1 : 0;
        other_notes += !draws && !same_notes ? 1 : 0;
        if (!song::SameTiming(*played_before.sequence, *played_after.sequence))
        {
            std::fprintf(kOut, "%s: %s plays its ticks at other times on the %s%s.\n", name.c_str(), sound_name.c_str(),
                         target.c_str(), mark);
            other_timing++;
        }

        // Where each render falls below the floor of the windows that the measure counts.
        const std::vector<double> levels_before = WindowLevels(before);
        const std::vector<double> levels_after = WindowLevels(after);
        double top = -200.0;
        for (const double level : levels_before)
        {
            top = std::max(top, level);
        }
        for (const double level : levels_after)
        {
            top = std::max(top, level);
        }
        const double floor = std::max(top - 50.0, -70.0);
        const double end_before = AudibleEnd(levels_before, floor);
        const double end_after = AudibleEnd(levels_after, floor);
        if (std::abs(end_before - end_after) > 0.1)
        {
            std::fprintf(kOut, "%s: %s ends at %.2f s, and at %.2f s on the %s%s.\n", name.c_str(), sound_name.c_str(),
                         end_before, end_after, target.c_str(), mark);
            ends_elsewhere++;
        }
        const song::LevelComparison comparison = song::CompareLevels(before.samples, after.samples, before.rate);
        const Worst kind = WorstOf(comparison);
        worst[kind]++;
        worst_random[kind] += draws ? 1 : 0;
        worst_other_notes[kind] += !draws && !same_notes ? 1 : 0;
        if (kind != Worst::kSilent)
        {
            gains.push_back(comparison.gain_db);
        }
        if (kind != Worst::kSilent && std::abs(comparison.gain_db) > 3.0)
        {
            std::fprintf(kOut, "%s: %s plays at a gain of %+.2f dB on the %s%s.\n", name.c_str(), sound_name.c_str(),
                         comparison.gain_db, target.c_str(), mark);
            large_gains++;
        }
        if (kind >= Worst::kSpans)
        {
            const std::vector<song::LevelPlace> places = song::WorstPlaces(comparison, song::kSpanFindingDb, 1);
            const song::LevelPlace& place = places.front();
            std::fprintf(kOut, "%s: %s: %zu findings%s. The worst: %s of %.2f s at %.2f s, %+.1f dB. Gain %+.2f dB.\n",
                         name.c_str(), sound_name.c_str(), comparison.places.size(), mark,
                         place.run ? "a run" : "a span", place.seconds, place.start, place.db, comparison.gain_db);
        }
        else if (biquads && kind == Worst::kNone)
        {
            std::fprintf(kOut, "%s: %s: no finding%s. Gain %+.2f dB.\n", name.c_str(), sound_name.c_str(), mark,
                         comparison.gain_db);
        }
        else if (biquads)
        {
            std::fprintf(kOut, "%s: %s is silent on both consoles%s.\n", name.c_str(), sound_name.c_str(), mark);
        }
    }

    // The summary.
    std::ranges::sort(gains);
    const double taken = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    std::fprintf(
        kOut,
        "%s: %zu of %zu sequence sounds went to the %s. %zu were refused, and %zu failed. %zu are silent on both. "
        "Findings: none %zu, spans from 1 to 3 dB %zu, beyond 3 dB %zu, runs %zu. %zu end elsewhere. ",
        name.c_str(), converted, chosen.size(), target.c_str(), Total(refusals), Total(failures), worst[Worst::kSilent],
        worst[Worst::kNone], worst[Worst::kSpans], worst[Worst::kBeyond], worst[Worst::kRun], ends_elsewhere);
    if (!gains.empty())
    {
        std::fprintf(kOut, "Gains from %+.2f to %+.2f dB, median %+.2f dB, %zu beyond 3 dB either way. ", gains.front(),
                     gains.back(), gains[gains.size() / 2], large_gains);
    }
    std::fprintf(kOut,
                 "%zu play their ticks at other times. %zu new files don't come back. The slowest conversion took "
                 "%.2f s. %.1f s.\n",
                 other_timing, lost, slowest, taken);
    std::fprintf(
        kOut,
        "%s: %zu sounds draw random numbers. Of them, findings: none %zu, spans from 1 to 3 dB %zu, beyond 3 dB %zu, "
        "runs %zu.\n",
        name.c_str(), random, worst_random[Worst::kNone], worst_random[Worst::kSpans], worst_random[Worst::kBeyond],
        worst_random[Worst::kRun]);
    std::fprintf(kOut,
                 "%s: %zu other sounds start other notes on the %s. Of them, findings: none %zu, spans from 1 to 3 dB "
                 "%zu, beyond 3 dB %zu, runs %zu.\n",
                 name.c_str(), other_notes, target.c_str(), worst_other_notes[Worst::kNone],
                 worst_other_notes[Worst::kSpans], worst_other_notes[Worst::kBeyond], worst_other_notes[Worst::kRun]);
    for (std::size_t k = 0; k < warned.size(); k++)
    {
        if (warned[k] > 0)
        {
            std::fprintf(kOut, "%s: %zu sounds with %s.\n", name.c_str(), warned[k],
                         k < kWarningKinds.size() ? kWarningKinds[k].name : "other warnings");
        }
    }
    for (const auto& [reason, count] : refusals)
    {
        std::fprintf(kOut, "%s: %zu refused: %s\n", name.c_str(), count, reason.c_str());
    }
    for (const auto& [reason, count] : failures)
    {
        std::fprintf(kOut, "%s: %zu failed: %s\n", name.c_str(), count, reason.c_str());
    }

    return result;
}

} // namespace

int main(int argc, char** argv)
{
    try
    {
        return Run(xsflib::console::Arguments(argc, argv));
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 2;
    }
}
