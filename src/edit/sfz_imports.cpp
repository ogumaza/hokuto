// SPDX-License-Identifier: MIT

#include "edit/sfz_imports.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <iterator>
#include <map>
#include <numbers>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <tuple>
#include <utility>

#include "seqlib/sf2/presets.h"
#include "seqlib/sf2/soundfont.h"
#include "seqlib/sf2/units.h"
#include "seqlib/sf2/writer.h"
#include "seqlib/wav.h"
#include "xsflib/files.h"
#include "xsflib/text.h"

#include "edit/sfz_parser.h"
#include "edit/sound_font_voices.h"

namespace hokuto::edit
{
namespace
{

namespace sf2 = seqlib::sf2;
using sfz_parser::Lowered;
using sfz_parser::Opcodes;
using sfz_parser::ReadSfzRegions;
using sfz_parser::SfzRegions;

// The ampeg_attack page of sfzformat.com states the fall of a decay or a release as a factor of e^-8 over its time, in
// decibels.
const double kSfzFall = 8.0 * 20.0 / std::numbers::ln10;

// A SoundFont's decay and release fall this far in their times (section 8.1.3 of the SoundFont 2.04 specification).
constexpr double kSoundFontFall = 100.0;

std::optional<double> NumberOf(const Opcodes& region, std::string_view name)
{
    const auto found = region.find(name);
    if (found == region.end())
    {
        return std::nullopt;
    }

    const std::optional<xsflib::DecimalNumber> read = xsflib::ReadDecimal(found->second);

    return read && read->length == found->second.size() ? std::optional<double>(read->value) : std::nullopt;
}

// A key as a number or a note's name: sfzformat.com defines c4 as 60.
std::optional<int> KeyOf(const Opcodes& region, std::string_view name)
{
    const auto found = region.find(name);
    if (found == region.end())
    {
        return std::nullopt;
    }

    const std::string text = Lowered(found->second);
    if (const std::optional<double> number = NumberOf(region, name))
    {
        return static_cast<int>(*number);
    }

    static constexpr std::string_view kLetters = "cdefgab";
    static constexpr std::array kSemitones = {0, 2, 4, 5, 7, 9, 11};
    const std::size_t letter = text.empty() ? std::string_view::npos : kLetters.find(text.front());
    if (letter == std::string_view::npos)
    {
        return std::nullopt;
    }

    int key = kSemitones.at(letter);
    std::size_t at = 1;
    if (at < text.size() && (text[at] == '#' || text[at] == 'b'))
    {
        key += text[at] == '#' ? 1 : -1;
        at++;
    }

    int octave = 0;
    const char* const end = text.data() + text.size();
    if (std::from_chars(text.data() + at, end, octave).ec != std::errc{})
    {
        return std::nullopt;
    }

    return key + (octave + 1) * 12;
}

// How a region names a part of a sample: the same part is added to the SoundFont once.
struct SamplePart
{
    auto operator<=>(const SamplePart&) const = default;

