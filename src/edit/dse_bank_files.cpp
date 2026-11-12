// SPDX-License-Identifier: MIT

// The bank and wave edits of edit/dse_files.h: a new bank or wave of a DSE set, the warnings for a new bank, and what
// plays a bank's waves. dse_files.cpp replaces the songs.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "ntdlib/dse/commands.h"
#include "ntdlib/dse/formats.h"
#include "ntdlib/sar/layout.h"
#include "ntdlib/sdat/formats.h"
#include "xsflib/binary.h"

#include "edit/dse_banks.h"
#include "edit/dse_files.h"
#include "edit/dse_files_parts.h"
#include "format/archive.h"

namespace hokuto::edit
{
namespace
{

namespace dse = ntdlib::dse;
using dse_files_parts::BankAt;
using dse_files_parts::kImaAdpcm;
using dse_files_parts::kTuningOfDoubledRate;
using dse_files_parts::kWaveFormat;
using dse_files_parts::NumberList;
using dse_files_parts::ProgramsOf;
using dse_files_parts::RequireSet;
using dse_files_parts::RetuneSplits;

// A bank's header: its id, its name, the main bank's id of a bank whose waves are in one, and the kind of the bank.
constexpr std::size_t kBankId = 0x0E;
constexpr std::size_t kName = 0x20;
constexpr std::size_t kNameSize = 16;
constexpr std::size_t kMainBankId = 0x40;
constexpr std::size_t kBankKind = 0x4B;
constexpr uint8_t kWavesInMainBank = 2;

// `file` with `bytes` in the place of the run of its native layout that starts at `offset`: the run of a file of the
// set that no sequence cuts. The fields of the set's directory follow the new size.
format::File WithFileRun(const format::File& file, std::span<const uint64_t> starts, uint64_t offset,
                         std::vector<uint8_t> bytes)
{
    const std::vector<ntdlib::sar::Piece>& pieces = *file.native->pieces;
    std::optional<std::size_t> piece;
    for (std::size_t p = 0; p < pieces.size(); ++p)
    {
        if (starts[p] == offset && std::holds_alternative<std::vector<uint8_t>>(pieces[p]))
        {
            piece = p;
            break;
        }
    }
    if (!piece)
    {
        throw std::invalid_argument("The bank's file shares its bytes with other data. Hokuto can't replace it.");
    }

    for (const ntdlib::sar::Field& field : file.native->fields)
    {
        if (field.at.piece == *piece && !field.at.sequence && !field.at.item)
        {
            throw std::invalid_argument("The bank's file holds a field of the set. Hokuto can't replace it.");
        }
    }

    format::File result = file;
    const auto write = [&](std::vector<ntdlib::sar::Piece>& edited)
    {
        edited[*piece] = std::move(bytes);
    };
    result.native.Edit([&write](format::NativeArchive& native) { native.pieces.Edit(write); });

    return result;
}

// The bytes of the samples that the loop of `wave` describes: its start and its length, in words.
std::size_t SamplesSize(const ntdlib::sdat::Wave& wave)
{
    return (std::size_t{wave.loop_start} + wave.loop_length) * 4;
}

// The wave of `bytes`, a DS wave's parameters and samples (ntdlib::sdat::WaveBytes), in the DS's form: an encoding, and
// a loop in words. Its data points into `bytes`. Throws std::invalid_argument with a message for the user for bytes
// that aren't such a wave, and for fewer samples than the loop requires.
ntdlib::sdat::Wave NewDsWave(std::span<const uint8_t> bytes)
{
    ntdlib::sdat::Wave made;
    try
    {
        made = ntdlib::sdat::Wave::Parse(xsflib::Reader(bytes), 0, bytes.size());
    }
    catch (const xsflib::FormatError& error)
    {
        throw std::invalid_argument(std::string("Can't read the new wave: ") + error.what() + ".");
    }

    if (made.data.size() < SamplesSize(made))
    {
        throw std::invalid_argument("The new wave holds fewer samples than its loop gives.");
    }

    return made;
}

} // namespace

std::vector<uint32_t> DseSoundsOfBank(const song::DseSet& set, uint32_t bank)
{
    const song::DseBank& found = BankAt(set, bank);
    std::vector<uint32_t> sounds;
    for (std::size_t sound = 0; sound < set.sounds.size(); ++sound)
    {
        for (const std::shared_ptr<const dse::WaveBank>& played : set.sounds[sound].banks)
        {
            if (played == found.bank)
            {
                sounds.push_back(static_cast<uint32_t>(sound));
                break;
            }
        }
    }

    return sounds;
}

dse::WaveBank ReadIncomingDseBank(std::span<const uint8_t> bytes)
{
    try
    {
        return dse::WaveBank::Parse(bytes);
    }
    catch (const xsflib::FormatError& error)
    {
        throw std::invalid_argument(std::string("Can't read the file as a DSE wave bank (swdl): ") + error.what() +
                                    ".");
    }
}

format::File ReplaceDseBank(const format::File& file, const song::DseSet& set, uint32_t bank,
                            const dse::WaveBank& incoming)
{
    RequireSet(file);
    const song::DseBank& found = BankAt(set, bank);
    const dse::WaveBank& present = *found.bank;

    // The new bank keeps the current bank's id and name. One whose waves are in a main bank keeps the current main
    // bank.
    dse::WaveBank next = incoming;
    std::copy_n(present.header.begin() + kBankId, 2, next.header.begin() + kBankId);
    std::copy_n(present.header.begin() + kName, kNameSize, next.header.begin() + kName);
    if (!next.samples)
    {
        if (present.samples)
        {
            throw std::invalid_argument(
                "The new bank has no samples. Its waves are in a main bank. Choose a bank "
                "file with samples to replace " +
                song::DseBankName(present) + ".");
        }

        std::copy_n(present.header.begin() + kMainBankId, 4, next.header.begin() + kMainBankId);
        next.header[kBankKind] = kWavesInMainBank;
    }

    const std::vector<uint8_t> bytes = next.Write();

    // Every file of the set that contains the bank gets it. The later files move, and the fields follow.
    format::File result = file;
    std::vector<uint64_t> offsets;
    for (const song::DseBank& each : set.banks)
    {
        if (each.bank == found.bank)
        {
            offsets.push_back(each.file_offset);
        }
    }

    std::ranges::sort(offsets, std::greater<>());
    const std::vector<uint64_t> starts = format::NativePieceStarts(file);
    for (const uint64_t offset : offsets)
    {
        // The files after the edited ones keep their starts: the edits go from the last file to the first.
        result = WithFileRun(result, starts, offset, bytes);
    }

    return result;
}

std::vector<std::string> DseBankWarnings(const song::DseSet& set, uint32_t bank, const dse::WaveBank& incoming)
{
    const song::DseBank& found = BankAt(set, bank);
    std::vector<std::string> warnings;

    // The programs that the bank's sounds select and that are missing from the new bank.
    std::set<uint32_t> missing;
    const std::vector<uint32_t> held = song::DseBankPrograms(incoming);
    const std::vector<uint32_t> sounds = DseSoundsOfBank(set, bank);
    for (const uint32_t sound : sounds)
    {
        const auto& input = set.sounds[sound];
        const std::vector<dse::Track>* tracks =
            input.song ? &input.song->tracks
            : input.effects && input.effect < input.effects->sequences.size() && input.effects->sequences[input.effect]
                ? &input.effects->sequences[input.effect]->tracks
                : nullptr;
        if (tracks == nullptr)
        {
            continue;
        }

        for (const uint32_t program : ProgramsOf(dse::DecodeTracks(*tracks)))
        {
            if (std::ranges::find(held, program) == held.end())
            {
                missing.insert(program);
            }
        }
    }
    if (!missing.empty())
    {
        warnings.push_back("Programs missing from the new bank: " + NumberList(missing) + ". Their notes are silent.");
    }

    // The waves that the new splits play: a bank whose waves are in a main bank plays the main bank's waves of the same
    // numbers.
    std::set<uint32_t> unheld;
    const dse::WaveBank* waves = incoming.samples ? &incoming : found.main_bank.get();
    if (incoming.instruments)
    {
        for (const std::optional<dse::Program>& program : incoming.instruments->programs)
        {
            if (!program)
            {
                continue;
            }

            for (const DseSplit& split : program->splits)
            {
                const auto wave = static_cast<uint32_t>(DseSplitWave(split));
                const bool in_table = wave < incoming.waves.size() && incoming.waves[wave];
                const bool in_main = waves != nullptr && wave < waves->waves.size() && waves->waves[wave];
                if (!in_table || !in_main)
                {
                    unheld.insert(wave);
                }
            }
        }
    }

    // The bank that contains the waves that the new splits play.
    const std::string holder = incoming.samples   ? std::string("the new bank")
                               : waves != nullptr ? "the main bank " + song::DseBankName(*waves)
                                                  : std::string("every bank");
    if (!unheld.empty())
    {
        warnings.push_back("Waves missing from " + holder + ": " + NumberList(unheld) + ". Their notes are silent.");
    }

    if (!incoming.samples && found.main_bank)
    {
        warnings.push_back("The new bank holds no samples. Its splits play the waves of " + holder +
                           " with the same numbers.");
    }

    // New samples may not fit in the game's memory.
    if (!sounds.empty() && incoming.samples && !found.bank->samples)
    {
        warnings.push_back(
            "The new bank contains samples. The current bank uses the main bank's waves. The game may not load the "
            "new samples.");
    }

    return warnings;
}

std::size_t DseWaveSize(std::span<const uint8_t> wave)
{
    return SamplesSize(NewDsWave(wave));
}

namespace
{

// Sets the encoding, the loop and the rate `rate` of the new samples in wave `wave` of `model`, a bank whose samples
// are `made`. With `holds_samples`, or where the wave's samples start at `old_position`, they start at `position`. Each
// split that plays the wave moves its tuning by the new rate (ReplaceDseWave). Throws std::invalid_argument with a
// message for the user for a tuning out of range.
void WithWave(dse::WaveBank& model, bool holds_samples, uint32_t wave, const ntdlib::sdat::Wave& made, uint32_t rate,
              uint32_t old_position, uint32_t position)
{
    const uint8_t format = static_cast<uint8_t>(made.encoding);
    dse::WaveInfo& held = *model.waves[wave];
    const uint32_t was = held.SampleRate();
    held.bytes[kWaveFormat] = format;
    held.SetLoops(made.loop != 0);
    held.SetSampleRate(rate);
    if (holds_samples || held.SamplePosition() == old_position)
    {
        held.SetSamplePosition(position);
    }

    held.SetLoopStart(made.loop_start);
    held.SetLoopLength(made.loop_length);
    if (was == 0 || was == rate || !model.instruments)
    {
        return;
    }

    RetuneSplits(model, wave, kTuningOfDoubledRate * std::log2(static_cast<double>(rate) / was));
}

// Copies the first `size` bytes of `made` into `samples`, the samples of a bank, for the wave that `info` describes.
// Returns where they start (ReplaceDseWave). Throws std::invalid_argument with a message for the user when the bank
// can't take more samples.
uint32_t PlaceSamples(std::vector<uint8_t>& samples, const ntdlib::sdat::Wave& made, std::size_t size,
                      const dse::WaveInfo& info)
{
    // The samples go where the wave's lay when they fit there, or else after the bank's samples.
    const uint32_t old_position = info.SamplePosition();
    const std::size_t old_size = std::size_t{info.LoopStart() + info.LoopLength()} * 4;
    uint32_t position = old_position;
    if (size > old_size || uint64_t{old_position} + size > samples.size())
    {
        const std::size_t end = (samples.size() + 3) / 4 * 4;
        if (end + size > std::numeric_limits<uint32_t>::max())
        {
            throw std::invalid_argument("The bank can't hold more samples.");
        }

        position = static_cast<uint32_t>(end);
        samples.resize(end + size, 0);
    }

    std::copy_n(made.data.begin(), size, samples.begin() + position);

    return position;
}

// Wave `wave` of bank `bank` of `set`, where a replacement can take its samples (ReplaceDseWave and
// ChangesOfDseWave). Throws std::invalid_argument with a message for the user for a wave that the bank doesn't
// contain, a bank without samples, and a square wave or noise.
const dse::WaveInfo& ReplaceableWave(const song::DseSet& set, uint32_t bank, uint32_t wave)
{
    const dse::WaveBank& holder = *BankAt(set, bank).bank;
    const std::string name = song::DseItemName(set, {.kind = song::ItemKind::kWave, .index = bank, .wave = wave});
    if (wave >= holder.waves.size() || !holder.waves[wave])
    {
        throw std::invalid_argument("The image has no " + name + ".");
    }
    if (!holder.samples)
    {
        throw std::invalid_argument(song::DseBankName(holder) +
                                    " holds no samples. Replace the wave in its main bank.");
    }

    const dse::WaveInfo& info = *holder.waves[wave];
    if (info.bytes[kWaveFormat] > kImaAdpcm)
    {
        throw std::invalid_argument("Square waves and noise have no samples to replace.");
    }

    return info;
}

// The banks of `set` that can play wave `wave` of bank `bank`: the bank, and those whose waves are in it, once each,
// with instruments. Each comes as its place in `set` and the count of its splits that play the wave (DseWaveUses and
// ChangesOfDseWave).
std::vector<std::pair<std::size_t, std::size_t>> SplitsPlaying(const song::DseSet& set, uint32_t bank, uint32_t wave)
{
    const song::DseBank& found = BankAt(set, bank);
    std::vector<std::pair<std::size_t, std::size_t>> counts;
    std::vector<std::shared_ptr<const dse::WaveBank>> done;
    for (std::size_t place = 0; place < set.banks.size(); place++)
    {
        const song::DseBank& each = set.banks[place];
        if ((each.bank != found.bank && each.main_bank != found.bank) ||
            std::ranges::find(done, each.bank) != done.end() || !each.bank->instruments)
        {
            continue;
        }

        done.push_back(each.bank);
        std::size_t splits = 0;
        for (const std::optional<dse::Program>& program : each.bank->instruments->programs)
        {
            if (!program)
            {
                continue;
            }

            for (const DseSplit& split : program->splits)
            {
                splits += DseSplitWave(split) == static_cast<int>(wave) ? 1 : 0;
            }
        }
        counts.emplace_back(place, splits);
    }

    return counts;
}

} // namespace

format::File ReplaceDseWave(const format::File& file, const song::DseSet& set, uint32_t bank, uint32_t wave,
                            const DseIncomingWave& incoming)
{
    RequireSet(file);
    const song::DseBank& found = BankAt(set, bank);
    const dse::WaveBank& holder = *found.bank;
    const dse::WaveInfo& info = ReplaceableWave(set, bank, wave);

    // The new samples, in the DS's form: the same encodings and loops in words.
    const ntdlib::sdat::Wave made = NewDsWave(incoming.wave);
    const std::size_t size = SamplesSize(made);

    dse::WaveBank next = holder;
    const uint32_t old_position = info.SamplePosition();
    const uint32_t position = PlaceSamples(*next.samples, made, size, info);

    // The table of the bank and of each bank that reads its waves from it. Each split that plays the wave there moves
    // its tuning by the new rate.
    WithWave(next, true, wave, made, incoming.rate, old_position, position);

    // The new bytes of each bank are written into every file that contains it, the last file first: the files before
    // keep their starts.
    std::map<uint64_t, std::vector<uint8_t>, std::greater<>> written;
    for (const song::DseBank& each : set.banks)
    {
        const bool same = each.bank == found.bank;
        if (!same && each.main_bank != found.bank)
        {
            continue;
        }

        if (same)
        {
            written.emplace(each.file_offset, next.Write());
            continue;
        }

        if (wave >= each.bank->waves.size() || !each.bank->waves[wave])
        {
            continue;
        }

        dse::WaveBank model = *each.bank;
        WithWave(model, false, wave, made, incoming.rate, old_position, position);
        written.emplace(each.file_offset, model.Write());
    }

    format::File result = file;
    const std::vector<uint64_t> starts = format::NativePieceStarts(file);
    for (auto& [offset, bytes] : written)
    {
        result = WithFileRun(result, starts, offset, std::move(bytes));
    }

    return result;
}

std::vector<std::string> DseWaveUses(const song::DseSet& set, uint32_t bank, uint32_t wave)
{
    std::vector<std::string> uses;
    for (const auto& [place, splits] : SplitsPlaying(set, bank, wave))
    {
        if (splits > 0)
        {
            uses.push_back(song::DseBankName(*set.banks[place].bank) + ": " + std::to_string(splits) +
                           (splits == 1 ? " split" : " splits"));
        }
    }

    return uses;
}

WaveChanges ChangesOfDseWave(const song::DseSet& set, uint32_t bank, uint32_t wave)
{
    const dse::WaveInfo& info = ReplaceableWave(set, bank, wave);
    const uint8_t format = info.bytes[kWaveFormat];

    // The DS's encodings, by the format's byte.
    constexpr std::array kEncodings = {ntdlib::sar::WaveEncoding::kPcm8, ntdlib::sar::WaveEncoding::kPcm16,
                                       ntdlib::sar::WaveEncoding::kImaAdpcm};
    const ntdlib::wave::Pcm pcm = song::DseWavePcm(set, bank, wave);
    WaveChanges changes;
    changes.encoding = kEncodings.at(format);
    changes.channels = 1;
    changes.rate = info.SampleRate();
    changes.loop = pcm.loop;
    changes.loop_start = pcm.loop ? pcm.loop_start : 0;
    changes.loop_end = pcm.loop_end;
    changes.size = std::size_t{info.LoopStart() + info.LoopLength()} * 4;

    // The banks whose splits play the wave: the bank, and those whose waves are in it.
    for (const auto& [place, splits] : SplitsPlaying(set, bank, wave))
    {
        if (splits > 0)
        {
            changes.banks.push_back(static_cast<uint32_t>(place));
        }
    }

    return changes;
}

} // namespace hokuto::edit
