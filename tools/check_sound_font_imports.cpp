// SPDX-License-Identifier: MIT

// Checks File > Import > Bank… of a SoundFont 2 file (src/edit/sound_font_imports.h) and of SFZ files
// (src/edit/sfz_imports.h) on sound archives of the 3DS, the Wii U, the Switch, the Wii and the DS. Each bank of an
// archive is exported as the browser's Export… writes it. Its SoundFont, or with `--sfz` its SFZ files, then come back
// in as new banks, through the import's edit:
//
// - The file saves and opens again in both forms.
// - The old banks play as they played. The tool checks the first program of the bank.
// - The console's model plays a note of each new instrument and of the instrument that it came from. The browser plays
//   the notes the same way. The tool prints how far the new note's level is from the old one's. A note fails where it
//   stays silent and the old one sounds, and where the level is more than 6 dB off. A note from 1 dB to 6 dB off counts
//   apart, with a line for each from 3 dB. A note that sounds where the old one stays silent counts apart too, with a
//   line.
//
// A refusal of the import fails with a line. An archive that accepts no new item, such as one without names, refuses
// each bank. The tool prints the reason once, and counts those refusals apart. So does an archive cut short. SFZ files
// whose regions or samples a SoundFont can't hold count apart too, and so does a refusal of the export. So do SFZ files
// without a region that plays a sample, where each program of the old bank stays silent. A file of another kind, such
// as a GBA ROM, fails with a line.
//
// With `--every n`, the tool checks every nth bank of each archive. With `--notes`, it prints a line for each note.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/sequence_export/convert.h"
#include "xsflib/console.h"
#include "xsflib/files.h"

#include "checked_archive.h"
#include "checked_saves.h"
#include "edit/ds_new_items.h"
#include "edit/new_items.h"
#include "edit/sfz_imports.h"
#include "edit/sound_font_imports.h"
#include "song/ds.h"
#include "song/items.h"
#include "song/midi_export.h"
#include "song/preview.h"
#include "synth/sound_font.h"

