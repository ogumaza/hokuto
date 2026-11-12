// SPDX-License-Identifier: MIT

// Checks the edits of banks, waves, streams and wave sounds through their models (src/edit/banks.h, src/edit/waves.h
// and src/edit/streams.h), on a sound archive of the 3DS, the Wii U or the Switch with the group files and the streams
// beside it. The tool imports the archive as a .hok file. The export has to give the game's archive and group files
// back byte for byte.
//
// The edits follow one another. Each one exports the file, and the next one edits the archive that the export builds.
// Before a bank's or a wave's edits, its file in that archive has to hold the game's bytes: the earlier edits leave it
// alone.
//
// Each bank that ntdlib reads goes through its model twice: with each region's volume halved, and with a copy of each
// instrument at a new program after the bank's programs. The exported bank has to read back as the model. Each program
// with an instrument then plays a note on ntdlib's model of the console for 1.5 s: before the edits, and after each.
// The browser plays a note the same way (song::NotePreview). A copy has to play the instrument's samples. With the
// volumes halved, the level has to fall by the ratio of the volumes of the region that the note plays, within 0.2 dB. A
// program that plays nothing, or plays below -60 dB, counts apart. So does the level of a region above full volume: the
// console limits its gains. The bank keeps its halved volumes for the later edits. A 3DS archive plays its notes on
// ntdlib's model of the DSP's audio firmware. On the 3DS, a note that peaks at half of full scale or more counts apart
// too: the DSP's limiter compresses it (kLimitedPeak). A bank whose file an earlier bank of the archive's list plays
// counts apart: the earlier bank's edits changed the file.
//
// Each wave of each wave archive inside the archive goes through its model three times. First at three quarters of its
// rate, with a loop from the place nearest its middle where the samples stay, up to three quarters of its length. Then
// from that version with its end back at the wave's length. Then from the game's wave in another encoding: PCM16 for a
// wave in DSP-ADPCM, and DSP-ADPCM for one in PCM. ntdlib then decodes each new wave as the console decodes it. The
// first edit has to keep the samples up to the new end, and the second all of them. In PCM16, every sample has to stay.
// In DSP-ADPCM, the tool measures the ratio of signal to noise. Decoded from the loop's context as the console decodes
// it, each later pass of a loop in DSP-ADPCM has to give the first pass's samples. The other encoding has to keep the
// rate, the channels and the loop. A loop in DSP-ADPCM may move forward to the next frame. The wave keeps the second
// edit for the later edits.
//
// Each stream that the file keeps goes through its values twice: at three quarters of its rate with a loop from the
// first block past its middle, and in the other encoding. ntdlib then decodes each new stream. The first edit has to
// keep every sample, and so does 16-bit PCM. The other encoding has to keep the rate, the channels and the loop. Its
// loop may move forward to a block. In DSP-ADPCM, the tool measures the ratio of signal to noise. Each later pass of a
// loop in DSP-ADPCM has to decode to the first pass's samples, after both edits. The file keeps the first edit. The
// properties of a stream refuse an AAC stream of the 3DS, and such a stream counts apart.
//
// Each wave sound whose values ntdlib reads takes a new pan, surround pan, pitch and sends where its file has places
// for them, and the next wave of the archive. ntdlib has to read the new values and the new wave back, and every other
// wave sound of the file has to keep its values.
//
// After the edits, every file of the archive but the banks, the wave archives, the wave sound files and the groups'
// files has to hold the game's bytes. The file with the edits has to come back from its binary and text forms.
//
// The tool prints a line for each problem, then a summary. It returns 1 when it finds a problem, and 2 when the archive
// can't be read.
//
// With `--part k/n`, the tool edits only part k of n of the items: the banks, the waves, the streams and the wave
// sounds whose places in their lists leave k - 1 over when divided by n. The edits of each part follow one another. n
// runs side by side cover a large archive faster than one run. Each run checks the archive's other files and both forms
// of the file too. With `--streams`, the tool edits only the streams, and with `--wave-sounds` only the wave sounds.
//
// An archive of the Wii (RSAR) takes the same edits through the Wii's models (ntdlib's rsar::BankFile and
// rsar::WsdFile). The tool imports it with the game's build of NW4R SND: from the main.dol that `--dol` names, or else
// from the main.dol of the extracted disc that holds the archive. Without either, the archive's version gives the
// build. The notes play on ntdlib's model of NW4R SND and AX with that build. A bank of the Wii keeps its waves in its
// wave data. A wave sound file does too. The waves of each such wave data take the edits of a wave archive's waves. A
// later pass of a loop decodes as the Wii's DSP decodes it (kWiiLoopDecoding). The bank's edits have to keep its waves,
// and the waves' edits the banks' instruments and the wave sounds' values. A bank whose regions hold values that the
// Bank Editor doesn't show, such as a surround pan, counts apart with the reason. Each wave sound also takes a new
// envelope, and its wave is the next wave of its file's wave data. The import reads the streams beside the archive.
// Each one takes the edits of the other consoles' streams through its stream file of the Wii (RSTM). A loop can start
// at the stream's first block. The edits have to keep the stream's tracks. After them, the file table of the exported
// archive has to give each stream's size. A file table that ntdlib's model of the archive can't read counts apart.
//
// With --model, the file goes into the model form after the import (format::InModelForm), and the archive that the
// model builds has to be the game's. Of the files that the edits make, the first 64 and every 64th after them have to
// save in both forms and open again as they were (tools/checked_archive.h). So does the file with all the edits.
//
// usage: hokuto_check_banks [--model] [--streams | --wave-sounds] [--part <k>/<n>] [--dol <main.dol>] <archive>

