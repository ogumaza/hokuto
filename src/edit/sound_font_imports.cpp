// SPDX-License-Identifier: MIT

#include "edit/sound_font_imports.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>

#include "ntdlib/rsar/bank_file.h"
#include "ntdlib/rsar/convert.h"
#include "ntdlib/rsar/wave_file.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/bank_file.h"
#include "ntdlib/sar/wave_file.h"
#include "ntdlib/sdat/formats.h"

#include "edit/ds_conversion.h"
#include "edit/ds_new_items.h"
#include "edit/files.h"
#include "edit/new_items.h"
#include "edit/other_console.h"
#include "edit/sound_font_banks.h"
#include "edit/sound_font_voices.h"
#include "format/archive.h"
#include "song/archive.h"
#include "synth/sound_font.h"

namespace hokuto::edit
{
namespace
{

namespace sar = ntdlib::sar;

// The console whose bank the SoundFont becomes first: the console of the file, or the Wii U for the Wii and the DS.
sar::Flavor FlavorOf(const format::File& file)
{
    return file.native->engine == format::Engine::kWii || file.native->engine == format::Engine::kDs
               ? sar::Flavor::kCafe
               : FlavorOf(*file.native);
}

// The presets of each MIDI bank that the bank can contain: its programs are those of MIDI, and MIDI bank 128 contains
// the drum kits of a SoundFont.
std::map<int, std::size_t> PresetsByMidiBank(std::span<const uint8_t> sf2)
{
    const synth::SoundFontData font = ReadSoundFontToImport(sf2);

    std::map<int, std::size_t> presets;
    for (const synth::PresetRecord& preset : font.presets)
    {
        if (preset.bank >= 0 && preset.bank <= 128 && preset.program >= 0 && preset.program < 128)
        {
            ++presets[preset.bank];
        }
    }
    if (presets.empty())
    {
        throw std::invalid_argument("The SoundFont has no presets.");
    }

    return presets;
}

// The waves of the wave archive of `bank` in the form that DsBankFilesOf and a bank of the Wii take.
template <typename Wave, typename Convert>
std::vector<std::optional<Wave>> SoundFontWavesOf(const SoundFontBank& bank, Convert convert)
{
    std::vector<std::optional<Wave>> waves;
    for (const std::vector<uint8_t>& wave : sar::WaveArchiveFile::Read(bank.wave_archive).waves)
    {
        waves.emplace_back(convert(sar::WaveFile::Read(wave)));
    }

    return waves;
}

std::vector<std::optional<ntdlib::rsar::WaveFile>> WiiWavesOf(const SoundFontBank& bank)
{
    return SoundFontWavesOf<ntdlib::rsar::WaveFile>(
        bank, [](const sar::WaveFile& wave) { return ntdlib::rsar::WiiWaveOf(wave); });
}

DsBankFiles DsFilesOf(const SoundFontBank& bank)
{
    // A SoundFont's samples hold one channel each.
    return DsBankFilesOf(sar::BankFile::Read(bank.bank),
                         SoundFontWavesOf<sar::WaveFile>(bank, [](const sar::WaveFile& wave) { return wave; }),
                         [](const MonoLevel&) { return 1.0; });
}

// The version that `file`'s new banks of the Wii take.
uint16_t WiiBankVersionOf(const format::File& file)
{
    return NewWiiFileVersion(*song::LoadWiiArchive(format::ExportArchive(file), "", ntdlib::rvl::snd::kRsarOptions),
                             FileKind::kBank);
}

} // namespace

bool IsSoundFont(std::span<const uint8_t> bytes)
{
    return bytes.size() >= 12 && std::memcmp(bytes.data(), "RIFF", 4) == 0 &&
           std::memcmp(bytes.data() + 8, "sfbk", 4) == 0;
}

bool IsSoundFontName(std::string_view name)
{
    constexpr std::string_view kExtension = ".sf2";
    return name.size() >= kExtension.size() &&
           std::ranges::equal(name.substr(name.size() - kExtension.size()), kExtension,
                              [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == b; });
}

SoundFontImport ReadSoundFontImport(const format::File& file, std::span<const uint8_t> sf2)
{
    if (!file.native || file.native->engine == format::Engine::kGba)
    {
        throw std::invalid_argument("Can't import a SoundFont into this file.");
    }

    SoundFontImport import;
    const uint16_t version = file.native->engine == format::Engine::kWii ? WiiBankVersionOf(file) : 0;
    std::size_t layered = 0;
    std::map<std::string, std::size_t> left_out;
    for (const auto& [midi_bank, presets] : PresetsByMidiBank(sf2))
    {
        import.midi_banks.push_back(midi_bank);
        import.presets += presets;
        const SoundFontBank bank = BankOfSoundFont(sf2, FlavorOf(file), midi_bank);
        layered += bank.layered;
        for (const auto& [what, count] : bank.left_out)
        {
            left_out[what] += count;
        }

        if (file.native->engine == format::Engine::kDs)
        {
            const DsBankFiles made = DsFilesOf(bank);
            import.bytes += made.bank.size() + made.waves.size();
            AddDsBankWarnings(made, true, import.warnings);
        }
        else if (file.native->engine == format::Engine::kWii)
        {
            const ntdlib::rsar::BankFile::Files files =
                WiiBankOfFile(bank.bank, FlavorOf(file), version, WiiWavesOf(bank)).Write();
            import.bytes += files.file.size() + files.wave_data.size();
        }
        else
        {
            import.bytes += bank.bank.size() + bank.wave_archive.size();
        }
    }

    // A warning that several banks repeat stays once.
    std::vector<std::string> warnings = SoundFontWarnings(layered, left_out);
    std::set<std::string> said;
    for (std::string& warning : import.warnings)
    {
        if (said.insert(warning).second)
        {
            warnings.push_back(std::move(warning));
        }
    }

    import.warnings = std::move(warnings);

    return import;
}

std::string SoundFontWaveArchiveName(const std::string& name)
{
    return "WARC_" + (name.starts_with("BANK_") ? name.substr(5) : name);
}

std::vector<std::string> SoundFontBankNames(const std::string& name, std::span<const int> midi_banks)
{
    std::vector<std::string> names;
    for (const int midi_bank : midi_banks)
    {
        names.push_back(midi_banks.size() == 1 ? name : name + "_" + std::to_string(midi_bank));
    }

    return names;
}

format::File AddSoundFont(const format::File& file, std::span<const uint8_t> sf2, const std::string& name,
                          std::span<const uint32_t> groups)
{
    if (!file.native || file.native->engine == format::Engine::kGba)
    {
        throw std::invalid_argument("Can't import a SoundFont into this file.");
    }

    std::vector<int> midi_banks;
    for (const auto& [midi_bank, presets] : PresetsByMidiBank(sf2))
    {
        midi_banks.push_back(midi_bank);
    }

    const std::vector<std::string> names = SoundFontBankNames(name, midi_banks);
    const uint16_t version = file.native->engine == format::Engine::kWii ? WiiBankVersionOf(file) : 0;
    // The wave archives of a DS archive, counted at the first bank. Each bank adds a wave archive at the end.
    std::size_t ds_wave_archives = 0;
    format::File result = file;
    for (std::size_t i = 0; i < midi_banks.size(); i++)
    {
        const SoundFontBank bank = BankOfSoundFont(sf2, FlavorOf(file), midi_banks[i]);
        if (file.native->engine == format::Engine::kDs)
        {
            if (i == 0)
            {
                ds_wave_archives = DsWaveArchiveCount(result);
            }

            const auto wave_archive = static_cast<uint16_t>(ds_wave_archives + i);
            const DsBankFiles made = DsFilesOf(bank);
            result = AddDsWaveArchive(result, SoundFontWaveArchiveName(names[i]), made.waves);
            result = AddDsBank(result, names[i], made.bank,
                               {wave_archive, ntdlib::sdat::kNoWaveArchive, ntdlib::sdat::kNoWaveArchive,
                                ntdlib::sdat::kNoWaveArchive});
        }
        else if (file.native->engine == format::Engine::kWii)
        {
            const ntdlib::rsar::BankFile::Files files =
                WiiBankOfFile(bank.bank, FlavorOf(file), version, WiiWavesOf(bank)).Write();
            result = AddWiiBank(result, names[i], files.file, files.wave_data, groups);
        }
        else
        {
            const auto wave_archive = static_cast<uint32_t>(result.native->tables->wave_archives.size());
            result = AddWaveArchive(result, SoundFontWaveArchiveName(names[i]), bank.wave_archive);
            result = AddBank(result, names[i], bank.bank, {{kSoundFontWaves, wave_archive}});
        }
    }

    return result;
}

} // namespace hokuto::edit
