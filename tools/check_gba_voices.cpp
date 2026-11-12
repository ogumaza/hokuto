// SPDX-License-Identifier: MIT

// Checks the Bank Editor's edits of GBA voices (src/edit/gba_voices.h) on the voice groups of GBA ROMs that
// MusicPlayer2000 plays. The tool imports each ROM into a Hokuto file the way the program does
// (format::ImportGbaRom). It lists every voice that the Bank Editor shows (app::MakeGbaBankView): each program that the
// songs select, and each row of a key split or a drum kit. It then makes five rounds of edits, each in one version of
// the file:
//
// - PSG envelopes: each PSG voice gets a new attack, decay, sustain and release.
// - PSG settings: a square wave gets the next duty, square 1 another sweep, the noise the other mode, and every PSG
//   voice another length.
// - Patterns: the steps of the pattern of each wave voice are turned upside down.
// - Samples: the voice of each sample gets the next sample of the ROM, the other pitch mode and a new envelope.
// - Drums: each voice of a drum kit gets another key, and its pan is turned on or off.
//
// A value of a PSG voice past the range of the PSG stays: the edit refuses it. After each round, the tool exports the
// ROM and checks it:
//
// - The ROM keeps its size, and only the bytes of the edited voices and patterns differ. Each contains the new values.
// - ntdlib's reader reads each edited voice with the new values, and the Bank Editor shows each voice group of the
//   new ROM.
// - The export's list of collisions reads again the songs whose voice groups play a changed byte, and no others
//   (song::GbaSongsWithOtherVoices). A group plays the voices of the programs that its songs select, the key tables of
//   their key splits, the voices of the tables of their key splits and drum kits at every key, and the patterns of the
//   wave voices among them. Some keys read bytes that the Bank Editor doesn't show as voices, such as a pattern, and a
//   key table can run on into a pattern.
// - The file comes back from its binary form and its text form, and produces the same ROM.
//
// A sixth round halves the sustain of each sample's voice whose sustain is 128 or more. ntdlib's model of the console
// plays a held note of each such voice as the Bank Editor's keyboard plays it, before and after the edit. Once the
// note has held its sustain for a quarter of a second, the level has to fall by the ratio of the sustains within
// 0.6 dB. The driver rounds each side's volume down to a whole step. The keyboard's note plays each side at about 62
// of 255 at full level, and at about 15 steps at a sustain of 64: a step there is 0.6 dB. The tool counts apart a voice
// whose sample ends: the sample can end before the note reaches its sustain. It also counts apart a voice whose note
// never reaches its sustain within 5 s, and one whose note reaches full scale before the edit. The driver mixes into
// samples of 8 bits, and the hardware's output moves in steps of 6 bits at its highest rate. The driver rounds each
// sample down, with an error of up to a step. That error lifts a quiet level. The tool counts apart a note whose new
// level would be less than 20 dB above it. A note whose output ends before the measure counts apart too.
//
// With `--new`, the tool checks the new data that the Bank Editor puts in the ROM's free space instead. A selection of
// n items, here and with `--songs`, selects every kth item, with k the count of items over n, rounded down: n to 2n - 1
// items, or every item where there are fewer than n.
//
// - Kinds: each voice changes to another kind. A sample becomes square wave 2, a square wave the noise, the noise a
//   wave on a new pattern, and a wave square wave 1. The checks above hold. Of a selection of 64 voices, each that
//   becomes a PSG voice with a sustain of 8 or more has to sound. A voice that a song selects can overlap a key split's
//   key table. The edit of the voice then sends some of the split's keys to other rows. Such a key counts apart.
// - Samples: each sample that a voice plays is exported as a WAV file and imported again as a new sample, on its first
//   voice. ntdlib has to read the new sample with the same samples, loop and size, and the same rate where the sample's
//   rate is a whole number of hertz. The voices of a selection of 64 new samples play a held note. Those with a whole
//   rate, and the same byte after the sample's last, have to play the same samples. A compressed sample, a loop that
//   starts at or past the sample's end, and a sample that finds no room count apart. In a ROM without room, the rest go
//   into its filler runs (NativeArchive::gba_filler). Any other refusal fails, with a line.
// - New voices: a selection of 16 voice groups each get a voice of the first sample of the ROM that contains a sound,
//   and a copy of the first program that the songs select, at their first free programs (edit::WithGbaVoicesAt). In the
//   ROM's size, only the bytes of the table and the songs' headers may differ. The songs' headers have to name the new
//   table. The list of collisions has to stay empty, and the first 4 songs of the group and every 32nd song of the ROM
//   have to play the same samples for 3 s. The copy has to play the samples of its source at key 60, and the new
//   sample's voice has to sound. The Bank Editor has to list both, and the deleted voice has to free its program. A
//   group that finds no room counts apart. Any other refusal fails, with a line.
// - Key rows: in each of those voice groups, each key split and drum kit that the songs select gets its rows again in
//   new tables (edit::WithGbaKeyRows). A note at the first key of a selection of 8 rows of each has to play the same
//   samples, and the Bank Editor has to show the same rows. The first single voice becomes a key split of one row, then
//   a drum kit with its voice at key 60, and each has to play its samples at key 60. A drum kit plays a voice at the
//   voice's key, with the voice's pan: only a voice of key 60 without a pan compares there. Other voices only have to
//   sound. A voice below key 24 plays too slowly to hear in the check's time. A row that contains values past a voice's
//   range is no voice of the driver: its bytes are other data, and the row can overlap a key table. The tool leaves
//   such a row out. A row that plays another voice after another row's edit counts apart. An edit that finds no room
//   counts apart. Any other refusal fails, with a line.
//
// With `--songs`, the tool checks the values of the songs that the Sound sections edit instead (edit/gba_tables.h). The
// headers of a selection of 64 songs of the song table, each header once, take a new priority and a new reverb, and
// their numbers the next music player with tracks. Only those bytes may differ in the ROM's size. ntdlib has to read
// the new values at every number that names the header, and every other number's values as before. The file has to come
// back from both forms, and the old values have to give the ROM back byte for byte. Up to 8 songs then play for 3 s. A
// song with samples plays otherwise with another reverb, and a song of PSG voices plays the same: the tool counts both.
//
// The tool prints a line for each failure and refusal, and a summary. It returns 1 when an edit is refused or produces
// other bytes than it should, a read, a view, a save or a render fails, or a level falls by another ratio. A refusal
// for want of room in the ROM counts apart. The export also has to give the ROM back without an edit.
//
// usage: hokuto_check_gba_voices [--new | --songs] <rom.gba>...