    std::string path;
    uint32_t first = 0;
    uint32_t last = 0;
    bool loop = false;
    uint32_t loop_start = 0;
    uint32_t loop_end = 0;
};

// What the words for the user call an opcode that stays out, or nothing for one that changes nothing that a SoundFont
// can hold or that the opcodes read.
std::string LeftOutName(std::string_view name, std::string_view value)
{
    // A key group's fade follows the console's rule. The opcodes off_mode and off_time therefore change nothing there.
    // A console also interpolates by its rules. A hint changes no sound.
    static constexpr std::array<std::string_view, 38> kRead = {
        "sample",
        "key",
        "lokey",
        "hikey",
        "lovel",
        "hivel",
        "pitch_keycenter",
        "transpose",
        "tune",
        "pitch_keytrack",
        "volume",
        "amplitude",
        "pan",
        "ampeg_delay",
        "ampeg_attack",
        "ampeg_hold",
        "ampeg_decay",
        "ampeg_sustain",
        "ampeg_release",
        "group",
        "off_by",
        "loop_mode",
        "loop_start",
        "loop_end",
        "offset",
        "end",
        "default_path",
        "note_offset",
        "octave_offset",
        "trigger",
        "seq_position",
        "seq_length",
        "lorand",
        "hirand",
        "off_mode",
        "off_time",
        "sample_quality",
        "oscillator_quality",
    };
    if (std::ranges::find(kRead, name) != kRead.end() || name.starts_with("hint_"))
    {
        return {};
    }
    if (name == "bend_up" || name == "bend_down")
    {
        return value == (name == "bend_up" ? "200" : "-200") ? std::string() : "pitch bend range";
    }
    if (name == "cutoff" || name == "resonance" || name.starts_with("fil") || name.starts_with("cutoff"))
    {
        return "filters";
    }
    if (name.starts_with("amplfo") || name.starts_with("pitchlfo") || name.starts_with("lfo"))
    {
        return "LFOs";
    }
    if (name.starts_with("pitcheg") || name.starts_with("eg"))
    {
        return "pitch and flex envelopes";
    }
    if (name.starts_with("sw_"))
    {
        return "key switches";
    }
    if (name.find("cc") != std::string_view::npos)
    {
        return "CC opcodes";
    }

    return std::string(name);
}

// The lowest and the highest key of a zone that RegionZone made: its first generator contains them.
std::pair<int, int> KeysOf(const sf2::Zone& zone)
{
    const uint16_t amount = zone.generators.front().amount;

    return {amount & 0xFF, amount >> 8};
}

// Whether `zone` plays as `next` does, but for its keys, up to the key before `next`'s.
bool GoesOnInto(const sf2::Zone& zone, const sf2::Zone& next)
{
    const auto same_generator = [](const sf2::Generator& a, const sf2::Generator& b)
    {
        return a.oper == b.oper && a.amount == b.amount;
    };
    const auto same_modulator = [](const sf2::Modulator& a, const sf2::Modulator& b)
    {
        return a.source == b.source && a.dest == b.dest && a.amount == b.amount && a.amount_source == b.amount_source &&
               a.transform == b.transform;
    };

    return KeysOf(zone).second + 1 == KeysOf(next).first &&
           std::ranges::equal(std::span(zone.generators).subspan(1), std::span(next.generators).subspan(1),
                              same_generator) &&
           std::ranges::equal(zone.modulators, next.modulators, same_modulator);
}

// Extends the last `count` zones of `zones` over the keys of `next`, where they go on into them. A filter that stays
// out can leave many such regions, and a SoundFont has room for 65,535 generators. Returns whether they did.
bool JoinsLastZones(std::vector<sf2::Zone>& zones, const std::vector<sf2::Zone>& next, std::size_t count)
{
    if (count != next.size() || zones.size() < count)
    {
        return false;
    }

    const std::size_t first = zones.size() - count;
    for (std::size_t i = 0; i < count; i++)
    {
        if (!GoesOnInto(zones[first + i], next[i]))
        {
            return false;
        }
    }

    for (std::size_t i = 0; i < count; i++)
    {
        zones[first + i].generators.front() =
            sf2::Generator::Range(sf2::kKeyRange, KeysOf(zones[first + i]).first, KeysOf(next[i]).second);
    }

    return true;
}

// Builds the SoundFont of the regions.
class Builder
{
public:
    explicit Builder(const SfzReader& read) : read_(read)
    {
    }

    void Add(const SfzInstrument& instrument)
    {
        const std::filesystem::path folder = instrument.path.parent_path();
        SfzRegions parsed = ReadSfzRegions(folder, read_, instrument.path);
        for (const auto& [section, count] : parsed.other_sections)
        {
            sections_[section] += count;
        }

        files_.push_back({instrument, folder, std::move(parsed.regions)});
    }

