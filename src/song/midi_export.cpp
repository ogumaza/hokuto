// SPDX-License-Identifier: MIT

#include "song/midi_export.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "ntdlib/sequence_export/sfz.h"
#include "xsflib/files.h"

#include "song/archive.h"
#include "song/items.h"

namespace hokuto::song
{
namespace
{

namespace export_ = ntdlib::sequence_export;

// Writes `bytes` to `path`. Throws std::runtime_error with a message for the user if the write fails.
void Write(const std::filesystem::path& path, std::span<const uint8_t> bytes)
{
    if (xsflib::WriteFile(path, bytes))
    {
        throw std::runtime_error(WriteFailure(xsflib::Utf8(path.filename())));
    }
}

void Write(const std::filesystem::path& path, const std::string& text)
{
    Write(path, std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
}

// Makes the folder `folder` and the folders above it. Throws std::runtime_error with a message for the user if it
// can't.
void MakeFolder(const std::filesystem::path& folder)
{
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    if (error)
    {
        throw std::runtime_error(FolderFailure(xsflib::Utf8(folder.filename()), error));
    }
}

// The samples of `samples` that the folder `folder` lacks. Exports share a folder of samples. A sample there with the
// same name and other bytes belongs to an earlier export from another archive. Writing over it would change how that
// export sounds. Throws std::runtime_error with a message for the user then. `where` names the folder in the message.
std::vector<const export_::SfzSample*> NewSamples(const std::vector<export_::SfzSample>& samples,
                                                  const std::filesystem::path& folder, const std::string& where)
{
    std::vector<const export_::SfzSample*> new_samples;
    for (const export_::SfzSample& sample : samples)
    {
        const std::filesystem::path path = folder / xsflib::PathFromUtf8(sample.name);
        std::error_code error;
        if (!std::filesystem::exists(path, error))
        {
            new_samples.push_back(&sample);
            continue;
        }

        std::vector<uint8_t> bytes;
        if (!xsflib::ReadFile(path, bytes) || bytes != sample.wav)
        {
            throw std::runtime_error("The folder " + where + " already holds another " + sample.name +
                                     ". It belongs to an earlier export. Export to another folder.");
        }
    }

    return new_samples;
}

// Writes `samples` into the folder `folder`. The folder is made when there are any.
void WriteSamples(const std::vector<const export_::SfzSample*>& samples, const std::filesystem::path& folder)
{
    if (!samples.empty())
    {
        MakeFolder(folder);
    }
    for (const export_::SfzSample* sample : samples)
    {
        Write(folder / xsflib::PathFromUtf8(sample->name), sample->wav);
    }
}

// A MIDI file's header chunk, of a size of 6, and the start of a chunk: its ID and its size.
constexpr std::size_t kMidiHeader = 14;
constexpr std::size_t kChunkStart = 8;

// The longest text that a meta event holds. Its length takes four bytes at most.
constexpr std::size_t kLongestMetaText = 0x0FFFFFFF;

// The big-endian 32-bit number at the start of `bytes`.
uint32_t BigEndian32(std::span<const uint8_t> bytes)
{
    return uint32_t{bytes[0]} << 24 | uint32_t{bytes[1]} << 16 | uint32_t{bytes[2]} << 8 | uint32_t{bytes[3]};
}

// Whether `bytes` holds the four characters of `id` at `at`.
bool HasId(std::span<const uint8_t> bytes, std::size_t at, std::string_view id)
{
    return bytes.size() >= at + id.size() &&
           std::equal(id.begin(), id.end(), bytes.begin() + static_cast<std::ptrdiff_t>(at));
}

// Reads the variable-length number at `at` in `data`: seven bits a byte, the highest first, with the top bit set in
// each byte but the last. Moves `at` past it. Nothing when it runs past the data or past four bytes.
std::optional<uint32_t> ReadVariable(std::span<const uint8_t> data, std::size_t& at)
{
    uint32_t value = 0;
    for (int i = 0; i < 4 && at < data.size(); i++)
    {
        const uint8_t byte = data[at++];
        value = value << 7 | (byte & 0x7Fu);
        if ((byte & 0x80) == 0)
        {
            return value;
        }
    }

    return std::nullopt;
}

// Appends a meta event of type `type` with `text` at a delta of 0 to `track`. Its length is a variable-length number.
void AppendMeta(std::vector<uint8_t>& track, uint8_t type, std::string_view text)
{
    track.insert(track.end(), {0x00, 0xFF, type});
    const auto length = static_cast<uint32_t>(text.size());
    for (int shift = 21; shift > 0; shift -= 7)
    {
        if (length >> shift != 0)
        {
            track.push_back(static_cast<uint8_t>(0x80 | ((length >> shift) & 0x7F)));
        }
    }
    track.push_back(static_cast<uint8_t>(length & 0x7F));
    track.insert(track.end(), text.begin(), text.end());
}

// Converts sound `sound` of `archive`, a sound archive of the 3DS, the Switch, the Wii U or the Wii, with `options` and
// `settings`. `sequence` says whether the archive has such a sequence sound. The performer sets the variables and makes
// the changes as a model of the sequence does.
template <typename Archive>
export_::Conversion Convert(const Archive& archive, uint32_t sound, bool sequence,
                            const export_::PerformOptions& options, const export_::Settings& settings,
                            const std::string& program)
{
    if (!sequence)
    {
        throw std::runtime_error("The archive has no sequence sound " + std::to_string(sound) + ".");
    }

    try
    {
        return export_::ConvertSequence(archive, sound, options, settings, program);
    }
    catch (const std::exception& error)
    {
        throw std::runtime_error("Hokuto can't convert " + archive.Sounds()[sound].name + ". " + error.what());
    }
}

} // namespace

export_::Conversion ConvertToMidi(const ntdlib::sar::SoundArchive& archive, uint32_t sound, const StartValues& values,
                                  const Changes& changes, const std::string& program)
{
    const bool sequence =
        sound < archive.Sounds().size() && archive.Sounds()[sound].type == ntdlib::sar::SoundType::kSequence;
    return Convert(archive, sound, sequence, PerformOptionsOf(values, changes, archive.ReadOptions().flavor),
                   SettingsOf(archive), program);
}

export_::Conversion ConvertToMidi(const ntdlib::rsar::SoundArchive& archive, uint32_t sound, const StartValues& values,
                                  const Changes& changes, const std::string& program,
                                  const std::optional<ntdlib::SndBuild>& build)
{
    const bool sequence =
        sound < archive.Sounds().size() && archive.Sounds()[sound].type == ntdlib::rsar::SoundType::kSequence;
    // The Wii's frames last 3 ms. ntdlib's model of the Wii plays the first tick in its frame 0. So does the read.
    return Convert(archive, sound, sequence, PerformOptionsOf(values, changes, kWiiFrameMs, 0),
                   SettingsOf(archive, build), program);
}

std::vector<std::string> ConversionReport(const export_::Conversion& conversion)
{
    std::vector<std::string> lines;
    for (const auto& [what, count] : conversion.performance.approximations)
    {
        lines.push_back("Approximated " + (count == 1 ? std::string("once") : std::to_string(count) + " times") + ": " +
                        what);
    }
    lines.insert(lines.end(), conversion.warnings.begin(), conversion.warnings.end());

    return lines;
}

std::vector<std::string> ReportFor(std::vector<std::string> report, bool sfz)
{
    const auto other_format = [sfz](const std::string& line)
    {
        const bool sound_font = line.find("SoundFont") != std::string::npos;
        const bool sfz_named = line.find("SFZ") != std::string::npos;
        return sfz ? sound_font && !sfz_named : sfz_named && !sound_font;
    };
    std::erase_if(report, other_format);

    return report;
}

std::vector<uint8_t> WithMidiTags(std::span<const uint8_t> midi, const MidiTags& tags)
{
    if (tags.title.empty() && tags.copyright.empty())
    {
        return {midi.begin(), midi.end()};
    }
    if (tags.title.size() > kLongestMetaText || tags.copyright.size() > kLongestMetaText)
    {
        throw std::runtime_error("A tag is too long for a MIDI file.");
    }

    // The header, then the first track's chunk. The track's first event names the sequence.
    const std::runtime_error unnamed(
        "Hokuto can't put the tags in the MIDI file. Its first track doesn't start with "
        "the sequence's name.");
    if (!HasId(midi, 0, "MThd") || midi.size() < kMidiHeader + kChunkStart || BigEndian32(midi.subspan(4)) != 6 ||
        !HasId(midi, kMidiHeader, "MTrk"))
    {
        throw unnamed;
    }
    const std::size_t kTrackStart = kMidiHeader + kChunkStart;
    const uint32_t size = BigEndian32(midi.subspan(kMidiHeader + 4));
    if (size > midi.size() - kTrackStart)
    {
        throw unnamed;
    }
    const std::span<const uint8_t> data = midi.subspan(kTrackStart, size);
    std::size_t at = 3;
    const bool named = data.size() >= at && data[0] == 0x00 && data[1] == 0xFF && data[2] == 0x03;
    const std::optional<uint32_t> length = named ? ReadVariable(data, at) : std::nullopt;
    if (!length || *length > data.size() - at)
    {
        throw unnamed;
    }
    const std::string_view name(reinterpret_cast<const char*>(data.data() + at), *length);

    // The notice, the title or the old name, then the track's other events.
    std::vector<uint8_t> track;
    if (!tags.copyright.empty())
    {
        AppendMeta(track, 0x02, tags.copyright);
    }
    AppendMeta(track, 0x03, tags.title.empty() ? name : std::string_view(tags.title));
    track.insert(track.end(), data.begin() + static_cast<std::ptrdiff_t>(at + *length), data.end());
    if (track.size() > UINT32_MAX)
    {
        throw std::runtime_error("A tag is too long for a MIDI file.");
    }

    // The header, the track with its new size, and the other tracks as they were.
    std::vector<uint8_t> result(midi.begin(), midi.begin() + static_cast<std::ptrdiff_t>(kMidiHeader));
    result.insert(result.end(), {'M', 'T', 'r', 'k'});
    const auto track_size = static_cast<uint32_t>(track.size());
    for (const int shift : {24, 16, 8, 0})
    {
        result.push_back(static_cast<uint8_t>(track_size >> shift));
    }
    result.insert(result.end(), track.begin(), track.end());
    result.insert(result.end(), midi.begin() + static_cast<std::ptrdiff_t>(kTrackStart + size), midi.end());

    return result;
}

void WriteMidiAndSoundFont(const export_::Conversion& conversion, const std::filesystem::path& midi)
{
    std::filesystem::path sound_font = midi;
    sound_font.replace_extension(".sf2");
    Write(midi, conversion.midi);
    Write(sound_font, conversion.sf2);
}

namespace
{

// The samples of `conversion` that the folder of samples beside the MIDI file `midi` lacks (NewSamples). The message
// names the folder with the system's separator. The question about replacing files uses the same separator.
std::vector<const export_::SfzSample*> NewSfzSamples(const export_::Conversion& conversion,
                                                     const std::filesystem::path& midi)
{
    const std::filesystem::path in_sfz =
        std::filesystem::path("sfz") / xsflib::PathFromUtf8(export_::kSfzSamplesFolder);
    return NewSamples(conversion.sfz.samples, midi.parent_path() / in_sfz,
                      xsflib::Utf8(in_sfz) + " beside " + xsflib::Utf8(midi.filename()));
}

} // namespace

void CheckSfzSamples(const export_::Conversion& conversion, const std::filesystem::path& midi)
{
    NewSfzSamples(conversion, midi);
}

void WriteMidiAndSfz(const export_::Conversion& conversion, const std::filesystem::path& midi)
{
    // Each sequence's files go in a folder of its own. The sequences share the samples' folder. A sample there with the
    // same name and other bytes stops the export before it writes anything.
    const std::filesystem::path sfz = midi.parent_path() / "sfz";
    const std::filesystem::path files = sfz / midi.stem();
    const std::filesystem::path samples = sfz / xsflib::PathFromUtf8(export_::kSfzSamplesFolder);
    const std::vector<const export_::SfzSample*> new_samples = NewSfzSamples(conversion, midi);

    Write(midi, conversion.midi);
    if (!conversion.sfz.files.empty())
    {
        MakeFolder(files);
    }
    for (const export_::SfzFile& file : conversion.sfz.files)
    {
        Write(files / xsflib::PathFromUtf8(file.name), file.text);
    }
    WriteSamples(new_samples, samples);
}

export_::BankConversion ConvertBank(const ntdlib::sar::SoundArchive& archive, uint32_t bank, const std::string& program)
{
    // The converter makes empty files of a bank that it can't read. The export stops instead.
    ItemFile(archive, {ItemKind::kBank, bank, 0});
    const std::string& name = archive.Banks()[bank].name;
    std::vector<uint32_t> programs;
    try
    {
        programs = ProgramsOf(archive, bank);
    }
    catch (const std::exception& error)
    {
        throw std::runtime_error("Hokuto can't read " + name + ". " + error.what());
    }
    if (programs.empty())
    {
        throw BankWithoutInstruments(name + " holds no instruments.");
    }

    try
    {
        return export_::ConvertBank(archive, bank, SettingsOf(archive), program);
    }
    catch (const std::exception& error)
    {
        throw std::runtime_error("Hokuto can't convert " + name + ". " + error.what());
    }
}

export_::BankConversion ConvertBank(const ntdlib::rsar::SoundArchive& archive, uint32_t bank,
                                    const std::string& program, const std::optional<ntdlib::SndBuild>& build)
{
    // The converter makes empty files of a bank that it can't read. The export stops instead.
    ItemFile(archive, {ItemKind::kBank, bank, 0});
    const std::string& name = archive.Banks()[bank].name;
    std::vector<uint32_t> programs;
    try
    {
        programs = ProgramsOf(archive, bank);
    }
    catch (const std::exception& error)
    {
        throw std::runtime_error("Hokuto can't read " + name + ". " + error.what());
    }
    if (programs.empty())
    {
        throw BankWithoutInstruments(name + " holds no instruments.");
    }

    const auto plays = [bank](const ntdlib::rsar::SoundInfo& sound)
    {
        return sound.type == ntdlib::rsar::SoundType::kSequence && sound.bank == bank;
    };
    const auto first = std::ranges::find_if(archive.Sounds(), plays);
    const uint8_t pan_mode = first != archive.Sounds().end() ? first->pan_mode : 1;
    try
    {
        return export_::ConvertBank(archive, bank, pan_mode, SettingsOf(archive, build), program);
    }
    catch (const std::exception& error)
    {
        throw std::runtime_error("Hokuto can't convert " + name + ". " + error.what());
    }
}

std::vector<std::string> SoundFontNames(const export_::BankConversion& conversion, const std::string& stem)
{
    if (conversion.sound_fonts.size() == 1)
    {
        return {stem + ".sf2"};
    }

    // Each program takes the digits of the last one.
    const std::size_t digits =
        conversion.sound_fonts.empty() ? 1 : std::to_string(conversion.sound_fonts.back().last).size();
    const auto padded = [digits](uint32_t program)
    {
        std::string number = std::to_string(program);
        number.insert(0, digits - number.size(), '0');
        return number;
    };

    std::vector<std::string> names;
    for (const export_::BankSoundFont& sound_font : conversion.sound_fonts)
    {
        names.push_back(stem + "_" + padded(sound_font.first) + "-" + padded(sound_font.last) + ".sf2");
    }

    return names;
}

bool WriteBankSfz(const export_::BankConversion& conversion, const std::filesystem::path& folder,
                  const std::string& name, const std::function<bool(const std::vector<std::string>&)>& replace)
{
    // A sample with other bytes stops the export first. The user then decides about the instruments' files that the
    // folder holds already.
    const std::filesystem::path files = folder / xsflib::PathFromUtf8(name);
    const std::filesystem::path samples = folder / xsflib::PathFromUtf8(export_::kSfzSamplesFolder);
    const std::vector<const export_::SfzSample*> new_samples =
        NewSamples(conversion.sfz.samples, samples, export_::kSfzSamplesFolder + " beside " + name);
    std::vector<std::string> existing;
    for (const export_::SfzFile& file : conversion.sfz.files)
    {
        std::error_code error;
        if (std::filesystem::exists(files / xsflib::PathFromUtf8(file.name), error))
        {
            existing.push_back(file.name);
        }
    }
    if (!existing.empty() && !replace(existing))
    {
        return false;
    }

    MakeFolder(files);
    for (const export_::SfzFile& file : conversion.sfz.files)
    {
        Write(files / xsflib::PathFromUtf8(file.name), file.text);
    }
    WriteSamples(new_samples, samples);

    return true;
}

} // namespace hokuto::song
