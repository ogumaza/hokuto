// SPDX-License-Identifier: MIT

#include "edit/gba_voices.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/mp2k/layout.h"
#include "ntdlib/mp2k/song.h"
#include "ntdlib/rounding.h"
#include "ntdlib/wave/pcm.h"
#include "xsflib/bytes.h"

#include "edit/native_runs.h"
#include "format/archive.h"
#include "format/file.h"
#include "song/gba_assembly.h"

namespace hokuto::edit
{
namespace
{

namespace mp2k = ntdlib::mp2k;

// The run of the ROM of `file` that contains `size` bytes at `offset` from the start of the ROM. Throws
// std::invalid_argument with a message for the user when no run contains them all, or when a field is among them.
RunPlace GbaRunOf(const format::File& file, uint64_t offset, uint64_t size)
{
    const std::variant<RunPlace, RunMiss> found = RunOf(*file.native, format::NativePieceStarts(file), offset, size);
    if (const auto* place = std::get_if<RunPlace>(&found))
    {
        return *place;
    }

    // The fields hold the addresses of the songs' tracks and headers.
    switch (std::get<RunMiss>(found))
    {
    case RunMiss::kSequence:
        throw std::invalid_argument("These bytes are part of a song, or overlap one. Can't change them here.");
    case RunMiss::kField:
        throw std::invalid_argument("These bytes contain the address of a song or a track. Can't change them here.");
    case RunMiss::kPastEnd:
        break;
    }

    throw std::invalid_argument("These bytes are past the end of the ROM.");
}

// Refuses `value` for the field `name` of a voice when it is above `highest` and differs from `before`, the value that
// the ROM contains.
void CheckValue(const char* name, uint32_t value, std::optional<uint32_t> before, int highest)
{
    if (value != before && value > static_cast<uint32_t>(highest))
    {
        throw std::invalid_argument(std::string("The ") + name + " of a PSG voice must be from 0 to " +
                                    std::to_string(highest) + ".");
    }
}

// Refuses the values of `voice` that MP2K's PSG voices don't take. `before` contains the values in the ROM for a voice
// that stays a PSG voice. Its values aren't checked.
void CheckPsgValues(const mp2k::Voice& voice, const mp2k::Voice* before)
{
    const auto held = [before](auto field) -> std::optional<uint32_t>
    {
        return before != nullptr ? std::optional<uint32_t>((*before).*field) : std::nullopt;
    };
    const int psg = voice.PsgChannel();
    if (psg == 0)
    {
        return;
    }

    CheckValue("attack", voice.attack, held(&mp2k::Voice::attack), kHighestPsgStep);
    CheckValue("decay", voice.decay, held(&mp2k::Voice::decay), kHighestPsgStep);
    CheckValue("sustain", voice.sustain, held(&mp2k::Voice::sustain), kHighestPsgSustain);
    CheckValue("release", voice.release, held(&mp2k::Voice::release), kHighestPsgStep);
    CheckValue("length", voice.length, held(&mp2k::Voice::length), kHighestPsgLength);
    if (psg == 1 && (voice.pan_sweep & 0x80) == 0 && (before == nullptr || (before->pan_sweep & 0x80) == 0))
    {
        CheckValue("sweep", voice.pan_sweep, held(&mp2k::Voice::pan_sweep), kHighestSweep);
    }

    if (psg <= 2)
    {
        CheckValue("duty cycle", voice.wave, held(&mp2k::Voice::wave), kHighestDuty);
    }

    if (psg == 4)
    {
        CheckValue("noise mode", voice.wave, held(&mp2k::Voice::wave), kHighestNoiseMode);
    }
}

} // namespace

format::File WithLastRunBytes(const format::File& file, std::size_t offset, std::span<const uint8_t> bytes,
                              uint8_t padding)
{
    format::File result = file;
    const auto write = [&](std::vector<ntdlib::sar::Piece>& pieces)
    {
        auto& run = std::get<std::vector<uint8_t>>(pieces.back());
        if (run.size() < offset + bytes.size())
        {
            run.resize(offset + bytes.size(), padding);
        }

        std::ranges::copy(bytes, run.begin() + static_cast<std::ptrdiff_t>(offset));
    };
    result.native.Edit([&write](format::NativeArchive& native) { native.pieces.Edit(write); });

    return result;
}

namespace
{

// The programs of the voice group at `group` that the songs select.
std::vector<int> SelectedPrograms(const song::GbaRom& rom, uint32_t group)
{
    for (const song::GbaVoiceGroup& known : song::GbaVoiceGroups(rom))
    {
        if (known.address == group)
        {
            return known.programs;
        }
    }

    return {};
}

// The programs of the voice group at `group` of `rom` that a new voice can use, where the songs select `taken`
// (FreeGbaPrograms).
std::vector<uint32_t> FreeProgramsOf(const song::GbaRom& rom, uint32_t group, const std::set<int>& taken)
{
    std::vector<uint32_t> free;
    for (int program = 0; program < 128; program++)
    {
        mp2k::Voice voice;
        const bool holds = mp2k::ReadVoice(rom.rom, group + mp2k::kVoiceSize * static_cast<uint32_t>(program), voice) &&
                           (voice.IsSplit() || song::GbaVoiceSounds(rom, voice));
        if (!taken.contains(program) && !holds)
        {
            free.push_back(static_cast<uint32_t>(program));
        }
    }

    return free;
}

// `file`'s voice group at `group`, moved into the free space. See WithGbaVoicesAt.
GbaGroupVersion MovedGbaGroup(const format::File& file, const song::GbaRom& rom, uint32_t group)
{
    const std::vector<int> programs = SelectedPrograms(rom, group);
    const std::set<int> selected(programs.begin(), programs.end());
    std::vector<uint8_t> table(128 * kGbaVoiceBytes);
    for (int program = 0; program < 128; program++)
    {
        const uint32_t entry = group + mp2k::kVoiceSize * static_cast<uint32_t>(program);
        mp2k::Voice voice;
        if (!rom.rom.Contains(entry, kGbaVoiceBytes) || !mp2k::ReadVoice(rom.rom, entry, voice) ||
            (!selected.contains(program) && !voice.IsSplit() && !song::GbaVoiceSounds(rom, voice)))
        {
            continue;
        }

        for (uint32_t i = 0; i < kGbaVoiceBytes; i++)
        {
            table[static_cast<std::size_t>(program) * kGbaVoiceBytes + i] = rom.rom.U8(entry + i);
        }
    }

    const PlacedGbaBytes placed = WithGbaNewBytes(file, table);
    GbaGroupVersion moved{placed.file, placed.address};
    std::array<uint8_t, 4> pointer{};
    xsflib::StoreLe32(pointer.data(), placed.address);

    // The header of each song that the cut made a piece starts the run after the piece. Such a song's copy in the free
    // space follows that header. Any other song's header is in a run at its address in the table. The header names the
    // voice group 4 bytes into it.
    const auto names_group = [group](const std::vector<uint8_t>& run, std::size_t at)
    {
        return at + 4 <= run.size() && xsflib::LoadLe32(&run[at]) == group;
    };
    const std::vector<ntdlib::sar::Piece> pieces = *moved.file.native->pieces;
    for (std::size_t piece = 0; piece + 1 < pieces.size(); piece++)
    {
        const auto* run = std::get_if<std::vector<uint8_t>>(&pieces[piece + 1]);
        if (std::holds_alternative<ntdlib::sar::SequencePiece>(pieces[piece]) && run != nullptr && names_group(*run, 4))
        {
            moved.file = WithRunBytes(moved.file, RunPlace{piece + 1, 4}, pointer);
        }
    }

    const std::vector<uint64_t> starts = format::NativePieceStarts(moved.file);
    for (const int song : song::GbaSongs(rom))
    {
        mp2k::SongHeader header;
        if (!mp2k::ReadSong(rom.rom, rom.info, song, header) || header.voices != group)
        {
            continue;
        }

        const uint64_t offset = uint64_t{header.address} - ntdlib::agb::kRomBase + 4;
        const std::variant<RunPlace, RunMiss> found = RunOf(*moved.file.native, starts, offset, 4);
        const auto* place = std::get_if<RunPlace>(&found);
        if (place != nullptr &&
            names_group(std::get<std::vector<uint8_t>>((*moved.file.native->pieces)[place->piece]), place->offset))
        {
            moved.file = WithRunBytes(moved.file, *place, pointer);
        }
    }

    // The instrument map of a MIDI import follows the group.
    if (const auto map = moved.file.instrument_maps.find(group); map != moved.file.instrument_maps.end())
    {
        auto instruments = std::move(map->second);
        moved.file.instrument_maps.erase(map);
        moved.file.instrument_maps.insert_or_assign(placed.address, std::move(instruments));
    }

    return moved;
}

} // namespace

std::array<uint8_t, kGbaVoiceBytes> GbaVoiceBytes(const mp2k::Voice& voice)
{
    // The type, the key, the length and the pan or the sweep, then the sample or the PSG's value, then the envelope or
    // a key split's key table. The ROM stores its numbers little-endian.
    std::array<uint8_t, kGbaVoiceBytes> bytes{};
    bytes[0] = voice.type;
    bytes[1] = voice.key;
    bytes[2] = voice.length;
    bytes[3] = voice.pan_sweep;
    xsflib::StoreLe32(&bytes[4], voice.wave);
    if ((voice.type & mp2k::kVoiceKeySplit) != 0)
    {
        xsflib::StoreLe32(&bytes[8], voice.key_table);
    }
    else
    {
        bytes[8] = voice.attack;
        bytes[9] = voice.decay;
        bytes[10] = voice.sustain;
        bytes[11] = voice.release;
    }

    return bytes;
}

GbaPattern GbaPatternOf(const ntdlib::agb::Rom& rom, uint32_t address)
{
    if (!rom.Contains(address, kGbaPatternSteps / 2))
    {
        throw std::invalid_argument("The wave pattern is outside the ROM.");
    }

    GbaPattern steps{};
    for (std::size_t i = 0; i < kGbaPatternSteps / 2; ++i)
    {
        const uint8_t byte = rom.U8(address + static_cast<uint32_t>(i));
        steps[2 * i] = static_cast<uint8_t>(byte >> 4);
        steps[2 * i + 1] = static_cast<uint8_t>(byte & 0x0F);
    }

    return steps;
}

format::File WithGbaBytes(const format::File& file, uint32_t address, std::span<const uint8_t> bytes)
{
    if (!file.native || file.native->engine != format::Engine::kGba)
    {
        throw std::invalid_argument("The file contains no GBA ROM.");
    }
    if (address < ntdlib::agb::kRomBase)
    {
        throw std::invalid_argument("These bytes are outside the ROM.");
    }

    return WithRunBytes(file, GbaRunOf(file, uint64_t{address} - ntdlib::agb::kRomBase, bytes.size()), bytes);
}

format::File WithGbaVoice(const format::File& file, const mp2k::Voice& voice)
{
    if (!file.native || file.native->engine != format::Engine::kGba)
    {
        throw std::invalid_argument("The file contains no GBA ROM.");
    }

    // The values that the ROM contains now. A split keeps its kind and its tables: the edits of the Bank Editor don't
    // change them.
    const uint64_t offset = uint64_t{voice.address} - ntdlib::agb::kRomBase;
    const RunPlace place = GbaRunOf(file, offset, kGbaVoiceBytes);
    const auto& run = std::get<std::vector<uint8_t>>((*file.native->pieces)[place.piece]);
    const mp2k::Voice before = [&run, at = place.offset, &voice]
    {
        mp2k::Voice held;
        held.address = voice.address;
        held.type = run[at];
        held.key = run[at + 1];
        held.length = run[at + 2];
        held.pan_sweep = run[at + 3];
        held.wave = xsflib::LoadLe32(&run[at + 4]);
        held.key_table = xsflib::LoadLe32(&run[at + 8]);
        held.attack = run[at + 8];
        held.decay = run[at + 9];
        held.sustain = run[at + 10];
        held.release = run[at + 11];
        return held;
    }();
    if (before.IsSplit() != voice.IsSplit() ||
        (voice.IsSplit() && (before.type != voice.type || before.wave != voice.wave ||
                             ((voice.type & mp2k::kVoiceKeySplit) != 0 && before.key_table != voice.key_table))))
    {
        throw std::invalid_argument("Can't change the tables of a key split or a drum kit.");
    }

    // A PSG voice accepts the values that fit in the PSG registers and the MP2K envelope.
    CheckPsgValues(voice, before.PsgChannel() != 0 ? &before : nullptr);

    const std::array<uint8_t, kGbaVoiceBytes> bytes = GbaVoiceBytes(voice);
    return WithGbaBytes(file, voice.address, bytes);
}

std::array<uint8_t, kGbaPatternSteps / 2> GbaPatternBytes(const GbaPattern& steps)
{
    std::array<uint8_t, kGbaPatternSteps / 2> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i)
    {
        if (steps[2 * i] > 15 || steps[2 * i + 1] > 15)
        {
            throw std::invalid_argument("Wave pattern steps must be from 0 to 15.");
        }

        bytes[i] = static_cast<uint8_t>((steps[2 * i] << 4) | steps[2 * i + 1]);
    }