#include "check_gba_voices.h"

namespace hokuto::tools::gba_voices
{

namespace
{

void CheckRom(const std::filesystem::path& path, bool only_new, bool only_songs, Totals& totals)
{
    const std::string rom_name = xsflib::Utf8(path);
    std::vector<uint8_t> bytes;
    if (!xsflib::ReadFile(path, bytes))
    {
        std::printf("%s: can't be read\n", rom_name.c_str());
        totals.failed++;
        return;
    }

    Game game{rom_name, {}, {}, nullptr, {}, {}};
    try
    {
        game.file = hokuto::format::ImportGbaRom(bytes);
        game.rom = hokuto::song::LoadGbaRom(bytes);
    }
    catch (const xsflib::FormatError&)
    {
        return; // no MP2K, or no song that plays
    }
    totals.roms++;

    try
    {
        // The export reproduces the ROM without an edit.
        game.bytes = hokuto::format::ExportArchive(game.file);
        if (game.bytes != bytes)
        {
            std::printf("%s: the export doesn't give the ROM back\n", rom_name.c_str());
            totals.failed++;
        }

        const std::shared_ptr<const hokuto::song::GbaRom>& rom = game.rom;
        std::map<uint32_t, GbaPattern> patterns;
        game.groups = hokuto::song::GbaVoiceGroups(*rom);
        game.spans = SpansOf(*rom, game.groups);
        const std::map<uint32_t, Place> places = PlacesOf(*rom, game.groups, patterns, totals);
        totals.voices += places.size();
        totals.patterns += patterns.size();
        if (places.empty())
        {
            return;
        }
        std::vector<uint32_t> samples;
        for (const hokuto::song::GbaVoiceGroup& group : game.groups)
        {
            for (const hokuto::app::GbaSample& sample : hokuto::app::MakeGbaBankView(*rom, group.address).samples)
            {
                samples.push_back(sample.address);
            }
        }
        std::ranges::sort(samples);
        samples.erase(std::ranges::unique(samples).begin(), samples.end());

        if (only_songs)
        {
            SongValues(game, totals);
            return;
        }
        if (only_new)
        {
            // A round that fails leaves the others to run: a model that refuses a ROM's mixer stops each at its first
            // note.
            const auto run = [&](auto&& round)
            {
                try
                {
                    round();
                }
                catch (const std::exception& error)
                {
                    std::printf("%s: %s\n", rom_name.c_str(), error.what());
                    totals.failed++;
                }
            };
            run([&] { NewKinds(game, places, totals); });
            run([&] { NewSamples(game, places, totals); });
            run([&] { NewVoices(game, samples, totals); });
            return;
        }

        Round(game, "PSG envelopes", PsgEnvelopes(places, totals), {}, totals);
        Round(game, "PSG settings", PsgSettings(places, totals), {}, totals);
        Round(game, "patterns", {}, Flipped(patterns), totals);
        Round(game, "samples", Samples(places, samples), {}, totals);
        Round(game, "drums", Drums(places), {}, totals);
        Levels(game, places, totals);
    }
    catch (const std::exception& error)
    {
        std::printf("%s: %s\n", rom_name.c_str(), error.what());
        totals.failed++;
    }
}

} // namespace

} // namespace hokuto::tools::gba_voices

