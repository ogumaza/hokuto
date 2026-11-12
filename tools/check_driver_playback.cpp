// SPDX-License-Identifier: MIT

// Checks Hokuto's playback of the songs of GBA ROMs with one of the GBA's other drivers, such as Konami's, against the
// driver's ArchivePlayer in ntdlib, run directly. For each sound of the ROM (song::DriverRom::Sounds), it checks:
//
// - From the start, the player's output matches the output of the reference, sample for sample, after the player's
//   first frame. The player's frames last a 64th of a second in strict mixing, and the console's frames don't fall
//   evenly in them. The player starts with the model's frame that plays the first tick (SequenceModel::Seek). Krawall's
//   first tick can come a frame or more after the model's first frame. The player's frames then match the reference's
//   from that frame on, and the reference's frames before it have to be silent. The other checks compare the same
//   frames.
// - Output that ends early ends where the reference has fallen silent for good, or holds a level on each side: exactly
//   in strict mixing, and within ±8 in enhanced mixing. The levels other than 0 are counted apart. With Cycle off, the
//   output of a song with a loop ends at the end of the loop's first pass. The output has to reach the loop's end.
// - Seeks to the middle of the output and to the player's second frame give the reference's frames. A seek that
//   produces no output counts as a difference.
// - A switch halfway through the output, to a new model of the same song, produces the frames of the reference after
//   the frame that crossfades. A catch-up runs the new model up to the player's frame first. The program switches the
//   same way after an edit during playback.
// - With a track muted, the player's output is the reference's with that track muted, and the track's meter stays at
//   0. The output with the mute ends where the output without it ends. It can end sooner only where the reference with
//   the mute holds a level on each side, by the rule of the second check. The other tracks' meters give the reference's
//   peaks, within two of the console's frames at the end of the run. On Brownie Brown's driver, they give the peaks of
//   the reference with the track muted.
//
// The tool also counts the note commands that the tracks run up to the end of the loop's first pass, and those without
// a note of the arrangement at their tick, on their track. On Konami's driver, the note can be on the next track: the
// driver can lose a note to the track before it. Such a command prints no line and doesn't count as a difference. A
// note of volume 0 or a sample that plays nothing makes no sound. Those commands are counted apart. On Nintendo R&D2's
// driver, a version of a song can mute tracks as the game's code does. Their note commands are counted apart too. On
// Brownie Brown's driver, a note waits for its length after it starts. A note can also sound in a frame after its
// command, and the arrangement shows it there. Such a command counts apart when a later note of its track names it. A
// track whose meter stays at 0 in the reference makes no sound, such as a wave channel that NR32 mutes. Its commands
// without a note count apart too. On Ubisoft Milan's driver, every track contains the one track of commands of the
// piece, and a note command counts on the track of its channel. On Rare's driver, the game silences some channels of
// the arrangement under water. Their note commands are counted apart as well. On Quintet's driver, a tie (D4) joins the
// note after it to the note before it when both have the same pitch: the arrangement shows one note. Such a note
// command is counted apart. The performer of the driver starts a note at the frame that plays the command's tick, and a
// note within a frame of the tick counts apart too. So does a note after the channel's volume commands have stepped its
// volume down to 0. It makes no sound, and the converter leaves it out. On Krawall, every track contains the rows of
// the module. A note command counts on the track of its channel. Its note is the track's note that names the command in
// the command's row: a note delay starts the note after the row's tick. ntdlib's Player of Krawall plays the module
// beside the conversion. The tool records whether the entry of each channel starts a voice in each pass of each row,
// and the levels of the voice. A note command without a note counts apart when its row starts no voice on its channel,
// for one of three causes. A note with a slide to it (effects 19 and 24) sets only the slide's goal. The player skips a
// note on a channel without a sample. A note delay at or past the row's speed never starts the note. A command whose
// voice starts and stays at level 0 makes no sound. It counts apart too. The converter leaves out such a note where
// more than 15 channels play notes. A command whose voice starts and reaches a level is a difference. The arrangement
// leaves out a note that plays, and the tool prints a line. Any other command without a note counts with the commands
// that have no note.
//
// With --enhanced, the player and the reference play in enhanced mixing. With --part k/n, the tool checks part k of n
// of each ROM's sounds. The tool prints a line for each sound that differs, and a summary. It returns 1 when any sound
// differs or can't be read. A stretch before the loop's end whose offsets the decoded commands don't hold gets a line
// and makes the run fail too. A file that contains none of the GBA's other drivers, or no song of its driver, gets a
// line, and doesn't count: a run can cover a whole folder. A ROM that fails to load for another reason can't be read.
//
//     check_driver_playback [--enhanced] [--part k/n] <rom.gba>... [seconds]