    return bytes;
}

format::File WithGbaPattern(const format::File& file, uint32_t address, const GbaPattern& steps)
{
    return WithGbaBytes(file, address, GbaPatternBytes(steps));
}

GbaPattern NewGbaPattern()
{
    GbaPattern steps{};
    for (std::size_t i = 0; i < steps.size(); ++i)
    {
        steps[i] = static_cast<uint8_t>(i < 16 ? i : 31 - i);
    }

    return steps;
}

std::vector<uint8_t> EncodeGbaSample(std::span<const int16_t> samples, uint32_t rate,
                                     std::optional<uint32_t> loop_start)
{
    // A rate above this doesn't fit the header's 32 bits, at 1024 steps to the hertz.
    constexpr uint32_t kHighestRate = 0xFFFFFFFF / 1024;
    if (samples.empty())
    {
        throw std::invalid_argument("A sample must contain at least one sample point.");
    }
    if (rate == 0 || rate > kHighestRate)
    {
        throw std::invalid_argument("The sample rate must be from 1 to " + std::to_string(kHighestRate) + " Hz.");
    }
    if (loop_start && *loop_start >= samples.size())
    {
        throw std::invalid_argument("The loop must start before the end of the sample.");
    }

    std::vector<uint8_t> bytes(16);
    xsflib::StoreLe32(&bytes[0], loop_start ? 0x40000000 : 0);
    xsflib::StoreLe32(&bytes[4], rate * 1024);
    xsflib::StoreLe32(&bytes[8], loop_start.value_or(0));
    xsflib::StoreLe32(&bytes[12], static_cast<uint32_t>(samples.size()));

    bytes.reserve(16 + samples.size() + 1);
    std::ranges::transform(samples, std::back_inserter(bytes), ntdlib::wave::EncodePcm8);
    bytes.push_back(ntdlib::wave::EncodePcm8(loop_start ? samples[*loop_start] : samples.back()));

    return bytes;
}