namespace
{

using hokuto::tools::gba_voices::CheckRom;
using hokuto::tools::gba_voices::Totals;

} // namespace

int main(int argc, char** argv)
{
    const std::vector<std::string> arguments = xsflib::console::Arguments(argc, argv);
    const bool only_new = arguments.size() > 1 && arguments[1] == "--new";
    const bool only_songs = arguments.size() > 1 && arguments[1] == "--songs";
    const std::size_t first = only_new || only_songs ? 2 : 1;
    if (arguments.size() <= first)
    {
        std::fprintf(stderr, "usage: hokuto_check_gba_voices [--new | --songs] <rom.gba>...\n");
        return 2;
    }

    Totals totals;
    for (std::size_t i = first; i < arguments.size(); i++)
    {
        CheckRom(xsflib::PathFromUtf8(arguments[i]), only_new, only_songs, totals);
    }
    if (only_songs)
    {
        std::printf(
            "%zu ROMs: %zu songs edited, %zu numbers read. %zu reverbs changed the samples, %zu didn't. %zu "
            "failures.\n",
            totals.roms, totals.songs_edited, totals.numbers_read, totals.reverbs_heard, totals.reverbs_unheard,
            totals.failed);
        return totals.failed > 0 ? 1 : 0;
    }
    if (only_new)
    {
        std::printf(
            "%zu ROMs, %zu voice groups, %zu voices. Kinds: %zu edits, %zu refused, %zu notes sounded, %zu keys that "
            "another row's edit moved, %zu without room. Samples: %zu "
            "came back, %zu notes played the same. Apart: %zu compressed, %zu with an odd loop, %zu without a whole "
            "rate, %zu with another byte after the last, %zu that another row's edit moved, %zu in filler runs, %zu "
            "without room, %zu untried after it. New voices: %zu tables "
            "moved, %zu in place, %zu filled by "
            "voices that no song selects, %zu without programs, %zu without room, %zu songs and %zu copies played the "
            "same. Key rows: %zu tables written again, %zu rows "
            "played the same, %zu single voices became a key split and a drum kit, %zu without room. %zu failures.\n",
            totals.roms, totals.groups, totals.voices, totals.edits, totals.refused, totals.kinds_sounded,
            totals.kinds_other_rows, totals.kinds_no_room, totals.new_samples, totals.notes_compared, totals.compressed,
            totals.odd_loops, totals.odd_rates, totals.other_ends, totals.other_rows, totals.in_filler, totals.no_room,
            totals.samples_untried, totals.groups_moved, totals.groups_in_place, totals.tables_filled,
            totals.groups_full, totals.voices_no_room, totals.songs_compared, totals.copies_compared,
            totals.tables_rewritten, totals.rows_compared, totals.made_splits, totals.rows_no_room, totals.failed);
        return totals.failed > 0 ? 1 : 0;
    }

    std::printf(
        "%zu ROMs, %zu voice groups, %zu voices, %zu patterns. %zu edits, %zu refused, %zu values kept past "
        "the PSG's range. %zu levels measured, the worst %.3f dB off the ratio of the sustains. Apart: %zu "
        "samples that end, %zu notes that never reach their sustain, %zu too quiet, %zu clipped, %zu notes that "
        "end before the measure. %zu failures.\n",
        totals.roms, totals.groups, totals.voices, totals.patterns, totals.edits, totals.refused, totals.kept,
        totals.levels, totals.worst, totals.ending, totals.slow, totals.quiet, totals.clipped, totals.ended,
        totals.failed);

    return totals.failed > 0 ? 1 : 0;
}
