// SPDX-License-Identifier: MIT

// Checks the Bank Editor's edits of DSE's banks (src/edit/dse_banks.h) on DSE's cartridge images. The tool imports each
// image into a Hokuto file (format::ImportDseImage) and lists every split of every bank that the set reads. A bank
// whose bytes two files hold counts once. It then makes four rounds of edits, each in one version of the file:
//
// - Envelopes: each split gets a new attack level, attack, decay, sustain, hold, second decay and release.
// - Values: each split gets a new volume, pan, key group, bend range, multiplier and tuning.
// - Keys: each split gets a narrower range of keys and velocities where it has room, and a root key 5 higher, wrapping
//   at 128.
// - Waves: each split changes to the next wave in the table of its bank.
//
// A value of a split past 127 stays. After each round, the tool exports the set and checks it:
//
// - The set keeps its size. The tool walks each bank file's bytes for its splits, without the reader, and works out
//   their new bytes by the format's rules. The export has to equal the image with those bytes, in each file that
//   contains the bank.
// - ntdlib's reader reads each split with the new bytes, and the Bank Editor shows each bank of the new set.
// - The file comes back from its binary form and its text form, and produces the same set.
//
// A round of wave edits assigns each wave of each bank with samples three quarters of its rate, at least 256 Hz, and a
// loop from the word at the middle of its samples. The tool works out the bytes by the format's rules: the rate and the
// loop in the bank's table and in the table of each bank that uses its waves, and the tuning of each split that plays
// the wave. The driver reads no rate. The tuning of a split alone sets the pitch of the wave. Each new tuning therefore
// moves by the semitones between the table's old rate and the new one. Each wave has to decode to its samples before
// the edit, and the file has to come back from both its forms. A square wave and noise have to refuse the edit. A wave
// of one word of samples counts apart.
//
// Rounds of renders then play a note of programs of each image on ntdlib's model of the driver, at velocity 127, for
// 1 s, before and after each of three edits. The model plays each note the way the Bank Editor's keyboard plays it. The
// programs are those where one split alone plays the note: every nth of them, with n their count divided by 128 and
// rounded down, and at least 1. An image with up to 255 such programs plays them all, and one with more plays 128 to
// 192 of them.
//
// - Half the split's volume. The driver squares a voice's level. The note's level has to fall by 40 log10 of the ratio
//   of the two levels within 1 dB.
// - A root key an octave higher. The note has to play an octave lower.
// - Half the rate that the table of the note's bank sets for the wave. The edit applies to the wave of the bank that
//   contains the samples. Each table moves the tunings of its splits by the semitones between the rate that it held and
//   the new one. The note has to play an octave lower.
//
// A note an octave lower plays the same samples at half the speed. The tool therefore counts the crossings of zero in a
// stretch of the first render and in the stretch of the second that plays the same samples: twice as late and twice as
// long. The second has to hold 0.7 to 1.4 of the first's crossings. An envelope acts in time and not in samples. The
// note for each pitch therefore gets an envelope that holds a level. The tool plays it an octave below its root key, or
// at the lowest key of its split, and at the mix's rate or below. A higher note drops samples, and their crossings with
// them. A program counts apart for a level when more than one split plays the note, when a volume LFO moves the level,
// or when the note is below -36 dB or reaches full scale. Below -36 dB, the driver's volumes take coarse steps. A
// program counts apart for a pitch when more than one split plays the note, when the note is above the mix's rate, when
// the note contains fewer than 40 crossings, or when its split plays noise. A rate counts apart when half of it is
// outside 256 to 192,000 Hz, or equals the rate of the bank that contains the samples. The summary shows a separate
// count of the checks that the tool leaves out: the root key of a split whose root key is outside 0 to 115, and a
// square wave's rate.
//
// The tool prints a line for each failure and refusal, and a summary. It returns 1 when an edit produces other bytes
// than it should, a read, a view, a save or a render fails, or a level or a pitch moves by another amount.
//
// With --replace, the tool checks Replace… of DSE's sounds, banks and waves instead (src/edit/dse_files.h):
//
// - Each sound is replaced with its song file: a song's file in the image, or a sound effect's settings and tracks as a
//   song file. The export has to give the image back. A file that contains bytes after a track's end counts apart: the
//   reader leaves them out, and no track reaches them. So do tracks that select none of the sound's banks: the
//   replacement assigns them the first bank of the sound, and the window says so.
// - A sample of the sounds then plays: every nth, with n their count divided by 128 and rounded down. Each is replaced
//   with the song file of the next sound of its kind. ntdlib's reader has to read the next sound's events in the
//   sound's file, but for the bank selects, and every other file and sound effect has to keep its bytes. The
//   arrangement has to hold the next sound's notes at their starts, keys and velocities. The voices of another bank can
//   end a note elsewhere or hide it, and the performer can find the loop a pass away. The tool counts those notes and
//   tracks apart.
// - The same sample is replaced with its MIDI file from Export…, through the converter that song::ConvertToMidi calls.
//   The MIDI file numbers its programs by the SoundFont's presets, in the order that the sound first plays them. Each
//   program comes back with the program of its preset. A user pairs them the same way in the window's Instruments. The
//   tool counts the MIDI file's notes that come back at their keys, within 2 ticks at 48 ticks a quarter note, and
//   those whose lengths move further. The notes count up to the file's loopEnd marker, or the end of the new sound's
//   first pass. The MIDI file leaves out the notes that start no voice and the legatos, and the tool counts the
//   arrangement's notes apart.
// - Each bank is replaced with its file, and the export has to reproduce the image. It is then replaced with the file
//   of the next bank that contains samples where it does. Every file that held the bank has to hold the next bank's
//   programs, waves and samples, with the bank's id and name, and every other file its bytes.
// - Each wave of each bank with samples is replaced with the WAV file that Export… writes, in the wave's encoding. A
//   sample of the waves then plays, every nth as above. 8-bit and 16-bit PCM have to decode to the same samples, at the
//   same rate and with the same loop. The tool counts IMA-ADPCM by its ratio of signal to noise. Each wave of the
//   sample also comes back at half its rate as 16-bit PCM. Each table of a bank that plays the wave has to contain the
//   new rate, and each split that plays it has to move its tuning by 3072 log2 of the ratio of the rates of that table.
//
// usage: hokuto_check_dse_banks [--replace] <image.nds>...

