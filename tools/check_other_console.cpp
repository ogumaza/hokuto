// SPDX-License-Identifier: MIT

// Checks the songs that go to another console (src/edit/other_console.h) on a sound archive of the 3DS, the Wii U or
// the Switch, with the group files and the streams beside it, on a sound archive of the Wii, on a GBA ROM with
// MusicPlayer2000, or on a sound archive of the DS or a DS cartridge image. As in the program's import, the tool uses
// the largest sound archive of a DS image. The tool imports the archive or the ROM as a .hok file. A Wii archive takes
// the build of NW4R SND from the main.dol of the extracted disc that holds it. The program's import takes the build
// from there too. Each checked sequence sound then goes to the console that --to names as a new file: 3ds, ds, wii,
// wii_u or switch. Without the option, a sound of the Switch, the 3DS or the Wii goes to the Wii U, and one of the Wii
// U to the Switch. A sound of the Wii goes to another console only. A song of the GBA and a sequence of the DS go to
// the Wii U without the option, and on from there with it. The new file has to come back from its binary form and its
// text form. The sound plays dry for `seconds` seconds on ntdlib's model of each console: from the game's archive, and
// from the archive that the new file exports (song::RenderDry). The Wii's model mixes at 32 kHz, by the rules of the
// file's build of NW4R SND. The 3DS's model runs on ntdlib's model of the DSP's audio firmware, and the DS's on
// NITRO-Composer at about 32,728 Hz. The render at the lower rate goes to the higher (song::AtRate). The audibility
// method's level measure compares the two renders (song::CompareLevels). A span of 1 s more than 1 dB off and a run of
// windows more than 6 dB off are its findings. The renders' gain counts apart: a listener sets the volume. A sound
// whose gain lies more than 3 dB either way still gets a line. A conversion that plays silent takes a gain of 0 dB
// instead. Every window where the game's sound plays then counts against the conversion.
//
// The tool checks up to `most` sequence sounds, spread evenly over the sound table, or every one with 0. It prints a
// line for each sound that doesn't convert or whose render has findings, then a summary: the sounds that convert, the
// reasons of those that don't, the findings by their worst kind, the gains, the sounds that end elsewhere, and the
// sounds with each kind of warning. A sound ends after its last window above the floor of the windows that the measure
// counts: 50 dB below the loudest window of either render, and -70 dBFS. A model can run on in silence after that. A
// sound that the program refuses with a message for the user counts apart from one that fails. It also counts the
// findings of the sounds whose commands draw random numbers, and of the other sounds that start other notes on the
// other console, such as a sound that waits for a voice to end. The engines' generator steps once a frame, a voice ends
// in a frame, and the consoles' frames differ. On the Wii and the DS, a note of another bank slot than the first starts
// the same note where it plays its program in its slot's range of the sound's one bank (edit::OtherConsoleSong). It
// counts the sounds whose conversions play their ticks at other times than the game's sound (song::SameTiming). On
// another console's engine, the program's playhead follows the conversion's ticks over the sound's arrangement. A song
// of the GBA plays at 24 ticks a quarter note, and its conversion at 48. Its notes count in milliseconds instead, those
// that start in the first `seconds`: the tool prints a line for each song whose notes start more than 20 ms from the
// times on the GBA, and prints the largest difference in the summary. It also prints a line for each track of a song or
// a DS sequence with notes that matches no track of the conversion (song::TracksOfConversion): the mixer's mute and
// meter of such a track reach nothing. The tool checks the songs that play (song::GbaSongs), and `--sound` takes a
// song's name, such as "Song 012". MP2K draws no random numbers and sets no biquad filters. A sound of another GBA
// driver, such as Konami's driver, converts the same way (song::DriverRom::Sounds). With `--midi`, the tool also
// measures the MIDI export of each song as the Export window does: against the same render on the GBA, for the same
// `seconds`. On the DS, it measures ntdlib's MIDI export of each sequence with its SoundFont the same way. The MIDI
// file of the GBA contains two passes of the loop, and that of the DS one. Each ends there. Both measures then end
// where the shorter render ends. A song whose conversion has a worse kind of finding than the MIDI export there gets a
// line, and the summary counts the kinds of both. The conversion passes through the MIDI file and the SoundFont, and
// includes their findings. A sequence of the DS counts its notes in milliseconds too: its conversion uses tempos that
// keep the timing of the DS. The DS driver and NintendoWare's engines scale the level of a note by the same curve of
// its velocity, and the velocities compare as they are. The tool doesn't tell the DS's sequences that draw random
// numbers from those that start other notes. The tool checks the DS's sequences (song::DsSounds) by their names. A DS
// track that the converter splits over several MIDI channels can join them back into one track (SongOfMidi's
// `joins_layers`). A sound of DSE (song::DseSounds) converts the same way as a sequence of the DS. The tool returns 1
// when a sound fails to convert or a file doesn't come back, and 2 when the archive can't be read. It makes no sound.
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
// With `--to ds`, a sound of the 3DS, the Wii U, the Switch or the Wii starts other notes as on the other consoles:
// when a track's notes start at other ticks, or with other keys, velocities or instruments (SameNotes). The DS's
// conversion keeps each command's ticks. The notes also count in milliseconds, with their keys and velocities as they
// are. The tool prints a line for each sound whose notes start more than 20 ms from the times of the source, and prints
// the largest difference in the summary. Those times decide nothing about other notes. A song of the GBA or of Konami's
// driver and a sound of DSE start other notes when their notes in milliseconds don't match (GbaNotesOff), on the DS as
// on the other consoles.
//
// The sound unit of the DS cuts each output sample to 10 bits, and a quiet tail stays at a level of about -60 dBFS
// there. With `--enhanced`, the DS's model plays the new file in enhanced mixing, without that cut. A finding that
// stays then doesn't come from the 10 bits. The option requires `--to ds`.
//
// usage: hokuto_check_other_console [--part <k>/<n>] [--to <console>] [--mono <on|off>] [--sound <name>]...
//        [--biquads] [--midi] [--enhanced] <archive or ROM> [most] [seconds]

