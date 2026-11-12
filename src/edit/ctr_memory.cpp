// SPDX-License-Identifier: MIT

// The memory and the voices of the 3DS in a conversion (edit/other_console.h): the gains of mono waves, the wave
// archives as the 3DS stores them, the trim of banks that don't fit, and the most notes and channels that sound at
// once.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/nwsnd/console.h"
#include "ntdlib/nwsnd/util.h"
#include "ntdlib/rounding.h"
#include "ntdlib/sar/bank_file.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/wave_file.h"

#include "edit/files.h"
#include "edit/new_items.h"
#include "edit/other_console_parts.h"
#include "edit/replace.h"
#include "edit/waves.h"
#include "song/driver.h"
#include "song/items.h"
#include "song/sequence.h"

namespace hokuto::edit
{

namespace
{

namespace sar = ntdlib::sar;
using other_console_parts::Converted;
using other_console_parts::ConvertWaveArchive;
using other_console_parts::GainAt;
using other_console_parts::Held;
using other_console_parts::KeepOnlyInstruments;
using other_console_parts::Mono;
using other_console_parts::MonoGains;
using other_console_parts::NotePan;
using other_console_parts::SizeText;
using other_console_parts::Song;
using other_console_parts::StoredSize;
using other_console_parts::TableEntriesOf;
using other_console_parts::Trim;

// The powers of a stereo wave of `level` at pan `pan` on the 3DS (Voice::CalcMixParam): as its two channels, and as
// their mean at gain 1. In the dual mode, the first channel plays at the pan less 1 and the second at the pan plus 1.
// In the balance mode, each channel plays on its side at the pan's gain for that side. The mean plays at the pan. A
// pan's gain for the left side is the pan curve's at the pan, and for the right side the curve's at its negation.
std::pair<double, double> PowersAt(const Mono& mono, const MonoLevel& level, float pan)
{
    const auto ratio = [&](float at)
    {
        return static_cast<double>(ntdlib::nwsnd::CalcPanRatio<ntdlib::nwsnd::CtrPolicy>(
            at, mono.pan_info, ntdlib::nwsnd::OutputMode::kStereo));
    };

    double first_left = 0.0;
    double first_right = 0.0;
    double second_left = 0.0;
    double second_right = 0.0;
    if (mono.pan_mode == 1)
    {
        first_left = ratio(pan);
        second_right = ratio(0.0f - pan);
    }
    else
    {
        first_left = ratio(pan - 1.0f);
        first_right = ratio(0.0f - (pan - 1.0f));
        second_left = ratio(pan + 1.0f);
        second_right = ratio(0.0f - (pan + 1.0f));
    }

    const double stereo = level.left * (first_left * first_left + first_right * first_right) +
                          level.right * (second_left * second_left + second_right * second_right) +
                          2.0 * level.both * (first_left * second_left + first_right * second_right);
    const double mean = (level.left + level.right + 2.0 * level.both) * 0.25;
    const double left = ratio(pan);
    const double right = ratio(0.0f - pan);
    return {stereo, mean * (left * left + right * right)};
}

} // namespace

namespace other_console_parts
{

// The gain that gives the mean of a stereo wave of `level` the power of its two channels, over notes at `pans`: the
// square root of the ratio of the two powers, each summed over the notes (PowersAt). It stops at 8. Silence gives 1.
double GainAt(const Mono& mono, const MonoLevel& level, std::span<const float> pans)
{
    double stereo = 0.0;
    double mean = 0.0;
    for (const float pan : pans)
    {
        const auto [two, one] = PowersAt(mono, level, pan);
        stereo += two;
        mean += one;
    }

    return mean > 0.0 ? std::min(std::sqrt(stereo / mean), 8.0) : 1.0;
}

} // namespace other_console_parts

namespace
{

// The gains of stereo wave `index` of wave archive `id` of `mono`. The samples take as much of the gain of a note in
// the centre as their peak allows.
const MonoGains& GainsOf(Mono& mono, uint32_t id, uint32_t index)
{
    std::optional<MonoGains>& gains = mono.waves.at(id).at(index);
    if (!gains)
    {
        const MonoLevel level = MonoLevelOf(sar::WaveFile::Read(mono.sources.at(id).waves.at(index)));
        const float kCentre = 0.0f;
        const double gain = std::max(GainAt(mono, level, {&kCentre, 1}), 1.0);
        const double samples = level.peak > 0 ? std::clamp(32767.0 / level.peak, 1.0, gain) : gain;
        gains = MonoGains{level, samples};
    }

    return *gains;
}

} // namespace

namespace other_console_parts
{

// Wave archive `id` of `archive` as a file of `to`. With `song.mono.on`, its stereo waves go in as mono, and `song`
// keeps them. Throws std::runtime_error and std::invalid_argument with a message for the user for a wave archive that
// the archive lacks, and for a file that can't be converted.
Converted ConvertWaveArchive(Song& song, const sar::SoundArchive& archive, uint32_t id, sar::Flavor to)
{
    const std::optional<std::size_t> index = sar::ItemIndex(id, sar::kWaveArchiveItem, archive.WaveArchives().size());
    if (!index)
    {
        throw std::invalid_argument("The archive has no wave archive " + std::to_string(sar::ItemNumber(id)) + ".");
    }

    const std::span<const uint8_t> file =
        song::ItemFile(archive, {.kind = song::ItemKind::kWaveArchive, .index = static_cast<uint32_t>(*index)});
    Converted converted{
        archive.WaveArchives()[*index].name, ReadIncomingFile(file, FileKind::kWaveArchive, to), {}, {}, {}};
    const std::vector<std::vector<uint8_t>> waves = sar::WaveArchiveFile::Read(file).waves;
    for (uint32_t w = 0; w < waves.size(); ++w)
    {
        const sar::WaveFile wave = sar::WaveFile::Read(waves[w]);
        converted.held_waves = std::max(converted.held_waves.value_or(0), wave.version);
        if (wave.encoding == sar::WaveEncoding::kPcm8)
        {
            converted.pcm8.push_back(w);
        }
    }

    if (!song.mono.on)
    {
        return converted;
    }

    // The stereo waves, and the bytes of the file with them as mono. They go in as mono once the trim has left out the
    // waves that the sound doesn't play (MonoWaveArchive).
    sar::WaveArchiveFile made = sar::WaveArchiveFile::Read(converted.incoming.bytes);
    sar::WaveArchiveFile whole = made;
    std::map<uint32_t, std::optional<MonoGains>>& stereo = song.mono.waves[id];
    for (uint32_t w = 0; w < made.waves.size(); ++w)
    {
        const sar::WaveFile wave = sar::WaveFile::Read(made.waves[w]);
        if (wave.channels.size() == 2)
        {
            stereo[w] = std::nullopt;
            made.waves[w].assign(MonoWaveBytes(wave), 0);
        }
    }

    converted.mono_bytes = made.Write().size();
    if (!stereo.empty())
    {
        song.mono.sources.insert_or_assign(id, std::move(whole));
    }

    return converted;
}

// `bytes`, wave archive `id` of the source archive as a file of the 3DS, with each stereo wave as mono (MonoWaveOf).
// The samples take their gain (GainsOf). `kept` gives the source's index of each wave of a wave archive that holds only
// some of them, or null for one that holds them all. `mono` counts the waves, and keeps the lowest ratio of signal to
// noise.
std::vector<uint8_t> MonoWaveArchive(std::span<const uint8_t> bytes, Mono& mono, uint32_t id,
                                     const std::vector<uint32_t>* kept)
{
    sar::WaveArchiveFile made = sar::WaveArchiveFile::Read(bytes);
    for (uint32_t w = 0; w < made.waves.size(); ++w)
    {
        const sar::WaveFile wave = sar::WaveFile::Read(made.waves[w]);
        if (wave.channels.size() != 2)
        {
            continue;
        }

        const MonoWave one = MonoWaveOf(wave, GainsOf(mono, id, kept != nullptr ? kept->at(w) : w).samples);
        made.waves[w] = one.wave.Write();
        mono.count++;
        if (one.signal_to_noise)
        {
            mono.lowest_ratio = std::min(mono.lowest_ratio.value_or(*one.signal_to_noise), *one.signal_to_noise);
        }
    }

    return made.Write();
}

// The bytes that a wave archive of `size` bytes takes in the memory of the 3DS's model. The model starts each wave
// archive at a multiple of 32 bytes.
std::size_t StoredSize(std::size_t size)
{
    return ntdlib::AlignUp(size, 32);
}

// `bytes` in words, such as "12.3 MB".
std::string SizeText(std::size_t bytes)
{
    std::array<char, 32> text{};
    if (bytes < 1024)
    {
        std::snprintf(text.data(), text.size(), "%zu bytes", bytes);
    }
    else if (bytes < 1024 * 1024)
    {
        std::snprintf(text.data(), text.size(), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    }
    else
    {
        std::snprintf(text.data(), text.size(), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    }

    return text.data();
}

// The places in the table of waves of `bank` that the regions of its instruments of `programs` name.
std::set<uint32_t> TableEntriesOf(const sar::BankFile& bank, const std::set<uint32_t>& programs)
{
    std::set<uint32_t> entries;
    for (const uint32_t program : programs)
    {
        if (program >= bank.instruments.size() || !bank.instruments[program])
        {
            continue;
        }

        for (const sar::BankFile::KeyRegion& key_region : bank.instruments[program]->key_regions)
        {
            for (const sar::BankFile::Region& region : key_region.regions)
            {
                if (region.values.wave_id_index < bank.waves.size())
                {
                    entries.insert(region.values.wave_id_index);
                }
            }
        }
    }

    return entries;
}

namespace
{

// Adds to `files` the name and the file of each of `banks` of `archive`, once, and to `named` the wave archives that
// its waves name, as files of `flavor` (TrimFor). A bank that ntdlib can't read stays behind.
void BankFilesAndWaveArchives(const sar::SoundArchive& archive, std::span<const uint32_t> banks, sar::Flavor flavor,
                              std::vector<std::pair<std::string, std::span<const uint8_t>>>& files,
                              std::set<uint32_t>& named)
{
    for (const uint32_t bank : std::set<uint32_t>(banks.begin(), banks.end()))
    {
        const std::optional<std::size_t> index = sar::ItemIndex(bank, sar::kBankItem, archive.Banks().size());
        if (!index)
        {
            continue;
        }

        try
        {
            const std::span<const uint8_t> file =
                song::ItemFile(archive, {.kind = song::ItemKind::kBank, .index = static_cast<uint32_t>(*index)});
            for (const NamedWaveArchive& wave_archive : WaveArchivesNamed(file, flavor))
            {
                named.insert(wave_archive.id);
            }

            const std::string& name = archive.Banks()[*index].name;
            files.emplace_back(name.empty() ? std::to_string(sar::ItemNumber(bank)) : name, file);
        }
        catch (const std::exception&)
        {
            // The bank stays behind.
        }
    }
}

// Stores in `trim` the waves of the instruments of its programs in the bank files of `files`, by wave archive
// (TrimFor). `counts` contains the waves of each wave archive that converts to the 3DS. Throws std::invalid_argument
// with a message for the user, after `too_large`, for a bank that ntdlib can't read.
void ProgramWaves(const std::vector<std::pair<std::string, std::span<const uint8_t>>>& files,
                  const std::map<uint32_t, std::size_t>& counts, const std::string& too_large, Trim& trim)
{
    std::map<uint32_t, std::set<uint32_t>> waves;
    for (const auto& [name, file] : files)
    {
        sar::BankFile bank;
        try
        {
            bank = sar::BankFile::Read(file);
        }
        catch (const std::exception&)
        {
            throw std::invalid_argument(too_large + "Can't read bank " + name + " to drop its unused instruments.");
        }

        for (const uint32_t entry : TableEntriesOf(bank, trim.programs))
        {
            const sar::WaveId& id = bank.waves[entry];
            const auto count = counts.find(id.wave_archive_item);
            if (count != counts.end() && id.index < count->second)
            {
                waves[id.wave_archive_item].insert(id.index);
            }
        }
    }

    for (const auto& [id, indices] : waves)
    {
        trim.waves[id].assign(indices.begin(), indices.end());
    }
}

} // namespace

// The trim of banks `banks` of `archive` for a sound that plays `sequence` from item `start` on the 3DS, or nothing
// when their wave archives fit in `memory` together as files of the 3DS. The instruments of program 0 and of the
// programs that the sound's program changes select stay, in every bank. The wave archives go to the 3DS first, and
// `song` keeps them. A bank or a wave archive that can't come along takes no memory: it stays behind later. Throws
// std::invalid_argument with a message for the user for wave archives that don't fit, when a variable or a random
// number gives a program, or ntdlib can't read the values of a bank.
std::optional<Trim> TrimFor(Song& song, const sar::SoundArchive& archive, std::span<const uint32_t> banks,
                            const ntdlib::commands::Sequence& sequence, uint32_t start, std::size_t memory)
{
    // The banks' files and the wave archives that they name, each once.
    const sar::Flavor flavor = archive.ReadOptions().flavor;
    std::vector<std::pair<std::string, std::span<const uint8_t>>> files;
    std::set<uint32_t> named;
    BankFilesAndWaveArchives(archive, banks, flavor, files, named);
    Trim trim;
    std::map<uint32_t, std::size_t> counts;
    for (const uint32_t id : named)
    {
        const std::optional<std::size_t> index =
            sar::ItemIndex(id, sar::kWaveArchiveItem, archive.WaveArchives().size());
        if (!index)
        {
            continue;
        }

        try
        {
            Converted converted = ConvertWaveArchive(song, archive, id, sar::Flavor::kCtr);
            trim.whole += StoredSize(converted.mono_bytes.value_or(converted.incoming.bytes.size()));
            counts[id] = song::WaveCountOf(archive, *index);
            song.converted.emplace(id, std::move(converted));
        }
        catch (const std::exception&)
        {
            // The wave archive stays behind with its banks.
        }
    }
    if (trim.whole <= memory)
    {
        return std::nullopt;
    }

    // The programs, and the waves of their instruments.
    const std::string too_large = "The sound's banks use " + SizeText(trim.whole) +
                                  " of wave archives. Hokuto's 3DS model has room for only " + SizeText(memory) + ". ";
    const Selections selections = ProgramsSelected(sequence, start);
    if (selections.undecided > 0)
    {
        throw std::invalid_argument(too_large +
                                    "Can't drop the unused instruments because a variable or a random number selects "
                                    "some of the programs.");
    }

    trim.programs.insert(0);
    for (const Program& program : selections.programs)
    {
        trim.programs.insert(static_cast<uint32_t>(program.number));
    }

    ProgramWaves(files, counts, too_large, trim);

    return trim;
}

// `bytes`, a wave archive file, with only its waves at `kept`, in their order.
std::vector<uint8_t> TrimmedWaveArchive(std::span<const uint8_t> bytes, std::span<const uint32_t> kept)
{
    sar::WaveArchiveFile file = sar::WaveArchiveFile::Read(bytes);
    std::vector<std::vector<uint8_t>> waves;
    for (const uint32_t index : kept)
    {
        waves.push_back(std::move(file.waves.at(index)));
    }

    file.waves = std::move(waves);

    return file.Write();
}

// `bytes`, a bank file, with only the instruments of the programs of `trim`. Its table of waves holds only the waves
// that those instruments play, in their order, at their places in the wave archives of `trim`. A wave that `trim`
// leaves out of its wave archive plays nothing. It played nothing before either.
std::vector<uint8_t> TrimmedBank(std::span<const uint8_t> bytes, const Trim& trim)
{
    sar::BankFile bank = sar::BankFile::Read(bytes);
    std::erase_if(bank.order, [&trim](uint32_t program) { return !trim.programs.contains(program); });
    const auto in_trim = [&trim](sar::WaveId id)
    {
        const uint32_t index = id.index;
        id.index = sar::kNoItem;
        if (const auto kept = trim.waves.find(id.wave_archive_item); kept != trim.waves.end())
        {
            const auto at = std::ranges::lower_bound(kept->second, index);
            if (at != kept->second.end() && *at == index)
            {
                id.index = static_cast<uint32_t>(at - kept->second.begin());
            }
        }

        return id;
    };
    KeepOnlyInstruments(bank, trim.programs, in_trim);

    return bank.Write();
}

// `bytes`, bank `id` of the source archive as a file of the 3DS. Each region that plays a wave of `mono` takes the gain
// of the wave at the pans of the region's notes (GainAt): the pan of each note's track at the note's start, plus the
// region's pan. A region without notes takes the gain at its pan. The region's volume takes the part of the gain that
// the wave's samples don't take (GainsOf), up to 255. `mono` counts the regions, and keeps the range of their gains and
// the regions held at 255. With `trim`, only the instruments of its programs change: the others stay out.
std::vector<uint8_t> WithMonoVolumes(std::span<const uint8_t> bytes, Mono& mono, uint32_t id, const Trim* trim)
{
    sar::BankFile bank = sar::BankFile::Read(bytes);
    for (uint32_t program = 0; program < bank.instruments.size(); ++program)
    {
        std::optional<sar::BankFile::Instrument>& instrument = bank.instruments[program];
        if (!instrument || (trim != nullptr && !trim->programs.contains(program)))
        {
            continue;
        }

        const auto notes = mono.notes.find({id, program});
        for (sar::BankFile::KeyRegion& key_region : instrument->key_regions)
        {
            for (sar::BankFile::Region& region : key_region.regions)
            {
                if (region.values.wave_id_index >= bank.waves.size())
                {
                    continue;
                }

                const sar::WaveId& wave = bank.waves[region.values.wave_id_index];
                const auto waves = mono.waves.find(wave.wave_archive_item);
                if (waves == mono.waves.end() || !waves->second.contains(wave.index))
                {
                    continue;
                }

                const float offset = static_cast<float>(static_cast<int>(region.values.pan) - 64) / 63.0f;
                std::vector<float> pans;
                for (const NotePan& note : notes != mono.notes.end() ? notes->second : std::vector<NotePan>{})
                {
                    if (note.key >= key_region.first_key && note.key <= key_region.last_key &&
                        note.velocity >= region.first_velocity && note.velocity <= region.last_velocity)
                    {
                        pans.push_back(note.pan + offset);
                    }
                }
                if (pans.empty())
                {
                    pans.push_back(offset);
                }

                const MonoGains& gains = GainsOf(mono, wave.wave_archive_item, wave.index);
                const double gain = GainAt(mono, gains.level, pans);
                const double db = 20.0 * std::log10(gain);
                mono.least_db = mono.regions == 0 ? db : std::min(mono.least_db, db);
                mono.most_db = mono.regions == 0 ? db : std::max(mono.most_db, db);
                mono.regions++;
                const long volume = std::lround(region.values.volume * gain / gains.samples);
                mono.held += volume > 255 ? 1 : 0;
                region.values.volume = static_cast<uint8_t>(std::clamp(volume, 0L, 255L));
            }
        }
    }

    return bank.Write();
}

namespace
{

// A change in the notes and channels that sound, at a tick (HeldOf).
struct Change
{
    uint32_t tick = 0;
    int notes = 0;
    int channels = 0;
    int mono_channels = 0;
};

// The channels of the wave that a note of a sound of `archive` plays, whose bank slots contain `banks` (HeldOf). Each
// bank and each wave is read once. A note whose wave is missing from the archive has none.
class NoteChannels
{
public:
    NoteChannels(const sar::SoundArchive& archive, const std::vector<uint32_t>& banks)
        : archive_(archive), options_(archive.ReadOptions()), banks_(banks)
    {
    }

    std::size_t Of(const song::Note& note);

private:
    const sar::SoundArchive& archive_;
    const sar::Options& options_;
    const std::vector<uint32_t>& banks_;
    std::map<uint32_t, std::optional<sar::Bank>> parsed_;
    std::map<std::pair<uint32_t, uint32_t>, std::size_t> channels_;
};

std::size_t NoteChannels::Of(const song::Note& note)
{
    const uint32_t item = note.bank_slot < banks_.size() ? banks_[note.bank_slot] : sar::kNoItem;
    const uint32_t bank = sar::ItemNumber(item);
    if (item == sar::kNoItem || bank >= archive_.Banks().size())
    {
        return 0;
    }

    auto [place, added] = parsed_.try_emplace(bank);
    if (added)
    {
        const std::span<const uint8_t> bytes = archive_.FileData(archive_.Banks()[bank].file_id);
        if (!bytes.empty())
        {
            place->second = sar::Bank::Parse(bytes, options_);
        }
    }

    const std::optional<sar::Bank>& read = place->second;
    if (!read || note.program >= read->instruments.size() || !read->instruments[note.program])
    {
        return 0;
    }

    const sar::VelocityRegion* region = read->instruments[note.program]->Find(note.key, note.velocity);
    if (region == nullptr || region->wave_id_index >= read->waves.size())
    {
        return 0;
    }

    const sar::WaveId& id = read->waves[region->wave_id_index];
    const auto known = channels_.find({id.wave_archive_item, id.index});
    if (known != channels_.end())
    {
        return known->second;
    }

    std::size_t count = 0;
    const uint32_t wave_archive = sar::ItemNumber(id.wave_archive_item);
    if (id.index != sar::kNoItem && wave_archive < archive_.WaveArchives().size())
    {
        const std::span<const uint8_t> bytes = archive_.FileData(archive_.WaveArchives()[wave_archive].file_id);
        const sar::WaveArchive waves = bytes.empty() ? sar::WaveArchive{} : sar::WaveArchive::Parse(bytes, options_);
        if (id.index < waves.waves.size())
        {
            count = sar::Wave::Parse(waves.waves[id.index], options_).channels.size();
        }
    }

    channels_.emplace(std::pair{id.wave_archive_item, id.index}, count);

    return count;
}

// The changes that the notes of sound `sound` of `archive`, whose bank slots hold `banks`, make in the notes and
// channels that sound (HeldOf). `channels_of` finds the channels of the wave of a note. `held` notes the stereo notes.
std::vector<Change> NoteChanges(const sar::SoundArchive& archive, uint32_t sound, const std::vector<uint32_t>& banks,
                                NoteChannels& channels_of, Held& held)
{
    std::vector<Change> changes;
    for (const song::Track& track : song::ReadSequence(archive, sound).tracks)
    {
        for (const song::Note& note : track.notes)
        {
            const auto count = static_cast<int>(channels_of.Of(note));
            if (count == 0)
            {
                continue;
            }

            const int mono = count == 2 ? 1 : count;
            held.stereo = held.stereo || count == 2;
            if (count == 2)
            {
                const auto after = std::ranges::upper_bound(track.pan, note.start, {}, &song::Setting::tick);
                const int pan = after == track.pan.begin() ? 0 : std::prev(after)->value;
                held.stereo_notes[{banks[note.bank_slot], note.program}].push_back(
                    {note.key, note.velocity, std::clamp(static_cast<float>(pan) / 63.0f, -1.0f, 1.0f)});
            }

            changes.push_back({note.start, 1, count, mono});
            changes.push_back({note.start + std::max<uint32_t>(note.length, 1), -1, -count, -mono});
        }
    }

    return changes;
}

} // namespace

// What ntdlib's performer reads of sound `sound` of `archive`. A note sounds from its start to its end. Each channel of
// its wave takes a voice of the console. A note whose wave the archive lacks counts for nothing. The 3DS plays each
// step of a track's pan from -64 to 63 as a 63rd of the way to a side (UpdateChannelParam).
Held HeldOf(const sar::SoundArchive& archive, uint32_t sound)
{
    const std::vector<uint32_t>& banks = archive.Sounds()[sound].banks;
    NoteChannels channels_of(archive, banks);

    // The changes in the notes and channels that sound, by their ticks. A note that ends where another starts makes
    // room for it.
    Held held;
    std::vector<Change> changes = NoteChanges(archive, sound, banks, channels_of, held);
    const auto earlier = [](const Change& a, const Change& b)
    {
        return a.tick != b.tick ? a.tick < b.tick : a.notes < b.notes;
    };
    std::ranges::sort(changes, earlier);
    int notes = 0;
    int sounding = 0;
    int mono = 0;
    for (const Change& change : changes)
    {
        notes += change.notes;
        sounding += change.channels;
        mono += change.mono_channels;
        held.notes = std::max(held.notes, static_cast<std::size_t>(notes));
        held.channels = std::max(held.channels, static_cast<std::size_t>(sounding));
        held.mono_channels = std::max(held.mono_channels, static_cast<std::size_t>(mono));
    }

    return held;
}

} // namespace other_console_parts

} // namespace hokuto::edit