    SfzSoundFont Take()
    {
        // A SoundFont has no level above full. The file is imported that much lower as a whole.
        double highest = 0.0;
        for (const File& file : files_)
        {
            for (const Opcodes& region : file.regions)
            {
                highest = std::max(highest, VolumeOf(region));
            }
        }

        sf2::SoundFont font;
        SfzSoundFont made;
        for (const File& file : files_)
        {
            sf2::Instrument instrument;
            instrument.name = NameOf(xsflib::Utf8(file.instrument.path.stem()));
            std::size_t last_zones = 0;
            for (const Opcodes& region : file.regions)
            {
                std::vector<sf2::Zone> zones = ZonesOf(file, region, highest, font);
                if (zones.empty())
                {
                    continue;
                }

                made.regions++;
                if (!JoinsLastZones(instrument.zones, zones, last_zones))
                {
                    last_zones = zones.size();
                    std::ranges::move(zones, std::back_inserter(instrument.zones));
                }
            }
            if (!instrument.zones.empty())
            {
                const std::string name = instrument.name;
                sf2::AddInstrumentPreset(font, file.instrument.program, name, std::move(instrument));
            }
        }
        if (made.regions == 0)
        {
            throw std::invalid_argument(unread_.empty()
                                            ? "The SFZ file has no regions with samples."
                                            : "Can't read the samples of the SFZ file, such as " +
                                                  unread_.begin()->first + ". Only WAV files are supported.");
        }

        if (highest > 0.0)
        {
            made.warnings.push_back(
                "The highest volume in the SFZ files is +" + std::to_string(std::lround(highest)) +
                " dB. SoundFont volumes can't go above 0 dB. Every region is lowered by that amount.");
        }

        if (!unread_.empty())
        {
            std::size_t regions = 0;
            for (const auto& [path, count] : unread_)
            {
                regions += count;
            }

            made.warnings.push_back(regions == 1 ? "Can't read the sample of 1 region: " + unread_.begin()->first +
                                                       ". Only WAV files are supported. The region is left out."
                                                 : "Can't read the samples of " + std::to_string(regions) +
                                                       " regions, such as " + unread_.begin()->first +
                                                       ". Only WAV files are supported. The regions are left out.");
        }

        for (const auto& [what, count] : left_out_)
        {
            made.warnings.push_back("Ignored in " + std::to_string(count) + (count == 1 ? " region: " : " regions: ") +
                                    what + ".");
        }

        for (const auto& [section, count] : sections_)
        {
            made.warnings.push_back("Ignored: " + std::to_string(count) + " " + section +
                                    (count == 1 ? " section." : " sections."));
        }

        try
        {
            made.sf2 = sf2::Write(font);
        }
        catch (const std::runtime_error&)
        {
            throw std::invalid_argument(
                "The SFZ files have more regions than a SoundFont can contain. Move some "
                "of the files to another folder.");
        }

        return made;
    }

private:
    struct File
    {
        SfzInstrument instrument;
        std::filesystem::path folder;
        std::vector<Opcodes> regions;
    };

    struct LoadedSample
    {
        std::vector<int16_t> points;
        std::vector<int16_t> right; // a stereo sample's second channel
        uint32_t rate = 0;
        bool loop = false;
        uint32_t loop_start = 0;
        uint32_t loop_end = 0; // one past the loop's last point
    };

    // A SoundFont's names hold 20 bytes of ASCII, with a zero at the end.
    static std::string NameOf(const std::string& text)
    {
        std::string name = text.substr(0, 19);
        std::ranges::replace_if(name, [](char c) { return static_cast<unsigned char>(c) >= 0x80; }, '_');

        return name;
    }

    static double VolumeOf(const Opcodes& region)
    {
        double volume = NumberOf(region, "volume").value_or(0.0);
        if (const std::optional<double> amplitude = NumberOf(region, "amplitude"); amplitude && *amplitude > 0.0)
        {
            volume += 20.0 * std::log10(*amplitude / 100.0);
        }

        return volume;
    }