namespace
{

namespace export_ = ntdlib::sequence_export;
namespace edit = hokuto::edit;
namespace format = hokuto::format;
namespace song = hokuto::song;
namespace synth = hokuto::synth;
namespace tools = hokuto::tools;

// The level below which a render counts as silent, in dB below full scale.
constexpr double kSilent = -80.0;
constexpr double kNearlySilent = -70.0;
constexpr double kFails = 6.0;
constexpr double kListed = 3.0;
constexpr double kClose = 1.0;

// A note lasts its hold and its release at most.
constexpr double kNoteSeconds = 1.5;

// The words of the refusals of SFZ files whose regions or samples a SoundFont can't hold (edit::SoundFontOfSfz).
constexpr std::string_view kBeyondSoundFont = "than a SoundFont can contain.";

// The refusal of SFZ files without a region that plays a sample (edit::SoundFontOfSfz).
constexpr std::string_view kWithoutSamples = "The SFZ file has no regions with samples.";

struct Totals
{
    std::size_t archives = 0;
    std::size_t banks = 0;
    std::size_t empty_banks = 0;
    std::size_t refused = 0;
    std::size_t new_banks = 0;
    std::size_t notes = 0;
    std::size_t both_silent = 0;
    std::size_t only_new = 0; // notes that sound where the old ones stay silent
    std::size_t within_1 = 0;
    std::size_t within_6 = 0;
    std::size_t failed = 0;
    double worst = 0.0;
    std::size_t regions = 0;
    std::map<std::string, std::size_t> refusals;
    std::map<std::string, std::size_t> warnings;
};

// The level of `preview`'s output in dB below full scale, over its first kNoteSeconds.
template <class Preview>
double LevelOf(Preview& preview)
{
    const auto frames = static_cast<std::size_t>(kNoteSeconds * preview.SampleRate());
    std::vector<float> samples(frames * 2);
    std::size_t done = 0;
    while (done < frames)
    {
        const std::size_t block = std::min<std::size_t>(1024, frames - done);
        const std::size_t rendered = preview.Render(samples.data() + done * 2, block);
        done += rendered;
        if (rendered < block)
        {
            break;
        }
    }
    double sum = 0.0;
    for (std::size_t i = 0; i < done * 2; i++)
    {
        sum += static_cast<double>(samples[i]) * static_cast<double>(samples[i]);
    }
    if (done == 0 || sum == 0.0)
    {
        return -std::numeric_limits<double>::infinity();
    }

    return 10.0 * std::log10(sum / static_cast<double>(done * 2));
}

std::size_t BankCount(const tools::CheckedArchive& archive)
{
    return archive.sar   ? archive.sar->Banks().size()
           : archive.wii ? archive.wii->Banks().size()
                         : archive.ds->archive.banks.size();
}

std::vector<uint32_t> ProgramsOf(const tools::CheckedArchive& archive, uint32_t bank)
{
    return archive.sar   ? song::ProgramsOf(*archive.sar, bank)
           : archive.wii ? song::ProgramsOf(*archive.wii, bank)
                         : song::DsBankPrograms(*archive.ds, bank);
}

export_::BankConversion Converted(const tools::CheckedArchive& archive, uint32_t bank,
                                  const std::vector<uint32_t>& programs)
{
    return archive.sar   ? song::ConvertBank(*archive.sar, bank, "Hokuto")
           : archive.wii ? song::ConvertBank(*archive.wii, bank, "Hokuto", archive.build)
                         : song::ConvertDsBank(*archive.ds, bank, programs);
}

// The level of a note of `program` of `bank` of `archive`.
double NoteLevel(const tools::CheckedArchive& archive, uint32_t bank, uint32_t program)
{
    if (archive.sar)
    {
        song::NotePreview preview(archive.sar, bank, program);
        return LevelOf(preview);
    }
    if (archive.wii)
    {
        song::NotePreview preview(archive.wii, bank, program, archive.build);
        return LevelOf(preview);
    }
    song::DsNotePreview preview(*archive.ds, bank, static_cast<int>(program));

    return LevelOf(preview);
}

// The SoundFont of the SFZ files of `conversion`, in folders as an export writes them. Fills in the regions.
edit::SfzSoundFont SoundFontOfConversion(const export_::BankConversion& conversion)
{
    const std::filesystem::path folder = "bank";
    std::map<std::filesystem::path, const std::string*> texts;
    std::map<std::filesystem::path, const std::vector<uint8_t>*> samples;
    std::vector<edit::SfzInstrument> instruments;
    for (const export_::SfzFile& file : conversion.sfz.files)
    {
        const std::filesystem::path path = folder / xsflib::PathFromUtf8(file.name);
        texts[path] = &file.text;
        const std::string stem = xsflib::Utf8(path.stem());
        instruments.push_back({path, static_cast<uint32_t>(std::stoul(stem.substr(stem.rfind('_') + 1)))});
    }
    for (const export_::SfzSample& sample : conversion.sfz.samples)
    {
        samples[std::filesystem::path(export_::kSfzSamplesFolder) / xsflib::PathFromUtf8(sample.name)] = &sample.wav;
    }

    const auto read = [&texts, &samples](const std::filesystem::path& path, std::vector<uint8_t>& bytes)
    {
        const std::filesystem::path normal = path.lexically_normal();
        if (const auto text = texts.find(normal); text != texts.end())
        {
            bytes.assign(text->second->begin(), text->second->end());
            return true;
        }
        if (const auto sample = samples.find(normal); sample != samples.end())
        {
            bytes = *sample->second;
            return true;
        }

        return false;
    };

    return edit::SoundFontOfSfz(instruments, read);
}

// The SoundFonts that bring in the bank's instruments, one for each of the conversion's SoundFonts or one of its SFZ
// files. With SFZ files, each comes with the warnings of its reading and the count of its regions.
struct Incoming
{
    std::vector<uint8_t> sf2;
    std::vector<std::string> warnings;
    std::size_t regions = 0;
};

std::vector<Incoming> IncomingOf(const export_::BankConversion& conversion, bool sfz)
{
    std::vector<Incoming> incoming;
    if (sfz)
    {
        edit::SfzSoundFont font = SoundFontOfConversion(conversion);
        incoming.push_back({std::move(font.sf2), std::move(font.warnings), font.regions});
        return incoming;
    }
    for (const export_::BankSoundFont& font : conversion.sound_fonts)
    {
        incoming.push_back({font.sf2, {}, 0});
    }

    return incoming;
}

// Checks one SoundFont of bank `bank`.
void CheckFont(const std::string& label, const format::File& file, const tools::CheckedArchive& archive, uint32_t bank,
               const std::vector<uint32_t>& programs, const Incoming& incoming, bool notes, Totals& totals)
{
    const edit::SoundFontImport import = edit::ReadSoundFontImport(file, incoming.sf2);
    const std::vector<int>& midi_banks = import.midi_banks;
    for (const std::string& warning : import.warnings)
    {
        totals.warnings[warning]++;
    }
    for (const std::string& warning : incoming.warnings)
    {
        totals.warnings[warning]++;
    }
    totals.regions += incoming.regions;
    const format::File added = edit::AddSoundFont(file, incoming.sf2, "IMPORTED");
    totals.new_banks += midi_banks.size();
    if (!tools::SavesAndOpens(added, true))
    {
        std::printf("%s: the file with the new banks doesn't open as it was\n", label.c_str());
        totals.failed++;
    }
    const tools::CheckedArchive built = tools::BuildChecked(added, "imported");

    const std::size_t first_new = BankCount(archive);
    if (!programs.empty())
    {
        const double before = NoteLevel(archive, bank, programs.front());
        const double after = NoteLevel(built, bank, programs.front());
        if (before != after)
        {
            std::printf("%s: program %u of the old bank plays at %.2f dB, against %.2f dB before\n", label.c_str(),
                        programs.front(), after, before);
            totals.failed++;
        }
    }

    const synth::SoundFontData font = synth::ReadSoundFont(incoming.sf2);
    for (const synth::PresetRecord& preset : font.presets)
    {
        const uint32_t program = static_cast<uint32_t>(preset.bank) * 128 + static_cast<uint32_t>(preset.program);
        const auto midi_bank = std::ranges::find(midi_banks, preset.bank);
        if (midi_bank == midi_banks.end() || std::ranges::find(programs, program) == programs.end())
        {
            continue;
        }

        const auto new_bank =
            static_cast<uint32_t>(first_new + static_cast<std::size_t>(midi_bank - midi_banks.begin()));
        const double old_level = NoteLevel(archive, bank, program);
        const double new_level = NoteLevel(built, new_bank, static_cast<uint32_t>(preset.program));
        totals.notes++;
        if (notes)
        {
            std::printf("%s: program %u: %.2f dB, against %.2f dB\n", label.c_str(), program, new_level, old_level);
        }
        // A note at the floor on one side can fall just below it on the other.
        const bool new_silent = new_level < kSilent;
        const bool old_silent = old_level < kSilent;
        if ((old_silent && new_level < kNearlySilent) || (new_silent && old_level < kNearlySilent))
        {
            totals.both_silent++;
            continue;
        }

        if (old_silent)
        {
            std::printf("%s: program %u sounds at %.2f dB, and the old one stays silent\n", label.c_str(), program,
                        new_level);
            totals.only_new++;
            continue;
        }
        if (new_silent)
        {
            std::printf("%s: program %u stays silent, against %.2f dB\n", label.c_str(), program, old_level);
            totals.failed++;
            continue;
        }
        const double difference = new_level - old_level;
        if (std::abs(difference) > std::abs(totals.worst))
        {
            totals.worst = difference;
        }
        if (std::abs(difference) <= kClose)
        {
            totals.within_1++;
        }
        else if (std::abs(difference) <= kFails)
        {
            totals.within_6++;
            if (std::abs(difference) >= kListed)
            {
                std::printf("%s: program %u is %+.2f dB off\n", label.c_str(), program, difference);
            }
        }
        else
        {
            std::printf("%s: program %u is %+.2f dB off: too far\n", label.c_str(), program, difference);
            totals.failed++;
        }
    }
}

// Why the archive of `file` accepts no new bank, or nothing when it accepts one: an archive without names, or an
// archive of the Wii or the DS whose model can't be read. `archive` is the archive that `file` builds.
std::optional<std::string> AdditionsRefused(const format::File& file, const tools::CheckedArchive& archive)
{
    if (archive.wii)
    {
        return edit::WiiAdditionsRefused(file);
    }
    if (archive.ds)
    {
        return edit::DsAdditionsRefused(file);
    }
    if (!edit::HoldsNames(archive.sar->Bytes(), archive.sar->ReadOptions().flavor))
    {
        return edit::NoNamesReason("bank");
    }

    return std::nullopt;
}

// Whether each program of `programs` of bank `bank` of `archive` stays silent. Such a bank is exported as SFZ files
// without a region that plays a sample.
bool AllSilent(const tools::CheckedArchive& archive, uint32_t bank, const std::vector<uint32_t>& programs)
{
    return std::ranges::all_of(programs, [&](uint32_t program) { return NoteLevel(archive, bank, program) < kSilent; });
}

void CheckArchive(const std::filesystem::path& path, bool sfz, std::size_t every, bool notes, Totals& totals)
{
    const std::string name = xsflib::Utf8(path.filename());
    std::vector<uint8_t> bytes;
    if (!xsflib::ReadFile(path, bytes))
    {
        std::printf("%s: can't read the file\n", name.c_str());
        totals.failed++;
        return;
    }
    const tools::CheckedArchive opened = tools::OpenChecked(path, bytes);
    if (!opened.sar && !opened.wii && !opened.ds)
    {
        std::printf("%s: not a sound archive of the 3DS, the Wii U, the Switch, the Wii or the DS\n", name.c_str());
        totals.failed++;
        return;
    }

    const format::File file = tools::ImportChecked(bytes, opened);
    const tools::CheckedArchive archive = tools::BuildChecked(file, name);
    totals.archives++;

    const std::optional<std::string> takes_none = AdditionsRefused(file, archive);
    if (takes_none)
    {
        std::printf("%s: takes no new bank: %s\n", name.c_str(), takes_none->c_str());
    }
    // The tables of an archive cut short, such as a rip that is missing its end, may not read again after an addition.
    const bool cut_short = opened.sar && opened.sar->Truncated();
    if (cut_short)
    {
        std::printf("%s: the archive is cut short, and the import's refusals count apart\n", name.c_str());
    }

    const std::size_t banks = BankCount(archive);
    for (std::size_t bank = 0; bank < banks; bank += every)
    {
        const auto index = static_cast<uint32_t>(bank);
        const std::string label = name + ": bank " + std::to_string(bank);

        // The tool exports the bank.
        std::vector<uint32_t> programs;
        export_::BankConversion conversion;
        try
        {
            programs = ProgramsOf(archive, index);
            if (programs.empty())
            {
                totals.empty_banks++;
                continue;
            }

            conversion = Converted(archive, index, programs);
        }
        catch (const song::BankWithoutInstruments&)
        {
            totals.empty_banks++;
            continue;
        }
        catch (const std::invalid_argument& error)
        {
            totals.refused++;
            totals.refusals[error.what()]++;
            continue;
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s\n", label.c_str(), error.what());
            totals.failed++;
            continue;
        }

        totals.banks++;

        // Its SoundFonts come back in.
        try
        {
            for (const Incoming& incoming : IncomingOf(conversion, sfz))
            {
                CheckFont(label, file, archive, index, programs, incoming, notes, totals);
            }
        }
        catch (const std::invalid_argument& error)
        {
            const std::string_view what = error.what();
            if (takes_none || cut_short || what.find(kBeyondSoundFont) != std::string_view::npos ||
                (what == kWithoutSamples && AllSilent(archive, index, programs)))
            {
                totals.refused++;
                totals.refusals[error.what()]++;
            }
            else
            {
                std::printf("%s: the import refuses the bank: %s\n", label.c_str(), error.what());
                totals.failed++;
            }
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s\n", label.c_str(), error.what());
            totals.failed++;
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    const std::vector<std::string> arguments = xsflib::console::Arguments(argc, argv);
    bool sfz = false;
    bool notes = false;
    std::size_t every = 1;
    std::vector<std::filesystem::path> paths;
    for (std::size_t i = 1; i < arguments.size(); i++)
    {
        const std::string& argument = arguments[i];
        if (argument == "--sfz")
        {
            sfz = true;
        }
        else if (argument == "--notes")
        {
            notes = true;
        }
        else if (argument == "--every" && i + 1 < arguments.size())
        {
            every = std::max<std::size_t>(std::stoul(arguments[++i]), 1);
        }
        else
        {
            paths.push_back(xsflib::PathFromUtf8(argument));
        }
    }
    if (paths.empty())
    {
        std::fprintf(stderr, "usage: hokuto_check_sound_font_imports [--sfz] [--every n] [--notes] <archive>...\n");
        return 2;
    }

    Totals totals;
    for (const std::filesystem::path& path : paths)
    {
        try
        {
            CheckArchive(path, sfz, every, notes, totals);
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s\n", xsflib::Utf8(path.filename()).c_str(), error.what());
            totals.failed++;
        }
    }

    std::printf(
        "%zu archives: %zu banks went out, %zu hold no instruments, %zu refused. %zu new banks, %zu notes, %zu "
        "silent on both, %zu silent only in the old bank. %zu within 1 dB, %zu within 6 dB, worst %+.2f dB. %zu "
        "regions. %zu failed.\n",
        totals.archives, totals.banks, totals.empty_banks, totals.refused, totals.new_banks, totals.notes,
        totals.both_silent, totals.only_new, totals.within_1, totals.within_6, totals.worst, totals.regions,
        totals.failed);
    for (const auto& [text, count] : totals.refusals)
    {
        std::printf("refused %zu: %s\n", count, text.c_str());
    }
    for (const auto& [text, count] : totals.warnings)
    {
        std::printf("warned %zu: %s\n", count, text.c_str());
    }

    return totals.failed == 0 ? 0 : 1;
}