PlacedGbaBytes WithGbaNewBytes(const format::File& file, std::span<const uint8_t> bytes)
{
    if (!file.native || file.native->engine != format::Engine::kGba)
    {
        throw std::invalid_argument("The file contains no GBA ROM.");
    }

    const std::vector<ntdlib::sar::Piece>& pieces = *file.native->pieces;
    const auto* last = pieces.empty() ? nullptr : std::get_if<std::vector<uint8_t>>(&pieces.back());
    if (last == nullptr)
    {
        throw std::invalid_argument("The ROM ends inside a song. There's no room for new bytes.");
    }

    // The bytes, then zeros up to a multiple of 4 bytes. The free space then starts after them.
    std::vector<uint8_t> placed(bytes.begin(), bytes.end());
    placed.resize(ntdlib::AlignUp<uint64_t>(placed.size() + 1, 4));

    const std::vector<uint64_t> starts = format::NativePieceStarts(file);
    const uint64_t run_start = starts[pieces.size() - 1];
    const uint64_t start = mp2k::FreeSpaceStart(*last, run_start);
    if (start + placed.size() <= mp2k::kLargestRom)
    {
        return {WithLastRunBytes(file, static_cast<std::size_t>(start - run_start), placed, 0xFF),
                static_cast<uint32_t>(start + ntdlib::agb::kRomBase)};
    }

    // No room after the ROM's data.
    if (file.native->gba_filler)
    {
        const std::shared_ptr<const song::GbaRom> rom = song::LoadGbaRom(format::ExportArchive(file));
        for (const mp2k::Span& filler : mp2k::FillerRuns(rom->rom, rom->info))
        {
            if (filler.size >= placed.size())
            {
                const auto found = RunOf(*file.native, starts, filler.start, placed.size());
                if (const auto* place = std::get_if<RunPlace>(&found))
                {
                    return {WithRunBytes(file, *place, placed), filler.start + ntdlib::agb::kRomBase};
                }
            }
        }

        throw std::invalid_argument("No room for " + std::to_string(placed.size()) +
                                    " more bytes. The ROM can't exceed 32 MiB, and no filler run is large enough.");
    }

    throw std::invalid_argument("No room for " + std::to_string(placed.size()) +
                                " more bytes. The ROM can't exceed 32 MiB. To use runs of 0xFF bytes inside the ROM, "
                                "turn on Move songs into filler runs in the Export window.");
}

