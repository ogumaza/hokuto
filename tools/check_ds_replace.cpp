// SPDX-License-Identifier: MIT

// Checks Replace… on the sound archives of DS cartridge images (src/edit/ds_files.h and the DS's parts of
// src/edit/ds_waves.h and src/edit/files.h). The tool imports each sound archive of an image (format::DsArchivesIn)
// into a Hokuto file the way the program does (format::ImportDsImage). Its rounds:
//
// - Back: each sequence, bank and wave archive is replaced with the bytes of the file that Export… writes, in one
//   version of the file. The export has to give the game's archive byte for byte. The edit refuses a bank or a wave
//   archive whose file ntdlib can't read or shares bytes with another file of the FAT. Such a file counts apart.
// - Next: each file is replaced with the next file of its kind, in one version. Every file of the export has to hold
//   the bytes of its new file, and the files have to keep their order without overlaps. The archive's header and its
//   FILE block have to give the sizes. ntdlib's readers have to read each new file. The file has to come back from both
//   its forms. Each file then gets its old bytes back, and the export has to reproduce the game's archive byte for
//   byte. The edit refuses a bank or a wave archive whose file or new file ntdlib can't read, or whose file shares
//   bytes with another file. An archive that shares its file with other data refuses a new size too. Such a file counts
//   apart.
// - Waves: each wave is imported again from the wave file that Export… writes. The wave file has to contain the bytes
//   of the wave in the file of its wave archive, and that file has to come back byte for byte. A wave whose parameters
//   and samples (ntdlib::sdat::WaveBytes) differ from its bytes in the file counts apart. A wave archive that the
//   tools' layout doesn't give back refuses, and counts apart. So does a wave archive that ntdlib can't read. Each wave
//   also comes from the WAV file that Export… writes: in 16-bit PCM, the new wave has to decode to the wave's samples
//   with the wave's loop, and in the wave's encoding the tool measures the signal-to-noise ratio itself. The ratio has
//   to equal the one that the Replace window shows. `--every n` selects every nth wave of the wave files and of the WAV
//   files.
// - Encodings: the first wave of each wave archive is converted to each other encoding through the edit of the
//   properties. The new wave has to keep the rate, the timer and the loop place of the wave, and 16-bit PCM its
//   samples. Every other wave of the wave archive has to keep its bytes. The edit refuses a wave of a wave archive that
//   the tools' layout doesn't give back, or whose file shares bytes with another file. It refuses a new size in an
//   archive that shares its file with other data. It also refuses a loop that would start past the 65,535 words of its
//   field in the new encoding. Such a wave counts apart.
// - New waves: the first wave of each wave archive is added again after the waves, in one version. Each wave archive
//   has to hold its waves as before, and the new one last. An archive that shares its file with other data refuses the
//   growth, and counts apart. So does a wave archive whose file shares bytes with another file.
// - Plays: the waves of every wave archive are converted to 16-bit PCM. The samples stay. The sound unit starts
//   IMA-ADPCM 8 of the wave's samples later than PCM (ntdlib::ntr::Sound). A wave from IMA-ADPCM therefore starts with
//   8 zeros. The wave archives that the banks of every 32nd sound name take their new waves, in one version. Each such
//   sound plays for 3 s on the console's model before and after the edit, and the output has to stay sample for sample.
//   A sound whose files then don't fit the DS's memory counts apart.
// - New items: up to 4 sequence sounds are added again through the edits of File > Import, in one version: each wave
//   archive of the sound's bank, the bank, and a sound that plays the new bank. Every other item has to keep its
//   record, its name and its number, and every other file its bytes. Each table of the INFO block has to grow by its
//   new records alone. The file's sequences have to keep their numbers, and the file has to come back from both its
//   forms. Each new sound has to play the samples of its model for 3 s on the console's model. A model that can't play
//   before the edit counts as one that can't be read. An archive that the tools' layout doesn't give back refuses, and
//   counts apart.
// - Sound values: up to 32 sounds of the table of sequences and 32 of the sequence archives get the edits of the Sound
//   sections, in one version: half their volume, the next priorities, and the next bank and the next player that the
//   archive contains. The archive has to keep its size, with at most 6 other bytes for each sound. ntdlib has to read
//   the new values, and every other sound's values as before. The file has to come back from both its forms. In another
//   version, up to 8 sequence sounds at a volume of 64 or more take half their volume. Each plays for 3 s on the
//   console's model before and after the edit. Its level has to fall by the ratio of the volumes, within 0.5 dB. A
//   sound that plays below -50 dBFS, or reaches full scale, counts apart.
// - Streams: each stream's file has to come back byte for byte from its parsed fields. The stream is imported again in
//   its place, and the file has to come back unchanged. It then gets three quarters of its rate and a loop from the
//   block past its middle: only those fields of the HEAD block may differ. It is converted to the other encoding,
//   IMA-ADPCM or 16-bit PCM, and has to decode to its samples. Its WAV file comes back in both encodings. The loop has
//   to start at a block, at or after the WAV file's loop start. The samples up to the old end have to stay, and the
//   samples after it have to repeat those of the loop. 16-bit PCM has to hold them exactly. In IMA-ADPCM, the samples
//   up to the old end and those after it each have to be 15 dB or more above their noise. The tool reports the lowest
//   ratio. The stream is added as a new stream with its settings. Each other item and file has to stay, and each new
//   version has to come back from both its forms. An archive that shares its file in the image with other data refuses
//   a new size of a stream (edit::WithDsFile). The stream then counts apart. Every other refusal fails.
//
// The tool prints a line for each failure, and a summary. It returns 1 when an edit produces other bytes than it
// should, a read, a save or a render fails, or the edits refuse an edit for another reason than the round expects.
//
// `--new` runs only the round of new items, `--values` only the round of sound values, and `--streams` only the round
// of streams.
//
// usage: hokuto_check_ds_replace [--every n] [--new | --values | --streams] <image.nds>...