#include "check_other_console.h"

namespace hokuto::tools::other_console
{

namespace
{

// The kinds of warning that the tool counts, by the start of their text.
struct WarningKind
{
    const char* name;
    const char* start;
    const char* within;
};

constexpr std::array<WarningKind, 53> kWarningKinds = {{
    {"times of settings left behind on the DS", "The DS applies settings at once", ""},
    {"values past what the DS's commands hold", "", "out of range for the DS"},
    {"timebases that change", "The sequence changes its ticks per quarter note", ""},
    {"tempos from variables at another timebase", "", "The DS uses 48 ticks per quarter note, but the song uses"},
    {"track variables that moved", "The DS has no track variables.", ""},
    {"calls and loops too deep for the DS", "The calls and counted loops of the sequence nest", ""},
    {"banks with their waves as one channel", "DS waves are mono.", ""},
    {"regions of velocities left out", "DS regions have no velocity ranges.", ""},
    {"regions with holds", "", "DS regions have no hold"},
    {"regions in key groups", "", "DS regions have no key groups"},
    {"waves that lose their loops", "", "65,535-word limit of a DS wave"},
    {"regions at a rate past a DS wave's", "", "above the DS maximum"},
    {"waves that go in at the DS's mix rate", "The DS mixes at ", ""},
    {"waves at lower rates", "The waves don't fit in Hokuto's DS model", ""},
    {"more notes at once than the DS plays", "The sound has up to ", "The DS has 16 channels"},
    {"banks left behind", "Bank ", " is left out"},
    {"banks with only the instruments that the sound selects", "The banks keep only", ""},
    {"banks of an older version", "Bank ", "is written as version"},
    {"wave archives of an older version", "Wave archive ", "is written as version"},
    {"waves without their loops' original starts", "The waves of ", ""},
    {"8-bit waves in 16-bit PCM", "", "converted to 16-bit PCM"},
    {"sequence files of an older version", "The sequence file is written as version", ""},
    {"sequence files with other sounds' commands", "The sequence file also contains the commands", ""},
    {"sequence files whose commands keep their places", "The commands in the sequence file must stay", ""},
    {"commands left out", "The new file leaves out ", ""},
    {"commands in their places that the 3DS skips", "", "The 3DS skip"},
    {"settings that stay behind", "The sound's 3D settings", ""},
    {"values that stay behind", "The sound's ", " is left out. "},
    {"strict mixing in place of the enhanced mode", "The Wii plays in strict mixing only", ""},
    {"more notes at once than the 3DS plays", "The 3DS plays ", " notes and "},
    {"stereo waves in as mono", "", " converted to mono. "},
    {"volumes above 127 in as 127", "The sound's volume is lowered", ""},
    {"pans of the build of October 2006", "The sound is set to the dual pan mode", ""},
    {"remote filters and user parameters that stay behind", "The sound's remote filter", ""},
    {"effects of the Wii that stay behind", "The ", "Hokuto plays Wii effects only on the Wii"},
    {"global variables given a starting value at the start", "The sound reads global variable", ""},
    {"waves that the Wii doesn't play", "", "an encoding that the Wii can't play"},
    {"banks in other slots that the notes may play", "The bank", "Hokuto can't follow the bank selects"},
    {"banks in slots that only a variable or a random number selects", "The bank", "Only a variable or a random"},
    {"banks in other slots that stay behind", "The bank", " other slot"},
    {"banks of several slots in one bank", "A ", " sound can use only one bank because"},
    {"notes that may play another instrument on the Wii or the DS", "", " note command"},
    {"banks of the Wii or the DS with only the instruments that the sound selects", "The bank keeps only", ""},
    {"empty first bank slots", "The sound's first bank slot is empty", ""},
    {"settings that lose their times", "NW4R SND applies volume2", ""},
    {"settings of the 3DS that lose their times", "The 3DS applies volume2", " dropped."},
    {"settings of the 3DS that keep their times", "The 3DS applies volume2", "stay at their offsets"},
    {"effects that stay behind for the Wii's AXFX", "The ", "The Wii uses AXFX effects instead"},
    {"biquad filters at the other console's rate", "The Switch runs biquad filter types", ""},
    {"tempos that move between whole tempos", "", " tempos must be whole BPM values"},
    {"events of the MIDI file left out", "", " left out: "},
    {"parts of the SoundFont left out", "Ignored at ", ""},
    {"reverbs that send to bus A without an effect", "The song sends reverb to bus A", ""},
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

// Checks that the mixer's mutes and meters reach each track of `before` with notes through a track of `after`, its
// conversion for the `target` console (song::TracksOfConversion). Prints each track that reaches none to `out`, with
// the archive's `name` and the sound's `sound_name`, and counts the tracks in `found` and `lost`.
void CheckMutesReach(const song::Sequence& before, const song::Sequence& after, FILE* out, const std::string& name,
                     const std::string& sound_name, const std::string& target, std::size_t& found, std::size_t& lost)
{
    const std::array<uint32_t, 32> tracks = song::TracksOfConversion(before, after);
    for (const song::Track& track : before.tracks)
    {
        const bool reached = tracks[static_cast<std::size_t>(track.number) & 31] != 0;
        found += !track.notes.empty() && reached ? 1 : 0;
        if (!track.notes.empty() && !reached)
        {
            lost++;
            std::fprintf(out, "%s: %s's track %d goes with no track of its conversion for the %s.\n", name.c_str(),
                         sound_name.c_str(), track.number, target.c_str());
        }
    }
}

// The options of the command line.
struct Options
{
    std::optional<Part> part = Part{};
    std::string console;
    std::optional<edit::MonoWaves> mono = edit::MonoWaves::kAuto;
    std::set<std::string> names;
    bool biquads = false;
    bool midi = false;
    song::Mixing ds_mixing = song::Mixing::kStrict;
    bool to_wii = false;
    bool to_ds = false;
    std::optional<sar::Flavor> named;
    std::filesystem::path path;
    std::size_t most = 64;
    double seconds = 10.0;
};

// The options of the command line's `arguments`, or nothing after a line about a wrong one.
std::optional<Options> ParseOptions(const std::vector<std::string>& arguments)
{
    Options o;
    std::size_t at = 1;
    while (at + 1 < arguments.size() && arguments[at].starts_with("--"))
    {
        if (arguments[at] == "--biquads" || arguments[at] == "--midi")
        {
            (arguments[at] == "--midi" ? o.midi : o.biquads) = true;
            at++;
            continue;
        }
        if (arguments[at] == "--enhanced")
        {
            o.ds_mixing = song::Mixing::kEnhanced;
            at++;
            continue;
        }
        if (arguments[at] == "--part")
        {
            o.part = PartOf(arguments[at + 1]);
        }
        else if (arguments[at] == "--to")
        {
            o.console = arguments[at + 1];
        }
        else if (arguments[at] == "--mono")
        {
            o.mono = arguments[at + 1] == "on"    ? std::optional(edit::MonoWaves::kOn)
                     : arguments[at + 1] == "off" ? std::optional(edit::MonoWaves::kOff)
                                                  : std::nullopt;
        }
        else if (arguments[at] == "--sound")
        {
            o.names.insert(arguments[at + 1]);
        }
        else
        {
            o.part.reset();
        }
        at += 2;
    }

    o.to_wii = o.console == format::kWiiArchiveName;
    o.to_ds = o.console == format::kDsArchiveName;
    o.named = o.console.empty() ? std::nullopt : format::FlavorNamed(o.console);
    if (arguments.size() < at + 1 || arguments.size() > at + 3 || !o.part || !o.mono ||
        (!o.console.empty() && !o.named && !o.to_wii && !o.to_ds) ||
        (o.ds_mixing == song::Mixing::kEnhanced && !o.to_ds))
    {
        std::fprintf(stderr,
                     "usage: hokuto_check_other_console [--part <k>/<n>] [--to <console>] [--mono <on|off>] "
                     "[--sound <name>]... [--biquads] [--midi] [--enhanced] <archive or ROM> [most] [seconds]\n");
        return std::nullopt;
    }
    o.path = xsflib::PathFromUtf8(arguments[at]);
    o.most = arguments.size() > at + 1 ? std::stoul(arguments[at + 1]) : 64;
    o.seconds = arguments.size() > at + 2 ? std::stod(arguments[at + 2]) : 10.0;

    return o;
}

// The game's archive of a check, and the file that its import makes. A Wii archive takes its game's build of NW4R SND.
struct OpenedArchive
{
    std::shared_ptr<const sar::SoundArchive> game;
    std::shared_ptr<const ntdlib::rsar::SoundArchive> wii;
    std::shared_ptr<const song::GbaRom> gba;
    std::shared_ptr<const song::DriverRom> driver; // a ROM with another of the GBA's drivers, such as Konami's
    std::string_view driver_name;
    std::shared_ptr<const song::DsArchive> ds;
    std::shared_ptr<const song::DseSet> dse;
    ntdlib::SndBuild build;
    format::File file;
    std::vector<std::optional<edit::SoundPlace>> sound_places;
    sar::Flavor to = sar::Flavor::kCafe;
};

// Opens the archive or ROM of `o.path`, `name`, as the check reads it. Throws where the check refuses it.
OpenedArchive OpenArchive(const Options& o, const std::string& name)
{
    OpenedArchive a;
    std::vector<uint8_t> bytes;
    xsflib::ReadFile(o.path, bytes);
    if (const format::Driver* found = format::IsGbaRom(bytes) ? format::DriverOfRom(bytes) : nullptr)
    {
        a.file = found->Import(bytes);
        a.driver_name = found->Facts().engine_name;
        a.driver = song::LoadDriverRom(found->Facts().engine, std::move(bytes));
        a.to = o.named.value_or(sar::Flavor::kCafe);
    }
    else if (format::IsGbaRom(bytes))
    {
        a.file = format::ImportGbaRom(bytes);
        a.gba = song::LoadGbaRom(std::move(bytes));
        a.to = o.named.value_or(sar::Flavor::kCafe);
    }
    else if (format::IsDsImage(bytes) && format::DsArchivesIn(bytes).empty() && format::HoldsDseFiles(bytes))
    {
        a.file = format::ImportDseImage(bytes);
        a.dse = song::LoadDseSet(bytes);
        a.to = o.named.value_or(sar::Flavor::kCafe);
    }
    else if (format::IsDsArchive(bytes) || format::IsDsImage(bytes))
    {
        const bool image = format::IsDsImage(bytes);
        a.file = image ? format::ImportDsImage(bytes) : format::ImportDsArchive(bytes);
        a.ds = song::LoadDsArchive(format::ExportArchive(a.file),
                                   image ? song::DsBuildOf(bytes) : ntdlib::ntr::snd::kDefaultBuild,
                                   edit::DsGameOfFile(a.file));
        a.to = o.named.value_or(sar::Flavor::kCafe);
    }
    else if (bytes.size() >= 4 && std::string_view(reinterpret_cast<const char*>(bytes.data()), 4) == "RSAR")
    {
        std::vector<uint8_t> dol;
        const std::optional<std::filesystem::path> found = song::FindMainDol(o.path);
        const std::optional<ntdlib::SndBuild> read =
            found && xsflib::ReadFile(*found, dol) ? song::ReadWiiBuild(dol) : std::nullopt;
        a.wii = song::LoadWiiArchive(bytes, name, ntdlib::rvl::snd::kRsarOptions);
        a.file = format::ImportWiiArchive(bytes, read);
        a.build = format::BuildOfWiiArchive(*a.file.native);
        a.sound_places = edit::SoundPlaces(*a.wii, format::SequencesOfFiles(a.file, bytes, *a.wii),
                                           format::StartFieldsOfSounds(a.file, bytes));
        a.to = o.named.value_or(sar::Flavor::kCafe);
    }
    else
    {
        a.game = song::LoadArchive(bytes, name, o.path);
        const sar::Flavor flavor = a.game->ReadOptions().flavor;
        a.file = format::ImportArchive(bytes, flavor, a.game->GroupFiles(), song::ReadStreams(*a.game, o.path),
                                       a.game->GroupFileGroups());
        a.sound_places = edit::SoundPlaces(*a.game, format::SequencesOfFiles(a.file, bytes, *a.game),
                                           format::StartFieldsOfSounds(a.file, bytes));
        a.to = o.named.value_or(song::OtherConsole(flavor));
    }
    if (!a.gba && !a.driver && !a.ds && !a.dse && !a.file.native->tables)
    {
        throw std::runtime_error("Hokuto can't read the archive's tables.");
    }
    if (a.gba && o.biquads)
    {
        throw std::runtime_error("MP2K sets no biquad filters.");
    }
    if (a.driver && o.biquads)
    {
        throw std::runtime_error(std::string(a.driver_name) + " driver sets no biquad filters.");
    }
    if (a.ds && o.biquads)
    {
        throw std::runtime_error("NITRO-Composer sets no biquad filters.");
    }
    if (a.dse && o.biquads)
    {
        throw std::runtime_error("DSE sets no biquad filters.");
    }
    if (!a.gba && !a.driver && !a.ds && !a.dse && o.midi)
    {
        throw std::runtime_error(
            "--midi measures the MIDI exports of a GBA ROM's songs, the DS's sequences and DSE's sounds alone.");
    }
    if (a.wii && o.to_wii)
    {
        throw std::runtime_error("A sound of the Wii goes to another console only.");
    }
    if (a.ds && o.to_ds)
    {
        throw std::runtime_error("A sequence of the DS goes to another console only.");
    }

    return a;
}

// The name of sound `sound` of the archive `a`.
std::string SoundNameOf(const OpenedArchive& a, uint32_t sound)
{
    return a.driver ? a.driver->SoundName(sound)
           : a.gba  ? song::GbaSongName(*a.gba, static_cast<int>(sound))
           : a.ds   ? song::DsSoundName(*a.ds, sound)
           : a.dse  ? song::DseSoundName(*a.dse, sound)
                    : a.file.native->tables->sounds[sound].name;
}

// Plays sound `sound` of the archive `a` for `seconds` seconds, as the program plays the game's sound.
Played PlaySource(const OpenedArchive& a, uint32_t sound, double seconds)
{
    return a.driver ? PlayDriver(a.driver, sound, seconds)
           : a.gba  ? PlayGba(a.gba, sound, seconds)
           : a.ds   ? PlayDs(a.ds, sound, seconds)
           : a.dse  ? PlayDse(a.dse, sound, seconds)
           : a.wii  ? PlayWii(a.wii, sound, seconds, a.build)
                    : Play(a.game, sound, seconds);
}

// Counts each kind of warning among `warnings` once in `warned`.
void CountWarningKinds(const std::vector<std::string>& warnings,
                       std::array<std::size_t, kWarningKinds.size() + 1>& warned)
{
    std::array<bool, kWarningKinds.size() + 1> kinds{};
    for (const std::string& warning : warnings)
    {
        kinds[KindOf(warning)] = true;
    }
    for (std::size_t k = 0; k < kinds.size(); k++)
    {
        warned[k] += kinds[k] ? 1 : 0;
    }
}

// The audible ends of the renders `before` and `after`, in seconds: where each falls below the floor of the windows
// that the measure counts.
std::pair<double, double> AudibleEnds(const song::DryRender& before, const song::DryRender& after)
{
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

    return {end_before, end_after};
}

// The renders of sound `sound` of the archive `a`, a GBA ROM or a DS archive or image, and of the MIDI export of it.
// `sequence` is the sound as the program reads it.
hokuto::measure::Renders RenderMidiExport(const OpenedArchive& a, uint32_t sound,
                                          const std::shared_ptr<const song::Sequence>& sequence)
{
    const std::atomic<bool> kCancel = false;
    const auto conversion = a.driver ? a.driver->ConvertToMidi(sound, "Hokuto")
                            : a.dse  ? song::ConvertToMidi(*a.dse, sound, "Hokuto")
                            : a.gba  ? song::ConvertToMidi(*a.gba, static_cast<int>(sound), "Hokuto")
                                     : song::ConvertToMidi(*a.ds, sound, {}, "Hokuto");
    const song::Played played = a.driver ? song::Played(a.driver)
                                : a.dse  ? song::Played(a.dse)
                                : a.gba  ? song::Played(a.gba)
                                         : song::Played(a.ds);
    return hokuto::measure::RenderExport(conversion, hokuto::measure::Instruments::kSoundFont,
                                         song::DryOf(played, sequence, song::Mixing::kStrict), kCancel);
}

// The stream that receives the lines of the tool.
FILE* const kOut = stdout;

// What the tool counts over the sounds, for the summary, and the tool's exit status.
struct Tallies
{
    int result = 0;
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
    std::size_t tracks_found = 0;
    std::size_t tracks_lost = 0;
    std::vector<double> notes_off;
    std::map<Worst, std::size_t> worst_midi;
    std::size_t worse_than_midi = 0;
    std::size_t lost = 0;
    std::array<std::size_t, kWarningKinds.size() + 1> warned{};
    double slowest = 0.0;
};

// The sounds of `a`, `name`, that the check selects by `o`, or nothing after a line about names that name no sequence
// sound.
std::optional<std::vector<uint32_t>> ChooseSounds(const Options& o, const OpenedArchive& a, const std::string& name)
{
    // Whether a command that a sound reaches passes a test.
    const auto reaches = [&](uint32_t sound, const Test& test)
    {
        return SoundReaches(a.game.get(), a.file, a.sound_places.at(sound), sound, test);
    };

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
        a.driver ? DriverChosen(*a.driver, o.most, o.names)
        : a.gba  ? GbaChosen(*a.gba, o.most, o.names)
        : a.ds   ? DsChosen(*a.ds, o.most, o.names)
        : a.dse  ? DseChosen(*a.dse, o.most, o.names)
                 : Chosen(a.file, o.most, o.names, o.biquads ? std::function<bool(uint32_t)>(sets_biquads) : nullptr);
    if (unread > 0)
    {
        std::fprintf(kOut, "%s: the tool can't read the commands of %zu sequence sounds, and leaves them out.\n",
                     name.c_str(), unread);
    }
    if (all.size() < o.names.size())
    {
        std::fprintf(stderr, "%s: %zu of the %zu names name no sequence sound.\n", name.c_str(),
                     o.names.size() - all.size(), o.names.size());
        return std::nullopt;
    }
    for (std::size_t place = 0; place < all.size(); place++)
    {
        if (place % o.part->n == o.part->k - 1)
        {
            chosen.push_back(all[place]);
        }
    }

    return chosen;
}

// A sound that went to the console of the check: the new file, and the archive that the file exports for that
// console.
struct Sent
{
    edit::OtherConsoleSong other;
    std::shared_ptr<const sar::SoundArchive> built;
    std::shared_ptr<const ntdlib::rsar::SoundArchive> built_wii;
    std::shared_ptr<const song::DsArchive> built_ds;
};

// Sends sound `sound_name` of `a`'s file to `target`, the console of the check, and loads the archive that the new
// file exports. Returns nothing after a line about a refusal or a failure. `tallies` counts them.
std::optional<Sent> SendSound(const Options& o, const OpenedArchive& a, const std::string& name,
                              const std::string& target, uint32_t sound, const std::string& sound_name,
                              Tallies& tallies)
{
    Sent sent;
    try
    {
        const auto start = std::chrono::steady_clock::now();
        sent.other = o.to_ds    ? edit::ForDs(a.file, sound)
                     : o.to_wii ? edit::ForWii(a.file, sound)
                                : edit::ForOtherConsole(a.file, sound, a.to, *o.mono);
        tallies.slowest =
            std::max(tallies.slowest, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
        if (o.to_ds)
        {
            sent.built_ds = song::LoadDsArchive(format::ExportArchive(sent.other.file));
        }
        else if (o.to_wii)
        {
            sent.built_wii = song::LoadWiiArchive(format::ExportArchive(sent.other.file), sound_name,
                                                  ntdlib::rvl::snd::kRsarOptions);
        }
        else
        {
            sent.built = song::LoadArchive(format::ExportArchive(sent.other.file), sound_name,
                                           format::ExportGroupFiles(sent.other.file));
        }
    }
    catch (const std::invalid_argument& error)
    {
        std::fprintf(kOut, "%s: %s doesn't go to the %s. %s\n", name.c_str(), sound_name.c_str(), target.c_str(),
                     error.what());
        tallies.refusals[error.what()]++;
        return std::nullopt;
    }
    catch (const std::exception& error)
    {
        std::fprintf(kOut, "%s: %s fails to go to the %s. %s\n", name.c_str(), sound_name.c_str(), target.c_str(),
                     error.what());
        tallies.failures[error.what()]++;
        tallies.result = 1;
        return std::nullopt;
    }

    return sent;
}

// The MIDI export of a sound by the converter, `renders`, against the same render on the GBA or the DS, for as long.
// `before` and `after` are the sound's renders from the game's archive and on `target`. A conversion whose finding is
// worse than the export's gets a line.
void CompareMidiExport(const hokuto::measure::Renders& renders, const song::DryRender& before,
                       const song::DryRender& after, double seconds, const std::string& name,
                       const std::string& sound_name, const std::string& target, Tallies& tallies)
{
    const std::size_t size = std::min({renders.hokuto.size(), renders.exported.size(),
                                       2 * static_cast<std::size_t>(seconds * static_cast<double>(renders.rate))});
    const song::LevelComparison exported = song::CompareLevels(std::span(renders.hokuto).first(size),
                                                               std::span(renders.exported).first(size), renders.rate);
    const Worst midi_kind = WorstOf(exported);
    tallies.worst_midi[midi_kind]++;

    // The conversion's measure up to the same time.
    const auto same_time = static_cast<std::size_t>(static_cast<double>(size / 2) * before.rate / renders.rate);
    const std::size_t samples = 2 * std::min({same_time, before.samples.size() / 2, after.samples.size() / 2});
    const song::LevelComparison window = song::CompareLevels(std::span(before.samples).first(samples),
                                                             std::span(after.samples).first(samples), before.rate);
    const Worst window_kind = WorstOf(window);
    if (window_kind > midi_kind && window_kind >= Worst::kSpans)
    {
        const std::vector<song::LevelPlace> places = song::WorstPlaces(window, song::kSpanFindingDb, 1);
        std::fprintf(kOut,
                     "%s: %s has a worse finding on the %s than in ntdlib's MIDI export: %s of %.2f s at "
                     "%.2f s, %+.1f dB.\n",
                     name.c_str(), sound_name.c_str(), target.c_str(), places.front().run ? "a run" : "a span",
                     places.front().seconds, places.front().start, places.front().db);
        tallies.worse_than_midi++;
    }
}

// Checks `sent`, sound `sound_name` of `a` on `target`: the new file's round trip, the notes, the mutes, the timing
// and the levels of the renders. Counts in `tallies`. Throws when a step fails.
void CheckSentSound(const Options& o, const OpenedArchive& a, const std::string& name, const std::string& target,
                    uint32_t sound, const std::string& sound_name, const Sent& sent, Tallies& tallies)
{
    CountWarningKinds(sent.other.warnings, tallies.warned);
    if (!(format::Read(format::Write(sent.other.file)) == sent.other.file) ||
        !(format::ReadText(format::WriteText(sent.other.file)) == sent.other.file))
    {
        std::fprintf(kOut, "%s: %s's new file doesn't come back from both forms.\n", name.c_str(), sound_name.c_str());
        tallies.lost++;
        tallies.result = 1;
    }

    // The renders and their levels, at the higher of the two rates.
    const Played played_before = PlaySource(a, sound, o.seconds);
    const Played played_after =
        o.to_ds    ? PlayDs(sent.built_ds, 0, o.seconds, o.ds_mixing)
        : o.to_wii ? PlayWii(sent.built_wii, 0, o.seconds, format::BuildOfWiiArchive(*sent.other.file.native))
                   : Play(sent.built, 0, o.seconds);
    const int rate = std::max(played_before.render.rate, played_after.render.rate);
    const song::DryRender before = song::AtRate(played_before.render, rate);
    const song::DryRender after = song::AtRate(played_after.render, rate);
    const bool ds_driver = a.ds || a.dse;
    const bool agb = a.gba || a.driver;
    const bool draws = !agb && !ds_driver && SoundReaches(a.game.get(), a.file, a.sound_places.at(sound), sound, Draws);
    const std::optional<double> off = agb || ds_driver || o.to_ds
                                          ? GbaNotesOff(*played_before.sequence, *played_after.sequence, o.seconds, agb)
                                          : std::nullopt;
    const bool same_notes = agb || ds_driver ? off.has_value()
                                             : SameNotes(InOneBank(*played_before.sequence, sent.other.slot_programs),
                                                         *played_after.sequence);
    const char* mark = draws ? ", with random numbers" : same_notes ? "" : ", with other notes";
    tallies.random += draws ? 1 : 0;
    tallies.other_notes += !draws && !same_notes ? 1 : 0;
    if (off)
    {
        tallies.notes_off.push_back(*off);
        if (*off > 20.0)
        {
            std::fprintf(kOut, "%s: %s's notes start up to %.1f ms from the %s's times on the %s.\n", name.c_str(),
                         sound_name.c_str(), *off,
                         ds_driver ? "DS"
                         : agb     ? "GBA"
                                   : "source",
                         target.c_str());
        }
    }

    // The mutes and meters of the mixer reach the tracks of a conversion through the tracks of the source.
    if (agb || ds_driver)
    {
        CheckMutesReach(*played_before.sequence, *played_after.sequence, kOut, name, sound_name, target,
                        tallies.tracks_found, tallies.tracks_lost);
    }

    // The DS's tracks always run 48 ticks to a quarter note: a tempo makes up for the other timebase of the source. The
    // times of its notes count above instead.
    if (!agb && !ds_driver && !o.to_ds && !song::SameTiming(*played_before.sequence, *played_after.sequence))
    {
        std::fprintf(kOut, "%s: %s plays its ticks at other times on the %s%s.\n", name.c_str(), sound_name.c_str(),
                     target.c_str(), mark);
        tallies.other_timing++;
    }

    // Where each render falls below the floor of the windows that the measure counts.
    const auto [end_before, end_after] = AudibleEnds(before, after);
    if (std::abs(end_before - end_after) > 0.1)
    {
        std::fprintf(kOut, "%s: %s ends at %.2f s, and at %.2f s on the %s%s.\n", name.c_str(), sound_name.c_str(),
                     end_before, end_after, target.c_str(), mark);
        tallies.ends_elsewhere++;
    }
    const song::LevelComparison comparison = song::CompareLevels(before.samples, after.samples, before.rate);
    const Worst kind = WorstOf(comparison);
    tallies.worst[kind]++;
    tallies.worst_random[kind] += draws ? 1 : 0;
    tallies.worst_other_notes[kind] += !draws && !same_notes ? 1 : 0;
    if (kind != Worst::kSilent)
    {
        tallies.gains.push_back(comparison.gain_db);
    }
    if (kind != Worst::kSilent && std::abs(comparison.gain_db) > 3.0)
    {
        std::fprintf(kOut, "%s: %s plays at a gain of %+.2f dB on the %s%s.\n", name.c_str(), sound_name.c_str(),
                     comparison.gain_db, target.c_str(), mark);
        tallies.large_gains++;
    }
    if (kind >= Worst::kSpans)
    {
        const std::vector<song::LevelPlace> places = song::WorstPlaces(comparison, song::kSpanFindingDb, 1);
        const song::LevelPlace& place = places.front();
        std::fprintf(kOut, "%s: %s: %zu findings%s. The worst: %s of %.2f s at %.2f s, %+.1f dB. Gain %+.2f dB.\n",
                     name.c_str(), sound_name.c_str(), comparison.places.size(), mark, place.run ? "a run" : "a span",
                     place.seconds, place.start, place.db, comparison.gain_db);
    }
    else if (o.biquads && kind == Worst::kNone)
    {
        std::fprintf(kOut, "%s: %s: no finding%s. Gain %+.2f dB.\n", name.c_str(), sound_name.c_str(), mark,
                     comparison.gain_db);
    }
    else if (o.biquads)
    {
        std::fprintf(kOut, "%s: %s is silent on both consoles%s.\n", name.c_str(), sound_name.c_str(), mark);
    }

    // The MIDI export by the converter against the same render on the GBA or the DS, for as long.
    if (o.midi)
    {
        CompareMidiExport(RenderMidiExport(a, sound, played_before.sequence), before, after, o.seconds, name,
                          sound_name, target, tallies);
    }
}

// Prints the summary of the check of `chosen` sounds of `a`, `name`, on `target`. `began` is the check's start.
void PrintSummary(const Options& o, const OpenedArchive& a, const std::string& name, const std::string& target,
                  std::size_t chosen, std::chrono::steady_clock::time_point began, Tallies& tallies)
{
    std::ranges::sort(tallies.gains);
    const double taken = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    std::fprintf(
        kOut,
        "%s: %zu of %zu sequence sounds went to the %s. %zu were refused, and %zu failed. %zu are silent on both. "
        "Findings: none %zu, spans from 1 to 3 dB %zu, beyond 3 dB %zu, runs %zu. %zu end elsewhere. ",
        name.c_str(), tallies.converted, chosen, target.c_str(), Total(tallies.refusals), Total(tallies.failures),
        tallies.worst[Worst::kSilent], tallies.worst[Worst::kNone], tallies.worst[Worst::kSpans],
        tallies.worst[Worst::kBeyond], tallies.worst[Worst::kRun], tallies.ends_elsewhere);
    if (!tallies.gains.empty())
    {
        std::fprintf(kOut, "Gains from %+.2f to %+.2f dB, median %+.2f dB, %zu beyond 3 dB either way. ",
                     tallies.gains.front(), tallies.gains.back(), tallies.gains[tallies.gains.size() / 2],
                     tallies.large_gains);
    }
    if (a.gba || a.driver || a.ds || a.dse)
    {
        std::ranges::sort(tallies.notes_off);
        std::fprintf(kOut, "The notes of %zu %s start where the %s's do", tallies.notes_off.size(),
                     a.dse  ? "sounds"
                     : a.ds ? "sequences"
                            : "songs",
                     a.ds || a.dse ? "DS" : "GBA");
        if (!tallies.notes_off.empty())
        {
            std::fprintf(kOut, ", up to %.1f ms from them, median %.1f ms", tallies.notes_off.back(),
                         tallies.notes_off[tallies.notes_off.size() / 2]);
        }
        std::fprintf(kOut, ". %zu tracks with notes go with tracks of their conversions, and %zu with none. ",
                     tallies.tracks_found, tallies.tracks_lost);
    }
    else if (o.to_ds)
    {
        std::ranges::sort(tallies.notes_off);
        std::fprintf(kOut, "The notes of %zu sounds start where the source's do", tallies.notes_off.size());
        if (!tallies.notes_off.empty())
        {
            std::fprintf(kOut, ", up to %.1f ms from them, median %.1f ms", tallies.notes_off.back(),
                         tallies.notes_off[tallies.notes_off.size() / 2]);
        }
        std::fprintf(kOut, ". ");
    }
    else
    {
        std::fprintf(kOut, "%zu play their ticks at other times. ", tallies.other_timing);
    }
    std::fprintf(kOut, "%zu new files don't come back. The slowest conversion took %.2f s. %.1f s.\n", tallies.lost,
                 tallies.slowest, taken);
    if (o.midi)
    {
        std::fprintf(kOut,
                     "%s: ntdlib's MIDI exports: silent %zu. Findings: none %zu, spans from 1 to 3 dB %zu, beyond "
                     "3 dB %zu, runs %zu. %zu conversions have a worse kind of finding.\n",
                     name.c_str(), tallies.worst_midi[Worst::kSilent], tallies.worst_midi[Worst::kNone],
                     tallies.worst_midi[Worst::kSpans], tallies.worst_midi[Worst::kBeyond],
                     tallies.worst_midi[Worst::kRun], tallies.worse_than_midi);
    }
    std::fprintf(
        kOut,
        "%s: %zu sounds draw random numbers. Of them, findings: none %zu, spans from 1 to 3 dB %zu, beyond 3 dB %zu, "
        "runs %zu.\n",
        name.c_str(), tallies.random, tallies.worst_random[Worst::kNone], tallies.worst_random[Worst::kSpans],
        tallies.worst_random[Worst::kBeyond], tallies.worst_random[Worst::kRun]);
    std::fprintf(kOut,
                 "%s: %zu other sounds start other notes on the %s. Of them, findings: none %zu, spans from 1 to 3 dB "
                 "%zu, beyond 3 dB %zu, runs %zu.\n",
                 name.c_str(), tallies.other_notes, target.c_str(), tallies.worst_other_notes[Worst::kNone],
                 tallies.worst_other_notes[Worst::kSpans], tallies.worst_other_notes[Worst::kBeyond],
                 tallies.worst_other_notes[Worst::kRun]);
    for (std::size_t k = 0; k < tallies.warned.size(); k++)
    {
        if (tallies.warned[k] > 0)
        {
            std::fprintf(kOut, "%s: %zu sounds with %s.\n", name.c_str(), tallies.warned[k],
                         k < kWarningKinds.size() ? kWarningKinds[k].name : "other warnings");
        }
    }
    for (const auto& [reason, count] : tallies.refusals)
    {
        std::fprintf(kOut, "%s: %zu refused: %s\n", name.c_str(), count, reason.c_str());
    }
    for (const auto& [reason, count] : tallies.failures)
    {
        std::fprintf(kOut, "%s: %zu failed: %s\n", name.c_str(), count, reason.c_str());
    }
}

// The tool, with the command line's `arguments`. Returns the tool's exit status.
int Run(const std::vector<std::string>& arguments)
{
    // The options, then the archive and the numbers.
    const std::optional<Options> options = ParseOptions(arguments);
    if (!options)
    {
        return 2;
    }
    const std::string name = xsflib::Utf8(options->path.filename());
    const auto began = std::chrono::steady_clock::now();

    // The game's archive, and the file that its import makes. A Wii archive takes its game's build of NW4R SND.
    std::optional<OpenedArchive> opened;
    try
    {
        opened = OpenArchive(*options, name);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
        return 2;
    }
    const std::string target = options->to_ds ? "DS" : options->to_wii ? "Wii" : song::ConsoleName(opened->to);

    const std::optional<std::vector<uint32_t>> chosen = ChooseSounds(*options, *opened, name);
    if (!chosen)
    {
        return 2;
    }

    Tallies tallies;
    for (const uint32_t sound : *chosen)
    {
        const std::string sound_name = SoundNameOf(*opened, sound);
        const std::optional<Sent> sent = SendSound(*options, *opened, name, target, sound, sound_name, tallies);
        if (!sent)
        {
            continue;
        }
        tallies.converted++;

        // A sound whose round trip or renders throw fails, and the run continues with the next.
        try
        {
            CheckSentSound(*options, *opened, name, target, sound, sound_name, *sent, tallies);
        }
        catch (const std::exception& error)
        {
            std::fprintf(kOut, "%s: %s fails after it went to the %s. %s\n", name.c_str(), sound_name.c_str(),
                         target.c_str(), error.what());
            tallies.failures[error.what()]++;
            tallies.result = 1;
        }
    }

    // The summary.
    PrintSummary(*options, *opened, name, target, chosen->size(), began, tallies);

    return tallies.result;
}

} // namespace

} // namespace hokuto::tools::other_console

using hokuto::tools::other_console::Run;

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