mp2k::Voice GbaVoiceOfKind(const mp2k::Voice& voice, int psg, uint32_t wave)
{
    if (psg == 0 && voice.PsgChannel() == 0)
    {
        mp2k::Voice same = voice;
        same.wave = wave;
        return same;
    }

    mp2k::Voice made = voice;
    const bool was_psg = voice.PsgChannel() != 0;
    made.type = static_cast<uint8_t>(psg | (voice.type & mp2k::kVoiceFixed));
    made.pan_sweep = (voice.pan_sweep & 0x80) != 0 ? voice.pan_sweep : 0;
    if (psg == 0)
    {
        made.wave = wave;
        made.length = 0;
        made.attack = 255;
        made.decay = 0;
        made.sustain = 255;
        made.release = 165;
        return made;
    }

    if (!was_psg)
    {
        made.length = 0;
        made.attack = 0;
        made.decay = 0;
        made.sustain = kHighestPsgSustain;
        made.release = 0;
    }

    const bool was_square = voice.PsgChannel() == 1 || voice.PsgChannel() == 2;
    made.wave = psg == 3 ? wave : psg == 4 ? 0 : was_square ? voice.wave : 2;

    return made;
}

mp2k::Voice NewGbaSampleVoice(uint32_t sample)
{
    return GbaVoiceOfKind(NewGbaSquareVoice(), 0, sample);
}

