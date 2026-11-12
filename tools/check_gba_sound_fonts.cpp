// SPDX-License-Identifier: MIT

// Checks File > Import > Bank… of a SoundFont 2 file on GBA ROMs that MusicPlayer2000 plays
// (src/edit/gba_sound_fonts.h). As the browser's Export… does, ntdlib's converter writes the voices that the songs of a
// voice group select as a SoundFont. The presets then go back into the same voice group through the import's edit, at
// the programs that no song selects.
//
// - Every song of the ROM has to play the voices that it played (song::GbaSongsWithOtherVoices).
// - The file comes back from its binary form and its text form, and produces the same ROM. The tool checks that for the
//   first voice group of each ROM that receives its presets: each form copies the ROM.
// - ntdlib's model of the console plays a note of each new voice and of the voice that it came from. The model plays
//   each note as the Bank Editor's list plays it: for 1 s, and for 0.5 s of the release. The tool prints how far the
//   new voice's level is from the old one's, and the residual of the two renders below the old one, by the kind of the
//   old voice. A sample's voice fails where the level is more than 3 dB off. The other kinds count apart: the SoundFont
//   contains a PSG voice as a recreation of its wave.
//
// With `--every n`, the tool selects every nth voice group of each ROM. With `--notes`, it prints a line for each note.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ntdlib/mp2k/song.h"
#include "ntdlib/sequence_export/mp2k/convert.h"
#include "xsflib/bytes.h"
#include "xsflib/console.h"
#include "xsflib/files.h"

#include "check_support.h"
#include "edit/gba_sound_fonts.h"
#include "edit/gba_voices.h"
#include "format/archive.h"
#include "format/file.h"
#include "format/text.h"
#include "song/gba.h"
#include "song/midi_export.h"
#include "song/preview.h"

