// SPDX-License-Identifier: MIT

// Checks the assembly of GBA songs (src/song/gba_assembly.h) on every song of GBA ROMs that MusicPlayer2000 plays. The
// tool writes each header's song as assembly, and reads the text back at the song's address. The bytes have to be those
// of the ROM, from the song's first track up to its header. The header's tracks, priority, reverb, count of blocks and
// voice group have to be the ROM's too. A read at another address has to decode to commands that give the ROM's bytes
// again at the song's address: an import does the same. A song that the writer refuses counts apart, with a line. Every
// song of a ROM then comes in from its text through File > Import > Sequence…'s edit, and the export has to give the
// ROM back byte for byte.
//
// Each sample that a voice group's listed programs reach, at every key of their tables, is exported in both of its
// forms (src/song/gba_samples.h). Its `.bin` form has to come back as the ROM's bytes, and its AIFF form with the ROM's
// samples, rate and loop. A header whose other bytes an AIFF file can't hold counts apart. Each voice group is then
// exported as assembly and imported again. A voice group that the writer refuses counts apart, with a line. The file
// has to give the ROM back byte for byte. With a mark in the names of its labels, the tables, key tables and patterns
// of the group are all written to the free space. Each listed program of the group that the export produces then has to
// contain the same voice, at every key, and the same pattern. A move that the edit refuses counts apart.
//
// With --assemble, LLVM's assembler also assembles every nth text, with the definitions of GbaAssemblyDefinitions, and
// LLVM's objcopy extracts the bytes of its section. Those bytes have to be the reader's at address 0, the voice group's
// word aside. An object file stores a word that names a label as the offset of the label in the section.
//
// With --project, the tool reads the files of a GBA project, such as a decompilation, against a ROM instead. Each `.s`
// file that contains a song has to contain a song of the ROM: its bytes, tracks, priority and reverb. A song without
// tracks counts apart. Each `.aif` file has to hold a sample of the ROM. A sample whose header or last sample differs
// counts apart, with a line. Files of other kinds stay out.
//
// usage: hokuto_check_gba_sources [--assemble <folder of llvm-mc and llvm-objcopy> <empty work folder> <n>]
// <rom.gba>...
//        hokuto_check_gba_sources --project <folder> <rom.gba>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/mp2k/commands.h"
#include "ntdlib/mp2k/driver.h"
#include "ntdlib/mp2k/song.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/console.h"
#include "xsflib/files.h"

#include "edit/gba_midi.h"
#include "edit/gba_voices.h"
#include "edit/replace.h"
#include "format/archive.h"
#include "format/file.h"
#include "song/gba.h"
#include "song/gba_assembly.h"
#include "song/gba_samples.h"

