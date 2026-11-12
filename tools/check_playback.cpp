// SPDX-License-Identifier: MIT

// Checks Hokuto's playback of a Switch sound archive against ntdlib's model of nn::atk, run directly. For each sequence
// sound, it checks:
//
// - From the start, the player's output matches the model's from the model's first frame that runs a tick. The model
//   makes no sound before that frame.
// - From a seek, to the loop's start or to the middle of the sequence, the output matches the model's after the first
//   frame. The first frame fades in. The seek restores a snapshot that the run from the start took. A seek that lies
//   further past the snapshot than 2 s and the lead mixes only the lead before its place. A catch-up does the same
//   (see --reverb). With an effect or in enhanced mixing, the output after the first frame therefore has to lie close
//   to the model's. The model's output has to be 60 dB or more above the difference: over the next second, and over
//   the rest. The effect then lacks the tail of what came before the lead. The lead lets that tail fall by 60 dB. A
//   seek can land in the quiet tail of a sound, such as the end of a jingle. The model's output over the second
//   before the lead therefore counts instead where it's louder. A difference 80 dB or more below full scale passes
//   too: a quiet passage can lie only a few steps of the 16-bit output above silence. A seek past the point where the
//   output has faded ends the output at once. The output fades once nothing plays or sounds and a frame has come out
//   silent. An effect's tail sounds on after the last channel.
// - Output that ends early ends where the model's output has faded.
// - The clock gives each frame's position: the model's tick count before the frame, round the loop. Once a sequence
//   without a loop has ended, the position goes on at the engine's tempo at the end. After a seek, the first frame's
//   position is the start.
// - Each note that the model starts is in the arrangement, on its track and key, at a position that the frame's ticks
//   reach.
//
// The model reads the archive by the strict rules. They stop at the first problem. The player reads the archive as the
// program opens it, with the group files beside it. The tool prints a line for each sequence that differs, and a
// summary. It returns 1 when any sequence differs or can't be read. A sequence with notes missing from the arrangement
// gets a line too, but doesn't count as a difference. The notes that the model cuts in the tick they start are missing.
// They're inaudible. The arrangement shows one pass of a loop. A later pass can differ from it. A random value can
// change a note's key or a wait. A track can also loop by itself, over a length other than the sequence's loop. The
// notes missing after the first pass are counted apart.
//
// With --reverb n, both play with nn::audio's I3DL2 preset n on aux bus A. Most of the Switch's music sends to bus A.
// One more run then plays without the reverb, and switches to a model with it halfway. The program switches the same
// way after an edit during playback. A catch-up runs the new model up to the player's frame first. It mixes only the
// lead before that frame (song::ModelCatchUp). The reverb then lacks the tail of what came before the lead. After the
// frame that crossfades, the output has to lie close to the model's with the reverb. Over the next second, the model's
// output has to be 60 dB or more above the difference, or the difference 80 dB or more below full scale. The summary
// gives the lowest ratio, and counts the switches whose output matches the model's exactly and those that pass by the
// floor alone.
//
// With --variables, the checks run once for each choice of each variable that a sequence reads, -1 aside (see
// song::Variable). A variable without choices runs once, with 1. The player plays the sequence with the variable at
// that starting value, and the arrangement reads it that way too. The model runs with the value set before its first
// frame. A game sets it at the same point, through the sound's handle. The runs with -1 are the checks without the
// option.
//
// With --changes, the checks run once for each choice of each variable that a sequence reads at any time, -1 aside. A
// variable without choices runs once, with 1. The variable takes the value while the sequence plays: halfway through
// the sequence's length, or halfway through the check if that comes first, 2 ms after the start of a frame. The player
// plays the sequence with the change, and the arrangement reads it that way too. The model runs with the value set at
// the start of the change's frame (song::ChangeFrame). A game sets it between two frames too, through the sound's
// handle. The summary counts the runs whose model output differs from the run without the change.
//
// A Wii U archive gets simpler checks against ntdlib's model of nw::snd and AX, run directly with the group files
// beside the archive: the frames of Hokuto's model from the start and after seeks back and forth, and the player's
// output after its first frame. The options above are for the Switch's archives.
// The player's output has to reach the frame where the model's run ends, within a frame.
//
// With --effect kind, a Wii U archive's checks play with an effect of the Wii U on the TV's aux bus A, at Hokuto's
// starting values (song::WiiUStartingParameters). The kinds have the names of a Hokuto file: reverb_hi, reverb_std,
// chorus and delay. One more run then plays without the effect, and switches to a model with it halfway through the
// player's output. After the frame that crossfades, the output has to lie close to the model's with the effect, as with
// --reverb. A seek far past the last snapshot mixes only the lead before its frame. With an effect or in enhanced
// mixing, the frame after such a seek has to lie close to the model's by the Switch's rule. The second before the lead
// counts here too.
//
// With --enhanced, the model and the player play in enhanced mixing. The player's output has to be the model's output
// in floating point, sample for sample. One more run then plays in strict mixing, without the effects, and switches to
// the enhanced model halfway, as after a click on the mixer's switch. After the frame that crossfades, the output has
// to lie close to the enhanced model's, as with --reverb. On a Wii U archive, the frames from the start and after seeks
// are the model's output rounded to 16 bits.
//
// A 3DS archive plays in Hokuto's player on ntdlib's model of the DSP's audio firmware. The reference is ntdlib's
// archive mode of the 3DS with Teakra running the DSP firmware that --firmware names, such as a game's dspaudio.cdc.
// The reference reads the archive as archive mode reads it, by the strict rules. A sequence that archive mode doesn't
// start fails, and so does a run that stops before the check's end. Hokuto's frames hold the DSP's output after three
// frames of silence (song::CtrModel). The player's output after its first frame has to be that output, sample for
// sample. Once the output has ended, the reference has to stay silent. A seek to the middle of the output, and one back
// to the second frame, have to give the reference's frames there. The DSP always mixes, and a seek runs every frame
// from the snapshot before it. Halfway through the output, the player then switches to a new model of the same
// sequence. A catch-up runs the new model up to the player's frame. The program switches the same way after a change of
// the variables during playback. The output has to stay the reference's, rounded to 16 bits in the frame that
// crossfades, and end where it ends without the switch. As for the Switch, the tool counts the notes that the reference
// starts and the arrangement lacks. It counts the legatos among them apart: the notes of a track with tie or mono on
// that continue a sounding channel. The arrangement shows a legato as part of the note that it continues. --variables
// and --changes check the variables as on the Switch. A change comes 2 ms into the 3DS's frame halfway through the
// sequence's length or through the check. With --effect kind, both play an effect of the 3DS on aux bus A at the
// libraries' defaults. The kinds have the names of a Hokuto file: 3ds_reverb, 3ds_delay, 3ds_nn_reverb, 3ds_nn_delay
// and 3ds_nn_reverb_2011. The reference runs the effect in bus A's aux callback, on ntdlib's model of the library's
// effect (ntdlib/ctr_fx/effects.h). The switch halfway then comes from a model without the effect. The effects' tails
// can keep an offset of a step or two. After the output's end, the reference then only has to stay within ±8. The
// summary counts the runs that sent sound to bus A. --reverb is for the Switch. --part k/n checks every nth sequence
// from the kth on, for runs side by side. Teakra prints on stdout. The tool's lines then go to stderr.
// The output of a looped sequence has to reach the frame of the loop's end, within a frame.
//
// With --enhanced in place of --firmware, a 3DS archive plays in enhanced mixing. The reference is ntdlib's archive
// mode on the model in its enhanced mode, run directly, with its frames after four frames of silence. The player's
// output and the frames after seeks have to be the reference's in floating point, sample for sample. The switch halfway
// comes from a model in strict mixing, as after a click on the mixer's switch. The player keeps the rate of strict
// mixing. After the frame that crossfades, its frames have to be the reference's frames through a song::FrameResampler
// that starts at that frame, sample for sample.
//
// A Wii archive gets the same checks as a Wii U archive, against WSF's archive mode: ntdlib's model of NW4R SND and AX
// with the game's AX microcode on ntdlib's DSP, and the build of NW4R SND that the game's main.dol gives. Like a file
// imported with the main.dol (song::ReadWiiBuild), Hokuto's model follows the same build. It mixes with ntdlib's model
// of AX's microcode: the build of December 2006, or the build of October 2006 with NW4R SND's build of 2006 without a
// banner. AX takes the same build on both sides. --dol gives the main.dol. With --archive-build, both follow the build
// that the archive's version gives, like a file imported without the main.dol. Both play without effects, unless
// --effect kind puts an effect of the Wii on aux bus A at Hokuto's starting values (song::WiiStartingParameters). The
// kinds have the names of a Hokuto file: wii_reverb_hi, wii_reverb_hi_2008, wii_chorus, wii_delay, wii_reverb_std and
// wii_reverb_std_2008. A switch halfway, from a model without the effect, follows. The Wii mixes every frame. The
// output after the switch therefore has to be the reference's. With an effect, a run goes on after its output last had
// sound for as long as Hokuto's model rings (song::WiiRingingFrames): a delay's echo can come back after a quiet gap.
// --variables and --changes check the variables as on the Switch, with a change 2 ms into a frame of 3 ms. Those runs
// check how the values reach the model, and the reference mixes with ntdlib's model of the microcode too. The runs
// without values check the mixing. With --archive-build, the reference follows the build that the archive's version
// gives, like Hokuto's model. A sequence that differs only without it plays otherwise in the game's build. --part k/n
// checks every nth sequence from the kth on, as for the 3DS.
//
// usage: hokuto_check_playback [--reverb n | --effect kind] [--variables | --changes] [--enhanced] <archive> [seconds]
//        hokuto_check_playback --firmware <dsp firmware> [--part k/n] [--effect kind] [--variables | --changes]
//                              <3DS archive> [seconds]
//        hokuto_check_playback --enhanced [--part k/n] [--effect kind] [--variables | --changes]
//                              <3DS archive> [seconds]
//        hokuto_check_playback --dol <main.dol> [--archive-build] [--part k/n] [--effect kind]
//                              [--variables | --changes] <Wii archive> [seconds]