mp2k::Voice NewGbaSquareVoice()
{
    mp2k::Voice voice;
    voice.type = 2;
    voice.key = 60;
    voice.wave = 2;
    voice.sustain = kHighestPsgSustain;

    return voice;
}

std::vector<uint32_t> FreeGbaPrograms(const song::GbaRom& rom, uint32_t group)
{
    const std::vector<int> programs = SelectedPrograms(rom, group);

    return FreeProgramsOf(rom, group, std::set<int>(programs.begin(), programs.end()));
}

std::vector<uint32_t> UnplayedGbaPrograms(const song::GbaRom& rom, uint32_t group)
{
    return GbaOpenProgramsOf(rom, group).unplayed;
}

GbaOpenPrograms GbaOpenProgramsOf(const song::GbaRom& rom, uint32_t group)
{
    const std::vector<int> programs = SelectedPrograms(rom, group);
    const std::set<int> taken(programs.begin(), programs.end());
    GbaOpenPrograms open;
    open.free = FreeProgramsOf(rom, group, taken);
    for (int program = 0; program < 128; program++)
    {
        if (!taken.contains(program) && !std::ranges::binary_search(open.free, static_cast<uint32_t>(program)))
        {
            open.unplayed.push_back(static_cast<uint32_t>(program));
        }
    }

    return open;
}

