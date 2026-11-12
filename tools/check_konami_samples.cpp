// SPDX-License-Identifier: MIT

// Checks the edits of the samples of Konami's GBA driver (edit/konami_samples.h) on a ROM. The samples are those that
// the ROM's songs play (song::KonamiSamples).
//
// - The values: each sample gets a rate of three quarters of its rate, within the rates that the table contains, and a
//   loop from the middle of the sample where it can loop. Without a loop, it keeps none. The exported ROM has to differ
//   from the ROM only in the step and the loop start of the sample's header, or in the word of a Dungeon Dice Monsters
//   entry. ntdlib has to read the new step or period and the new loop. The tool works the step out from the mixer's
//   rate, and the period from the CPU's clock.
// - The reach of each value edit: the Bank Editor lists the songs that play the sample (KonamiRom::SamplesOfSound), and
//   the other songs whose notes of other samples take its rate (song::KonamiBorrowedRates). Like KonamiBorrowedRates,
//   the round then runs each sound on ntdlib's model of the output stage (konami::Output, without the mixer) for two
//   passes of its loop, from the ROM and from the edited ROM. A sound whose playing voices differ in a frame has to be
//   listed. A voice that doesn't play uses the values of the start of its next note. A listed sound whose voices don't
//   differ counts apart: a note command that two passes don't reach plays nothing there. The round leaves out the
//   revision without a mixer. Its voices keep no rate: the period of a note is written to the timer of the FIFO. Its
//   output stage reads only the entry of the sample of the voice's note.
// - The replacements: each sample is replaced with its WAV file (song::KonamiSampleWav) in its place, one after another
//   in one file, the packed samples first and then the shorter ones. A sample that finds no room in the ROM's 32 MiB
//   stays as it is, and its refusal is counted. The new ROM's entries have to name new places in the free space at the
//   ROM's end. ntdlib has to read the same PCM, the same loop, and the same step or period from them. A WAV file stores
//   a whole rate, and a step or a period can then move. Such samples count apart. So do packed samples: the new sample
//   isn't packed, and the mixer plays it on another path. The levels of that path can round otherwise. The file has to
//   come back byte for byte from the binary and the text forms. Then each sound plays for `--seconds` on ntdlib's
//   ArchivePlayer, from the ROM and from the new ROM, in strict mixing. The two have to give the same samples, except
//   for the sounds that play a sample counted apart.
//
// usage: hokuto_check_konami_samples <rom.gba> [--seconds n]

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/konami/archive_player.h"
#include "ntdlib/konami/driver.h"
#include "ntdlib/konami/output.h"
#include "ntdlib/konami/sequencer.h"
#include "xsflib/console.h"
#include "xsflib/files.h"

#include "edit/konami_samples.h"
#include "format/archive.h"
#include "format/file.h"
#include "format/text.h"
#include "song/konami.h"

namespace
{

namespace edit = hokuto::edit;
namespace format = hokuto::format;
namespace konami = ntdlib::konami;
namespace song = hokuto::song;

bool Dungeon(const song::KonamiRom& rom)
{
    return rom.info.revision == konami::Revision::kDungeonDiceMonsters;
}

// The step or the period that matches `rate`. The edits work it out the same way.
uint32_t StepOf(const song::KonamiRom& rom, uint32_t rate)
{
    if (Dungeon(rom))
    {
        return static_cast<uint32_t>(std::lround(static_cast<double>(ntdlib::agb::kSecondCycles) / rate));
    }

    return static_cast<uint32_t>(std::lround(rate * 4096.0 / rom.info.mix_rate));
}

uint32_t HeldStep(const konami::SampleInfo& info)
{
    return info.period != 0 ? info.period : info.step;
}

// The places of the bytes in which `a` and `b` differ, up to the shorter's end, and from there on as one place.
std::vector<std::size_t> Differences(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
    std::vector<std::size_t> places;
    const std::size_t shorter = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < shorter; i++)
    {
        if (a[i] != b[i])
        {
            places.push_back(i);
        }
    }
    if (a.size() != b.size())
    {
        places.push_back(shorter);
    }

    return places;
}