namespace
{

namespace mp2k = ntdlib::mp2k;

struct Options
{
    std::filesystem::path llvm;
    std::filesystem::path work;
    int every = 0;
};

struct Totals
{
    int roms = 0;
    int songs = 0;
    int headers = 0;
    int written = 0;
    int refused = 0;
    int assembled = 0;
    int imported = 0;
    int outside = 0; // songs whose commands stay among the ROM's bytes
    int project_files = 0;
    int project_matched = 0;
    int project_lacking = 0;
    int project_empty = 0;
    int samples = 0;
    int samples_compressed = 0;
    int samples_other_header = 0;
    int samples_empty = 0;
    int groups = 0;
    int groups_refused = 0;
    int groups_moved = 0;
    int groups_move_refused = 0;
    int groups_assembled = 0;
    int project_samples = 0;
    int project_samples_other = 0;
    int project_samples_lacking = 0;
    std::size_t bytes = 0;
    int failed = 0;
};

std::string Quoted(const std::filesystem::path& path)
{
    return "\"" + xsflib::Utf8(path) + "\"";
}

bool WriteText(const std::filesystem::path& path, const std::string& text)
{
    return !xsflib::WriteFile(path, std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
}

// The bytes of the section that LLVM's tools produce from `text`. Throws std::runtime_error where a tool fails.
std::vector<uint8_t> Assembled(const Options& options, const std::string& text)
{
    const std::filesystem::path source = options.work / "song.s";
    const std::filesystem::path object = options.work / "song.o";
    const std::filesystem::path section = options.work / "song.bin";
    if (!WriteText(source, text))
    {
        throw std::runtime_error("the text can't be written");
    }

    const std::string assemble = Quoted(options.llvm / "llvm-mc") + " -triple=armv4t-none-eabi -filetype=obj -I " +
                                 Quoted(options.work) + " -o " + Quoted(object) + " " + Quoted(source);
    if (std::system(assemble.c_str()) != 0)
    {
        throw std::runtime_error("llvm-mc refuses the text");
    }
    const std::string objcopy = Quoted(options.llvm / "llvm-objcopy") + " -O binary --only-section=.rodata " +
                                Quoted(object) + " " + Quoted(section);
    if (std::system(objcopy.c_str()) != 0)
    {
        throw std::runtime_error("llvm-objcopy refuses the object");
    }

    std::vector<uint8_t> bytes;
    if (!xsflib::ReadFile(section, bytes))
    {
        throw std::runtime_error("the section can't be read");
    }

    return bytes;
}

// Fails with `what`, with the song's place.
void Fail(const std::string& rom, int song, const std::string& what, Totals& totals)
{
    std::printf("%s: song %d: %s\n", rom.c_str(), song, what.c_str());
    totals.failed++;
}

// Text that names the first offset at which `a` and `b` differ.
std::string FirstDifference(std::span<const uint8_t> a, std::span<const uint8_t> b)
{
    const auto found = std::ranges::mismatch(a, b);
    return "at offset " + std::to_string(found.in1 - a.begin()) + ", of sizes " + std::to_string(a.size()) + " and " +
           std::to_string(b.size());
}

// The text of the song, or nothing for a song that the writer refuses.
std::optional<std::string> CheckSong(const std::string& name, const hokuto::song::GbaRom& rom, int song,
                                     const mp2k::SongHeader& header, const Options& options, Totals& totals)
{
    std::string text;
    try
    {
        text = hokuto::song::GbaSongAssembly(rom, song, hokuto::song::GbaSongName(rom, song));
    }
    catch (const std::invalid_argument& error)
    {
        std::printf("%s: song %d: the writer refuses it: %s\n", name.c_str(), song, error.what());
        totals.refused++;
        return std::nullopt;
    }
    totals.written++;

    const uint32_t base = *std::ranges::min_element(header.tracks);
    std::vector<uint8_t> expected;
    expected.reserve(header.address - base);
    for (uint32_t address = base; address < header.address; address++)
    {
        expected.push_back(rom.rom.U8(address));
    }
    totals.bytes += expected.size();

    try
    {
        const hokuto::song::GbaSongSource source = hokuto::song::ReadGbaAssembly(text, base);
        if (source.bytes != expected)
        {
            Fail(name, song, "the text's bytes differ " + FirstDifference(source.bytes, expected), totals);
        }
        if (source.entries != header.tracks)
        {
            Fail(name, song, "the text's tracks differ", totals);
        }
        if (source.priority != header.priority || source.reverb != header.reverb)
        {
            Fail(name, song, "the text's priority or reverb differs", totals);
        }
        if (source.blocks != rom.rom.U8(header.address + 1))
        {
            Fail(name, song, "the text's count of blocks differs", totals);
        }
        std::array<char, 32> group{};
        std::snprintf(group.data(), group.size(), "voicegroup_%08X", header.voices);
        if (source.voices_symbol != group.data() || source.voices.has_value())
        {
            Fail(name, song, "the text's voice group is " + source.voices_symbol, totals);
        }

        // An import decodes the text from another address, and encodes the commands at the song's new place.
        const hokuto::song::GbaSongSource away = hokuto::song::ReadGbaAssembly(text);
        const int note_arguments = mp2k::NoteArguments(rom.info);
        const mp2k::Decoded decoded = mp2k::DecodeSequence(away.bytes, away.base, away.entries, note_arguments);
        const mp2k::Encoded encoded = mp2k::EncodeSequence(decoded.sequence, base, note_arguments);
        if (decoded.opaque)
        {
            Fail(name, song, "the text decodes as data", totals);
        }
        else if (encoded.bytes != expected)
        {
            Fail(name, song, "the text's commands give other bytes " + FirstDifference(encoded.bytes, expected),
                 totals);
        }

        if (options.every > 0 && totals.written % options.every == 0)
        {
            const hokuto::song::GbaSongSource zero = hokuto::song::ReadGbaAssembly(text, 0);
            const std::vector<uint8_t> section = Assembled(options, text);
            totals.assembled++;

            // The section contains the bytes of the song, its padding and its header. The words of the tracks in the
            // header contain the offsets of the labels, and the word of the voice group contains 0.
            const std::size_t header_at = zero.bytes.size();
            const std::size_t end = header_at + 8 + 4 * header.tracks.size();
            bool same = section.size() >= end && std::equal(zero.bytes.begin(), zero.bytes.end(), section.begin()) &&
                        section[header_at] == header.tracks.size() && section[header_at + 1] == zero.blocks &&
                        section[header_at + 2] == zero.priority && section[header_at + 3] == zero.reverb;
            for (std::size_t t = 0; same && t < zero.entries.size(); t++)
            {
                const std::size_t at = header_at + 8 + 4 * t;
                const uint32_t word = xsflib::LoadLe32(section.data() + at);
                same = word == zero.entries[t];
            }
            if (!same)
            {
                Fail(name, song, "LLVM's bytes differ " + FirstDifference(section, zero.bytes), totals);
            }
        }
    }
    catch (const std::exception& error)
    {
        Fail(name, song, error.what(), totals);
    }

    return text;
}

// Reads an AIFF file, and looks for the bytes of its sample in the ROM. A sample that the project's tools rewrote can
// differ in its header or in the sample past the last. Such a file counts apart, with a line. A file whose samples the
// ROM doesn't hold fails, with a line.
void CheckProjectSample(const hokuto::song::GbaRom& rom, const std::filesystem::path& path, Totals& totals)
{
    const std::string name = xsflib::Utf8(path.stem());
    std::vector<uint8_t> file;
    if (!xsflib::ReadFile(path, file))
    {
        return;
    }
    try
    {
        const std::vector<uint8_t> read = hokuto::song::GbaSampleOfFile(file);
        const uint8_t* const start = rom.rom.Ptr(ntdlib::agb::kRomBase);
        const uint8_t* const end = start + rom.rom.Size();
        if (std::search(start, end, read.begin(), read.end()) != end)
        {
            totals.project_samples++;
        }
        else
        {
            const auto data = std::span(read).subspan(16, read.size() - 17);
            const bool samples = std::search(start, end, data.begin(), data.end()) != end;
            std::printf("%s: %s\n", name.c_str(),
                        samples ? "the ROM holds its samples with another header or last sample"
                                : "the ROM doesn't hold its samples");
            totals.project_samples_other += samples ? 1 : 0;
            totals.project_samples_lacking += samples ? 0 : 1;
        }
    }
    catch (const std::exception& error)
    {
        std::printf("%s: %s\n", name.c_str(), error.what());
        totals.failed++;
    }
}

// Reads each `.s` file under `folder` that contains a song, and looks for the song among those of the ROM at `path`.
// The file's bytes, tracks, priority and reverb have to be those of a song of the ROM. A song that isn't in the ROM
// fails with a line. A song without tracks counts apart. CheckProjectSample checks each `.aif` file. Files of other
// kinds stay out.
void CheckProject(const std::filesystem::path& path, const std::filesystem::path& folder, Totals& totals)
{
    std::vector<uint8_t> bytes;
    if (!xsflib::ReadFile(path, bytes))
    {
        std::printf("%s: can't be read\n", xsflib::Utf8(path).c_str());
        totals.failed++;
        return;
    }
    const std::shared_ptr<const hokuto::song::GbaRom> rom = hokuto::song::LoadGbaRom(std::move(bytes));

    std::map<std::size_t, std::vector<mp2k::SongHeader>> by_size;
    std::set<uint32_t> headers;
    for (const int song : hokuto::song::GbaSongs(*rom))
    {
        mp2k::SongHeader header;
        if (mp2k::ReadSong(rom->rom, rom->info, song, header) && headers.insert(header.address).second &&
            *std::ranges::min_element(header.tracks) < header.address)
        {
            by_size[header.address - *std::ranges::min_element(header.tracks)].push_back(std::move(header));
        }
    }

    for (const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(folder))
    {
        if (entry.path().extension() == ".aif")
        {
            CheckProjectSample(*rom, entry.path(), totals);
            continue;
        }
        if (entry.path().extension() != ".s")
        {
            continue;
        }
        std::vector<uint8_t> file;
        if (!xsflib::ReadFile(entry.path(), file))
        {
            continue;
        }
        const std::string text(file.begin(), file.end());
        const std::string name = xsflib::Utf8(entry.path().filename());
        if (text.find("NumTrks") == std::string::npos && text.find("NumBlks") == std::string::npos)
        {
            continue; // no song
        }
        totals.project_files++;

        try
        {
            const hokuto::song::GbaSongSource source = hokuto::song::ReadGbaAssembly(text);
            if (source.entries.empty())
            {
                totals.project_empty++; // the song table's songs without tracks don't play
                continue;
            }
            bool found = false;
            const auto sized = by_size.find(source.bytes.size());
            for (const mp2k::SongHeader& header :
                 sized != by_size.end() ? sized->second : std::vector<mp2k::SongHeader>())
            {
                const uint32_t base = *std::ranges::min_element(header.tracks);
                const hokuto::song::GbaSongSource placed = hokuto::song::ReadGbaAssembly(text, base);
                std::vector<uint8_t> expected;
                for (uint32_t address = base; address < header.address; address++)
                {
                    expected.push_back(rom->rom.U8(address));
                }
                if (placed.bytes == expected && placed.entries == header.tracks && placed.priority == header.priority &&
                    placed.reverb == header.reverb)
                {
                    found = true;
                    break;
                }
            }
            totals.project_matched += found ? 1 : 0;
            if (!found)
            {
                std::printf("%s: no song of the ROM holds these bytes (%zu bytes)\n", name.c_str(),
                            source.bytes.size());
                totals.project_lacking++;
            }
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s\n", name.c_str(), error.what());
            totals.failed++;
        }
    }
}

// Checks the two forms of the sample at `address`.
void CheckSample(const std::string& rom_name, const hokuto::song::GbaRom& rom, uint32_t address, Totals& totals)
{
    mp2k::Wave wave;
    if (!mp2k::ReadWave(rom.rom, address, wave))
    {
        return;
    }
    const auto fail = [&](const std::string& what)
    {
        std::printf("%s: the sample at 0x%08X: %s\n", rom_name.c_str(), address, what.c_str());
        totals.failed++;
    };

    if (wave.type != 0)
    {
        totals.samples_compressed++;
        try
        {
            hokuto::song::GbaSampleBin(rom, address);
            fail("a compressed sample goes out");
        }
        catch (const std::invalid_argument&)
        {
        }
        return;
    }

    // A row whose word lands before a real sample's header reads as a sample without samples. The driver plays nothing
    // there, and the Export window offers no file of it.
    if (wave.size == 0)
    {
        totals.samples_empty++;
        return;
    }

    totals.samples++;
    try
    {
        const std::vector<uint8_t> bin = hokuto::song::GbaSampleBin(rom, address);
        if (!hokuto::song::IsGbaSampleFile(bin) || hokuto::song::GbaSampleOfFile(bin) != bin)
        {
            fail("the .bin form doesn't come back");
        }

        const std::vector<uint8_t> aiff = hokuto::song::GbaSampleAiff(rom, address);
        if (!hokuto::song::IsGbaSampleFile(aiff))
        {
            fail("the AIFF form isn't a sample's file");
            return;
        }
        const std::vector<uint8_t> back = hokuto::song::GbaSampleOfFile(aiff);
        if (back == bin)
        {
            return;
        }

        // The samples, the rate and the loop have to stay.
        const auto word = [&back](std::size_t at)
        {
            return xsflib::LoadLe32(back.data() + at);
        };
        const bool loops = back.size() >= 16 && (back[3] & 0xC0) != 0;
        if (back.size() != bin.size() || !std::equal(back.begin() + 16, back.end(), bin.begin() + 16) ||
            word(4) != wave.frequency || word(12) != wave.size || loops != (wave.loops != 0) ||
            (loops && word(8) != wave.loop_start))
        {
            fail("the AIFF form gives other samples, another rate or another loop");
            return;
        }
        totals.samples_other_header++;
    }
    catch (const std::exception& error)
    {
        fail(error.what());
    }
}

// Whether `a` of `rom_a` and `b` of `rom_b` play alike at every key.
bool SameVoice(const hokuto::song::GbaRom& rom_a, const mp2k::Voice& a, const hokuto::song::GbaRom& rom_b,
               const mp2k::Voice& b, bool top)
{
    if (a.type != b.type || a.key != b.key || a.length != b.length || a.pan_sweep != b.pan_sweep)
    {
        return false;
    }
    if (a.IsSplit() && top)
    {
        for (int key = 0; key < 128; key++)
        {
            const uint32_t place_a = mp2k::SplitVoice(rom_a.rom, a, key);
            const uint32_t place_b = mp2k::SplitVoice(rom_b.rom, b, key);
            mp2k::Voice voice_a;
            mp2k::Voice voice_b;
            const bool read_a = place_a != 0 && mp2k::ReadVoice(rom_a.rom, place_a, voice_a);
            const bool read_b = place_b != 0 && mp2k::ReadVoice(rom_b.rom, place_b, voice_b);
            if (read_a != read_b || (read_a && !SameVoice(rom_a, voice_a, rom_b, voice_b, false)))
            {
                return false;
            }
        }
        return true;
    }
    if (a.IsSplit())
    {
        return a.wave == b.wave && a.key_table == b.key_table;
    }
    if (a.attack != b.attack || a.decay != b.decay || a.sustain != b.sustain || a.release != b.release)
    {
        return false;
    }
    if (a.PsgChannel() == 3 && top && rom_a.rom.Contains(a.wave, 16))
    {
        return rom_b.rom.Contains(b.wave, 16) &&
               std::equal(rom_a.rom.Ptr(a.wave), rom_a.rom.Ptr(a.wave) + 16, rom_b.rom.Ptr(b.wave));
    }

    return a.wave == b.wave;
}

// `text` with a mark in the names of its tables, key tables and patterns. The names then end in no address.
std::string WithMarkedLabels(std::string text)
{
    for (const std::string_view prefix : {"voicegroup_", "KeySplitTable_", "ProgrammableWaveData_"})
    {
        std::size_t at = 0;
        while ((at = text.find(prefix, at)) != std::string::npos)
        {
            text[at + prefix.size() - 1] = 'x';
            at += prefix.size();
        }
    }

    return text;
}

// `text` with the samples' names defined for LLVM's assembler. A project's files define the names the same way.
std::string WithSampleDefinitions(const std::string& text)
{
    static constexpr std::string_view kSample = "DirectSoundWaveData_";
    std::set<std::string> names;
    for (std::size_t at = text.find(kSample); at != std::string::npos; at = text.find(kSample, at + 1))
    {
        names.insert(text.substr(at, kSample.size() + 8));
    }
    std::string out;
    for (const std::string& name : names)
    {
        out += ".equ " + name + ", 0x" + name.substr(kSample.size()) + "\n";
    }

    return out + text;
}

// Checks the samples and the assembly of every voice group of `rom`. `file` and `bytes` are the ROM as a file and as
// bytes.
void CheckVoiceGroups(const std::string& name, const hokuto::song::GbaRom& rom, const hokuto::format::File& file,
                      std::span<const uint8_t> bytes, const Options& options, Totals& totals)
{
    std::set<uint32_t> samples;
    for (const hokuto::song::GbaVoiceGroup& group : hokuto::song::GbaVoiceGroups(rom))
    {
        const auto fail = [&](const std::string& what)
        {
            std::printf("%s: the voice group at 0x%08X: %s\n", name.c_str(), group.address, what.c_str());
            totals.failed++;
        };

        const std::vector<int> listed = hokuto::song::GbaListedPrograms(rom, group);
        for (const int program : listed)
        {
            mp2k::Voice voice;
            if (!mp2k::ReadVoice(rom.rom, group.address + 12 * static_cast<uint32_t>(program), voice))
            {
                continue;
            }
            if (!voice.IsSplit())
            {
                samples.insert(voice.PsgChannel() == 0 ? voice.wave : 0);
                continue;
            }
            for (int key = 0; key < 128; key++)
            {
                mp2k::Voice inner;
                const uint32_t place = mp2k::SplitVoice(rom.rom, voice, key);
                if (place != 0 && mp2k::ReadVoice(rom.rom, place, inner) && !inner.IsSplit() && inner.PsgChannel() == 0)
                {
                    samples.insert(inner.wave);
                }
            }
        }

        totals.groups++;
        std::string text;
        try
        {
            text = hokuto::song::GbaVoiceGroupAssembly(rom, group);
        }
        catch (const std::invalid_argument& error)
        {
            std::printf("%s: the voice group at 0x%08X: the writer refuses it: %s\n", name.c_str(), group.address,
                        error.what());
            totals.groups_refused++;
            continue;
        }

        try
        {
            const hokuto::edit::GbaGroupVersion same =
                hokuto::edit::WithGbaVoiceGroupFile(file, rom, group.address, text);
            if (same.group != group.address || !std::ranges::equal(hokuto::format::ExportArchive(same.file), bytes))
            {
                fail("the file gives another ROM");
            }

            if (options.every > 0 && totals.groups % options.every == 0)
            {
                const std::string defined = WithSampleDefinitions(text);
                const std::vector<uint8_t> section = Assembled(options, defined);
                const std::vector<uint8_t> read = hokuto::song::ReadGbaVoiceGroup(defined).bytes;
                totals.groups_assembled++;

                // An object file stores a word that names a label as the offset of the label, and the reader as the
                // address of the label.
                bool differs = section.size() != read.size();
                for (std::size_t at = 0; !differs && at + 4 <= read.size(); at += 4)
                {
                    const uint32_t word_section = xsflib::LoadLe32(section.data() + at);
                    const uint32_t word_read = xsflib::LoadLe32(read.data() + at);
                    differs = word_section != word_read && word_section + hokuto::song::kGbaAssemblyBase != word_read;
                }
                if (differs)
                {
                    fail("LLVM's bytes differ " + FirstDifference(section, read));
                }
            }

            const std::string marked = WithMarkedLabels(text);
            hokuto::edit::GbaGroupVersion moved;
            try
            {
                moved = hokuto::edit::WithGbaVoiceGroupFile(file, rom, group.address, marked);
            }
            catch (const std::invalid_argument& error)
            {
                totals.groups_move_refused++;
                std::printf("%s: the voice group at 0x%08X refuses the move: %s\n", name.c_str(), group.address,
                            error.what());
                continue;
            }
            totals.groups_moved++;
            const std::shared_ptr<const hokuto::song::GbaRom> after =
                hokuto::song::LoadGbaRom(hokuto::format::ExportArchive(moved.file));
            for (const int program : listed)
            {
                mp2k::Voice before_voice;
                mp2k::Voice after_voice;
                const auto at = 12 * static_cast<uint32_t>(program);
                if (!mp2k::ReadVoice(rom.rom, group.address + at, before_voice) ||
                    !mp2k::ReadVoice(after->rom, moved.group + at, after_voice) ||
                    !SameVoice(rom, before_voice, *after, after_voice, true))
                {
                    fail("program " + std::to_string(program) + " plays otherwise after the move");
                }
            }
        }
        catch (const std::exception& error)
        {
            fail(error.what());
        }
    }

    samples.erase(0);
    for (const uint32_t sample : samples)
    {
        CheckSample(name, rom, sample, totals);
    }
}

void CheckRom(const std::filesystem::path& path, const Options& options, Totals& totals)
{
    const std::string name = xsflib::Utf8(path);
    std::vector<uint8_t> bytes;
    if (!xsflib::ReadFile(path, bytes))
    {
        std::printf("%s: can't be read\n", name.c_str());
        totals.failed++;
        return;
    }

    std::shared_ptr<const hokuto::song::GbaRom> rom;
    hokuto::format::File current;
    try
    {
        current = hokuto::format::ImportGbaRom(bytes);
        rom = hokuto::song::LoadGbaRom(bytes);
    }
    catch (const xsflib::FormatError&)
    {
        return; // no MP2K
    }
    totals.roms++;

    CheckVoiceGroups(name, *rom, current, bytes, options, totals);

    const std::vector<int> songs = hokuto::song::GbaSongs(*rom);
    std::vector<std::optional<uint32_t>> sequences;
    for (int song = 0; song <= (songs.empty() ? -1 : songs.back()); song++)
    {
        sequences.push_back(hokuto::format::GbaSongSequence(current, rom->info.song_table, song));
    }
    const auto places = hokuto::edit::GbaSoundPlaces(
        sequences, hokuto::format::GbaTrackFields(current, rom->info.song_table, sequences.size()));

    std::set<uint32_t> headers;
    for (const int song : songs)
    {
        totals.songs++;
        mp2k::SongHeader header;
        if (!mp2k::ReadSong(rom->rom, rom->info, song, header) || !headers.insert(header.address).second)
        {
            continue;
        }
        totals.headers++;
        const std::optional<std::string> text = CheckSong(name, *rom, song, header, options, totals);
        const auto number = static_cast<std::size_t>(song);
        if (!text || number >= places.size() || !places[number])
        {
            totals.outside += text ? 1 : 0;
            continue;
        }
        try
        {
            current =
                hokuto::edit::WithGbaMidiSong(current, places, static_cast<uint32_t>(song),
                                              hokuto::edit::GbaSongOfAssembly(*text, mp2k::NoteArguments(rom->info)));
            totals.imported++;
        }
        catch (const std::exception& error)
        {
            Fail(name, song, std::string("the import refuses the text: ") + error.what(), totals);
        }
    }

    try
    {
        if (hokuto::format::ExportArchive(current) != bytes)
        {
            std::printf("%s: the imported songs give another ROM\n", name.c_str());
            totals.failed++;
        }
    }
    catch (const std::exception& error)
    {
        std::printf("%s: the export fails: %s\n", name.c_str(), error.what());
        totals.failed++;
    }
}

} // namespace

int main(int argc, char** argv)
{
    const std::vector<std::string> arguments = xsflib::console::Arguments(argc, argv);
    if (arguments.size() == 4 && arguments[1] == "--project")
    {
        Totals totals;
        try
        {
            CheckProject(xsflib::PathFromUtf8(arguments[3]), xsflib::PathFromUtf8(arguments[2]), totals);
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s\n", arguments[3].c_str(), error.what());
            totals.failed++;
        }
        std::printf(
            "%d files of songs: %d are songs of the ROM, %d hold no tracks, %d aren't songs of the ROM. AIFF files: "
            "%d are samples of the ROM, %d differ in the header or the last sample, %d aren't. %d failed\n",
            totals.project_files, totals.project_matched, totals.project_empty, totals.project_lacking,
            totals.project_samples, totals.project_samples_other, totals.project_samples_lacking, totals.failed);
        return totals.failed == 0 && totals.project_lacking == 0 && totals.project_samples_lacking == 0 ? 0 : 1;
    }

    Options options;
    std::size_t first = 1;
    if (first < arguments.size() && arguments[first] == "--assemble")
    {
        if (arguments.size() < first + 4)
        {
            std::fprintf(stderr, "--assemble takes a folder of LLVM's tools, a work folder and a number\n");
            return 2;
        }

        options.llvm = xsflib::PathFromUtf8(arguments[first + 1]);
        options.work = xsflib::PathFromUtf8(arguments[first + 2]);
        options.every = std::atoi(arguments[first + 3].c_str());
        first += 4;
        if (!WriteText(options.work / "MPlayDef.s", hokuto::song::GbaAssemblyDefinitions()))
        {
            std::fprintf(stderr, "the work folder can't be written\n");
            return 2;
        }
    }
    if (first >= arguments.size())
    {
        std::fprintf(stderr,
                     "usage: hokuto_check_gba_sources [--assemble <folder of llvm-mc and llvm-objcopy> <empty "
                     "work folder> <n>] <rom.gba>...\n"
                     "       hokuto_check_gba_sources --project <folder> <rom.gba>\n");
        return 2;
    }

    Totals totals;
    // A ROM whose reading throws fails with a line, and the run goes on.
    for (std::size_t i = first; i < arguments.size(); i++)
    {
        try
        {
            CheckRom(xsflib::PathFromUtf8(arguments[i]), options, totals);
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s\n", arguments[i].c_str(), error.what());
            totals.failed++;
        }
    }

    std::printf("%d samples: %d with a header that an AIFF file can't hold, %d without samples, and %d compressed\n",
                totals.samples, totals.samples_other_header, totals.samples_empty, totals.samples_compressed);
    std::printf("%d voice groups: %d refused, %d assembled, %d moved, %d refused the move\n", totals.groups,
                totals.groups_refused, totals.groups_assembled, totals.groups_moved, totals.groups_move_refused);
    std::printf(
        "%d ROMs, %d songs, %d different headers: %d written, %zu bytes read back, %d refused, %d assembled, %d "
        "imported, %d among the ROM's bytes, %d failed\n",
        totals.roms, totals.songs, totals.headers, totals.written, totals.bytes, totals.refused, totals.assembled,
        totals.imported, totals.outside, totals.failed);

    return totals.failed == 0 ? 0 : 1;
}