#include "check_banks.h"

namespace hokuto::tools::banks
{

bool Holds(const Part& part, std::size_t place)
{
    return place % part.n == part.k - 1;
}

void PrintSummary(const std::string& heading, const Totals& totals, double seconds)
{
    const std::string refused =
        totals.refused_banks > 0 ? ", " + std::to_string(totals.refused_banks) + " refused" : std::string();
    std::printf(
        "%s: %zu banks (%zu unread%s, %zu sharing a file), %zu programs (%zu quiet, %zu above full volume, %zu "
        "limited), levels of %zu within %.3f dB; %zu waves (%zu unread), %zu loops in DSP-ADPCM, %zu waves to "
        "DSP-ADPCM at "
        "%.1f dB or more (%zu below %.0f dB); %zu streams",
        heading.c_str(), totals.banks, totals.unread_banks, refused.c_str(), totals.shared_banks, totals.programs,
        totals.quiet_programs, totals.loud_programs, totals.limited_programs, totals.level_programs, totals.level_error,
        totals.waves, totals.unread_waves, totals.adpcm_loops, totals.adpcm_waves,
        std::isinf(totals.least_snr) ? 0.0 : totals.least_snr, totals.low_snr_waves, kLowSignalToNoise, totals.streams);
    if (!std::isinf(totals.least_stream_snr))
    {
        std::printf(" (DSP-ADPCM at %.1f dB or more)", totals.least_stream_snr);
    }
    if (totals.aac_streams > 0)
    {
        std::printf(", %zu AAC streams apart", totals.aac_streams);
    }
    std::printf("; %zu wave sounds (%zu unread); %zu other files; %zu problems; %.1f s\n", totals.wave_sounds,
                totals.unread_wave_sounds, totals.other_files, totals.problems, seconds);
}

namespace
{

// The options of a run, and the path of its archive.
struct BankOptions
{
    bool streams_only = false;
    bool wave_sounds_only = false;
    bool dol_given = false;
    std::string dol_path;
    Part part;
    std::string archive;
};

// The options of a run in `arguments`, or nothing when they don't fit the usage.
std::optional<BankOptions> ParseBankOptions(std::vector<std::string> arguments)
{
    hokuto::tools::TakeModelOption(arguments);
    const auto streams_flag = std::find(arguments.begin(), arguments.end(), "--streams");
    const bool streams_only = streams_flag != arguments.end();
    if (streams_only)
    {
        arguments.erase(streams_flag);
    }
    const auto wave_sounds_flag = std::find(arguments.begin(), arguments.end(), "--wave-sounds");
    const bool wave_sounds_only = wave_sounds_flag != arguments.end();
    if (wave_sounds_only)
    {
        arguments.erase(wave_sounds_flag);
    }
    std::string dol_path;
    const auto dol_option = std::find(arguments.begin(), arguments.end(), "--dol");
    const bool dol_given = dol_option != arguments.end();
    if (dol_given && dol_option + 1 != arguments.end())
    {
        dol_path = *(dol_option + 1);
        arguments.erase(dol_option, dol_option + 2);
    }
    std::optional<Part> part = Part{};
    std::size_t at = 1;
    if (arguments.size() == 4 && arguments[1] == "--part")
    {
        part = PartOf(arguments[2]);
        at = 3;
    }
    if (arguments.size() != at + 1 || !part || (dol_given && dol_path.empty()))
    {
        return std::nullopt;
    }

    return BankOptions{.streams_only = streams_only,
                       .wave_sounds_only = wave_sounds_only,
                       .dol_given = dol_given,
                       .dol_path = dol_path,
                       .part = *part,
                       .archive = arguments[at]};
}

// Checks each bank of `game` in the run's `part`, and returns the instruments of each bank whose volumes the check
// halved.
std::vector<std::pair<uint32_t, sar::BankFile>> CheckEveryBank(Edited& edited, const sar::SoundArchive& game,
                                                               const Part& part, const std::string& name,
                                                               Totals& totals)
{
    std::vector<std::pair<uint32_t, sar::BankFile>> halved;
    for (uint32_t bank = 0; bank < game.Banks().size(); ++bank)
    {
        if (Holds(part, bank))
        {
            if (std::optional<sar::BankFile> model = CheckBank(edited, game, bank, name, totals))
            {
                halved.emplace_back(bank, std::move(*model));
            }
        }
    }

    return halved;
}

// Checks the waves of each wave archive inside `game`, the streams and the wave sounds, of the run's `part`. The
// streams are left out with `wave_sounds_only`, and the wave sounds with `streams_only`. The waves are checked only
// with `items`.
void CheckWaveSoundsAndWaves(Edited& edited, const sar::SoundArchive& game, const Part& part, bool items,
                             bool streams_only, bool wave_sounds_only, const std::string& name, Totals& totals)
{
    // The wave sounds have to keep their values through the edits of the waves and the streams.
    std::vector<std::pair<uint32_t, edit::WaveSoundFields>> sound_fields;
    for (uint32_t sound = 0; sound < game.Sounds().size(); ++sound)
    {
        if (game.Sounds()[sound].type == sar::SoundType::kWaveSound)
        {
            try
            {
                sound_fields.emplace_back(sound, edit::WaveSoundFieldsOf(*edited.archive, sound));
            }
            catch (const std::exception&)
            {
                continue;
            }
        }
    }

    std::vector<sar::WaveId> waves;
    for (uint32_t wave_archive = 0; wave_archive < game.WaveArchives().size(); ++wave_archive)
    {
        std::size_t count = 0;
        try
        {
            count = song::WaveCountOf(game, wave_archive);
        }
        catch (const std::exception&)
        {
            continue;
        }
        for (uint32_t wave = 0; wave < count; ++wave)
        {
            waves.push_back({sar::ItemId(sar::kWaveArchiveItem, wave_archive), wave});
            if (Holds(part, waves.size() - 1) && items)
            {
                CheckWave(edited, game, wave_archive, wave, name, totals);
            }
        }
    }
    std::vector<uint32_t> streams;
    for (const auto& [file_id, stream] : edited.file.native->streams)
    {
        streams.push_back(file_id);
    }
    for (std::size_t i = 0; i < streams.size() && !wave_sounds_only; ++i)
    {
        if (Holds(part, i))
        {
            CheckStream(edited, streams[i], name, totals);
        }
    }
    for (const auto& [sound, fields] : sound_fields)
    {
        if (!SameFields(edit::WaveSoundFieldsOf(*edited.archive, sound), fields))
        {
            Problem(totals, name + ", " + game.Sounds()[sound].name,
                    "the edits of the waves and the streams changed its values");
        }
    }
    for (uint32_t sound = 0; sound < game.Sounds().size() && !waves.empty() && !streams_only; ++sound)
    {
        if (game.Sounds()[sound].type == sar::SoundType::kWaveSound && Holds(part, sound))
        {
            CheckWaveSound(edited, sound, waves, name, totals);
        }
    }
}

// The game's archive as it opens, and the Hokuto file that it makes, with the archive that the file builds.
struct BanksFile
{
    std::shared_ptr<const sar::SoundArchive> opened;
    Edited edited;
};

// Opens the archive at `path`, named `name`, in `bytes`, and imports it with the group files and the streams beside
// it. Throws where the archive can't be read.
BanksFile OpenBanksFile(const std::filesystem::path& path, const std::string& name, std::span<const uint8_t> bytes)
{
    BanksFile banks;
    banks.opened = song::OpenArchive(path);
    banks.edited.file = format::ImportArchive(bytes, banks.opened->ReadOptions().flavor, banks.opened->GroupFiles(),
                                              song::ReadStreams(*banks.opened, path), banks.opened->GroupFileGroups());
    banks.edited.file = hokuto::tools::CheckedForm(std::move(banks.edited.file), bytes);
    banks.edited.archive = Built(banks.edited.file, name);

    return banks;
}

// Runs the edits of `banks`, the archive `name` in `bytes`, on the run's `part`, and counts them in `totals`. `items`
// edits the banks and the waves too, and `streams_only` and `wave_sounds_only` take one kind alone.
void RunEdits(BanksFile& banks, const std::vector<uint8_t>& bytes, const Part& part, bool items, bool streams_only,
              bool wave_sounds_only, const std::string& name, Totals& totals)
{
    Edited& edited = banks.edited;
    const std::shared_ptr<const sar::SoundArchive>& opened = banks.opened;

    // The unedited archive comes back byte for byte.
    const std::shared_ptr<const sar::SoundArchive> game = edited.archive;
    if (format::ExportArchive(edited.file) != bytes || format::ExportGroupFiles(edited.file) != opened->GroupFiles())
    {
        Problem(totals, name, "the unedited archive doesn't come back byte for byte");
    }

    // The banks, the waves of each wave archive inside the archive, the streams and the wave sounds, of the run's part.
    const std::vector<std::pair<uint32_t, sar::BankFile>> halved =
        items ? CheckEveryBank(edited, *game, part, name, totals) : std::vector<std::pair<uint32_t, sar::BankFile>>();
    CheckWaveSoundsAndWaves(edited, *game, part, items, streams_only, wave_sounds_only, name, totals);

    // The banks keep their halved volumes, the other files keep their bytes, and the file comes back from both forms.
    for (const auto& [bank, model] : halved)
    {
        if (edit::BankModelOf(edited.file, *edited.archive, bank) != model)
        {
            Problem(totals, name + ", " + game->Banks()[bank].name, "the later edits changed the bank's instruments");
        }
    }
    CheckOtherFiles(edited, *game, name, totals);
    if (!hokuto::tools::SavesAndOpens(edited.file))
    {
        Problem(totals, name, "the file with the edits doesn't come back from both forms");
    }
}

} // namespace

int Run(std::vector<std::string> arguments)
{
    // The options, then the archive.
    const std::optional<BankOptions> options = ParseBankOptions(std::move(arguments));
    if (!options)
    {
        std::fprintf(stderr,
                     "usage: hokuto_check_banks [--model] [--streams | --wave-sounds] [--part <k>/<n>] "
                     "[--dol <main.dol>] <archive>\n");
        return 2;
    }

    // The game's archive. An archive of the Wii takes the checks of the Wii. The others are imported with the group
    // files and the streams beside them.
    const bool streams_only = options->streams_only;
    const bool wave_sounds_only = options->wave_sounds_only;
    const bool items = !streams_only && !wave_sounds_only;
    const Part& part = options->part;
    const std::filesystem::path path = xsflib::PathFromUtf8(options->archive);
    const std::string name = xsflib::Utf8(path.filename());
    const hokuto::tools::SavesLine saves_line(name);
    const auto began = std::chrono::steady_clock::now();
    std::vector<uint8_t> bytes;
    if (!xsflib::ReadFile(path, bytes))
    {
        std::fprintf(stderr, "%s can't be read\n", name.c_str());
        return 2;
    }
    if (IsWiiArchive(bytes))
    {
        return RunWii(path, bytes, options->dol_path, part, items, !streams_only, !wave_sounds_only);
    }
    if (options->dol_given)
    {
        std::fprintf(stderr, "%s: --dol takes an archive of the Wii\n", name.c_str());
        return 2;
    }
    BanksFile banks;
    try
    {
        banks = OpenBanksFile(path, name, bytes);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
        return 2;
    }
    Totals totals;
    RunEdits(banks, bytes, part, items, streams_only, wave_sounds_only, name, totals);

    // The summary, with the part where the run edits one.
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    const std::string heading =
        part.n > 1 ? name + " (part " + std::to_string(part.k) + " of " + std::to_string(part.n) + ")" : name;
    PrintSummary(heading, totals, seconds);

    return totals.problems == 0 ? 0 : 1;
}

} // namespace hokuto::tools::banks

namespace
{

using hokuto::tools::banks::Run;

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