#include "check_dse_banks.h"

namespace hokuto::tools::dse_banks
{

void Fail(const Game& game, Totals& totals, const std::string& message)
{
    std::printf("%s: %s\n", game.name.c_str(), message.c_str());
    std::fflush(stdout);
    totals.failed++;
}

namespace
{

void CheckImage(const std::filesystem::path& path, bool replace, Totals& totals)
{
    const std::string image_name = path.filename().string();
    try
    {
        Game game;
        game.name = image_name;
        std::vector<uint8_t> image;
        if (!xsflib::ReadFile(path, image))
        {
            Fail(game, totals, "can't read the image");
            return;
        }

        game.file = hokuto::format::ImportDseImage(image);
        game.bytes = hokuto::format::ExportArchive(game.file);
        game.starts = hokuto::format::NativePieceStarts(game.file);
        game.build = hokuto::song::LoadDseSet(image)->build;
        game.set = hokuto::song::LoadDseSet(game.bytes, game.build);
        totals.images++;

        const DseSet& set = *game.set;
        for (const uint32_t bank : DistinctBanks(set))
        {
            totals.banks++;
            const dse::WaveBank& held = *set.banks[bank].bank;
            if (!held.instruments)
            {
                continue;
            }

            for (const std::optional<dse::Program>& program : held.instruments->programs)
            {
                totals.splits += program ? program->splits.size() : 0;
            }
        }

        // The walk has to find what the reader finds, or the rounds would compare little.
        for (const hokuto::song::DseBank& bank : set.banks)
        {
            const Walk walk = WalkBank(std::span(game.bytes).subspan(static_cast<std::size_t>(bank.file_offset)));
            std::size_t held = 0;
            if (bank.bank->instruments)
            {
                for (const std::optional<dse::Program>& program : bank.bank->instruments->programs)
                {
                    held += program ? program->splits.size() : 0;
                }
            }
            if (walk.splits.size() != held)
            {
                Fail(game, totals,
                     "the walk finds other splits than the reader in " + hokuto::song::DseBankName(*bank.bank));
            }
        }

        if (replace)
        {
            // Each round prints the time that it took.
            const auto timed = [&](const char* round, const std::function<void()>& run)
            {
                const auto start = std::chrono::steady_clock::now();
                run();
                const std::chrono::duration<double> took = std::chrono::steady_clock::now() - start;
                std::printf("%s: the %s took %.0f s\n", game.name.c_str(), round, took.count());
                std::fflush(stdout);
            };
            timed("sounds", [&] { SoundRounds(game, totals); });
            timed("banks", [&] { BankRounds(game, totals); });
            timed("waves", [&] { WaveReplaceRound(game, totals); });
            return;
        }

        Round(game, "envelopes", Envelopes, totals);
        Round(game, "values", Values, totals);
        Round(game, "keys", Keys, totals);
        Round(game, "waves of splits", Waves, totals);
        WaveRound(game, totals);
        Renders(game, totals);
    }
    catch (const std::exception& error)
    {
        std::printf("%s: %s\n", image_name.c_str(), error.what());
        totals.failed++;
    }
}

} // namespace

} // namespace hokuto::tools::dse_banks