namespace
{

namespace mp2k = ntdlib::mp2k;
using hokuto::format::File;

// The frames of a note that the tool compares: the 1 s that the note holds, and part of its release.
constexpr double kSeconds = 1.5;

// A new voice's level is this far from the old one's at most, in dB, for a sample's voice.
constexpr double kMostLevelOff = 3.0;

// A render below this level, in dB below full scale, has too little to compare.
constexpr double kFloor = -70.0;

// The kinds of voice that the tool counts apart.
enum Kind : std::size_t
{
    kSample,
    kPsg,
    kSplit,
    kDrums,
    kKinds,
};
constexpr std::array<const char*, kKinds> kKindNames = {"samples", "PSG voices", "key splits", "drum kits"};

struct Counts
{
    std::size_t notes = 0;
    std::size_t quiet = 0;    // too little to compare
    std::size_t silent = 0;   // the old voice sounds, and the new one doesn't
    std::size_t within_1 = 0; // the level, in dB
    std::size_t within_3 = 0;
    std::size_t beyond_3 = 0;
    std::size_t residual_30 = 0; // the residual, in dB below the old render
    std::size_t residual_20 = 0;
    std::size_t residual_10 = 0;
    std::size_t residual_more = 0;
    double worst_level = 0.0;
};

struct Totals
{
    std::size_t roms = 0;
    std::size_t groups = 0;
    std::size_t without_voices = 0;
    std::size_t presets = 0;
    std::size_t left_out = 0;
    std::size_t given_way = 0;
    std::size_t bytes = 0;
    std::size_t no_room = 0;
    std::size_t no_program = 0;
    std::size_t failed = 0;
    bool verbose = false;
    std::array<Counts, kKinds> counts;
};

// The left and right samples of a note of `program` of the voice group of song `song`.
std::vector<float> NoteOf(const std::shared_ptr<const hokuto::song::GbaRom>& rom, int song, int program)
{
    hokuto::song::GbaNotePreview preview(rom, song, program);
    const auto most = static_cast<std::size_t>(preview.SampleRate() * kSeconds) * 2;
    std::vector<float> samples;
    std::vector<float> block(2 * 1024);
    while (samples.size() < most)
    {
        const std::size_t got = preview.Render(block.data(), 1024);
        if (got == 0)
        {
            break;
        }
        samples.insert(samples.end(), block.begin(), block.begin() + static_cast<std::ptrdiff_t>(2 * got));
    }
    samples.resize(std::min(samples.size(), most));

    return samples;
}

double DecibelsOf(double power)
{
    return power > 0.0 ? 10.0 * std::log10(power) : -300.0;
}

double PowerOf(std::span<const float> samples)
{
    double sum = 0.0;
    for (const float sample : samples)
    {
        sum += double(sample) * double(sample);
    }

    return samples.empty() ? 0.0 : sum / static_cast<double>(samples.size());
}

Kind KindOf(const mp2k::Voice& voice)
{
    if (voice.type & mp2k::kVoiceDrumKit)
    {
        return kDrums;
    }
    if (voice.type & mp2k::kVoiceKeySplit)
    {
        return kSplit;
    }

    return voice.PsgChannel() != 0 ? kPsg : kSample;
}

void CompareNote(const std::string& name, const std::shared_ptr<const hokuto::song::GbaRom>& rom,
                 const std::shared_ptr<const hokuto::song::GbaRom>& built, int song, int old_program, int new_program,
                 Kind kind, Totals& totals)
{
    Counts& counts = totals.counts[kind];
    const std::vector<float> before = NoteOf(rom, song, old_program);
    const std::vector<float> after = NoteOf(built, song, new_program);
    counts.notes++;
    const double old_level = DecibelsOf(PowerOf(before));
    const double new_level = DecibelsOf(PowerOf(after));
    if (old_level < kFloor)
    {
        counts.quiet++;
        return;
    }
    if (new_level < kFloor)
    {
        counts.silent++;
        std::printf("%s: program %d plays at %.1f dB, and its preset at program %d is silent\n", name.c_str(),
                    old_program, old_level, new_program);
        totals.failed += kind == kSample ? 1 : 0;
        return;
    }

    const double off = new_level - old_level;
    counts.within_1 += std::abs(off) <= 1.0 ? 1 : 0;
    counts.within_3 += std::abs(off) > 1.0 && std::abs(off) <= kMostLevelOff ? 1 : 0;
    counts.beyond_3 += std::abs(off) > kMostLevelOff ? 1 : 0;
    counts.worst_level = std::abs(off) > std::abs(counts.worst_level) ? off : counts.worst_level;

    std::vector<float> difference(std::min(before.size(), after.size()));
    for (std::size_t i = 0; i < difference.size(); i++)
    {
        difference[i] = before[i] - after[i];
    }
    const double residual = DecibelsOf(PowerOf(difference)) - old_level;
    counts.residual_30 += residual <= -30.0 ? 1 : 0;
    counts.residual_20 += residual > -30.0 && residual <= -20.0 ? 1 : 0;
    counts.residual_10 += residual > -20.0 && residual <= -10.0 ? 1 : 0;
    counts.residual_more += residual > -10.0 ? 1 : 0;
    if (totals.verbose)
    {
        std::printf("%s: %s, program %d at program %d: level %.1f dB, %+.2f dB off, residual %.1f dB\n", name.c_str(),
                    kKindNames[kind], old_program, new_program, old_level, off, residual);
    }

    if (kind == kSample && std::abs(off) > kMostLevelOff)
    {
        std::printf("%s: program %d's preset at program %d plays %+.2f dB off, with a residual of %.1f dB\n",
                    name.c_str(), old_program, new_program, off, residual);
        totals.failed++;
    }
}

// Checks the import of the presets of `group`. With `forms`, the file also has to come back from its forms. `forms`
// turns false once the group receives its presets.
void CheckGroup(const std::string& rom_name, const File& file, const std::vector<uint8_t>& original,
                const std::shared_ptr<const hokuto::song::GbaRom>& rom, const hokuto::song::GbaVoiceGroup& group,
                bool& forms, Totals& totals)
{
    const std::string name = rom_name + ", " + hokuto::song::GbaVoiceGroupName(group.address);
    totals.groups++;
    if (group.songs.empty() || group.programs.empty())
    {
        totals.without_voices++;
        return;
    }

    ntdlib::sequence_export::BankConversion conversion;
    try
    {
        conversion = hokuto::song::ConvertVoiceGroup(*rom, group.address, group.programs, "Hokuto");
    }
    catch (const hokuto::song::BankWithoutInstruments&)
    {
        totals.without_voices++;
        return;
    }
    if (conversion.sound_fonts.size() != 1)
    {
        std::printf("%s: the conversion holds %zu SoundFonts\n", name.c_str(), conversion.sound_fonts.size());
        totals.failed++;
        return;
    }

    const hokuto::edit::GbaSoundFontVoices voices =
        hokuto::edit::GbaVoicesOfSoundFont(conversion.sound_fonts[0].sf2, rom->info);
    const std::vector<uint32_t> free = hokuto::edit::FreeGbaPrograms(*rom, group.address);
    const std::vector<uint32_t> unplayed = hokuto::edit::UnplayedGbaPrograms(*rom, group.address);
    const std::vector<std::optional<uint32_t>> programs = hokuto::edit::GbaSoundFontPrograms(voices, free, unplayed);
    hokuto::edit::GbaGroupVersion version;
    try
    {
        version = hokuto::edit::WithGbaSoundFont(file, *rom, group.address, voices);
    }
    catch (const std::invalid_argument& error)
    {
        // Only a lack of room or of a free program is expected. Any other refusal of the import fails.
        std::printf("%s: %s\n", name.c_str(), error.what());
        if (hokuto::tools::LacksRoom(error))
        {
            totals.no_room++;
        }
        else if (std::string_view(error.what()).ends_with("None is free for the import."))
        {
            totals.no_program++;
        }
        else
        {
            totals.failed++;
        }
        return;
    }
    totals.presets += voices.presets.size();
    totals.bytes += voices.bytes;

    const std::vector<uint8_t> exported = hokuto::format::ExportArchive(version.file);
    if (forms &&
        (hokuto::format::ExportArchive(hokuto::format::Read(hokuto::format::Write(version.file))) != exported ||
         hokuto::format::ExportArchive(hokuto::format::ReadText(hokuto::format::WriteText(version.file))) != exported))
    {
        std::printf("%s: the file doesn't come back from its forms\n", name.c_str());
        totals.failed++;
    }
    forms = false;

    const std::shared_ptr<const hokuto::song::GbaRom> built = hokuto::song::LoadGbaRom(exported);
    const std::vector<int> others = hokuto::song::GbaSongsWithOtherVoices(*rom, *built);
    if (!others.empty())
    {
        std::printf("%s: %zu songs play other voices, such as song %d\n", name.c_str(), others.size(), others.front());
        totals.failed++;
    }
    const std::vector<uint8_t> unchanged = hokuto::format::ExportArchive(file);
    if (unchanged != original)
    {
        std::printf("%s: the edit changed the file that it started from\n", name.c_str());
        totals.failed++;
    }

    const int song = group.songs.front();
    for (std::size_t i = 0; i < voices.presets.size(); i++)
    {
        if (!programs[i])
        {
            totals.left_out++;
            continue;
        }
        totals.given_way += std::ranges::find(unplayed, *programs[i]) != unplayed.end() ? 1 : 0;

        const int old_program = voices.presets[i].number;
        mp2k::Voice voice;
        if (!mp2k::ReadVoice(rom->rom, group.address + 12 * static_cast<uint32_t>(old_program), voice))
        {
            continue;
        }
        if (totals.verbose)
        {
            mp2k::Voice made;
            mp2k::ReadVoice(built->rom, version.group + 12 * *programs[i], made);
            const hokuto::edit::GbaSoundFontRow& row = voices.presets[i].rows.front();
            const std::vector<uint8_t>& sample = voices.samples.at(row.sample);
            const auto word = [&sample](std::size_t at)
            {
                return xsflib::LoadLe32(sample.data() + at);
            };
            mp2k::Wave wave;
            const bool has_wave = voice.PsgChannel() == 0 && mp2k::ReadWave(rom->rom, voice.wave, wave);
            std::printf(
                "%s: program %d: type %02X key %d pan %02X env %d %d %d %d, wave rate %u size %u loop %u; new: "
                "type %02X, %zu rows, first row key %d pan %02X env %d %d %d %d, sample rate %u size %u loop %u\n",
                name.c_str(), old_program, voice.type, voice.key, voice.pan_sweep, voice.attack, voice.decay,
                voice.sustain, voice.release, has_wave ? wave.frequency : 0, has_wave ? wave.size : 0,
                has_wave && wave.loops ? wave.loop_start : 0, made.type, voices.presets[i].rows.size(), row.voice.key,
                row.voice.pan_sweep, row.voice.attack, row.voice.decay, row.voice.sustain, row.voice.release, word(4),
                word(12), word(8));
        }
        CompareNote(name, rom, built, song, old_program, static_cast<int>(*programs[i]), KindOf(voice), totals);
    }
}

void CheckRom(const std::filesystem::path& path, std::size_t every, Totals& totals)
{
    const std::string name = xsflib::Utf8(path.filename());
    std::vector<uint8_t> bytes;
    if (!xsflib::ReadFile(path, bytes))
    {
        std::printf("%s: can't be read\n", name.c_str());
        totals.failed++;
        return;
    }

    // A ROM that can't be imported or exported counts as a failure.
    File file;
    std::shared_ptr<const hokuto::song::GbaRom> rom;
    std::vector<uint8_t> original;
    try
    {
        file = hokuto::format::ImportGbaRom(bytes);
        rom = hokuto::song::LoadGbaRom(bytes);
    }
    catch (const xsflib::FormatError&)
    {
        return; // no MP2K, or no song that plays
    }
    catch (const std::exception& error)
    {
        std::printf("%s: can't be imported: %s\n", name.c_str(), error.what());
        totals.failed++;
        return;
    }
    totals.roms++;
    try
    {
        original = hokuto::format::ExportArchive(file);
    }
    catch (const std::exception& error)
    {
        std::printf("%s: can't be exported: %s\n", name.c_str(), error.what());
        totals.failed++;
        return;
    }

    const std::vector<hokuto::song::GbaVoiceGroup> groups = hokuto::song::GbaVoiceGroups(*rom);
    bool forms = true;
    for (std::size_t i = 0; i < groups.size(); i += every)
    {
        try
        {
            CheckGroup(name, file, original, rom, groups[i], forms, totals);
        }
        catch (const std::exception& error)
        {
            std::printf("%s, %s: %s\n", name.c_str(), hokuto::song::GbaVoiceGroupName(groups[i].address).c_str(),
                        error.what());
            totals.failed++;
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    const std::vector<std::string> arguments = xsflib::console::Arguments(argc, argv);
    std::size_t first = 1;
    std::size_t every = 1;
    if (arguments.size() > 2 && arguments[1] == "--every")
    {
        every = std::max<std::size_t>(std::stoul(arguments[2]), 1);
        first = 3;
    }
    Totals totals;
    if (arguments.size() > first && arguments[first] == "--notes")
    {
        totals.verbose = true;
        first++;
    }
    if (arguments.size() <= first)
    {
        std::fprintf(stderr, "usage: hokuto_check_gba_sound_fonts [--every n] [--notes] <rom.gba>...\n");
        return 2;
    }

    for (std::size_t i = first; i < arguments.size(); i++)
    {
        CheckRom(xsflib::PathFromUtf8(arguments[i]), every, totals);
    }

    std::printf(
        "%zu ROMs, %zu voice groups, %zu without voices, %zu without room, %zu without a free program. %zu presets, "
        "%zu bytes, %zu stayed out for want of a program, %zu took a voice's place.\n",
        totals.roms, totals.groups, totals.without_voices, totals.no_room, totals.no_program, totals.presets,
        totals.bytes, totals.left_out, totals.given_way);
    for (std::size_t kind = 0; kind < kKinds; kind++)
    {
        const Counts& counts = totals.counts[kind];
        std::printf(
            "%s: %zu notes, %zu too quiet to compare, %zu silent. Levels: %zu within 1 dB, %zu within 3 dB, %zu "
            "beyond, the worst %+.2f dB. Residuals: %zu at -30 dB or less, %zu to -20 dB, %zu to -10 dB, %zu "
            "above.\n",
            kKindNames[kind], counts.notes, counts.quiet, counts.silent, counts.within_1, counts.within_3,
            counts.beyond_3, counts.worst_level, counts.residual_30, counts.residual_20, counts.residual_10,
            counts.residual_more);
    }
    std::printf("%zu failures.\n", totals.failed);

    return totals.failed > 0 ? 1 : 0;
}
