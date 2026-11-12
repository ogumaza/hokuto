// SPDX-License-Identifier: MIT

// The rounds of hokuto_check_gba_voices that edit the voices that the Bank Editor shows, and their levels.

#include "check_gba_voices.h"

namespace hokuto::tools::gba_voices
{

std::vector<Span> SpansOf(const hokuto::song::GbaRom& rom, const std::vector<hokuto::song::GbaVoiceGroup>& groups)
{
    constexpr uint32_t kVoiceBytes = 12;
    constexpr uint32_t kPatternBytes = 16;
    constexpr uint32_t kKeyTableBytes = 128;
    std::vector<Span> spans;
    for (const hokuto::song::GbaVoiceGroup& group : groups)
    {
        const auto add = [&](uint32_t address)
        {
            spans.push_back({address, kVoiceBytes, &group.songs});
            mp2k::Voice voice;
            if (mp2k::ReadVoice(rom.rom, address, voice) && voice.PsgChannel() == 3)
            {
                spans.push_back({voice.wave, kPatternBytes, &group.songs});
            }
            return voice;
        };
        for (const int program : group.programs)
        {
            const mp2k::Voice voice = add(group.address + kVoiceBytes * static_cast<uint32_t>(program));
            if ((voice.type & mp2k::kVoiceKeySplit) != 0)
            {
                spans.push_back({voice.key_table, kKeyTableBytes, &group.songs});
            }
            for (int key = 0; voice.IsSplit() && key <= 127; key++)
            {
                const uint32_t address = mp2k::SplitVoice(rom.rom, voice, key);
                if (address != 0)
                {
                    add(address);
                }
            }
        }
    }

    return spans;
}

std::map<uint32_t, Place> PlacesOf(const hokuto::song::GbaRom& rom,
                                   const std::vector<hokuto::song::GbaVoiceGroup>& groups,
                                   std::map<uint32_t, GbaPattern>& patterns, Totals& totals)
{
    std::map<uint32_t, Place> places;
    for (const hokuto::song::GbaVoiceGroup& group : groups)
    {
        const hokuto::app::GbaBankView view = hokuto::app::MakeGbaBankView(rom, group.address);
        totals.groups++;
        patterns.insert(view.patterns.begin(), view.patterns.end());
        for (const hokuto::app::GbaProgram& program : view.programs)
        {
            const std::string name = view.name + ", program " + std::to_string(program.program);
            const bool drums = (program.voice.voice.type & mp2k::kVoiceDrumKit) != 0;
            if (!program.voice.voice.IsSplit())
            {
                if (program.voice.kind != "Unknown")
                {
                    places.try_emplace(program.voice.voice.address,
                                       Place{name, program.program, 60, view.song, false, program.voice.voice});
                }
                continue;
            }
            for (const hokuto::app::GbaVoiceRow& row : program.rows)
            {
                const int key = drums ? row.first_key : std::clamp(60, row.first_key, row.last_key);
                places.try_emplace(row.voice.address, Place{name + ", key " + std::to_string(key), program.program, key,
                                                            view.song, drums, row.voice});
            }
        }
    }

    return places;
}

namespace
{

// `value` moved by `step` within 0 to `highest`, or as it is when the ROM stores it past `highest`: the edit keeps it.
template <typename Value>
Value Moved(Value value, int step, int highest, Totals& totals)
{
    if (value > static_cast<uint32_t>(highest))
    {
        totals.kept++;
        return value;
    }

    return static_cast<Value>((static_cast<int>(value) + step) % (highest + 1));
}

} // namespace

std::map<uint32_t, mp2k::Voice> PsgEnvelopes(const std::map<uint32_t, Place>& places, Totals& totals)
{
    using namespace hokuto::edit;
    std::map<uint32_t, mp2k::Voice> edits;
    for (const auto& [address, place] : places)
    {
        if (place.voice.PsgChannel() == 0)
        {
            continue;
        }
        mp2k::Voice voice = place.voice;
        voice.attack = Moved(voice.attack, 3, kHighestPsgStep, totals);
        voice.decay = Moved(voice.decay, 5, kHighestPsgStep, totals);
        voice.sustain = Moved(voice.sustain, 7, kHighestPsgSustain, totals);
        voice.release = Moved(voice.release, 2, kHighestPsgStep, totals);
        edits.emplace(address, voice);
    }

    return edits;
}

std::map<uint32_t, mp2k::Voice> PsgSettings(const std::map<uint32_t, Place>& places, Totals& totals)
{
    using namespace hokuto::edit;
    std::map<uint32_t, mp2k::Voice> edits;
    for (const auto& [address, place] : places)
    {
        const int psg = place.voice.PsgChannel();
        if (psg == 0)
        {
            continue;
        }
        mp2k::Voice voice = place.voice;
        if (psg <= 2)
        {
            voice.wave = Moved(voice.wave, 1, kHighestDuty, totals);
        }
        if (psg == 1 && (voice.pan_sweep & 0x80) == 0)
        {
            // The time moves on by one, and the direction turns.
            const int time = (voice.pan_sweep >> 4 & 7) + 1;
            voice.pan_sweep = static_cast<uint8_t>((time & 7) << 4 | ((voice.pan_sweep ^ 0x08) & 0x0F));
        }
        if (psg == 4)
        {
            voice.wave = Moved(voice.wave, 1, kHighestNoiseMode, totals);
        }
        voice.length = Moved(voice.length, 17, kHighestPsgLength, totals);
        edits.emplace(address, voice);
    }

    return edits;
}

std::map<uint32_t, mp2k::Voice> Samples(const std::map<uint32_t, Place>& places, const std::vector<uint32_t>& samples)
{
    std::map<uint32_t, mp2k::Voice> edits;
    for (const auto& [address, place] : places)
    {
        if (place.voice.PsgChannel() != 0)
        {
            continue;
        }
        mp2k::Voice voice = place.voice;
        const auto found = std::ranges::find(samples, voice.wave);
        if (found != samples.end() && samples.size() > 1)
        {
            voice.wave = found + 1 == samples.end() ? samples.front() : *(found + 1);
        }
        voice.type ^= mp2k::kVoiceFixed;
        voice.attack++;
        voice.decay++;
        voice.sustain++;
        voice.release++;
        edits.emplace(address, voice);
    }

    return edits;
}

std::map<uint32_t, mp2k::Voice> Drums(const std::map<uint32_t, Place>& places)
{
    std::map<uint32_t, mp2k::Voice> edits;
    for (const auto& [address, place] : places)
    {
        if (!place.drum)
        {
            continue;
        }
        mp2k::Voice voice = place.voice;
        voice.key = static_cast<uint8_t>((voice.key + 5) % 128);
        voice.pan_sweep = (voice.pan_sweep & 0x80) != 0 ? 0 : 0xC0;
        edits.emplace(address, voice);
    }

    return edits;
}

std::map<uint32_t, GbaPattern> Flipped(const std::map<uint32_t, GbaPattern>& patterns)
{
    std::map<uint32_t, GbaPattern> flipped;
    for (auto [address, steps] : patterns)
    {
        for (uint8_t& step : steps)
        {
            step = static_cast<uint8_t>(15 - step);
        }
        flipped.emplace(address, steps);
    }

    return flipped;
}

std::shared_ptr<const hokuto::song::GbaRom> Round(const Game& game, const char* round,
                                                  std::map<uint32_t, mp2k::Voice>& voices,
                                                  const std::map<uint32_t, GbaPattern>& patterns, Totals& totals)
{
    const std::string& rom_name = game.name;
    const std::vector<uint8_t>& bytes = game.bytes;
    const File& file = game.file;
    File edited = file;
    for (auto it = voices.begin(); it != voices.end();)
    {
        try
        {
            edited = hokuto::edit::WithGbaVoice(edited, it->second);
            totals.edits++;
            ++it;
        }
        catch (const std::invalid_argument& error)
        {
            std::printf("%s: %s: the voice at 0x%08X refused its edit: %s\n", rom_name.c_str(), round, it->first,
                        error.what());
            totals.refused++;
            totals.failed++;
            it = voices.erase(it);
        }
    }
    for (const auto& [address, steps] : patterns)
    {
        edited = hokuto::edit::WithGbaPattern(edited, address, steps);
        totals.edits++;
    }

    // The bytes that the ROM should hold.
    std::vector<uint8_t> expected = bytes;
    for (const auto& [address, voice] : voices)
    {
        const auto voice_bytes = hokuto::edit::GbaVoiceBytes(voice);
        std::ranges::copy(voice_bytes, expected.begin() + (address - ntdlib::agb::kRomBase));
    }
    for (const auto& [address, steps] : patterns)
    {
        for (std::size_t i = 0; i < steps.size() / 2; ++i)
        {
            expected[address - ntdlib::agb::kRomBase + i] = static_cast<uint8_t>(steps[2 * i] << 4 | steps[2 * i + 1]);
        }
    }

    const std::vector<uint8_t> exported = hokuto::format::ExportArchive(edited);
    if (exported != expected)
    {
        std::size_t at = 0;
        while (at < std::min(exported.size(), expected.size()) && exported[at] == expected[at])
        {
            at++;
        }
        std::printf(
            "%s: %s: the ROM holds other bytes than the edits give, first at 0x%08zX (%zu bytes, %zu "
            "expected)\n",
            rom_name.c_str(), round, at + ntdlib::agb::kRomBase, exported.size(), expected.size());
        totals.failed++;
        return nullptr;
    }

    // ntdlib reads the new values, and the Bank Editor shows the new ROM.
    const std::shared_ptr<const hokuto::song::GbaRom> rom = hokuto::song::LoadGbaRom(exported);
    for (const auto& [address, voice] : voices)
    {
        mp2k::Voice read;
        if (!mp2k::ReadVoice(rom->rom, address, read) ||
            hokuto::edit::GbaVoiceBytes(read) != hokuto::edit::GbaVoiceBytes(voice))
        {
            std::printf("%s: %s: ntdlib reads the voice at 0x%08X otherwise\n", rom_name.c_str(), round, address);
            totals.failed++;
        }
    }
    for (const hokuto::song::GbaVoiceGroup& group : hokuto::song::GbaVoiceGroups(*rom))
    {
        hokuto::app::MakeGbaBankView(*rom, group.address);
    }
    std::vector<uint32_t> changed;
    for (std::size_t i = 0; i < bytes.size(); ++i)
    {
        if (expected[i] != bytes[i])
        {
            changed.push_back(static_cast<uint32_t>(ntdlib::agb::kRomBase + i));
        }
    }
    std::set<int> played;
    for (const Span& span : game.spans)
    {
        const auto first = std::ranges::lower_bound(changed, span.address);
        if (first != changed.end() && *first - span.address < span.size)
        {
            played.insert(span.songs->begin(), span.songs->end());
        }
    }
    const std::vector<int> listed = hokuto::song::GbaSongsWithOtherVoices(*game.rom, *rom);
    if (listed != std::vector<int>(played.begin(), played.end()))
    {
        std::string extra;
        std::string missing;
        for (const int song : listed)
        {
            extra += played.contains(song) ? "" : " " + std::to_string(song);
        }
        for (const int song : played)
        {
            missing += std::ranges::find(listed, song) != listed.end() ? "" : " " + std::to_string(song);
        }
        std::printf(
            "%s: %s: the export's list of collisions reads other songs than those of the edited voices. "
            "Also:%s. Without:%s.\n",
            rom_name.c_str(), round, extra.empty() ? " none" : extra.c_str(),
            missing.empty() ? " none" : missing.c_str());
        totals.failed++;
    }

    // Both forms of the file give the same ROM.
    if (hokuto::format::ExportArchive(hokuto::format::Read(hokuto::format::Write(edited))) != exported ||
        hokuto::format::ExportArchive(hokuto::format::ReadText(hokuto::format::WriteText(edited))) != exported)
    {
        std::printf("%s: %s: a saved file gives another ROM\n", rom_name.c_str(), round);
        totals.failed++;
    }

    return rom;
}

std::shared_ptr<const hokuto::song::GbaRom> Round(const Game& game, const char* round,
                                                  std::map<uint32_t, mp2k::Voice>&& voices,
                                                  const std::map<uint32_t, GbaPattern>& patterns, Totals& totals)
{
    return Round(game, round, voices, patterns, totals);
}

namespace
{

// The frames, at `rate`, that the driver's envelope takes to reach the sustain of `voice` from a note's start. The
// attack adds its value to the level each frame up to 255. The decay keeps decay/256 of the level each frame until it
// reaches the sustain, and a decay of 0 reaches it at once. 0 when the note never reaches its sustain within 5 s.
std::size_t SustainFrames(const mp2k::Voice& voice, int rate)
{
    constexpr int kMostFrames = 300;
    if (voice.attack == 0)
    {
        return 0;
    }
    int frames = (255 + voice.attack - 1) / voice.attack;
    int level = 255;
    while (level > voice.sustain && voice.decay != 0 && frames < kMostFrames)
    {
        level = level * voice.decay >> 8;
        frames++;
    }
    if (frames >= kMostFrames)
    {
        return 0;
    }

    const double frame = static_cast<double>(rate) * static_cast<double>(ntdlib::agb::kFrameCycles) /
                         static_cast<double>(ntdlib::agb::kSecondCycles);
    return static_cast<std::size_t>(std::ceil(frame * (frames + 2)));
}

// A held note's level in dBFS, and its peak as a fraction of full scale.
struct Held
{
    double level = -200.0;
    double peak = 0.0;
    bool ended = false; // the output ended before the frames that the level covers
};

// The level and the peak of a held note of `place` from `rom`, over `count` frames from frame `from` of its output.
Held HeldLevel(const std::shared_ptr<const hokuto::song::GbaRom>& rom, const Place& place, std::size_t from,
               std::size_t count)
{
    auto release = std::make_shared<std::atomic<bool>>(false);
    hokuto::song::GbaNotePreview preview(rom, place.song, static_cast<int>(place.program), place.key, 127, release);
    std::vector<float> out((from + count) * 2);
    const std::size_t made = preview.Render(out.data(), from + count);
    if (made < from + count)
    {
        return {.ended = true};
    }
    double sum = 0.0;
    Held held;
    for (std::size_t i = from * 2; i < out.size(); ++i)
    {
        sum += static_cast<double>(out[i]) * out[i];
        held.peak = std::max(held.peak, static_cast<double>(std::abs(out[i])));
    }
    held.level = 10.0 * std::log10(std::max(sum / static_cast<double>(count * 2), 1e-20));

    return held;
}

} // namespace

void Levels(const Game& game, const std::map<uint32_t, Place>& places, Totals& totals)
{
    const std::string& rom_name = game.name;
    const std::shared_ptr<const hokuto::song::GbaRom>& rom = game.rom;
    constexpr int kLeastSustain = 128;
    std::map<uint32_t, mp2k::Voice> halved;
    for (const auto& [address, place] : places)
    {
        if (place.voice.PsgChannel() == 0 && place.voice.sustain >= kLeastSustain && place.song >= 0)
        {
            mp2k::Voice voice = place.voice;
            voice.sustain = static_cast<uint8_t>(voice.sustain / 2);
            halved.emplace(address, voice);
        }
    }
    const std::shared_ptr<const hokuto::song::GbaRom> after = Round(game, "sustains", halved, {}, totals);
    if (after == nullptr)
    {
        return;
    }

    // A quarter of a second after the envelope reaches the sustain, for a tenth of a second.
    const int rate = hokuto::song::GbaRate(*rom);
    const auto quarter = static_cast<std::size_t>(rate / 4);
    const auto tenth = static_cast<std::size_t>(rate / 10);

    // The level of the error of a sample rounded down to a step: 8 bits, down to 6 bits at the hardware's highest rate.
    // The new level has to be at least 20 dB above it.
    const int bits = std::min(8, 9 - (rom->info.dac_bits >> 6 & 3));
    const double steps = 20.0 * std::log10(2.0 / static_cast<double>(1 << bits) / std::sqrt(3.0));
    for (const auto& [address, voice] : halved)
    {
        // The halved sustain takes the decay longer to reach. A sample that ends can end before the note holds it.
        const Place& place = places.at(address);
        const std::size_t reached = SustainFrames(voice, rate);
        mp2k::Wave wave;
        if (!mp2k::ReadWave(rom->rom, voice.wave, wave) || !wave.loops)
        {
            totals.ending++;
            continue;
        }
        if (reached == 0)
        {
            totals.slow++;
            continue;
        }
        try
        {
            const Held before = HeldLevel(rom, place, reached + quarter, tenth);
            if (before.ended)
            {
                totals.ended++;
                continue;
            }
            const double wanted = 20.0 * std::log10(static_cast<double>(voice.sustain) / place.voice.sustain);
            if (before.level + wanted < steps + 20.0)
            {
                totals.quiet++;
                continue;
            }
            if (before.peak >= 0.999)
            {
                totals.clipped++;
                continue;
            }
            const double old_level = before.level;
            const double new_level = HeldLevel(after, place, reached + quarter, tenth).level;
            const double off = std::abs(new_level - old_level - wanted);
            totals.levels++;
            totals.worst = std::max(totals.worst, off);
            if (off > 0.6)
            {
                std::printf("%s: %s: the level fell by %.2f dB, and the sustains by %.2f dB, from %.2f dBFS\n",
                            rom_name.c_str(), place.name.c_str(), old_level - new_level, -wanted, old_level);
                totals.failed++;
            }
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s: %s\n", rom_name.c_str(), place.name.c_str(), error.what());
            totals.failed++;
        }
    }
}

} // namespace hokuto::tools::gba_voices