GbaGroupVersion WithGbaVoicesAt(const format::File& file, const song::GbaRom& rom, uint32_t group,
                                std::span<const std::pair<uint32_t, mp2k::Voice>> voices)
{
    if (!file.native || file.native->engine != format::Engine::kGba)
    {
        throw std::invalid_argument("The file contains no GBA ROM.");
    }

    for (const auto& [program, voice] : voices)
    {
        if (program > 127)
        {
            throw std::invalid_argument("Programs in a voice group must be from 0 to 127.");
        }

        const uint32_t address = group + mp2k::kVoiceSize * program;
        mp2k::Voice before;
        const bool held = rom.rom.Contains(address, kGbaVoiceBytes) && mp2k::ReadVoice(rom.rom, address, before) &&
                          before.PsgChannel() != 0;
        CheckPsgValues(voice, held ? &before : nullptr);
    }

    // A table that Hokuto wrote, or a table of a game that's as safe: every entry is a voice or empty, up to the data
    // of the ROM.
    const uint64_t offset = uint64_t{group} - ntdlib::agb::kRomBase;
    const std::vector<uint64_t> starts = format::NativePieceStarts(file);
    const auto found = RunOf(*file.native, starts, offset, 128 * kGbaVoiceBytes);
    const auto* place = std::get_if<RunPlace>(&found);
    const bool in_place = place != nullptr && place->piece + 1 == file.native->pieces->size() &&
                          song::GbaTableHoldsVoices(rom, group, SelectedPrograms(rom, group));
    GbaGroupVersion at = in_place ? GbaGroupVersion{file, group} : MovedGbaGroup(file, rom, group);
    for (const auto& [program, voice] : voices)
    {
        mp2k::Voice placed = voice;
        placed.address = at.group + mp2k::kVoiceSize * program;
        const std::array<uint8_t, kGbaVoiceBytes> bytes = GbaVoiceBytes(placed);
        at.file = WithGbaBytes(at.file, placed.address, bytes);
    }

    return at;
}

GbaGroupVersion WithGbaVoiceGroupFile(const format::File& file, const song::GbaRom& rom, uint32_t group,
                                      std::string_view text)
{
    // A part that contains the bytes of the ROM at the address of its label stays there. The other parts go together
    // into the free space. Their size doesn't depend on where they go. Zeros of that size therefore go in first and
    // give the place for their bytes.
    const song::GbaVoiceGroupSource first = song::ReadGbaVoiceGroup(text);
    std::map<std::string, uint32_t> placed;
    std::vector<song::GbaVoiceGroupPart> moved;
    std::size_t moved_size = 0;
    for (const song::GbaVoiceGroupPart& part : first.parts)
    {
        const std::optional<uint32_t> address = song::GbaAddressOfName(part.label);
        const auto size = static_cast<uint32_t>(part.size);
        if (address && rom.rom.Contains(*address, size) &&
            std::equal(first.bytes.begin() + static_cast<std::ptrdiff_t>(part.offset),
                       first.bytes.begin() + static_cast<std::ptrdiff_t>(part.offset + part.size),
                       rom.rom.Ptr(*address)))
        {
            placed[part.label] = *address;
        }
        else
        {
            moved_size = ntdlib::AlignUp<uint64_t>(moved_size, 4) + part.size;
            moved.push_back(part);
        }
    }

    uint32_t start = 0;
    if (!moved.empty())
    {
        start = WithGbaNewBytes(file, std::vector<uint8_t>(moved_size)).address;
        std::size_t offset = 0;
        for (const song::GbaVoiceGroupPart& part : moved)
        {
            offset = ntdlib::AlignUp<uint64_t>(offset, 4);
            placed[part.label] = start + static_cast<uint32_t>(offset);
            offset += part.size;
        }
    }

    const song::GbaVoiceGroupSource source = song::ReadGbaVoiceGroup(text, placed);
    format::File with_parts = file;
    if (!moved.empty())
    {
        std::vector<uint8_t> bytes(moved_size);
        for (const song::GbaVoiceGroupPart& part : moved)
        {
            std::copy_n(source.bytes.begin() + static_cast<std::ptrdiff_t>(part.offset), part.size,
                        bytes.begin() + (placed.at(part.label) - start));
        }

        with_parts = WithGbaNewBytes(file, bytes).file;
    }

    // Only the voices that the file changes go in: a voice of the game's can hold values that an edit refuses.
    std::vector<std::pair<uint32_t, mp2k::Voice>> voices;
    for (std::size_t program = 0; program < source.voices; program++)
    {
        const std::span<const uint8_t> bytes =
            std::span(source.bytes).subspan(source.table + program * kGbaVoiceBytes, kGbaVoiceBytes);
        const uint32_t address = group + static_cast<uint32_t>(program * kGbaVoiceBytes);
        if (rom.rom.Contains(address, kGbaVoiceBytes) &&
            std::ranges::equal(bytes, std::span(rom.rom.Ptr(address), kGbaVoiceBytes)))
        {
            continue;
        }

        const auto word = [bytes](std::size_t at)
        {
            return xsflib::LoadLe32(&bytes[at]);
        };
        mp2k::Voice voice;
        voice.type = bytes[0];
        voice.key = bytes[1];
        voice.length = bytes[2];
        voice.pan_sweep = bytes[3];
        voice.wave = word(4);
        voice.attack = bytes[8];
        voice.decay = bytes[9];
        voice.sustain = bytes[10];
        voice.release = bytes[11];
        voice.key_table = word(8);
        voices.emplace_back(static_cast<uint32_t>(program), voice);
    }
    if (voices.empty())
    {
        return {file, group};
    }

    return WithGbaVoicesAt(with_parts, rom, group, voices);
}