    // The sample at `path`, or nothing for a file that Hokuto can't read.
    const LoadedSample* SampleAt(const std::filesystem::path& path)
    {
        const std::string key = xsflib::Utf8(path);
        const auto cached = samples_.find(key);
        if (cached != samples_.end())
        {
            return cached->second ? &*cached->second : nullptr;
        }

        std::optional<LoadedSample>& loaded = samples_[key];
        std::vector<uint8_t> bytes;
        if (!read_(path, bytes))
        {
            return nullptr;
        }

        try
        {
            seqlib::WavContents wav = seqlib::ReadWav(bytes);
            if (wav.pcm.channels.empty() || wav.pcm.channels.front().empty())
            {
                return nullptr;
            }

            LoadedSample sample;
            sample.rate = wav.rate;
            sample.loop = wav.pcm.loop && wav.loop_forward;
            sample.loop_start = wav.pcm.loop_start;
            sample.loop_end = wav.pcm.loop_end;
            sample.points = std::move(wav.pcm.channels.front());
            if (wav.pcm.channels.size() > 1)
            {
                sample.right = std::move(wav.pcm.channels[1]);
            }

            if (wav.pcm.channels.size() > 2)
            {
                ++left_out_["sample channels after the second"];
            }

            loaded = std::move(sample);
        }
        catch (const std::exception&)
        {
            return nullptr;
        }

        return &*loaded;
    }

    // The zone of `region`, or the two zones of a stereo sample's channels. No zones for a region that plays no sample.
    std::vector<sf2::Zone> ZonesOf(const File& file, const Opcodes& region, double highest, sf2::SoundFont& font)
    {
        if (!Played(region))
        {
            return {};
        }

        const auto named = region.find("sample");
        if (named == region.end() || named->second.empty())
        {
            return {};
        }

        if (named->second.front() == '*')
        {
            ++left_out_["built-in waveform " + named->second];
            return {};
        }

        CountLeftOut(region);

        // note_offset shifts the note that reaches the regions: a region's keys and root are that many keys lower for
        // the note that comes in.
        const int offset = static_cast<int>(NumberOf(region, "note_offset").value_or(0.0)) +
                           12 * static_cast<int>(NumberOf(region, "octave_offset").value_or(0.0));
        const std::optional<int> key = KeyOf(region, "key");
        const int low = KeyOf(region, "lokey").value_or(key.value_or(0)) - offset;
        const int high = KeyOf(region, "hikey").value_or(key.value_or(127)) - offset;
        const int root = KeyOf(region, "pitch_keycenter").value_or(key.value_or(60)) - offset;
        const int low_velocity = static_cast<int>(NumberOf(region, "lovel").value_or(0.0));
        const int high_velocity = static_cast<int>(NumberOf(region, "hivel").value_or(127.0));
        if (high < 0 || low > 127 || high < low || low_velocity > high_velocity)
        {
            return {};
        }

        std::string path = NamedValue(region, "default_path") + named->second;
        std::ranges::replace(path, '\\', '/');
        const LoadedSample* sample = SampleAt(file.folder / xsflib::PathFromUtf8(path));
        if (sample == nullptr)
        {
            ++unread_[named->second];
            return {};
        }

        const std::optional<std::size_t> index = PartOf(*sample, path, region, font);
        if (!index)
        {
            return {};
        }
        if (*index >= UINT16_MAX)
        {
            throw std::invalid_argument("The SFZ files use more samples than a SoundFont can contain.");
        }

        sf2::ZoneValues values;
        values.keys = {std::clamp(low, 0, 127), std::clamp(high, 0, 127)};
        values.velocities = {std::clamp(low_velocity, 0, 127), std::clamp(high_velocity, 0, 127)};
        values.attenuation = std::clamp((highest - VolumeOf(region)) * 10.0, 0.0, 1440.0);
        const int pan =
            static_cast<int>(std::lround(std::clamp(NumberOf(region, "pan").value_or(0.0), -100.0, 100.0) * 5.0));
        SetEnvelope(values, region);
        values.root_key = std::clamp(root, 0, 127);
        values.coarse_tune = static_cast<int>(NumberOf(region, "transpose").value_or(0.0));
        values.fine_tune = static_cast<int>(NumberOf(region, "tune").value_or(0.0));
        values.loop = font.samples.at(*index).loop_end > font.samples.at(*index).loop_start;
        SetExclusiveClass(values, region);

        // A SoundFont plays a stereo sample's channels as two voices at the two sides.
        if (sample->right.empty())
        {
            return {ZoneOf(values, region, *index, pan)};
        }

        return {ZoneOf(values, region, *index, pan - 500), ZoneOf(values, region, *index + 1, pan + 500)};
    }