#include "check_playback.h"

#include "check_part.h"

namespace
{

namespace atk = ntdlib::nx::atk;
namespace cafe = ntdlib::cafe::snd;
namespace sar = ntdlib::sar;
namespace song = hokuto::song;
using hokuto::tools::Part;
using hokuto::tools::PartOf;
using hokuto::tools::playback::CheckCtrSequence;
using hokuto::tools::playback::CheckSequence;
using hokuto::tools::playback::CheckWiiArchive;
using hokuto::tools::playback::CheckWiiUSequence;
using hokuto::tools::playback::CtrEffect;
using hokuto::tools::playback::ForEachChoice;
using hokuto::tools::playback::kCtrEffects;
using hokuto::tools::playback::kCtrFrameMs;
using hokuto::tools::playback::kFrameMs;
using hokuto::tools::playback::kFramesPerSecond;
using hokuto::tools::playback::kStrict;
using hokuto::tools::playback::kWiiEffects;
using hokuto::tools::playback::kWiiUEffects;
using hokuto::tools::playback::kWiiUFramesPerSecond;
using hokuto::tools::playback::kWiiUStrict;
using hokuto::tools::playback::PrintSwitches;
using hokuto::tools::playback::Totals;
using hokuto::tools::playback::WiiEffects;
using hokuto::tools::playback::WiiUEffects;

// The options of the command line.
struct Options
{
    std::string firmware_path;
    std::string dol_path;
    bool archive_build = false;
    std::size_t part = 0;
    std::size_t parts = 1;
    bool enhanced = false;
    atk::EngineOptions options;
    song::AuxEffects effects;
    int preset = -1;
    WiiUEffects wii_u_effects;
    WiiEffects wii_effects;
    const CtrEffect* ctr_effect = nullptr;
    std::string effect_name;
    bool wii_u_effect = false;
    bool variables = false;
    bool changes = false;
    std::filesystem::path path;
    int seconds = 30;
};

// Moves --firmware, --dol, --archive-build, --part and --enhanced from `arguments` into `o`. Returns false after a line
// about a wrong --part.
bool TakeNamedOptions(std::vector<std::string>& arguments, Options& o)
{
    // --firmware names the DSP firmware that the reference of a 3DS archive plays on, and --part k/n checks part k of n
    // of the sequences.
    const auto firmware_option = std::find(arguments.begin(), arguments.end(), "--firmware");
    if (firmware_option != arguments.end() && firmware_option + 1 != arguments.end())
    {
        o.firmware_path = *(firmware_option + 1);
        arguments.erase(firmware_option, firmware_option + 2);
    }

    // --dol names the main.dol of a Wii archive's game. --archive-build takes the build from the archive's version.
    const auto dol_option = std::find(arguments.begin(), arguments.end(), "--dol");
    if (dol_option != arguments.end() && dol_option + 1 != arguments.end())
    {
        o.dol_path = *(dol_option + 1);
        arguments.erase(dol_option, dol_option + 2);
    }
    const auto archive_build_option = std::find(arguments.begin(), arguments.end(), "--archive-build");
    o.archive_build = archive_build_option != arguments.end();
    if (o.archive_build)
    {
        arguments.erase(archive_build_option);
    }
    const auto part_option = std::find(arguments.begin(), arguments.end(), "--part");
    if (part_option != arguments.end() && part_option + 1 != arguments.end())
    {
        const std::optional<Part> given = PartOf(*(part_option + 1));
        if (!given)
        {
            std::fprintf(stderr, "--part takes k/n, with k from 1 to n.\n");
            return false;
        }
        o.part = given->k - 1;
        o.parts = given->n;
        arguments.erase(part_option, part_option + 2);
    }
    const auto enhanced_option = std::find(arguments.begin(), arguments.end(), "--enhanced");
    o.enhanced = enhanced_option != arguments.end();
    if (o.enhanced)
    {
        arguments.erase(enhanced_option);
    }
    o.options.enhanced = o.enhanced;

    return true;
}

// Moves --reverb and --effect from `arguments` into `o`.
void TakeEffectOptions(std::vector<std::string>& arguments, Options& o)
{
    if (arguments.size() > 2 && arguments[1] == "--reverb")
    {
        o.preset = std::atoi(arguments[2].c_str());
        if (o.preset >= 0 && o.preset < static_cast<int>(ntdlib::nx::audio::kI3dl2ReverbPresets.size()))
        {
            o.options.aux_reverbs[0] = ntdlib::nx::audio::kI3dl2ReverbPresets[static_cast<std::size_t>(o.preset)];
            o.effects[0].push_back(*o.options.aux_reverbs[0]);
        }
        arguments.erase(arguments.begin() + 1, arguments.begin() + 3);
    }
    if (arguments.size() > 2 && arguments[1] == "--effect")
    {
        o.wii_u_effect = true;
        o.effect_name = arguments[2];
        for (const auto& [name, type] : kWiiUEffects)
        {
            if (arguments[2] == name)
            {
                const song::WiiUEffect effect = song::WiiUEffectOf(type, song::WiiUStartingParameters(type));
                o.wii_u_effects.setups.push_back({cafe::kEffectDeviceTv, 0, type, effect.words, {}});
                o.wii_u_effects.effects[0].push_back(effect);
            }
        }
        for (const auto& [name, type] : kWiiEffects)
        {
            if (arguments[2] == name)
            {
                const song::WiiEffect effect = song::WiiEffectOf(type, song::WiiStartingParameters(type));
                ntdlib::rvl::snd::EffectSetup& setup = o.wii_effects.setups.emplace_back();
                setup.type = type;
                std::copy(effect.words.begin(), effect.words.end(), setup.params.begin());
                o.wii_effects.effects[0].push_back(effect);
            }
        }
        for (const CtrEffect& effect : kCtrEffects)
        {
            if (arguments[2] == effect.name)
            {
                o.ctr_effect = &effect;
            }
        }
        arguments.erase(arguments.begin() + 1, arguments.begin() + 3);
    }
}

// The options of the command line's `arguments`, or nothing after a line about a wrong one.
std::optional<Options> ParseOptions(std::vector<std::string> arguments)
{
    // --reverb n puts nn::audio's I3DL2 preset n on aux bus A, for the model and for the player. --effect puts an
    // effect of the Wii U, the Wii or the 3DS there. --variables checks the choices of the variables, and --changes
    // checks them as changes while the sequence plays. --enhanced plays in enhanced mixing.
    Options o;
    if (!TakeNamedOptions(arguments, o))
    {
        return std::nullopt;
    }
    TakeEffectOptions(arguments, o);
    o.variables = arguments.size() > 1 && arguments[1] == "--variables";
    o.changes = arguments.size() > 1 && arguments[1] == "--changes";
    if (o.variables || o.changes)
    {
        arguments.erase(arguments.begin() + 1);
    }
    const bool unknown_effect =
        o.wii_u_effect && o.wii_u_effects.setups.empty() && o.wii_effects.setups.empty() && o.ctr_effect == nullptr;
    if (arguments.size() < 2 || arguments.size() > 3 || (o.preset != -1 && o.effects[0].empty()) || unknown_effect ||
        (o.preset != -1 && o.wii_u_effect))
    {
        std::fprintf(stderr,
                     "usage: hokuto_check_playback [--reverb n | --effect kind] [--variables | --changes] "
                     "[--enhanced] <archive> [seconds]\n"
                     "       hokuto_check_playback --firmware <dsp firmware> [--part k/n] [--effect kind] "
                     "[--variables | --changes] <3DS archive> [seconds]\n"
                     "       hokuto_check_playback --enhanced [--part k/n] [--effect kind] [--variables | --changes] "
                     "<3DS archive> [seconds]\n"
                     "       hokuto_check_playback --dol <main.dol> [--archive-build] [--part k/n] [--effect kind] "
                     "[--variables | --changes] <Wii archive> [seconds]\n");
        return std::nullopt;
    }

    o.path = xsflib::PathFromUtf8(arguments[1]);
    o.seconds = arguments.size() > 2 ? std::max(std::atoi(arguments[2].c_str()), 1) : 30;

    return o;
}

// Prints `totals` of a run on a 3DS archive with the options `o`, in which `reading` sequences read variables.
void PrintCtrTotals(const Options& o, const Totals& totals, std::size_t reading)
{
    const std::string runs =
        o.variables || o.changes ? std::to_string(reading) + " sequences read variables. Their " : "";
    const char* checked = o.changes     ? "runs with a change were"
                          : o.variables ? "runs with a choice were"
                                        : "3DS sequences";
    std::fprintf(stderr,
                 "%s%zu %s checked for %d s, %zu with a loop. %zu differ, and %zu can't be read. %zu of %zu notes "
                 "missing from the arrangement, %zu of them legatos and %zu after the first pass of a loop. %zu of "
                 "%zu switches kept the reference's output.\n",
                 runs.c_str(), totals.sequences, checked, o.seconds, totals.looped, totals.failed, totals.unread,
                 totals.missed_notes, totals.notes, totals.missed_legatos, totals.missed_later, totals.exact,
                 totals.switches);
    if (totals.tickless > 0)
    {
        std::fprintf(stderr, "%zu sequences ran no tick in archive mode, and compared from frame 0.\n",
                     totals.tickless);
    }
    if (o.changes)
    {
        std::fprintf(stderr, "The change altered the model's output in %zu runs.\n", totals.affected);
    }
    if (o.ctr_effect != nullptr)
    {
        std::fprintf(stderr,
                     "With %s on bus A, %zu runs sent sound to the bus. %zu ended while the reference gave samples "
                     "of ±8 at most.\n",
                     std::string(o.ctr_effect->name).c_str(), totals.sent, totals.quiet_tails);
    }
}

// Checks the sequences of `strict`, a 3DS archive, and of `archive`, the program's reading of it, with the options `o`.
// Returns the tool's exit status.
int RunCtrArchive(const Options& o, const sar::SoundArchive& strict,
                  const std::shared_ptr<const sar::SoundArchive>& archive)
{
    std::vector<uint8_t> firmware;
    if (o.preset != -1 || (o.wii_u_effect && o.ctr_effect == nullptr) || o.enhanced == !o.firmware_path.empty())
    {
        std::fprintf(stderr,
                     "A 3DS archive takes --firmware or --enhanced, and no other option but --part, --effect with "
                     "an effect of the 3DS, --variables and --changes.\n");
        return 2;
    }
    if (!o.enhanced && !xsflib::ReadFile(xsflib::PathFromUtf8(o.firmware_path), firmware))
    {
        std::fprintf(stderr, "can't read %s\n", o.firmware_path.c_str());
        return 2;
    }
    ntdlib::ctr::SilenceStdout();
    const std::size_t ctr_frames =
        song::FramesIn(static_cast<uint64_t>(o.seconds) * 1000, song::FrameMsOf(sar::Flavor::kCtr));
    Totals totals;
    std::size_t reading = 0;
    for (std::size_t sound = 0; sound < strict.Sounds().size(); sound++)
    {
        if (strict.Sounds()[sound].type != sar::SoundType::kSequence || sound % o.parts != o.part)
        {
            continue;
        }

        const auto index = static_cast<uint32_t>(sound);
        if (!o.variables && !o.changes)
        {
            CheckCtrSequence(firmware, strict, archive, index, ctr_frames, {}, {}, o.enhanced, o.ctr_effect, totals);
            continue;
        }

        // A sequence that can't be read gets a line from the run without values.
        song::Sequence plain;
        try
        {
            plain = song::ReadSequence(*archive, index);
        }
        catch (const std::exception&)
        {
            CheckCtrSequence(firmware, strict, archive, index, ctr_frames, {}, {}, o.enhanced, o.ctr_effect, totals);
            continue;
        }

        // A change comes 2 ms into the frame halfway through the sequence's length or through the check.
        const std::vector<song::Variable>& read = o.changes ? plain.read_variables : plain.variables;
        const double middle = std::min(song::MsOfTick(plain, plain.length / 2.0), o.seconds * 500.0);
        const uint64_t frame = song::FramesIn(static_cast<uint64_t>(middle), kCtrFrameMs);
        const auto time = static_cast<uint32_t>(frame * kCtrFrameMs.numerator / kCtrFrameMs.denominator + 2);
        reading += read.empty() ? 0 : 1;
        ForEachChoice(read, o.changes, time,
                      [&](const song::StartValues& values, const song::Changes& changes)
                      {
                          CheckCtrSequence(firmware, strict, archive, index, ctr_frames, values, changes, o.enhanced,
                                           o.ctr_effect, totals);
                      });
    }
    PrintCtrTotals(o, totals, reading);

    return totals.failed == 0 && totals.unread == 0 ? 0 : 1;
}

// Checks the sequences of `strict`, a Wii U archive, and of `archive`, the program's reading of it, with the options
// `o`. Returns the tool's exit status.
int RunWiiUArchive(const Options& o, const sar::SoundArchive& strict,
                   const std::shared_ptr<const sar::SoundArchive>& archive)
{
    Totals totals;
    for (std::size_t sound = 0; sound < strict.Sounds().size(); sound++)
    {
        if (strict.Sounds()[sound].type == sar::SoundType::kSequence)
        {
            CheckWiiUSequence(strict, archive, static_cast<uint32_t>(sound),
                              static_cast<std::size_t>(o.seconds) * kWiiUFramesPerSecond, o.wii_u_effects,
                              o.enhanced ? song::Mixing::kEnhanced : song::Mixing::kStrict, totals);
        }
    }

    const std::string with = std::string(o.wii_u_effect ? " with " + o.effect_name + " on bus A" : "") +
                             (o.enhanced ? " in enhanced mixing" : "");
    std::printf("%zu Wii U sequences checked for %d s%s, %zu with a loop. %zu differ, and %zu can't be read.\n",
                totals.sequences, o.seconds, with.c_str(), totals.looped, totals.failed, totals.unread);
    if (o.wii_u_effect)
    {
        std::printf("%zu ended while the effect's tail gave samples of ±8 at most.\n", totals.quiet_tails);
    }
    PrintSwitches(totals);

    return totals.failed == 0 && totals.unread == 0 ? 0 : 1;
}

// Checks the sequences of `strict`, a Switch archive, and of `archive`, the program's reading of it, with the options
// `o`. Returns the tool's exit status.
int RunSwitchArchive(const Options& o, const sar::SoundArchive& strict,
                     const std::shared_ptr<const sar::SoundArchive>& archive)
{
    const std::size_t frames = static_cast<std::size_t>(o.seconds) * kFramesPerSecond;

    // With --variables, each choice of each variable that a sequence reads gets a run, -1 aside, and a variable without
    // choices a run with 1. With --changes, the variables that the sequence reads at any time get the same runs, each
    // with a change. A sequence that can't be read gets a line from the run without values.
    Totals totals;
    std::size_t reading = 0;
    for (std::size_t sound = 0; sound < strict.Sounds().size(); sound++)
    {
        if (strict.Sounds()[sound].type != sar::SoundType::kSequence)
        {
            continue;
        }

        const auto index = static_cast<uint32_t>(sound);
        if (!o.variables && !o.changes)
        {
            CheckSequence(strict, archive, index, frames, o.options, o.effects, {}, {}, totals);
            continue;
        }

        song::Sequence plain;
        try
        {
            plain = song::ReadSequence(*archive, index);
        }
        catch (const std::exception&)
        {
            CheckSequence(strict, archive, index, frames, o.options, o.effects, {}, {}, totals);
            continue;
        }

        // A change comes halfway through the sequence's length or through the check, 2 ms into a frame.
        const std::vector<song::Variable>& read = o.changes ? plain.read_variables : plain.variables;
        const double middle = std::min(song::MsOfTick(plain, plain.length / 2.0), o.seconds * 500.0);
        const auto time =
            static_cast<uint32_t>(static_cast<uint64_t>(middle) / kFrameMs.numerator * kFrameMs.numerator + 2);
        reading += read.empty() ? 0 : 1;
        ForEachChoice(
            read, o.changes, time, [&](const song::StartValues& values, const song::Changes& changes)
            { CheckSequence(strict, archive, index, frames, o.options, o.effects, values, changes, totals); });
    }

    const std::string with = (o.preset >= 0 ? " with I3DL2 preset " + std::to_string(o.preset) + " on bus A" : "") +
                             (o.enhanced ? " in enhanced mixing" : "");
    const std::string runs =
        o.variables || o.changes ? std::to_string(reading) + " sequences read variables. Their " : "";
    const char* checked = o.changes ? "runs with a change were" : o.variables ? "runs with a choice were" : "sequences";
    std::printf(
        "%s%zu %s checked for %d s%s, %zu with a loop. %zu differ, and %zu can't be read. %zu of %zu notes missing "
        "from the arrangement, %zu of them after the first pass of a loop.\n",
        runs.c_str(), totals.sequences, checked, o.seconds, with.c_str(), totals.looped, totals.failed, totals.unread,
        totals.missed_notes, totals.notes, totals.missed_later);
    if (o.changes)
    {
        std::printf("The change altered the model's output in %zu runs.\n", totals.affected);
    }
    PrintSwitches(totals);

    return totals.failed == 0 && totals.unread == 0 ? 0 : 1;
}

// An archive of the Switch, the Wii U or the 3DS, as the program reads it and as the strict reading reads it, with
// its console.
struct PlaybackArchive
{
    std::shared_ptr<const sar::SoundArchive> archive;
    std::optional<sar::SoundArchive> strict;
    bool wii_u = false;
    bool ctr = false;
};

// Reads the archive at `path` both ways. Throws where the archive can't be read.
PlaybackArchive OpenPlaybackArchive(const std::filesystem::path& path)
{
    PlaybackArchive opened;
    opened.archive = song::OpenArchive(path);
    std::vector<uint8_t> bytes;
    xsflib::ReadFile(path, bytes);
    opened.wii_u = song::FlavorOfArchive(bytes) == sar::Flavor::kCafe;
    opened.ctr = song::FlavorOfArchive(bytes) == sar::Flavor::kCtr;
    opened.strict.emplace(sar::SoundArchive::Load(std::move(bytes), opened.wii_u ? kWiiUStrict
                                                                    : opened.ctr ? ntdlib::ctr::kSarOptions
                                                                                 : kStrict));

    // The model of a Wii U archive plays the group files beside it too.
    if (opened.wii_u)
    {
        for (const std::vector<uint8_t>& group : opened.archive->GroupFiles())
        {
            opened.strict->AddGroupFile(group);
        }
    }

    return opened;
}

} // namespace