struct Counts
{
    std::size_t edits = 0;
    std::size_t looped = 0;
    std::size_t refused = 0;
    std::size_t failed = 0;
    std::size_t replaced = 0;
    std::size_t moved_steps = 0;
    std::size_t packed = 0;
    std::size_t sounds = 0;
    std::size_t sounds_apart = 0;
    std::size_t sounds_differ = 0;
    std::size_t no_room = 0;
    std::size_t reach_sounds = 0;   // the sounds that ran for each edit, summed over the edits
    std::size_t reach_differ = 0;   // those whose voices differ
    std::size_t reach_borrowed = 0; // those that only the borrowed rates list
    std::size_t reach_unlisted = 0; // those that differ and aren't listed: failures
    std::size_t reach_quiet = 0;    // those that are listed and don't differ
};

// What the Bank Editor lists for each sample: the sounds that play it, and those whose notes of other samples take its
// rate. The voices of each sound on the ROM, frame by frame (VoiceFrames).
struct Reach
{
    std::map<int, std::set<uint32_t>> playing;
    std::map<int, std::set<uint32_t>> borrowing;
    std::map<uint32_t, std::vector<uint64_t>> frames;
};

// The voices of `sound` on ntdlib's model of the output stage, without the mixer, as a hash for each frame. The frames
// run as song::KonamiBorrowedRates runs them: up to two passes of the song's loop, its stop, or the end of its tracks.
std::vector<uint64_t> VoiceFrames(const song::KonamiRom& rom, uint32_t sound)
{
    konami::Output output(rom.rom, rom.info);
    output.Play(song::KonamiSongOf(rom, sound), song::KonamiRequestOf(rom, sound));
    std::vector<uint64_t> frames;
    if (!output.Song()->Valid())
    {
        return frames;
    }

    for (uint32_t frame = 0; frame < konami::kMaxFrames; frame++)
    {
        const bool staged = output.Frame();
        const konami::Sequencer& sequencer = *output.Song();
        if (!staged || sequencer.LoopsDone() >= 2 || sequencer.Stopped() || !sequencer.AnyTrackActive())
        {
            break;
        }

        // FNV-1a over the fields that the output stage sets.
        uint64_t hash = 0xCBF29CE484222325;
        const auto add = [&hash](uint64_t value)
        {
            hash = (hash ^ value) * 0x100000001B3;
        };
        for (const konami::OutputVoice& voice : output.Voices())
        {
            // A voice that doesn't play uses the values of the start of its next note.
            if (!(voice.flags & konami::kVoiceOn))
            {
                add(0);
                continue;
            }

            add(voice.position);
            add(static_cast<uint32_t>(voice.count));
            add(voice.step);
            add(voice.fraction);
            add(voice.key);
            add(voice.flags);
            add(voice.volume);
            add(voice.start);
            add(voice.period);
        }
        frames.push_back(hash);
    }

    return frames;
}

Reach ReachOf(const song::KonamiRom& rom)
{
    Reach reach;
    for (const uint32_t sound : song::KonamiSounds(rom))
    {
        for (const int sample : rom.SamplesOfSound(sound))
        {
            reach.playing[sample].insert(sound);
        }
        for (const int sample : song::KonamiBorrowedRates(rom, sound))
        {
            reach.borrowing[sample].insert(sound);
        }
        reach.frames[sound] = VoiceFrames(rom, sound);
    }

    return reach;
}

// The reach of an edit of sample `sample` whose ROM is `built`. Returns the count of failures.
std::size_t CheckReach(const std::string& name, const song::KonamiRom& rom, const std::vector<uint8_t>& built,
                       int sample, const Reach& reach, Counts& counts)
{
    const std::shared_ptr<const song::KonamiRom> edited = song::LoadKonamiRom(built);
    const std::set<uint32_t>& playing =
        reach.playing.contains(sample) ? reach.playing.at(sample) : std::set<uint32_t>();
    const std::set<uint32_t>& borrowing =
        reach.borrowing.contains(sample) ? reach.borrowing.at(sample) : std::set<uint32_t>();
    std::size_t failures = 0;
    for (const auto& [sound, frames] : reach.frames)
    {
        counts.reach_sounds++;
        const bool listed = playing.contains(sound) || borrowing.contains(sound);
        const bool differ = VoiceFrames(*edited, sound) != frames;
        counts.reach_differ += differ ? 1 : 0;
        counts.reach_borrowed += borrowing.contains(sound) && !playing.contains(sound) ? 1 : 0;
        if (listed && !differ)
        {
            counts.reach_quiet++;
        }
        if (differ && !listed)
        {
            counts.reach_unlisted++;
            failures++;
            std::printf("%s: %s FAILED: an edit of %s changes its voices, and the editor doesn't list it\n",
                        name.c_str(), song::KonamiSoundName(rom, sound).c_str(),
                        song::KonamiSampleName(sample).c_str());
        }
    }

    return failures;
}