#include "check_driver_playback.h"

#include "check_part.h"
#include "check_support.h"

namespace hokuto::tools::driver_playback
{

namespace
{

using playback_steps::CheckFrames;
using playback_steps::Differences;
using playback_steps::EndLevels;
using playback_steps::FailedSeeksText;
using playback_steps::LevelsAfterEnd;
using playback_steps::PrintDifferences;
using playback_steps::Render;
using playback_steps::SeekCheck;
using playback_steps::StartCheck;
using playback_steps::StartResult;
using playback_steps::SwitchCheck;

// The model's frame that the player's output starts with: the frame that plays the first tick (SequenceModel::Seek).
// A sequence that the model can't play starts at frame 0.
std::size_t StartFrame(const std::shared_ptr<const song::DriverRom>& rom,
                       const std::shared_ptr<const song::Sequence>& sequence, song::Mixing mixing)
{
    song::SequenceModel model(rom, sequence, song::ModelOptions{.mixing = mixing});
    return model.Seek(0) && model.Frames() > 0 ? static_cast<std::size_t>(model.Frames() - 1) : 0;
}

// The reference's frames before frame `start` that hold a sample other than 0.
std::size_t SoundingFrames(const Reference& reference, std::size_t start, std::size_t frame_floats)
{
    std::size_t sounding = 0;
    for (std::size_t frame = 0; frame < start && (frame + 1) * frame_floats <= reference.samples.size(); frame++)
    {
        const auto first = reference.samples.begin() + static_cast<std::ptrdiff_t>(frame * frame_floats);
        if (std::any_of(first, first + static_cast<std::ptrdiff_t>(frame_floats),
                        [](float sample) { return sample != 0.0f; }))
        {
            sounding++;
        }
    }

    return sounding;
}

// Whether each side of `samples`, in interleaved stereo, stays within `span` of a level.
bool HoldsLevel(std::span<const float> samples, float span)
{
    for (std::size_t side = 0; side < 2 && side < samples.size(); side++)
    {
        float low = samples[side];
        float high = samples[side];
        for (std::size_t i = side; i < samples.size(); i += 2)
        {
            low = std::min(low, samples[i]);
            high = std::max(high, samples[i]);
        }
        if (high - low > span)
        {
            return false;
        }
    }

    return true;
}

} // namespace

// The largest meter of each of `tracks` tracks over the first `samples` samples of the reference, with track `muted`
// muted where it isn't -1, in the console's frames. The meter of a frame that starts before `samples` counts.
Peaks ReferencePeaks(const song::DriverRom& rom, uint32_t sound, bool enhanced, std::size_t samples, int tracks,
                     int muted)
{
    const std::unique_ptr<ReferencePlayer> player = MakeReference(rom);
    player->Start(sound, enhanced);
    if (muted >= 0)
    {
        player->MuteTrack(muted);
    }
    player->MeterTracks();
    Peaks peaks(static_cast<std::size_t>(tracks));
    std::vector<int16_t> strict;
    std::vector<float> floats;
    std::size_t done = 0;
    while (done < samples)
    {
        strict.clear();
        floats.clear();
        if (enhanced)
        {
            player->RunFrame(floats);
        }
        else
        {
            player->RunFrame(strict);
        }
        done += (enhanced ? floats.size() : strict.size()) / 2;
        for (std::size_t t = 0; t < peaks.size(); t++)
        {
            const std::array<int32_t, 2> frame = player->TrackPeaks(static_cast<int>(t));
            peaks[t] = {std::max(peaks[t][0], frame[0]), std::max(peaks[t][1], frame[1])};
        }
    }

    return peaks;
}

namespace
{

// The model's meters over `frames` frames have to fall between the reference's meters over the same samples less two of
// the console's frames and those over the same samples plus two. The frames of the console don't fall evenly in the
// frames of the model. A muted track's meter has to stay at 0, and the other tracks' meters have to stay as they are.
// On Brownie Brown's driver, the PSG channel of a sound effect takes over the hardware from the music channel of the
// same PSG channel, and a mute silences the hardware. Every track's meter then has to fall between the reference's
// meters with the same track muted. In enhanced mixing, the muted track of the reference can show a meter other than 0
// too. ntdlib's ArchivePlayer assigns the peaks of a PSG channel in a frame to the track that has the channel at the
// end of the frame. The tool counts such sounds apart.
void CheckMeters(const std::string& name, const std::shared_ptr<const song::DriverRom>& rom,
                 const std::shared_ptr<const song::Sequence>& sequence, song::Mixing mixing, std::size_t frames,
                 int muted, Totals& totals)
{
    const int tracks = rom->MakePlayer()->Tracks();
    const auto run = [&](uint32_t mutes)
    {
        song::SequenceModel model(rom, sequence, song::ModelOptions{.mixing = mixing});
        model.SetMutes(mutes);
        Peaks peaks(static_cast<std::size_t>(tracks));
        for (std::size_t frame = 0; frame < frames && model.SeekFrame(frame); frame++)
        {
            for (std::size_t t = 0; t < peaks.size(); t++)
            {
                const std::array<int32_t, 2> part = model.TrackPeaks(static_cast<int>(t));
                peaks[t] = {std::max(peaks[t][0], part[0]), std::max(peaks[t][1], part[1])};
            }
        }

        return peaks;
    };

    const bool enhanced = mixing == song::Mixing::kEnhanced;
    const std::size_t frame_samples =
        song::SequenceModel(rom, sequence, song::ModelOptions{.mixing = mixing}).FrameSamples();
    const std::size_t console_samples = static_cast<std::size_t>(std::lround(
        static_cast<double>(song::SequenceModel(rom, sequence, song::ModelOptions{.mixing = mixing}).SampleRate()) /
        ntdlib::agb::kFrameRate));
    const std::size_t samples = frames * frame_samples;
    const Peaks low = ReferencePeaks(*rom, sequence->sound, enhanced,
                                     samples > 2 * console_samples ? samples - 2 * console_samples : 1, tracks);
    const Peaks high = ReferencePeaks(*rom, sequence->sound, enhanced, samples + 2 * console_samples, tracks);
    const Peaks heard = run(0);
    const Peaks silent = run(uint32_t{1} << muted);
    const bool shared = song::BrownieOf(rom.get()) != nullptr;
    bool handed_over = false;
    const std::size_t low_samples = samples > 2 * console_samples ? samples - 2 * console_samples : 1;
    const Peaks low_muted =
        shared ? ReferencePeaks(*rom, sequence->sound, enhanced, low_samples, tracks, muted) : Peaks();
    const Peaks high_muted =
        shared ? ReferencePeaks(*rom, sequence->sound, enhanced, samples + 2 * console_samples, tracks, muted)
               : Peaks();
    totals.meters++;
    for (std::size_t t = 0; t < heard.size(); t++)
    {
        for (std::size_t side = 0; side < 2; side++)
        {
            const bool outside = heard[t][side] < low[t][side] || heard[t][side] > high[t][side];
            const bool other_wrong = shared
                                         ? silent[t][side] < low_muted[t][side] || silent[t][side] > high_muted[t][side]
                                         : silent[t][side] != heard[t][side];
            const bool wrong_mute = static_cast<int>(t) == muted && !shared ? silent[t][side] != 0 : other_wrong;
            if (shared && static_cast<int>(t) == muted && silent[t][side] != 0 && !wrong_mute && !outside)
            {
                handed_over = true;
            }
            if (outside || wrong_mute)
            {
                std::printf(
                    "%s: track %zu's meter on side %zu gives %d, and %d with track %d muted. The reference gives "
                    "%d to %d.\n",
                    name.c_str(), t, side, heard[t][side], silent[t][side], muted, low[t][side], high[t][side]);
                if (shared)
                {
                    std::printf("%s: with track %d muted, the reference gives track %zu %d to %d.\n", name.c_str(),
                                muted, t, low_muted[t][side], high_muted[t][side]);
                }
                totals.failed++;
                return;
            }
        }
    }
    totals.handed += handed_over ? 1 : 0;
}

// Checks the output with `track` muted for a tenth of the check's `frames` against the reference with the track muted.
// The output with the mute ends where the output without it, `heard` frames, ends. `span` is the widest span on a side
// of a level that holds.
void CheckMutedTrack(const std::string& name, const std::shared_ptr<const song::DriverRom>& rom, uint32_t number,
                     const std::shared_ptr<const song::Sequence>& sequence, song::Mixing mixing, bool enhanced,
                     const song::Track& track, std::size_t frames, std::size_t frame_samples, std::size_t frame_floats,
                     std::size_t start, std::size_t lead, std::size_t heard, float span, Totals& totals)
{
    const std::size_t muted_frames = std::max<std::size_t>(frames / 10, 2);
    const Reference without =
        RunReference(*rom, number, enhanced, (start + muted_frames + 2) * frame_samples, track.number);
    const auto muted_model = std::make_shared<song::SequenceModel>(rom, sequence, song::ModelOptions{.mixing = mixing});
    const auto muted_clock = std::make_shared<song::PlaybackClock>(frame_samples);
    muted_clock->SetMutes(uint32_t{1} << track.number);
    song::SequencePlayer muted_player(muted_model, 0, muted_clock);
    std::vector<float> muted_output;
    Render(muted_player, muted_frames, frame_samples, muted_output);
    const std::size_t muted_heard = muted_output.size() / frame_floats;
    muted_output.resize(muted_heard > 1 ? (muted_heard - 1) * frame_floats : 0);
    const std::size_t muted_differences = Differences(muted_output, 1, without.samples, frame_floats, lead) +
                                          SoundingFrames(without, start, frame_floats);

    // The output with the mute ends where the output without it ends. It can end sooner where the reference with
    // the mute holds a level on each side up to the end of the run.
    const std::size_t expected = std::min(heard, muted_frames);
    const std::size_t muted_rest = std::min(lead + muted_heard * frame_floats, without.samples.size());
    const std::size_t muted_stop = std::min(lead + muted_frames * frame_floats, without.samples.size());
    const std::span<const float> muted_after =
        std::span<const float>(without.samples).subspan(muted_rest, muted_stop - std::min(muted_rest, muted_stop));
    const bool ends_right = muted_heard == expected || (muted_heard < expected && HoldsLevel(muted_after, span));
    totals.mutes++;
    if (muted_differences > 0 || !ends_right)
    {
        const std::string end_text = ends_right ? ""
                                                : ", and the output ends at frame " + std::to_string(muted_heard) +
                                                      " in place of " + std::to_string(expected);
        std::printf("%s: with track %d muted, %zu of %zu frames differ%s.\n", name.c_str(), track.number,
                    muted_differences, muted_heard, end_text.c_str());
        totals.failed++;
    }
}

void CheckSound(const char* path, const std::shared_ptr<const song::DriverRom>& rom, uint32_t number, int seconds,
                bool enhanced, Totals& totals)
{
    const std::string name = std::string(path) + ": " + rom->SoundName(number);
    std::shared_ptr<const song::Sequence> sequence;
    try
    {
        sequence = std::make_shared<const song::Sequence>(rom->ReadSequence(number));
    }
    catch (const std::exception& error)
    {
        std::printf("%s: can't be read. %s\n", name.c_str(), error.what());
        totals.unread++;
        return;
    }
    totals.sounds++;
    totals.looped += sequence->loop ? 1 : 0;

    const song::Mixing mixing = enhanced ? song::Mixing::kEnhanced : song::Mixing::kStrict;
    const auto model = std::make_shared<song::SequenceModel>(rom, sequence, song::ModelOptions{.mixing = mixing});
    const std::size_t frame_samples = model->FrameSamples();
    const std::size_t frame_floats = frame_samples * 2;
    const std::size_t frames = CheckFrames(*model, seconds);
    const std::size_t start = StartFrame(rom, sequence, mixing);
    const std::size_t lead = start * frame_floats;
    Reference reference;
    try
    {
        reference = RunReference(*rom, number, enhanced, (start + frames + 2) * frame_samples);
    }
    catch (const std::exception& error)
    {
        std::printf("%s: the reference can't play it. %s\n", name.c_str(), error.what());
        totals.failed++;
        return;
    }

    // From the start. The output's last frame can fade out, and the comparison leaves it out.
    const StartResult from_start = StartCheck(model, frames, reference.samples, lead);
    const bool ended = from_start.ended;
    const std::size_t heard = from_start.heard;
    const std::size_t differences = from_start.differences + SoundingFrames(reference, start, frame_floats);

    // A loop's output has to reach the end of the loop's first pass. A model that fails or falls silent ends it sooner.
    const bool short_loop = ended && sequence->loop && model->After() < static_cast<double>(sequence->loop->end);

    // After an end of the output, the reference holds a level on each side. A PSG channel that stops can leave its
    // level. In strict mixing, the level holds exactly. In enhanced mixing, the reverb's tail rings on below the
    // console's steps, and each side stays within ±8 of a level (SequenceModel's end). A loop's output ends at the end
    // of the loop's pass, and the reference plays on round the loop.
    const std::size_t rest = std::min(lead + heard * frame_floats, reference.samples.size());
    const std::size_t stop = std::min(lead + frames * frame_floats, reference.samples.size());
    const std::span<const float> after =
        std::span<const float>(reference.samples).subspan(rest, stop - std::min(rest, stop));
    const float span = enhanced ? 2.0f * static_cast<float>(xsflib::rip::kSilenceLevel) / 32768.0f : 0.0f;
    const EndLevels end =
        ended && !sequence->loop && after.size() >= 2 ? LevelsAfterEnd(after, frame_floats, span) : EndLevels();
    if (!end.sounds_on && end.level > 0.0f)
    {
        totals.levels++;
        totals.largest_level = std::max(totals.largest_level, end.level);
    }

    // A seek to the middle of the output, then one back to the second frame. The output starts at the model's frame
    // `start`.
    const auto [seeks, failed_seeks] = SeekCheck(*model, start, heard, reference.samples, frame_floats, totals);

    // A switch halfway through the output.
    const std::string switch_text =
        SwitchCheck(rom, sequence, mixing, reference.samples, heard, frames, frame_floats, lead, totals);

    const std::string loop = LoopText(sequence->loop);
    if (differences + seeks + failed_seeks > 0 || end.sounds_on || short_loop || heard == 0 || !switch_text.empty())
    {
        std::string after_text = FailedSeeksText(failed_seeks);
        if (end.sounds_on)
        {
            after_text += ", and the reference sounds after the output's end: " +
                          std::to_string(std::lround(end.range * 32768.0f)) + " steps in all, and " +
                          std::to_string(std::lround(end.widest * 32768.0f)) + " within frame " +
                          std::to_string(end.widest_at) + " after the end";
        }
        if (short_loop)
        {
            after_text += ", and the output ends before the loop's end";
        }
        PrintDifferences(name, differences, heard, seeks, after_text, switch_text, loop);
        totals.failed++;
    }

    // A mute of the busiest track for a tenth of the check, and the meters with that track.
    const song::Track* busiest = BusiestTrack(*sequence);
    if (busiest)
    {
        CheckMutedTrack(name, rom, number, sequence, mixing, enhanced, *busiest, frames, frame_samples, frame_floats,
                        start, lead, heard, span, totals);
        CheckMeters(name, rom, sequence, mixing, std::max<std::size_t>(frames / 10, 2), busiest->number, totals);
    }

    CountCommands(name, *rom, *sequence, enhanced, model->SampleRate(), totals);
}

// The options of a run: the mixing, the part of the sounds, the ROMs and the seconds of each check.
struct Options
{
    bool enhanced = false;
    std::size_t part = 0;
    std::size_t parts = 1;
    std::vector<std::string> paths;
    int seconds = 30;
};

// The options in `arguments`, or nothing after a message on stderr.
std::optional<Options> ParseArguments(const std::vector<std::string>& arguments)
{
    Options options;
    for (std::size_t a = 1; a < arguments.size(); a++)
    {
        if (arguments[a] == "--enhanced")
        {
            options.enhanced = true;
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
            std::fprintf(stderr, "Hokuto's playback check of the GBA's drivers has no option %s.\n",
                         arguments[a].c_str());
            return std::nullopt;
        }
        else
        {
            options.paths.push_back(arguments[a]);
        }
    }
    if (options.paths.empty() || options.seconds <= 0)
    {
        std::fprintf(stderr, "usage: check_driver_playback [--enhanced] [--part k/n] <rom.gba>... [seconds]\n");
        return std::nullopt;
    }

    return options;
}

// Checks each sound of the ROM at `path` in the part that `options` selects. Counts a ROM that contains one of the
// GBA's other drivers in `roms`.
void CheckRom(const std::string& path, const Options& options, std::size_t& roms, Totals& totals)
{
    std::vector<uint8_t> bytes;
    std::shared_ptr<const song::DriverRom> rom;
    try
    {
        if (!xsflib::ReadFile(xsflib::PathFromUtf8(path), bytes))
        {
            std::printf("%s: can't be read\n", path.c_str());
            totals.unread++;
            return;
        }
        const hokuto::format::Driver* driver = hokuto::format::DriverOfRom(bytes);
        if (driver == nullptr)
        {
            std::printf("%s: holds none of the GBA's other drivers\n", path.c_str());
            return;
        }
        rom = song::LoadDriverRom(driver->Facts().engine, std::move(bytes));
    }
    catch (const xsflib::FormatError& error)
    {
        std::printf("%s: %s\n", path.c_str(), error.what());
        return;
    }
    catch (const std::exception& error)
    {
        std::printf("%s: %s\n", path.c_str(), error.what());
        totals.unread++;
        return;
    }

    roms++;
    for (const uint32_t number : rom->Sounds())
    {
        if (static_cast<std::size_t>(number) % options.parts != options.part)
        {
            continue;
        }

        // A sound whose later steps throw fails, and the run goes on with the next.
        try
        {
            CheckSound(path.c_str(), rom, number, options.seconds, options.enhanced, totals);
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s: %s\n", path.c_str(), rom->SoundName(number).c_str(), error.what());
            totals.failed++;
        }
    }
}

} // namespace

} // namespace hokuto::tools::driver_playback