    // Whether a note plays `region`. A region that a release, a legato note, a later turn of a round robin or a later
    // random choice starts stays out. trigger=first plays a note that starts alone, and stays in.
    bool Played(const Opcodes& region)
    {
        const auto trigger = region.find("trigger");
        if (trigger != region.end() && trigger->second != "attack" && trigger->second != "first")
        {
            ++left_out_[trigger->second == "legato" ? "legato triggers" : "release triggers"];
            return false;
        }

        if (NumberOf(region, "seq_position").value_or(1.0) > 1.0)
        {
            ++left_out_["round robins after the first"];
            return false;
        }

        if (NumberOf(region, "lorand").value_or(0.0) > 0.0)
        {
            ++left_out_["random choices after the first"];
            return false;
        }

        return true;
    }

    // Counts each kind of opcode of `region` that the import leaves out, once for the region.
    void CountLeftOut(const Opcodes& region)
    {
        std::set<std::string> left_out;
        for (const auto& [name, value] : region)
        {
            std::string what = LeftOutName(name, value);
            if (!what.empty())
            {
                left_out.insert(std::move(what));
            }
        }

        for (const std::string& what : left_out)
        {
            ++left_out_[what];
        }
    }

    // Sets the volume envelope of `values` from the ampeg opcodes of `region`.
    static void SetEnvelope(sf2::ZoneValues& values, const Opcodes& region)
    {
        // The generators' limits come from section 8.1.3 of the SoundFont 2.04 specification.
        const auto time = [&region](std::string_view name, double fallback, double scale, int longest)
        {
            return sf2::ToTimecents(NumberOf(region, name).value_or(fallback) * scale * 1000.0, -12000, longest);
        };
        const double fall = kSoundFontFall / kSfzFall;
        if (NumberOf(region, "ampeg_delay").value_or(0.0) > 0.0)
        {
            values.delay = time("ampeg_delay", 0.0, 1.0, 5000);
        }

        values.attack = time("ampeg_attack", 0.0, 1.0, 8000);
        values.hold = time("ampeg_hold", 0.0, 1.0, 5000);
        values.decay = time("ampeg_decay", 0.0, fall, 8000);

        // sfzformat.com lists a default of 1 ms for ampeg_release.
        values.release = time("ampeg_release", 0.001, fall, 8000);
        const double sustain = NumberOf(region, "ampeg_sustain").value_or(100.0);
        values.sustain =
            sustain <= 0.0 ? 1440
                           : static_cast<int>(std::clamp(std::lround(200.0 * std::log10(100.0 / sustain)), 0L, 1440L));
    }

    // Sets the exclusive class of `values` where the region's group turns itself off. Counts an off_by of another
    // group. The import leaves such an off_by out.
    void SetExclusiveClass(sf2::ZoneValues& values, const Opcodes& region)
    {
        const std::optional<double> group = NumberOf(region, "group");
        const std::optional<double> off_by = NumberOf(region, "off_by");
        if (group && *group != 0.0 && off_by && *off_by == *group)
        {
            values.exclusive_class = static_cast<int>(*group);
        }
        else if (off_by && *off_by != 0.0)
        {
            ++left_out_["off_by set to another group"];
        }
    }