// The values round: one edit of each sample, each from the ROM's file.
void CheckValues(const std::string& name, const format::File& file, const song::KonamiRom& rom,
                 const std::vector<uint8_t>& bytes, const std::vector<int>& samples, Counts& counts)
{
    const edit::KonamiRates rates = edit::KonamiSampleRates(rom);
    const Reach reach = Dungeon(rom) ? Reach() : ReachOf(rom);
    for (const int sample : samples)
    {
        const konami::SampleInfo before = rom.info.Sample(rom.rom, sample);
        edit::KonamiSampleValues values;
        values.rate = std::clamp(static_cast<uint32_t>(std::lround(before.Rate(rom.info.mix_rate) * 0.75)), rates.least,
                                 rates.most);
        if (edit::KonamiSampleLoops(rom, sample) && before.length > 1)
        {
            values.loop_start = static_cast<uint32_t>(before.length / 2);
        }

        std::vector<uint8_t> built;
        try
        {
            built = format::ExportArchive(edit::WithKonamiSampleValues(file, rom, sample, values));
        }
        catch (const std::invalid_argument& error)
        {
            counts.refused++;
            std::printf("%s: %s refused: %s\n", name.c_str(), song::KonamiSampleName(sample).c_str(), error.what());
            continue;
        }
        counts.edits++;
        counts.looped += values.loop_start ? 1 : 0;

        // The bytes that may change: the step and the loop start of the header, or the word of the entry.
        std::set<std::size_t> allowed;
        const uint32_t entry = sample >= 0x8000 ? rom.info.live_sample_table + 4 * static_cast<uint32_t>(sample & 0xFFF)
                               : Dungeon(rom)   ? rom.info.sample_table + 8 * static_cast<uint32_t>(sample)
                                                : rom.info.sample_table + 4 * static_cast<uint32_t>(sample);
        const uint32_t header = Dungeon(rom) ? entry + 4 : rom.rom.U32(entry);
        for (uint32_t i = 0; i < 4; i++)
        {
            allowed.insert(header - ntdlib::agb::kRomBase + i);
            if (!Dungeon(rom))
            {
                allowed.insert(header + 8 - ntdlib::agb::kRomBase + i);
            }
        }
        std::string problem;
        for (const std::size_t place : Differences(bytes, built))
        {
            if (!allowed.contains(place))
            {
                problem = "byte " + std::to_string(place) + " changed";
                break;
            }
        }

        ntdlib::agb::Rom read;
        read.Assign(built);
        const konami::SampleInfo after = rom.info.Sample(read, sample);
        const uint32_t step = StepOf(rom, values.rate);
        if (problem.empty() && !after.valid)
        {
            problem = "ntdlib reads an invalid entry";
        }
        if (problem.empty() && HeldStep(after) != step)
        {
            problem = "ntdlib reads a step of " + std::to_string(HeldStep(after)) + ", not " + std::to_string(step);
        }
        if (problem.empty() && (after.length != before.length || after.data != before.data))
        {
            problem = "the sample's length or data moved";
        }
        if (problem.empty() && after.Looped() != values.loop_start.has_value())
        {
            problem = "the loop didn't change";
        }
        if (problem.empty() && values.loop_start && static_cast<uint32_t>(after.loop_start) != *values.loop_start)
        {
            problem = "the loop starts at " + std::to_string(after.loop_start);
        }
        if (!problem.empty())
        {
            counts.failed++;
            std::printf("%s: %s FAILED: %s\n", name.c_str(), song::KonamiSampleName(sample).c_str(), problem.c_str());
            continue;
        }

        if (!Dungeon(rom))
        {
            counts.failed += CheckReach(name, rom, built, sample, reach, counts);
        }
    }
}