format::File WithoutGbaVoice(const format::File& file, uint32_t address)
{
    const std::array<uint8_t, kGbaVoiceBytes> zeros{};
    return WithGbaBytes(file, address, zeros);
}

mp2k::Voice GbaSplitOf(const mp2k::Voice& voice, uint8_t type)
{
    mp2k::Voice split;
    split.address = voice.address;
    split.type = type;
    split.key = voice.key;

    return split;
}

std::vector<GbaKeyRow> GbaRowsWithNew(std::span<const GbaKeyRow> rows, bool drums, std::optional<int> selected)
{
    std::vector<GbaKeyRow> made(rows.begin(), rows.end());
    const auto present = selected ? std::ranges::find(made, *selected, &GbaKeyRow::first_key) : made.end();
    if (made.empty())
    {
        made.push_back(drums ? GbaKeyRow{60, 60, NewGbaSquareVoice()} : GbaKeyRow{0, 127, NewGbaSquareVoice()});
        return made;
    }

    const GbaKeyRow source = present != made.end() ? *present : made.front();

    if (!drums)
    {
        if (source.first_key == source.last_key)
        {
            throw std::invalid_argument(
                "This row covers a single key and can't be split. Select a row with two or more keys.");
        }

        const auto row = std::ranges::find(made, source.first_key, &GbaKeyRow::first_key);
        const int middle = (source.first_key + source.last_key + 1) / 2;
        row->last_key = middle - 1;
        made.insert(row + 1, GbaKeyRow{middle, source.last_key, source.voice});
        return made;
    }

    const auto free_from = [&made](int first) -> std::optional<int>
    {
        for (int key = first; key <= 127; key++)
        {
            if (std::ranges::find(made, key, &GbaKeyRow::first_key) == made.end())
            {
                return key;
            }
        }

        return std::nullopt;
    };
    std::optional<int> key = free_from(source.first_key);
    key = key ? key : free_from(0);
    if (!key)
    {
        throw std::invalid_argument("Every key of the drum kit already has a voice.");
    }

    made.push_back({*key, *key, source.voice});
    std::ranges::sort(made, {}, &GbaKeyRow::first_key);

    return made;
}

std::vector<GbaKeyRow> GbaSplitRowsWithout(std::span<const GbaKeyRow> rows, int first_key)
{
    if (rows.size() < 2)
    {
        throw std::invalid_argument(
            "A key split must have at least one voice. To keep only this voice, use Make Single Voice.");
    }

    std::vector<GbaKeyRow> made(rows.begin(), rows.end());
    const auto row = std::ranges::find(made, first_key, &GbaKeyRow::first_key);
    if (row == made.end())
    {
        return made;
    }

    if (row != made.begin())
    {
        (row - 1)->last_key = row->last_key;
    }
    else
    {
        (row + 1)->first_key = row->first_key;
    }

    made.erase(row);

    return made;
}