namespace
{

using hokuto::tools::dse_banks::CheckImage;
using hokuto::tools::dse_banks::kLeastImaRatio;
using hokuto::tools::dse_banks::Totals;

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> arguments = xsflib::console::Arguments(argc, argv);
    const auto flag = std::ranges::find(arguments, "--replace");
    const bool replace = flag != arguments.end();
    if (replace)
    {
        arguments.erase(flag);
    }
    if (arguments.size() < 2)
    {
        std::fprintf(stderr, "usage: hokuto_check_dse_banks [--replace] <image.nds>...\n");
        return 2;
    }

    Totals totals;
    for (std::size_t i = 1; i < arguments.size(); i++)
    {
        CheckImage(xsflib::PathFromUtf8(arguments[i]), replace, totals);
    }
    if (replace)
    {
        std::printf(
            "%zu images, %zu sounds: %zu from their song files, %zu apart, %zu refused. %zu sounds played: %zu from "
            "the next "
            "sound's file, %zu refused, %zu notes of other lengths, %zu tracks of more or fewer notes. %zu from their "
            "MIDI files, %zu refused: %zu of their %zu notes came back, %zu with other lengths, %zu sounds with all "
            "their notes, %zu without a note before their end, %zu notes in the game's arrangements. %zu banks from "
            "their files, %zu from the next bank's, %zu refused, %zu without a next bank. "
            "%zu waves from their WAV files, %zu with the "
            "image's bytes. %zu waves played: %zu of 8-bit and 16-bit PCM with the same samples, %zu of IMA-ADPCM at "
            "%.1f dB or more, %zu below %.0f dB, %zu at half their rate with %zu tunings, %zu refused. %zu failed.\n",
            totals.images, totals.sounds, totals.same_sounds, totals.sounds_apart, totals.sounds_refused,
            totals.sampled_sounds, totals.next_sounds, totals.next_refused, totals.next_lengths, totals.next_counts,
            totals.midi_sounds, totals.midi_refused, totals.midi_matched, totals.midi_notes, totals.midi_lengths,
            totals.midi_whole, totals.midi_noteless, totals.midi_arranged, totals.same_banks, totals.next_banks,
            totals.next_banks_refused, totals.next_banks_apart, totals.replaced_waves, totals.same_waves,
            totals.sampled_waves, totals.exact_waves, totals.ima_waves, totals.ima_waves > 0 ? totals.lowest_ima : 0.0,
            totals.ima_below, kLeastImaRatio, totals.half_rates, totals.half_tunings, totals.half_refused,
            totals.failed);
        return totals.failed == 0 ? 0 : 1;
    }

    std::printf(
        "%zu images, %zu banks, %zu splits, %zu edits of splits. %zu waves, %zu edits, %zu square waves and "
        "noise refused, %zu of one word, %zu refused. %zu levels, %zu apart. %zu root keys and %zu rates by "
        "pitch, %zu apart, with %.3f to %.3f of the crossings. %zu checks of a pitch left out. %zu failed.\n",
        totals.images, totals.banks, totals.splits, totals.split_edits, totals.waves, totals.wave_edits,
        totals.waves_refused, totals.waves_apart, totals.edits_refused, totals.levels, totals.levels_apart,
        totals.root_pitches, totals.rate_pitches, totals.pitches_apart, totals.lowest_ratio, totals.highest_ratio,
        totals.pitch_checks_left_out, totals.failed);

    return totals.failed == 0 ? 0 : 1;
}