// The largest difference between a sample of the output of `sound` of both ROMs, for `frames` of the driver's frames.
// 0 for the same output. With `stop`, the comparison stops at the first frame that differs.
int MostDifference(const song::KonamiRom& rom, const song::KonamiRom& replaced, uint32_t sound, int frames, bool stop)
{
    konami::ArchivePlayer first(rom.rom, rom.info);
    konami::ArchivePlayer second(replaced.rom, replaced.info);
    const konami::ArchivePlayerOptions options{.live = song::KonamiSoundIsLive(rom, sound)};
    first.Start(song::KonamiSongOf(rom, sound), options);
    second.Start(song::KonamiSongOf(replaced, sound), options);
    std::vector<int16_t> a;
    std::vector<int16_t> b;
    int most = 0;
    for (int frame = 0; frame < frames; frame++)
    {
        a.clear();
        b.clear();
        first.RunFrame(a);
        second.RunFrame(b);
        for (std::size_t i = 0; i < std::min(a.size(), b.size()); i++)
        {
            most = std::max(most, std::abs(int{a[i]} - int{b[i]}));
        }
        if (a.size() != b.size())
        {
            most = std::max(most, 65536);
        }
        if (stop && most != 0)
        {
            break;
        }
    }

    return most;
}

// Plays `sound` of both ROMs for `frames` of the driver's frames, and returns whether their output matched.
bool SameOutput(const song::KonamiRom& rom, const song::KonamiRom& replaced, uint32_t sound, int frames)
{
    return MostDifference(rom, replaced, sound, frames, true) == 0;
}

