// SPDX-License-Identifier: MIT

// Checks Hokuto's playback of the MP2K songs of GBA ROMs against ntdlib's ArchivePlayer, run directly. For each song
// that plays (song::GbaSongs), it checks:
//
// - From the start, the player's output matches the output of the reference, sample for sample, after the player's
//   first frame. The player's frames last a 64th of a second in strict mixing, and the console's frames don't fall
//   evenly in them.
// - Output that ends early ends where the reference has fallen silent for good, or holds a level on each side: exactly
//   in strict mixing, and within ±8 in enhanced mixing. The levels other than 0 are counted apart. With Cycle off, the
//   output of a song with a loop ends at the end of the loop's first pass. The output has to reach the loop's end.
// - Seeks to the middle of the output and to the player's second frame give the reference's frames. A seek that
//   produces no output counts as a difference.
// - A switch halfway through the output, to a new model of the same song, produces the frames of the reference after
//   the frame that crossfades. A catch-up runs the new model up to the player's frame first. The program switches the
//   same way after an edit during playback.
// - Each note that the driver starts is in the arrangement, on its track and key, at a tick of the frame that starts
//   it. As in ntdlib's converter, only a note with a voice and a velocity counts, and only once its channel gets louder
//   than silence. The arrangement shows the first pass of a loop. The notes missing after it are counted apart. In
//   enhanced mixing, the driver grows its channels, and a note that the console never starts plays. The arrangement
//   lists such a note among the notes that the driver never started (song::Track::unstarted). Those are counted apart
//   too.
//
// With --voices, the tool plays a note of each voice that the browser lists instead (song::GbaVoiceGroups). A click in
// the browser plays the note the same way (song::GbaNotePreview). A voice without a key that plays something has to
// play nothing. Any other voice has to start, and to end before the preview's limit of 10 s. A preview that renders no
// samples doesn't start. The tool counts apart the voices whose note stays silent, and reports the longest time that a
// preview took to start.
//
// With --enhanced, the player and the reference play in enhanced mixing. With --part k/n, the tool checks part k of n
// of each ROM's songs, or of its voice groups. The tool prints a line for each song that differs, and a summary. It
// returns 1 when any song differs or can't be read. Missing notes get a line too, but don't count as a difference. A
// file that contains no song of MusicPlayer2000 gets a line, and doesn't count: a run can cover a whole folder. A ROM
// that fails to load for another reason can't be read.
//
//     check_gba_playback [--enhanced] [--voices] [--part k/n] <rom.gba>... [seconds]

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/mp2k/archive_player.h"
#include "xsflib/binary.h"
#include "xsflib/console.h"
#include "xsflib/files.h"
#include "xsflib/rip/length.h"

#include "check_part.h"
#include "check_playback_steps.h"
#include "check_support.h"
#include "song/gba.h"
#include "song/preview.h"
#include "song/sequence.h"
#include "song/sequence_model.h"