    // The zone of `values` that plays sample `sample_index` at pan `zone_pan`, with the generators of `region` that
    // ZoneValues has no field for.
    static sf2::Zone ZoneOf(sf2::ZoneValues values, const Opcodes& region, std::size_t sample_index, int zone_pan)
    {
        values.sample = static_cast<uint16_t>(sample_index);
        values.pan = std::clamp(zone_pan, -500, 500);
        sf2::Zone zone = sf2::RegionZone(values);

        // A generator at its default uses room for nothing.
        const auto at_default = [](const sf2::Generator& generator)
        {
            return generator.amount == sf2::Generator::Value(0, -12000).amount
                       ? generator.oper == sf2::kAttackVolEnv || generator.oper == sf2::kHoldVolEnv ||
                             generator.oper == sf2::kDecayVolEnv || generator.oper == sf2::kReleaseVolEnv
                       : generator.oper == sf2::kSustainVolEnv && generator.amount == 0;
        };
        std::erase_if(zone.generators, at_default);
        const double keytrack = NumberOf(region, "pitch_keytrack").value_or(100.0);
        if (keytrack != 100.0)
        {
            zone.generators.insert(
                zone.generators.end() - 1,
                sf2::Generator::Value(sf2::kScaleTuning, static_cast<int16_t>(std::lround(keytrack))));
        }

        if (NamedValue(region, "loop_mode") == "one_shot")
        {
            zone.generators.insert(zone.generators.end() - 1, sf2::Generator::Value(kPlaysPastRelease, 1));
        }

        return zone;
    }

    static std::string NamedValue(const Opcodes& region, std::string_view name)
    {
        const auto found = region.find(name);

        return found != region.end() ? found->second : std::string();
    }

    // The index in `font` of the part of `sample` that the region plays, with its loop. A stereo sample's right channel
    // follows its left.
    std::optional<std::size_t> PartOf(const LoadedSample& sample, const std::string& path, const Opcodes& region,
                                      sf2::SoundFont& font)
    {
        const auto size = static_cast<uint32_t>(sample.points.size());
        const double end_opcode = NumberOf(region, "end").value_or(size - 1.0);
        if (end_opcode < 0.0)
        {
            ++left_out_["end=-1"];
            return std::nullopt;
        }

        const auto first = static_cast<uint32_t>(std::max(NumberOf(region, "offset").value_or(0.0), 0.0));
        const uint32_t last = std::min(static_cast<uint32_t>(end_opcode), size - 1);
        if (first > last)
        {
            return std::nullopt;
        }

        // loop_end is the loop's last sample. The loop of a WAV file names the sample after its last.
        const std::string mode = NamedValue(region, "loop_mode");
        const std::optional<double> loop_start = NumberOf(region, "loop_start");
        const std::optional<double> loop_end = NumberOf(region, "loop_end");
        const bool asks_loop = mode == "loop_continuous" || mode == "loop_sustain";
        bool loop = asks_loop || (mode.empty() && (sample.loop || (loop_start && loop_end)));
        if (mode == "loop_sustain")
        {
            ++left_out_["loop_sustain (the loop continues through the release)"];
        }

        const uint32_t file_start = sample.loop ? sample.loop_start : 0;
        const uint32_t file_stop = sample.loop ? sample.loop_end : last + 1;
        uint32_t start = loop ? static_cast<uint32_t>(loop_start.value_or(file_start)) : 0;
        uint32_t stop = loop ? (loop_end ? static_cast<uint32_t>(*loop_end) + 1 : file_stop) : 0;
        if (loop && (start < first || stop > last + 1 || start >= stop))
        {
            ++left_out_["loops outside the sample"];
            loop = false;
            start = 0;
            stop = 0;
        }

        const SamplePart part{path, first, last, loop, start, stop};
        const auto [found, added] = parts_.emplace(part, font.samples.size());
        if (added)
        {
            sf2::Sample made;
            made.name = NameOf(xsflib::Utf8(xsflib::PathFromUtf8(path).stem()));
            made.data.assign(sample.points.begin() + first, sample.points.begin() + last + 1);
            made.sample_rate = sample.rate;
            made.original_key = 60;
            made.loop_start = loop ? start - first : 0;
            made.loop_end = loop ? stop - first : 0;
            if (!sample.right.empty())
            {
                sf2::Sample other = made;
                other.data.assign(sample.right.begin() + first, sample.right.begin() + last + 1);
                made.type = sf2::kLeft;
                made.link = static_cast<uint16_t>(font.samples.size() + 1);
                other.type = sf2::kRight;
                other.link = static_cast<uint16_t>(font.samples.size());
                font.samples.push_back(std::move(made));
                font.samples.push_back(std::move(other));
            }
            else
            {
                font.samples.push_back(std::move(made));
            }
        }

        return found->second;
    }