// The replacements round: every sample in one file.
void CheckReplacements(const std::string& name, const format::File& file, const song::KonamiRom& rom,
                       const std::vector<int>& samples, double seconds, Counts& counts)
{
    // Packed samples go first, then the shorter ones: a ROM with little room then fits the most of them.
    std::vector<int> order = samples;
    std::ranges::stable_sort(order,
                             [&rom](int a, int b)
                             {
                                 const konami::SampleInfo first = rom.info.Sample(rom.rom, a);
                                 const konami::SampleInfo second = rom.info.Sample(rom.rom, b);
                                 if (first.packed != second.packed)
                                 {
                                     return first.packed;
                                 }
                                 return first.length < second.length;
                             });
    format::File replaced = file;
    std::set<int> moved;             // the samples counted apart
    std::map<int, uint32_t> stepped; // the samples whose step or period moved, and the step of their WAV file
    std::vector<int> done;
    for (const int sample : order)
    {
        const edit::IncomingKonamiSample incoming = edit::ReadIncomingKonamiSample(song::KonamiSampleWav(rom, sample));
        try
        {
            // The entries and the headers of the other samples stay as the ROM stores them.
            replaced = edit::WithKonamiSample(replaced, rom, sample, incoming);
        }
        catch (const std::invalid_argument& error)
        {
            counts.no_room++;
            if (counts.no_room == 1)
            {
                std::printf("%s: %s refused: %s\n", name.c_str(), song::KonamiSampleName(sample).c_str(), error.what());
            }
            continue;
        }
        done.push_back(sample);
        counts.replaced++;
        const konami::SampleInfo info = rom.info.Sample(rom.rom, sample);
        counts.packed += info.packed ? 1 : 0;
        if (StepOf(rom, incoming.rate) != HeldStep(info))
        {
            moved.insert(sample);
            stepped.emplace(sample, StepOf(rom, incoming.rate));
        }
        if (info.packed)
        {
            moved.insert(sample);
        }
    }

    counts.moved_steps += stepped.size();
    const std::vector<uint8_t> built = format::ExportArchive(replaced);
    const std::shared_ptr<const song::KonamiRom> new_rom = song::LoadKonamiRom(built);
    const uint32_t free_start = file.native->driver->free_start + ntdlib::agb::kRomBase;
    for (const int sample : done)
    {
        const konami::SampleInfo before = rom.info.Sample(rom.rom, sample);
        const konami::SampleInfo after = new_rom->info.Sample(new_rom->rom, sample);
        std::string problem;
        if (!after.valid || after.data < free_start)
        {
            problem = "the entry names no new place";
        }
        else if (konami::SamplePcm(new_rom->rom, after) != konami::SamplePcm(rom.rom, before))
        {
            problem = "the PCM differs";
        }
        else if (after.Looped() != before.Looped() || (before.Looped() && after.loop_start != before.loop_start))
        {
            problem = "the loop differs";
        }
        else if (!stepped.contains(sample) && HeldStep(after) != HeldStep(before))
        {
            problem = "the step differs";
        }
        else if (stepped.contains(sample) && HeldStep(after) != stepped.at(sample))
        {
            problem = "the step isn't the WAV file's";
        }
        if (!problem.empty())
        {
            counts.failed++;
            std::printf("%s: replaced %s FAILED: %s\n", name.c_str(), song::KonamiSampleName(sample).c_str(),
                        problem.c_str());
        }
    }

    if (format::ExportArchive(format::Read(format::Write(replaced))) != built ||
        format::ExportArchive(format::ReadText(format::WriteText(replaced))) != built)
    {
        counts.failed++;
        std::printf("%s: the replaced file FAILED to come back from the binary or the text form\n", name.c_str());
    }

    const int frames = static_cast<int>(std::lround(seconds * ntdlib::agb::kFrameRate));
    for (const uint32_t sound : song::KonamiSounds(rom))
    {
        counts.sounds++;
        const std::vector<int> played = rom.SamplesOfSound(sound);
        const bool apart = std::ranges::any_of(played, [&moved](int sample) { return moved.contains(sample); });
        if (apart)
        {
            counts.sounds_apart++;
            std::printf("%s: %s counted apart: the output differs by %d at most\n", name.c_str(),
                        song::KonamiSoundName(rom, sound).c_str(), MostDifference(rom, *new_rom, sound, frames, false));
            continue;
        }
        if (!SameOutput(rom, *new_rom, sound, frames))
        {
            // The samples whose replacement alone changes the output.
            counts.sounds_differ++;
            std::string alone;
            for (const int sample : played)
            {
                if (std::ranges::find(done, sample) == done.end())
                {
                    continue;
                }
                const format::File one = edit::WithKonamiSample(
                    file, rom, sample, edit::ReadIncomingKonamiSample(song::KonamiSampleWav(rom, sample)));
                const std::shared_ptr<const song::KonamiRom> one_rom = song::LoadKonamiRom(format::ExportArchive(one));
                if (!SameOutput(rom, *one_rom, sound, frames))
                {
                    alone += " " + song::KonamiSampleName(sample);
                }
            }
            std::printf("%s: %s FAILED: the output differs after the replacements. Alone:%s\n", name.c_str(),
                        song::KonamiSoundName(rom, sound).c_str(), alone.empty() ? " none" : alone.c_str());
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    const std::vector<std::string> arguments = xsflib::console::Arguments(argc, argv);
    if (arguments.size() != 2 && !(arguments.size() == 4 && arguments[2] == "--seconds"))
    {
        std::fprintf(stderr, "usage: hokuto_check_konami_samples <rom.gba> [--seconds n]\n");
        return 2;
    }
    const double seconds = arguments.size() == 4 ? std::atof(arguments[3].c_str()) : 30.0;

    const std::filesystem::path path = xsflib::PathFromUtf8(arguments[1]);
    const std::string name = xsflib::Utf8(path.filename());
    std::vector<uint8_t> bytes;
    if (!xsflib::ReadFile(path, bytes))
    {
        std::fprintf(stderr, "%s: can't read the file\n", name.c_str());
        return 1;
    }

    try
    {
        const format::File file = format::ImportKonamiRom(bytes);
        const std::shared_ptr<const song::KonamiRom> rom = song::LoadKonamiRom(bytes);
        const std::vector<int> samples = song::KonamiSamples(*rom);
        Counts counts;
        CheckValues(name, file, *rom, bytes, samples, counts);
        CheckReplacements(name, file, *rom, samples, seconds, counts);

        std::printf("%s: %zu samples. %zu value edits, %zu with a loop, %zu refused.\n", name.c_str(), samples.size(),
                    counts.edits, counts.looped, counts.refused);
        std::printf(
            "%s: reach: %zu sounds ran, %zu differ, %zu listed by borrowed rates alone, %zu listed without a "
            "difference, %zu unlisted.\n",
            name.c_str(), counts.reach_sounds, counts.reach_differ, counts.reach_borrowed, counts.reach_quiet,
            counts.reach_unlisted);
        std::printf(
            "%s: %zu replacements, %zu of them packed, %zu with a step that moved, %zu refused for room. %zu sounds "
            "played for %.0f s: %zu counted apart, %zu differ.\n",
            name.c_str(), counts.replaced, counts.packed, counts.moved_steps, counts.no_room, counts.sounds, seconds,
            counts.sounds_apart, counts.sounds_differ);
        std::printf("%s: %zu failed.\n", name.c_str(), counts.failed + counts.sounds_differ);

        return counts.failed + counts.sounds_differ == 0 ? 0 : 1;
    }
    catch (const std::exception& error)
    {
        std::printf("%s: FAILED: %s\n", name.c_str(), error.what());
        return 1;
    }
}