int main(int argc, char** argv)
{
    const std::optional<Options> parsed = ParseOptions(xsflib::console::Arguments(argc, argv));
    if (!parsed)
    {
        return 2;
    }
    const Options& o = *parsed;
    if (!o.dol_path.empty())
    {
        if (o.preset != -1 || !o.wii_u_effects.setups.empty() || o.ctr_effect != nullptr || o.enhanced ||
            !o.firmware_path.empty())
        {
            std::fprintf(stderr,
                         "A Wii archive takes no other option but --dol, --archive-build, --part, --effect with an "
                         "effect of the Wii, --variables and --changes.\n");
            return 2;
        }

        return CheckWiiArchive(o.path, xsflib::PathFromUtf8(o.dol_path), o.seconds, o.variables, o.changes,
                               o.archive_build, o.part, o.parts, o.wii_effects, o.effect_name);
    }
    if (!o.wii_effects.setups.empty())
    {
        std::fprintf(stderr, "An effect of the Wii plays only on a Wii archive, with --dol.\n");
        return 2;
    }
    PlaybackArchive opened;
    try
    {
        opened = OpenPlaybackArchive(o.path);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 2;
    }

    // A 3DS archive's reference plays on the firmware, with each variable at its starting value, or with --enhanced on
    // the model's enhanced mode. It plays dry, or with an effect of the 3DS on bus A. Teakra prints the DSP's accesses
    // that it doesn't model on stdout. The tool's lines go to stderr instead.
    if (opened.ctr)
    {
        return RunCtrArchive(o, *opened.strict, opened.archive);
    }
    if (o.ctr_effect != nullptr)
    {
        std::fprintf(stderr, "An effect of the 3DS plays only on a 3DS archive.\n");
        return 2;
    }
    if (!o.firmware_path.empty() || o.parts != 1)
    {
        std::fprintf(stderr,
                     "--firmware checks only the 3DS's archives, and --part only those of the 3DS and the Wii.\n");
        return 2;
    }

    // A Wii U archive plays dry or with its effect, with each variable at its starting value. --effect is for the Wii
    // U's archives, and --reverb, --variables and --changes for the Switch's.
    if (opened.wii_u && (o.preset != -1 || o.variables || o.changes))
    {
        std::fprintf(stderr, "--reverb, --variables and --changes check only the Switch's archives.\n");
        return 2;
    }
    if (!opened.wii_u && o.wii_u_effect)
    {
        std::fprintf(stderr, "--effect checks only the Wii U's archives.\n");
        return 2;
    }

    return opened.wii_u ? RunWiiUArchive(o, *opened.strict, opened.archive)
                        : RunSwitchArchive(o, *opened.strict, opened.archive);
}