using hokuto::tools::driver_playback::CheckRom;
using hokuto::tools::driver_playback::Options;
using hokuto::tools::driver_playback::ParseArguments;
using hokuto::tools::driver_playback::Totals;

int main(int argc, char** argv)
{
    std::vector<std::string> arguments = xsflib::console::Arguments(argc, argv);
    const std::optional<Options> options = ParseArguments(arguments);
    if (!options)
    {
        return 2;
    }

    Totals totals;
    std::size_t roms = 0;
    for (const std::string& path : options->paths)
    {
        CheckRom(path, *options, roms, totals);
    }

    std::printf(
        "%zu ROMs, %zu sounds, %zu with a loop, %zu can't be read, %zu differ. %zu seeks. %zu of %zu switches exact. "
        "%zu mutes, %zu meters. %zu of %zu note commands have no note of the arrangement at their ticks, %zu are in "
        "tracks that the sound mutes, %zu continue the note before them through a tie, %zu have a note within a frame "
        "of their tick, and %zu play at volume 0. After the output's end, the reference holds a level other than 0 in "
        "%zu sounds, %d steps at most. %zu sounds' muted track has a meter other than 0 in the reference too. %zu note "
        "commands lead to a note that sounds in a later frame. %zu note commands without a note are in tracks that "
        "never sound in the reference. %zu stretches have no commands at their offsets.\n",
        roms, totals.sounds, totals.looped, totals.unread, totals.failed, totals.seeks, totals.exact, totals.switches,
        totals.mutes, totals.meters, totals.lost, totals.commands, totals.muted, totals.tied, totals.framed,
        totals.zero_volume, totals.levels, static_cast<int>(std::lround(totals.largest_level * 32768.0f)),
        totals.handed, totals.late, totals.silent, totals.unmatched);
    if (totals.krawall > 0)
    {
        std::printf(
            "On Krawall, %zu note commands without a note slide to their note, %zu are on a channel without a "
            "sample, %zu have a note delay past their row, and %zu start a voice that stays at level 0. They "
            "count apart. %zu of the others start a voice that sounds.\n",
            totals.slides, totals.unsampled, totals.delayed, totals.unheard, totals.heard);
    }

    return totals.failed + totals.unread + totals.unmatched > 0 ? 1 : 0;
}