#include "check_ds_replace.h"

namespace
{

namespace song = hokuto::song;
using hokuto::tools::ds_replace::Back;
using hokuto::tools::ds_replace::Game;
using hokuto::tools::ds_replace::NewItems;
using hokuto::tools::ds_replace::Next;
using hokuto::tools::ds_replace::Plays;
using hokuto::tools::ds_replace::SoundValues;
using hokuto::tools::ds_replace::Streams;
using hokuto::tools::ds_replace::Totals;
using hokuto::tools::ds_replace::Waves;

void CheckImage(const std::filesystem::path& path, uint32_t every, bool only_new, bool only_values, bool only_streams,
                Totals& totals)
{
    const std::string image_name = xsflib::Utf8(path);
    std::vector<uint8_t> image;
    if (!xsflib::ReadFile(path, image))
    {
        std::printf("%s: can't be read\n", image_name.c_str());
        totals.failed++;
        return;
    }

    try
    {
        const ntdlib::ntr::snd::Build build = song::DsBuildOf(image);
        for (const hokuto::format::DsArchivePlace& where : hokuto::format::DsArchivesIn(image))
        {
            Game game;
            game.name = image_name + ": " + where.path + " at " + std::to_string(where.offset);
            game.file = hokuto::format::ImportDsImage(image, where);
            game.shares_file = hokuto::format::DsArchiveSharesFile(game.file);
            game.bytes = hokuto::format::ExportArchive(game.file);
            game.archive = song::LoadDsArchive(game.bytes, build);
            totals.archives++;
            if (only_streams)
            {
                Streams(game, totals);
                std::fflush(stdout);
                continue;
            }
            if (!only_new && !only_values)
            {
                Streams(game, totals);
                Back(game, totals);
                Next(game, totals);
                Waves(game, every, totals);
                Plays(game, totals);
            }
            if (!only_values)
            {
                NewItems(game, totals);
            }
            if (!only_new)
            {
                SoundValues(game, totals);
            }
            std::fflush(stdout);
        }
    }
    catch (const std::exception& error)
    {
        std::printf("%s: %s\n", image_name.c_str(), error.what());
        totals.failed++;
    }
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> arguments = xsflib::console::Arguments(argc, argv);
    uint32_t every = 1;
    if (arguments.size() >= 3 && arguments[1] == "--every")
    {
        every = static_cast<uint32_t>(std::max(1, std::stoi(arguments[2])));
        arguments.erase(arguments.begin() + 1, arguments.begin() + 3);
    }
    bool only_new = false;
    if (arguments.size() >= 2 && arguments[1] == "--new")
    {
        only_new = true;
        arguments.erase(arguments.begin() + 1);
    }
    bool only_values = false;
    if (arguments.size() >= 2 && arguments[1] == "--values")
    {
        only_values = true;
        arguments.erase(arguments.begin() + 1);
    }
    bool only_streams = false;
    if (arguments.size() >= 2 && arguments[1] == "--streams")
    {
        only_streams = true;
        arguments.erase(arguments.begin() + 1);
    }
    if (arguments.size() < 2)
    {
        std::fprintf(stderr,
                     "usage: hokuto_check_ds_replace [--every n] [--new | --values | --streams] <image.nds>...\n");
        return 2;
    }

    Totals totals;
    for (std::size_t i = 1; i < arguments.size(); i++)
    {
        CheckImage(xsflib::PathFromUtf8(arguments[i]), every, only_new, only_values, only_streams, totals);
    }

    std::printf(
        "%zu archives. Back: %zu sequence files, %zu banks, %zu wave archives, %zu apart. Next: %zu files, %zu apart. "
        "Waves: %zu, %zu wave files back, %zu apart, %zu wave archives laid out otherwise, %zu that can't be read. WAV "
        "files: %zu, %zu apart, %zu exact in their encodings, the lowest ratio %.1f dB. Encodings: %zu, %zu apart. New "
        "waves: %zu, %zu apart. Plays: %zu sounds the same in 16-bit PCM, %zu silent, %zu too large for memory, %zu "
        "that can't be read. New items: %zu sounds, %zu banks, %zu wave archives, %zu archives apart, %zu sounds "
        "played "
        "as their models, %zu silent, %zu too large for memory, %zu that can't be read. Sound values: %zu edits, %zu "
        "levels measured, the worst %.3f dB off the ratio of the volumes, %zu apart. Streams: %zu, %zu apart, %zu "
        "loops "
        "from their WAV files, %zu of them moved to a block, the lowest ratio %.1f dB. %zu failures.\n",
        totals.archives, totals.sequences, totals.banks, totals.wave_archives, totals.back_apart, totals.next_files,
        totals.next_apart, totals.waves, totals.wave_files_back, totals.wave_files_apart, totals.laid_otherwise,
        totals.unread_wave_archives, totals.wavs, totals.wavs_apart, totals.exact, totals.lowest_ratio,
        totals.encodings, totals.encodings_apart, totals.new_waves, totals.new_waves_apart, totals.played,
        totals.silent, totals.too_large, totals.sequence_apart, totals.new_sounds, totals.new_banks,
        totals.new_wave_archives, totals.new_apart, totals.new_played, totals.new_silent, totals.new_too_large,
        totals.new_unread, totals.value_edits, totals.levels, totals.worst_level, totals.levels_apart, totals.streams,
        totals.streams_apart, totals.stream_loops, totals.stream_loops_moved, totals.lowest_stream_ratio,
        totals.failed);

    return totals.failed > 0 ? 1 : 0;
}