namespace
{

namespace song = hokuto::song;
using hokuto::tools::LoopText;
using hokuto::tools::Part;
using hokuto::tools::PartOf;
using hokuto::tools::playback_steps::ArrangedAt;
using hokuto::tools::playback_steps::CheckFrames;
using hokuto::tools::playback_steps::EndLevels;
using hokuto::tools::playback_steps::FailedSeeksText;
using hokuto::tools::playback_steps::kBlock;
using hokuto::tools::playback_steps::LevelsAfterEnd;
using hokuto::tools::playback_steps::PrintDifferences;
using hokuto::tools::playback_steps::SeekCheck;
using hokuto::tools::playback_steps::StartCheck;
using hokuto::tools::playback_steps::StepTotals;
using hokuto::tools::playback_steps::SwitchCheck;

// A note that the driver started: the console's frame that started it, its track and its key, and whether its channel
// got louder than silence.
struct StartedNote
{
    std::size_t frame = 0;
    int track = 0;
    int key = 0;
    bool audible = false;
};

// What ntdlib's player produces when it runs directly: the output in the player's units, the tick count after each of
// the console's frames, the samples that each console's frame ends at, and the notes.
struct Reference
{
    std::vector<float> samples; // at a full scale of 1
    std::vector<uint32_t> ticks;
    std::vector<std::size_t> ends;
    std::vector<StartedNote> notes;
};

struct Totals : StepTotals
{
    std::size_t voices = 0;        // the voices played, with --voices
    std::size_t keyless = 0;       // those without a key that plays something
    std::size_t silent_voices = 0; // those whose note stayed silent
    double slowest_start_ms = 0.0; // the longest time that a preview took to start
    std::size_t songs = 0;
    std::size_t looped = 0;
    std::size_t unread = 0;
    std::size_t failed = 0;
    std::size_t notes = 0;
    std::size_t missed_notes = 0;
    std::size_t missed_later = 0;
    std::size_t grown_notes = 0; // notes of enhanced mixing among those that the console never starts
    std::size_t levels = 0;      // songs whose reference holds a level other than 0 after the output's end
    float largest_level = 0.0f;  // the largest such level
};

Reference RunReference(const song::GbaRom& rom, int number, bool enhanced, std::size_t samples)
{
    using ntdlib::mp2k::Action;
    ntdlib::mp2k::ArchivePlayer player(rom.rom, rom.info);
    Reference reference;
    player.Start(number, ntdlib::mp2k::ArchivePlayerOptions{.enhanced = enhanced});
    std::map<int, std::size_t> playing; // the note that each channel plays, by the channel
    std::vector<int16_t> strict;
    while (reference.samples.size() < samples * 2)
    {
        if (enhanced)
        {
            player.RunFrame(reference.samples);
        }
        else
        {
            strict.clear();
            player.RunFrame(strict);
            for (const int16_t sample : strict)
            {
                reference.samples.push_back(static_cast<float>(sample) / 32768.0f);
            }
        }

        // The arrangement shows a note with a voice and a velocity whose channel gets louder than silence. ntdlib's
        // converter keeps the same notes.
        for (const Action& action : player.Actions())
        {
            if (action.kind == Action::kNoteOn && action.program >= 0 && action.velocity != 0)
            {
                playing[action.channel] = reference.notes.size();
                reference.notes.push_back({reference.ticks.size(), action.track, action.key});
            }
            else if (action.kind == Action::kNoteOn || action.kind == Action::kNoteEnd)
            {
                playing.erase(action.channel);
            }
            else if (action.kind == Action::kAudible && playing.contains(action.channel))
            {
                reference.notes[playing[action.channel]].audible = true;
            }
        }
        reference.ticks.push_back(static_cast<uint32_t>(player.Driver().Tick()));
        reference.ends.push_back(reference.samples.size());
    }

    return reference;
}

// Whether the arrangement contains a note on `track` and `key` that starts from `first` to `last`, round the loop.
bool Arranged(const song::Sequence& sequence, int track, int key, uint32_t first, uint32_t last,
              std::vector<song::Note> song::Track::* list = &song::Track::notes)
{
    for (uint32_t tick = first; tick <= last; tick++)
    {
        if (ArrangedAt(sequence, track, key, tick, list))
        {
            return true;
        }
    }

    return false;
}

// Counts the notes of `reference` up to the end of the output of `heard` frames that `sequence`, the arrangement,
// misses, and prints a line when it misses any. `loop` is the loop's text for the line.
void CheckNotes(const std::string& name, const std::string& loop, const Reference& reference,
                const song::Sequence& sequence, song::Mixing mixing, std::size_t heard, std::size_t frame_floats,
                Totals& totals)
{
    std::size_t checked = 0;
    std::size_t missed = 0;
    std::size_t missed_later = 0;
    const StartedNote* first_missed = nullptr;
    for (const StartedNote& note : reference.notes)
    {
        if (!note.audible || note.frame >= reference.ends.size() || reference.ends[note.frame] > heard * frame_floats)
        {
            continue;
        }

        checked++;
        const uint32_t first = note.frame > 0 ? reference.ticks[note.frame - 1] : 0;
        const uint32_t last = reference.ticks[note.frame];
        if (Arranged(sequence, note.track, note.key, first, last))
        {
            continue;
        }
        if (mixing == song::Mixing::kEnhanced &&
            Arranged(sequence, note.track, note.key, first, last, &song::Track::unstarted))
        {
            totals.grown_notes++;
        }
        else
        {
            first_missed = first_missed ? first_missed : &note;
            missed++;
            missed_later += sequence.loop && first >= sequence.loop->end ? 1 : 0;
        }
    }
    totals.notes += checked;
    totals.missed_notes += missed;
    totals.missed_later += missed_later;
    if (missed > 0)
    {
        std::printf(
            "%s (loop %s): %zu of %zu notes missing from the arrangement, %zu after the first pass. The first is "
            "on track %d, key %d. It starts in frame %zu.\n",
            name.c_str(), loop.c_str(), missed, checked, missed_later, first_missed->track, first_missed->key,
            first_missed->frame);
    }
}

void CheckSong(const char* path, const std::shared_ptr<const song::GbaRom>& rom, int number, int seconds, bool enhanced,
               Totals& totals)
{
    const std::string name = std::string(path) + ": " + song::GbaSongName(*rom, number);
    std::shared_ptr<const song::Sequence> sequence;
    try
    {
        sequence = std::make_shared<const song::Sequence>(song::ReadSequence(*rom, number));
    }
    catch (const std::exception& error)
    {
        std::printf("%s: can't be read. %s\n", name.c_str(), error.what());
        totals.unread++;
        return;
    }
    totals.songs++;
    totals.looped += sequence->loop ? 1 : 0;

    const song::Mixing mixing = enhanced ? song::Mixing::kEnhanced : song::Mixing::kStrict;
    const auto model = std::make_shared<song::SequenceModel>(rom, sequence, song::ModelOptions{.mixing = mixing});
    const std::size_t frame_samples = model->FrameSamples();
    const std::size_t frame_floats = frame_samples * 2;
    const std::size_t frames = CheckFrames(*model, seconds);
    Reference reference;
    try
    {
        reference = RunReference(*rom, number, enhanced, (frames + 2) * frame_samples);
    }
    catch (const std::exception& error)
    {
        std::printf("%s: the reference can't play it. %s\n", name.c_str(), error.what());
        totals.failed++;
        return;
    }

    // From the start. The output's last frame can fade out, and the comparison leaves it out.
    const auto [ended, heard, differences] = StartCheck(model, frames, reference.samples);

    // A loop's output has to reach the end of the loop's first pass. A model that fails or falls silent ends it sooner.
    const bool short_loop = ended && sequence->loop && model->After() < static_cast<double>(sequence->loop->end);

    // After an end of the output, the reference holds a level on each side. A PSG channel that stops can leave its
    // level. In strict mixing, the level holds exactly. In enhanced mixing, the reverb's tail rings on below the
    // console's steps, and each side stays within ±8 of a level (SequenceModel's end). A loop's output ends at the end
    // of the loop's pass, and the reference plays on round the loop.
    const std::size_t rest = std::min(heard * frame_floats, reference.samples.size());
    const std::span<const float> after = std::span<const float>(reference.samples)
                                             .subspan(rest, std::min(frames * frame_floats, reference.samples.size()) -
                                                                std::min(rest, frames * frame_floats));
    const float span = enhanced ? 2.0f * static_cast<float>(xsflib::rip::kSilenceLevel) / 32768.0f : 0.0f;
    EndLevels levels;
    if (ended && !sequence->loop && after.size() >= 2)
    {
        levels = LevelsAfterEnd(after, frame_floats, span);
    }
    const auto [sounds_on, level, widest, range, widest_at] = levels;
    if (!sounds_on && level > 0.0f)
    {
        totals.levels++;
        totals.largest_level = std::max(totals.largest_level, level);
    }

    // A seek to the middle of the output, then one back to the second frame.
    const auto [seeks, failed_seeks] = SeekCheck(*model, 0, heard, reference.samples, frame_floats, totals);

    // A switch halfway through the output.
    const std::string switch_text =
        SwitchCheck(rom, sequence, mixing, reference.samples, heard, frames, frame_floats, 0, totals);

    const std::string loop = LoopText(sequence->loop);
    if (differences + seeks + failed_seeks > 0 || sounds_on || short_loop || heard == 0 || !switch_text.empty())
    {
        std::string after_text = FailedSeeksText(failed_seeks);
        if (sounds_on)
        {
            after_text +=
                ", and the reference sounds after the output's end: " + std::to_string(std::lround(range * 32768.0f)) +
                " steps in all, and " + std::to_string(std::lround(widest * 32768.0f)) + " within frame " +
                std::to_string(widest_at) + " after the end";
        }
        if (short_loop)
        {
            after_text += ", and the output ends before the loop's end";
        }
        PrintDifferences(name, differences, heard, seeks, after_text, switch_text, loop);
        totals.failed++;
    }

    // The notes up to the output's end. A frame runs the ticks after the last frame's count up to its count.
    CheckNotes(name, loop, reference, *sequence, mixing, heard, frame_floats, totals);
}

// The frames of a note preview's output, and its largest sample.
struct Rendered
{
    std::size_t frames = 0;
    float peak = 0.0f;
};

// Renders `preview` in blocks of kBlock frames until a block comes back short or `limit` frames have rendered.
Rendered RenderPreview(song::GbaNotePreview& preview, std::size_t limit)
{
    std::vector<float> out(2 * kBlock);
    Rendered result;
    for (std::size_t rendered = kBlock; rendered == kBlock && result.frames < limit; result.frames += rendered)
    {
        rendered = preview.Render(out.data(), kBlock);
        for (std::size_t i = 0; i < 2 * rendered; i++)
        {
            result.peak = std::max(result.peak, std::abs(out[i]));
        }
    }

    return result;
}

// Plays a note of each program of the voice groups of `rom` that the browser lists, with part `part` of `parts` of the
// voice groups. A click in the browser plays the note the same way.
void CheckVoices(const char* path, const std::shared_ptr<const song::GbaRom>& rom, bool enhanced, std::size_t part,
                 std::size_t parts, Totals& totals)
{
    const std::vector<song::GbaVoiceGroup> groups = song::GbaVoiceGroups(*rom);
    for (std::size_t g = 0; g < groups.size(); g++)
    {
        const song::GbaVoiceGroup& group = groups[g];
        if (g % parts != part || group.songs.empty())
        {
            continue;
        }

        const std::string name = song::GbaVoiceGroupName(group.address);
        for (const int program : group.programs)
        {
            totals.voices++;
            const int key = song::GbaNotePreview::KeyOf(*rom, group.address, program);
            float peak = 0.0f;
            try
            {
                const auto start = std::chrono::steady_clock::now();
                song::GbaNotePreview preview(rom, group.songs.front(), program,
                                             enhanced ? song::Mixing::kEnhanced : song::Mixing::kStrict);
                totals.slowest_start_ms = std::max(
                    totals.slowest_start_ms,
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
                const std::size_t limit = static_cast<std::size_t>(preview.SampleRate()) * 11;
                const Rendered rendered = RenderPreview(preview, limit);
                const std::size_t frames = rendered.frames;
                peak = rendered.peak;
                const bool ended = frames < limit;
                const bool limited =
                    frames >=
                    static_cast<std::size_t>(preview.SampleRate() * song::NotePreview::kLongestMs / 1000) - 2 * kBlock;
                if (key < 0 ? frames > 0 : (frames == 0 || !ended || limited))
                {
                    const char* const problem = key < 0       ? "plays though no key plays anything"
                                                : frames == 0 ? "renders nothing though a key plays something"
                                                              : "plays to the preview's limit";
                    std::printf("%s: %s, program %d: %s\n", path, name.c_str(), program, problem);
                    totals.failed++;
                }
            }
            catch (const std::exception& error)
            {
                std::printf("%s: %s, program %d fails: %s\n", path, name.c_str(), program, error.what());
                totals.failed++;
                continue;
            }
            totals.keyless += key < 0 ? 1 : 0;
            totals.silent_voices += key >= 0 && peak == 0.0f ? 1 : 0;
        }
    }
}

// The options of the command line.
struct Options
{
    bool enhanced = false;
    bool voices = false;
    std::size_t part = 0;
    std::size_t parts = 1;
    std::vector<std::string> paths;
    int seconds = 30;
};

// The options of the command line's `arguments`, or nothing after a line about a wrong one.
std::optional<Options> ParseArguments(const std::vector<std::string>& arguments)
{
    Options options;
    for (std::size_t a = 1; a < arguments.size(); a++)
    {
        if (arguments[a] == "--enhanced")
        {
            options.enhanced = true;
        }
        else if (arguments[a] == "--voices")
        {
            options.voices = true;
        }
        else if (arguments[a] == "--part" && a + 1 < arguments.size())
        {
            const std::optional<Part> given = PartOf(arguments[++a]);
            if (!given)
            {
                std::fprintf(stderr, "--part takes k/n, with k from 1 to n.\n");
                return std::nullopt;
            }
            options.part = given->k - 1;
            options.parts = given->n;
        }
        else if (a + 1 == arguments.size() && !arguments[a].empty() &&
                 arguments[a].find_first_not_of("0123456789") == std::string::npos)
        {
            options.seconds = std::atoi(arguments[a].c_str());
        }
        else if (arguments[a].starts_with("--"))
        {
            std::fprintf(stderr, "Hokuto's GBA playback check has no option %s.\n", arguments[a].c_str());
            return std::nullopt;
        }
        else
        {
            options.paths.push_back(arguments[a]);
        }
    }
    if (options.paths.empty() || options.seconds <= 0)
    {
        std::fprintf(stderr, "usage: check_gba_playback [--enhanced] [--voices] [--part k/n] <rom.gba>... [seconds]\n");
        return std::nullopt;
    }

    return options;
}

// Checks the ROM at `path`: its songs, or with --voices the programs of its voice groups. Returns whether the ROM could
// be read and loaded.
bool CheckRom(const std::string& path, const Options& options, Totals& totals)
{
    std::vector<uint8_t> bytes;
    std::shared_ptr<const song::GbaRom> rom;
    try
    {
        if (!xsflib::ReadFile(xsflib::PathFromUtf8(path), bytes))
        {
            std::printf("%s: can't be read\n", path.c_str());
            totals.unread++;
            return false;
        }
        rom = song::LoadGbaRom(std::move(bytes));
    }
    catch (const xsflib::FormatError& error)
    {
        std::printf("%s: %s\n", path.c_str(), error.what());
        return false;
    }
    catch (const std::exception& error)
    {
        std::printf("%s: %s\n", path.c_str(), error.what());
        totals.unread++;
        return false;
    }

    if (options.voices)
    {
        CheckVoices(path.c_str(), rom, options.enhanced, options.part, options.parts, totals);
        return true;
    }
    for (const int number : song::GbaSongs(*rom))
    {
        if (static_cast<std::size_t>(number) % options.parts == options.part)
        {
            CheckSong(path.c_str(), rom, number, options.seconds, options.enhanced, totals);
        }
    }

    return true;
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> arguments = xsflib::console::Arguments(argc, argv);
    const std::optional<Options> parsed = ParseArguments(arguments);
    if (!parsed)
    {
        return 2;
    }
    const Options& options = *parsed;
    const bool voices = options.voices;

    Totals totals;
    std::size_t roms = 0;
    for (const std::string& path : options.paths)
    {
        roms += CheckRom(path, options, totals) ? 1 : 0;
    }

    if (voices)
    {
        std::printf(
            "%zu ROMs, %zu voices, %zu without a key that plays something, %zu silent, %zu fail. The slowest "
            "preview took %.1f ms to start.\n",
            roms, totals.voices, totals.keyless, totals.silent_voices, totals.failed, totals.slowest_start_ms);
        return totals.failed + totals.unread > 0 ? 1 : 0;
    }
    std::printf(
        "%zu ROMs, %zu songs, %zu with a loop, %zu can't be read, %zu differ. %zu seeks. %zu of %zu switches exact. "
        "%zu of %zu notes missing from the arrangement, %zu after the first pass. %zu notes that the console never "
        "starts play on grown channels. After the output's end, the reference holds a level other than 0 in %zu songs, "
        "%d steps at most.\n",
        roms, totals.songs, totals.looped, totals.unread, totals.failed, totals.seeks, totals.exact, totals.switches,
        totals.missed_notes, totals.notes, totals.missed_later, totals.grown_notes, totals.levels,
        static_cast<int>(std::lround(totals.largest_level * 32768.0f)));

    return totals.failed + totals.unread > 0 ? 1 : 0;
}