std::vector<GbaKeyRow> GbaRowsWithKeys(std::span<const GbaKeyRow> rows, bool drums, int first_key, int new_first,
                                       int new_last)
{
    const auto present = std::ranges::find(rows, first_key, &GbaKeyRow::first_key);
    if (present == rows.end())
    {
        return {rows.begin(), rows.end()};
    }

    const GbaKeyRow moved{new_first, new_last, present->voice};
    std::vector<GbaKeyRow> made;
    for (const GbaKeyRow& row : rows)
    {
        if (row.first_key == first_key)
        {
            continue;
        }

        const bool overlaps = row.first_key <= new_last && row.last_key >= new_first;
        if (drums && overlaps)
        {
            throw std::invalid_argument("Another voice of the drum kit is already at that key.");
        }

        if (!overlaps)
        {
            made.push_back(row);
            continue;
        }

        // The part of the row below the moved row, and the part above it.
        if (row.first_key < new_first)
        {
            made.push_back({row.first_key, new_first - 1, row.voice});
        }

        if (row.last_key > new_last)
        {
            made.push_back({new_last + 1, row.last_key, row.voice});
        }
    }

    made.push_back(moved);
    std::ranges::sort(made, {}, &GbaKeyRow::first_key);

    return made;
}

format::File WithGbaKeyRows(const format::File& file, const mp2k::Voice& split, std::span<const GbaKeyRow> rows)
{
    const bool drums = (split.type & mp2k::kVoiceDrumKit) != 0;
    if (!split.IsSplit())
    {
        throw std::invalid_argument("This voice isn't a key split or a drum kit.");
    }
    if (rows.empty() && !drums)
    {
        throw std::invalid_argument("A key split must have at least one voice.");
    }

    int next = 0;
    std::vector<GbaKeyRow> sorted(rows.begin(), rows.end());
    std::ranges::sort(sorted, {}, &GbaKeyRow::first_key);
    for (const GbaKeyRow& row : sorted)
    {
        if (row.first_key < next || row.last_key < row.first_key || row.last_key > 127)
        {
            throw std::invalid_argument("Key ranges can't overlap or go above key 127.");
        }
        if (drums && row.first_key != row.last_key)
        {
            throw std::invalid_argument("Each voice of a drum kit must cover a single key.");
        }
        if (row.voice.IsSplit())
        {
            throw std::invalid_argument("A key split or a drum kit can't contain another key split or drum kit.");
        }

        next = row.last_key + 1;
    }

    mp2k::Voice voice = split;
    format::File edited = file;
    if (drums)
    {
        std::vector<uint8_t> table(128 * kGbaVoiceBytes);
        for (const GbaKeyRow& row : sorted)
        {
            std::ranges::copy(GbaVoiceBytes(row.voice), table.begin() + row.first_key * std::ptrdiff_t{kGbaVoiceBytes});
        }

        PlacedGbaBytes placed = WithGbaNewBytes(edited, table);
        voice.wave = placed.address;
        edited = std::move(placed.file);
    }
    else
    {
        // The table's last entry stays empty for the keys that no row plays.
        std::vector<uint8_t> table((sorted.size() + 1) * kGbaVoiceBytes);
        std::vector<uint8_t> keys(128, static_cast<uint8_t>(sorted.size()));
        for (std::size_t index = 0; index < sorted.size(); index++)
        {
            std::ranges::copy(GbaVoiceBytes(sorted[index].voice),
                              table.begin() + static_cast<std::ptrdiff_t>(index * kGbaVoiceBytes));
            for (int key = sorted[index].first_key; key <= sorted[index].last_key; key++)
            {
                keys[static_cast<std::size_t>(key)] = static_cast<uint8_t>(index);
            }
        }

        PlacedGbaBytes placed_table = WithGbaNewBytes(edited, table);
        PlacedGbaBytes placed_keys = WithGbaNewBytes(placed_table.file, keys);
        voice.wave = placed_table.address;
        voice.key_table = placed_keys.address;
        edited = std::move(placed_keys.file);
    }

    return WithGbaProgramVoice(edited, split.address, voice);
}

format::File WithGbaProgramVoice(const format::File& file, uint32_t address, const mp2k::Voice& voice)
{
    const std::array<uint8_t, kGbaVoiceBytes> bytes = GbaVoiceBytes(voice);
    return WithGbaBytes(file, address, bytes);
}

} // namespace hokuto::edit