    const SfzReader& read_;
    std::vector<File> files_;
    std::map<std::string, std::optional<LoadedSample>> samples_;
    std::map<SamplePart, std::size_t> parts_;
    std::map<std::string, std::size_t> left_out_;
    std::map<std::string, std::size_t> sections_;
    std::map<std::string, std::size_t> unread_;
};

// The name before a program at the end of `stem`, with the program: BANK_GM and 5 of BANK_GM_005.
std::optional<std::pair<std::string, uint32_t>> SplitProgram(const std::string& stem)
{
    const std::size_t underscore = stem.rfind('_');
    if (underscore == std::string::npos || underscore == 0 || underscore + 1 == stem.size())
    {
        return std::nullopt;
    }

    uint32_t program = 0;
    const char* const first = stem.data() + underscore + 1;
    const char* const last = stem.data() + stem.size();
    const auto [end, error] = std::from_chars(first, last, program);
    if (error != std::errc{} || end != last || program >= 128 * 128)
    {
        return std::nullopt;
    }

    return std::pair(stem.substr(0, underscore), program);
}

} // namespace

bool IsSfzName(std::string_view name)
{
    return name.size() >= 4 && Lowered(name.substr(name.size() - 4)) == ".sfz";
}

std::vector<SfzInstrument> SfzInstrumentsBeside(const std::filesystem::path& chosen)
{
    const std::optional<std::pair<std::string, uint32_t>> own = SplitProgram(xsflib::Utf8(chosen.stem()));
    if (!own)
    {
        return {{chosen, 0}};
    }

    std::map<uint32_t, std::filesystem::path> files;
    std::error_code error;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(chosen.parent_path(), error))
    {
        if (!IsSfzName(xsflib::Utf8(entry.path().filename())))
        {
            continue;
        }

        const std::optional<std::pair<std::string, uint32_t>> other = SplitProgram(xsflib::Utf8(entry.path().stem()));
        if (other && other->first == own->first)
        {
            files.emplace(other->second, entry.path());
        }
    }

    files.insert_or_assign(own->second, chosen);

    std::vector<SfzInstrument> instruments;
    for (const auto& [program, path] : files)
    {
        instruments.push_back({path, program});
    }

    return instruments;
}

std::string SfzImportName(std::span<const SfzInstrument> instruments, const std::string& chosen)
{
    if (instruments.size() < 2)
    {
        return chosen;
    }

    const std::optional<std::pair<std::string, uint32_t>> split =
        SplitProgram(xsflib::Utf8(xsflib::PathFromUtf8(chosen).stem()));

    return split ? split->first + ".sfz" : chosen;
}

SfzSoundFont SoundFontOfSfz(std::span<const SfzInstrument> instruments, const SfzReader& read)
{
    Builder builder(read);
    for (const SfzInstrument& instrument : instruments)
    {
        builder.Add(instrument);
    }

    return builder.Take();
}

} // namespace hokuto::edit
