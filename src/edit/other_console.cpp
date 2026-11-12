// SPDX-License-Identifier: MIT

// Sends a sound to another console (edit/other_console.h): the path between the 3DS, the Wii U and the Switch, and the
// choice of path. The other paths and the parts that they share are in other_console_parts.cpp, console_commands.cpp,
// ctr_memory.cpp, single_bank.cpp, from_wii.cpp, from_converters.cpp, to_wii.cpp and to_ds.cpp.

#include "edit/other_console.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/ctr/nnsnd/dsp_abi.h"
#include "ntdlib/ctr/snd/engine.h"
#include "ntdlib/ctr/snd/voice.h"
#include "ntdlib/dsp/memory.h"
#include "ntdlib/nwsnd/util.h"
#include "ntdlib/sar/archive_model.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/sequence_file.h"

#include "edit/bytecode.h"
#include "edit/files.h"
#include "edit/new_items.h"
#include "edit/other_console_parts.h"
#include "edit/replace.h"
#include "edit/tables.h"
#include "edit/timelines.h"
#include "edit/waves.h"
#include "format/archive.h"
#include "format/engines.h"
#include "format/kinds.h"
#include "format/model.h"
#include "format/shared.h"
#include "song/archive.h"
#include "song/biquads.h"
#include "song/items.h"

namespace hokuto::edit
{

namespace
{

namespace sar = ntdlib::sar;
using other_console_parts::AddSharingWarning;
using other_console_parts::AfterStep;
using other_console_parts::Converted;
using other_console_parts::ConvertWaveArchive;
using other_console_parts::CopyParts;
using other_console_parts::CopyValues;
using other_console_parts::CountSkipped;
using other_console_parts::CtrTimesWarning;
using other_console_parts::FromConverter;
using other_console_parts::FromWii;
using other_console_parts::GoesThroughConverter;
using other_console_parts::Held;
using other_console_parts::HeldOf;
using other_console_parts::kSettingsLeft;
using other_console_parts::ListOf;
using other_console_parts::MonoWaveArchive;
using other_console_parts::RequireTables;
using other_console_parts::SameLengths;
using other_console_parts::Settle;
using other_console_parts::SizeText;
using other_console_parts::SkippedWarning;
using other_console_parts::Song;
using other_console_parts::SourceSequenceSound;
using other_console_parts::StoredSize;
using other_console_parts::Trim;
using other_console_parts::TrimFor;
using other_console_parts::TrimmedBank;
using other_console_parts::TrimmedWaveArchive;
using other_console_parts::WithMonoVolumes;
using other_console_parts::WithoutCtrTimes;
using other_console_parts::WithoutSkipped;

static_assert(kCtrWaveMemory == ntdlib::ctr::snd::kArchiveModeFcram);
static_assert(kWiiWaveMemory == ntdlib::dsp::kMem2Size);

// `song`'s file with `waves`, waves of wave archive `place` in 8-bit PCM, in 16-bit PCM, through the properties' edit
// of a wave's encoding (edit/waves.h). The Switch's renderer decodes no 8-bit PCM. Its voices play such a wave as
// silence. `name` names the wave archive in the warning.
void WithoutPcm8(Song& song, uint32_t place, std::span<const uint32_t> waves, const std::string& name)
{
    for (const uint32_t wave : waves)
    {
        format::File& file = song.result.file;
        const std::shared_ptr<const sar::SoundArchive> built =
            song::LoadArchive(format::ExportArchive(file), "the new archive", format::ExportGroupFiles(file));
        format::WaveModel model = WaveModelOf(file, *built, place, wave);
        model.encoding = sar::WaveEncoding::kPcm16;
        file = WithWaveModel(file, *built, place, wave, model);
    }

    const bool one = waves.size() == 1;
    song.result.warnings.push_back(std::to_string(waves.size()) + (one ? " wave of " : " waves of ") + name +
                                   (one ? " is" : " are") +
                                   " converted to 16-bit PCM without loss. The Switch audio renderer doesn't support "
                                   "8-bit PCM.");
}

// The warning about `converted`, a file that went in for the console of `to` with an older version than it held, or
// nothing. `what` names the file, such as "Bank BANK_SE".
std::optional<std::string> OlderVersionWarning(const Converted& converted, const std::string& what, sar::Flavor to)
{
    const std::optional<ConvertedFile>& source = converted.incoming.converted;
    if (!source || !song::VersionsCompare(source->flavor, to) || source->version <= converted.incoming.version)
    {
        return std::nullopt;
    }

    return what + " is written as version " + song::VersionText(to, converted.incoming.version) + " instead of " +
           song::VersionText(source->flavor, source->version) + ". Its values are unchanged.";
}

// Reads bank `id` of `archive`, item `index` of its list with the name `name`, into `bank` as a file of `to`, and the
// wave archives that its waves name and that `song` doesn't hold yet into `wave_archives` (AddBankOf). With `trim`,
// the bank holds only the instruments of its programs, and each wave archive only the waves of its list. Throws for a
// bank or a wave archive that can't come along.
void ConvertBank(Song& song, const sar::SoundArchive& archive, uint32_t id, sar::Flavor to, const Trim* trim,
                 std::optional<std::size_t> index, const std::string& name, std::optional<Converted>& bank,
                 std::vector<std::pair<uint32_t, Converted>>& wave_archives)
{
    if (!index)
    {
        throw std::invalid_argument("The archive has no such bank.");
    }

    const std::span<const uint8_t> file =
        song::ItemFile(archive, {.kind = song::ItemKind::kBank, .index = static_cast<uint32_t>(*index)});
    bank = Converted{name, ReadIncomingFile(file, FileKind::kBank, to), {}, {}, {}};
    if (song.mono.on)
    {
        for (const NamedWaveArchive& named : WaveArchivesNamed(bank->incoming.bytes, to))
        {
            if (!song.wave_archives.contains(named.id) && !song.converted.contains(named.id))
            {
                song.converted.emplace(named.id, ConvertWaveArchive(song, archive, named.id, to));
            }
        }

        if (std::ranges::any_of(song.mono.waves, [](const auto& waves) { return !waves.second.empty(); }))
        {
            try
            {
                bank->incoming.bytes = WithMonoVolumes(bank->incoming.bytes, song.mono, id, trim);
            }
            catch (const std::exception&)
            {
                song.result.warnings.push_back("Can't read bank " + name +
                                               " to raise the volumes of its regions for mono. Its mono waves "
                                               "play quieter than in stereo.");
            }
        }
    }

    if (trim != nullptr)
    {
        bank->incoming.bytes = TrimmedBank(bank->incoming.bytes, *trim);
    }

    for (const NamedWaveArchive& named : WaveArchivesNamed(bank->incoming.bytes, to))
    {
        if (!song.wave_archives.contains(named.id))
        {
            const auto ready = song.converted.find(named.id);
            Converted converted = ready != song.converted.end() ? std::move(ready->second)
                                                                : ConvertWaveArchive(song, archive, named.id, to);
            if (ready != song.converted.end())
            {
                song.converted.erase(ready);
            }

            std::optional<std::vector<uint32_t>> kept;
            if (trim != nullptr)
            {
                const auto found = trim->waves.find(named.id);
                kept = found != trim->waves.end() ? found->second : std::vector<uint32_t>{};
                converted.incoming.bytes = TrimmedWaveArchive(converted.incoming.bytes, *kept);
            }

            if (song.mono.on)
            {
                converted.incoming.bytes =
                    MonoWaveArchive(converted.incoming.bytes, song.mono, named.id, kept ? &*kept : nullptr);
            }

            wave_archives.emplace_back(named.id, std::move(converted));
        }
    }
}

// Adds `wave_archives`, then `bank`, bank `id` of `archive`, to `song` as files of `to` (AddBankOf), with their
// warnings. Returns the item ID of the new bank.
uint32_t AddConvertedBank(Song& song, const sar::SoundArchive& archive, uint32_t id, sar::Flavor to,
                          const std::vector<std::pair<uint32_t, Converted>>& wave_archives, const Converted& bank)
{
    // The wave archives, then the bank. The bank names them by their places in the new archive's list.
    for (const auto& [wave_archive, converted] : wave_archives)
    {
        const auto place = static_cast<uint32_t>(song.result.file.native->tables->wave_archives.size());
        song.result.file = AddWaveArchive(song.result.file, converted.name, converted.incoming.bytes);
        song.wave_archives[wave_archive] = place;
        song.wave_bytes += StoredSize(converted.incoming.bytes.size());
        const std::string what = converted.name.empty()
                                     ? "Wave archive " + std::to_string(sar::ItemNumber(wave_archive))
                                     : "Wave archive " + converted.name;
        if (const std::optional<std::string> warning = OlderVersionWarning(converted, what, to))
        {
            song.result.warnings.push_back(*warning);
        }

        const std::optional<uint32_t>& waves = converted.incoming.converted->wave_version;
        const sar::Flavor from = archive.ReadOptions().flavor;
        if (converted.held_waves && song::HoldsOriginalStart(from, *converted.held_waves) && waves &&
            !song::HoldsOriginalStart(to, *waves))
        {
            song.older_waves.push_back(what.substr(std::string("Wave archive ").size()));
        }

        if (to == sar::Flavor::kNx && !converted.pcm8.empty())
        {
            WithoutPcm8(song, place, converted.pcm8, what.substr(std::string("Wave archive ").size()));
        }
    }

    std::map<uint32_t, uint32_t> map;
    for (const NamedWaveArchive& named : WaveArchivesNamed(bank.incoming.bytes, to))
    {
        map[named.id] = song.wave_archives.at(named.id);
    }

    song.result.file = AddBank(song.result.file, bank.name, bank.incoming.bytes, map);
    if (const std::optional<std::string> warning = OlderVersionWarning(bank, "Bank " + bank.name, to))
    {
        song.result.warnings.push_back(*warning);
    }

    const auto place = static_cast<uint32_t>(song.result.file.native->tables->banks.size() - 1);
    song.banks[id] = sar::ItemId(sar::kBankItem, place);

    return song.banks[id];
}

// Adds bank `id` of `archive` and the wave archives that its waves name to `song` as files of `to`. Each goes in once.
// With `trim`, the bank holds only the instruments of its programs, and each wave archive only the waves of its list.
// Gives the new bank's item ID, or kNoItem for a bank that stays behind with a warning.
uint32_t AddBankOf(Song& song, const sar::SoundArchive& archive, uint32_t id, sar::Flavor to, const Trim* trim)
{
    const auto known = song.banks.find(id);
    if (known != song.banks.end())
    {
        return known->second;
    }

    // Everything that goes in is read and converted first. A bank that can't come along leaves the new file as it was.
    const std::optional<std::size_t> index = sar::ItemIndex(id, sar::kBankItem, archive.Banks().size());
    const std::string name = index && !archive.Banks()[*index].name.empty()
                                 ? archive.Banks()[*index].name
                                 : "BANK_" + std::to_string(sar::ItemNumber(id));
    std::optional<Converted> bank;
    std::vector<std::pair<uint32_t, Converted>> wave_archives;
    try
    {
        ConvertBank(song, archive, id, to, trim, index, name, bank, wave_archives);
    }
    catch (const std::exception& error)
    {
        song.result.warnings.push_back("Bank " + name + " is left out. Its slot is empty. " +
                                       format::AsSentence(error.what()));
        song.banks[id] = sar::kNoItem;
        return sar::kNoItem;
    }

    return AddConvertedBank(song, archive, id, to, wave_archives, *bank);
}

// `file`, a file in the model form, after its archive gains sequence sound `name`. The sound plays the sequence file
// `bytes`, a file of the archive's console, from offset `start`, with bank slots `banks`. When the console's decoder
// reads the file's commands in their places, the file's sequences gain them. Otherwise the model keeps the file's
// bytes. The sound plays on the archive's first player, with the settings of an entry that sets nothing. Throws
// std::invalid_argument with a message for the user for a start where no command starts, and for an archive that the
// model can't lay out with the new sound.
format::File WithFixedSequence(const format::File& file, const std::string& name, std::vector<uint8_t> bytes,
                               uint32_t start, const std::vector<uint32_t>& banks)
{
    // The commands as the console's decoder reads them, laid out in their places again.
    const Incoming incoming = ReadIncoming(bytes);
    const std::optional<uint32_t> item =
        ItemAtOffset(Offsets(incoming.sequence, song::Console{.engine = format::EngineOf(incoming.flavor)}), start);
    if (!item)
    {
        throw std::invalid_argument("The sound doesn't start at a command of its converted sequence.");
    }

    try
    {
        return AddSequenceSound(file, std::nullopt, name, incoming.sequence, *item, {}, banks);
    }
    catch (const std::invalid_argument&)
    {
        // The cut keeps the file in its bytes.
    }

    format::Model model = format::ModelOf(file);
    const auto id = static_cast<uint32_t>(model.archive.files.size());
    model.archive.files.push_back({.bytes = std::move(bytes)});
    sar::SoundModel sound;
    sound.name = name;
    sound.type = sar::SoundType::kSequence;
    sound.file_id = id;
    sound.player_id = sar::ItemId(sar::kPlayerItem, 0);
    sound.banks = banks;
    sound.allocate_track_flags = TracksOpened(incoming.sequence, *item);
    sound.start_offset = start;
    model.archive.sounds.push_back(std::move(sound));
    try
    {
        return format::WithModel(file, model);
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Can't add a sound to this archive. ") +
                                    format::AsSentence(error.what()));
    }
}

// The 3DS's voices and the channels of its DSP.
constexpr std::size_t kNotes = ntdlib::ctr::snd::VoiceManager::kVoiceCount;
constexpr std::size_t kChannels = ntdlib::ctr::nnsnd::kNumSources;

// The banks of `banks`, the bank slots of a sound of `archive`, in `song`'s file on console `to`, by slot. With
// `trim`, the banks keep only the instruments that the sound uses (TrimFor). Throws std::invalid_argument with a
// message for the user where the wave archives don't fit in `ctr_wave_memory` on the 3DS.
std::vector<uint32_t> SlotBanks(Song& song, const sar::SoundArchive& archive, std::span<const uint32_t> banks,
                                sar::Flavor to, const std::optional<Trim>& trim, std::size_t ctr_wave_memory)
{
    std::vector<uint32_t> slots;
    for (const uint32_t bank : banks)
    {
        slots.push_back(bank == sar::kNoItem ? sar::kNoItem
                                             : AddBankOf(song, archive, bank, to, trim ? &*trim : nullptr));
    }

    if (to == sar::Flavor::kCtr && song.wave_bytes > ctr_wave_memory)
    {
        throw std::invalid_argument("The sound's banks use " + SizeText(song.wave_bytes) + " of wave archives" +
                                    (trim ? ", even without the unused instruments" : "") +
                                    ". Hokuto's 3DS model has room for only " + SizeText(ctr_wave_memory) + ".");
    }

    if (trim)
    {
        song.result.warnings.push_back(
            "The banks keep only the instruments of program 0 and of the programs that the sound uses. Their wave "
            "archives shrink from " +
            SizeText(trim->whole) + " to " + SizeText(song.wave_bytes) + ". Hokuto's 3DS model has room for " +
            SizeText(ctr_wave_memory) + ".");
    }

    return slots;
}

// The warning about the stereo waves that `song` converts to mono. `at_once` contains the notes and channels that the
// sound plays at once.
std::string MonoWarning(const Song& song, const Held& at_once)
{
    const bool one = song.mono.count == 1;
    std::string warning =
        std::to_string(song.mono.count) +
        (one ? " stereo wave is converted to mono. It becomes" : " stereo waves are converted to mono. Each becomes") +
        " the mean of its two channels.";
    if (at_once.channels > kChannels)
    {
        warning += " In stereo, the sound has up to " + std::to_string(at_once.channels) +
                   " channels at once, not counting releases. The 3DS has " + std::to_string(kChannels) + ".";
    }

    if (song.mono.regions > 0 && song.mono.most_db > 0.05)
    {
        std::array<char, 48> db{};
        if (std::abs(song.mono.most_db - song.mono.least_db) < 0.05)
        {
            std::snprintf(db.data(), db.size(), "%.1f dB", song.mono.most_db);
        }
        else
        {
            std::snprintf(db.data(), db.size(), "%.1f to %.1f dB", song.mono.least_db, song.mono.most_db);
        }

        warning += std::string(" The mean is raised by ") + db.data() +
                   " to keep the power of the two channels at the pans of the notes. The samples are raised as "
                   "far as their peaks allow, up to the raise at the centre. The volumes of the regions that use " +
                   (one ? "the wave" : "the waves") + " make up the rest.";
    }

    if (song.mono.held > 0)
    {
        warning += " " + std::to_string(song.mono.held) +
                   (song.mono.held == 1 ? " region is already at the highest volume, 255. It plays"
                                        : " regions are already at the highest volume, 255. They play") +
                   " quieter than in stereo.";
    }

    if (song.mono.lowest_ratio)
    {
        std::array<char, 16> ratio{};
        std::snprintf(ratio.data(), ratio.size(), "%.1f", *song.mono.lowest_ratio);
        warning += one ? std::string(" The wave is in DSP-ADPCM and is encoded again. Its signal-to-noise ratio is ") +
                             ratio.data() + " dB."
                       : std::string(" The waves in DSP-ADPCM are encoded again. Their signal-to-noise ratio is ") +
                             ratio.data() + " dB or more.";
    }

    return warning + (one ? " The wave loses its stereo width." : " The waves lose their stereo width.");
}

// The sound, with the source sound's name and values. The sequence file takes the version that Export… writes for
// the other console. Commands that have to keep their places go in byte for byte, converted. The 3DS reads the
// settings of LFOs 1 to 3 without their bytes, and such a command can't keep its place. Other commands leave the
// sequence where they play otherwise on the other console. From the 3DS, volume2 and the main volume lose their
// times (WithoutCtrTimes). Commands that keep their places keep the times too. The biquad filters take the types
// and values that come nearest on the other console (MapBiquads), in their places or not.
//
// ConvertSequence adds that sound to `song`'s file, and returns the new sequence where the model contains the commands.
std::optional<uint32_t> ConvertSequence(Song& song, const sar::SoundArchive& archive, uint32_t sound,
                                        const format::NativeSound& source, const ntdlib::commands::Sequence& sequence,
                                        uint32_t start, sar::Flavor from, sar::Flavor to,
                                        const std::vector<uint32_t>& slots, const std::optional<song::Console>& origin)
{
    const std::string name = source.name.empty() ? "SEQ_" + std::to_string(sound) : source.name;
    const song::Console reads{.engine = format::EngineOf(from)};
    const song::Console writes{.engine = format::EngineOf(to)};
    std::optional<uint32_t> new_sequence;
    if (Fixed(sequence))
    {
        if (!SameLengths(sequence, from, to))
        {
            throw std::invalid_argument("The commands in the sequence file must stay at their offsets, but the " +
                                        song::ConsoleName(to) + " reads some of them with other lengths.");
        }

        song::ConvertedSequence converted = song::OtherConsoleSequenceFile(archive, sound, to);
        song.result.file = WithFixedSequence(song.result.file, name, std::move(converted.file),
                                             archive.Sounds()[sound].start_offset, slots);
        song.result.warnings.push_back(
            "The commands in the sequence file must stay at their offsets. The file is converted with each command "
            "at its offset.");
        if (const std::optional<std::string> warning =
                SkippedWarning(CountSkipped(sequence, reads, writes), reads, writes, false))
        {
            song.result.warnings.push_back(*warning);
        }

        ntdlib::commands::Sequence timed = sequence;
        if (const std::size_t times = from == sar::Flavor::kCtr ? WithoutCtrTimes(timed) : 0; times > 0)
        {
            song.result.warnings.push_back(CtrTimesWarning(times, to, false));
        }

        if (const std::optional<std::string> warning =
                song::BiquadWarning(converted.biquads, origin.value_or(reads), writes))
        {
            song.result.warnings.push_back(*warning);
        }
    }
    else
    {
        auto [kept, kept_start] = WithoutSkipped(sequence, start, reads, writes);
        const std::size_t times = from == sar::Flavor::kCtr ? WithoutCtrTimes(kept) : 0;
        const song::BiquadChanges biquads =
            song::MapBiquads(kept, std::span<const uint32_t>(&kept_start, 1), reads, writes);
        song.result.file = AddSequenceSound(song.result.file, std::nullopt, name, kept, kept_start, {}, slots);
        new_sequence = static_cast<uint32_t>(song.result.file.sequences.size() - 1);
        if (const std::optional<std::string> warning =
                SkippedWarning(CountSkipped(sequence, reads, writes), reads, writes, true))
        {
            song.result.warnings.push_back(*warning);
        }

        if (times > 0)
        {
            song.result.warnings.push_back(CtrTimesWarning(times, to, true));
        }

        if (const std::optional<std::string> warning = song::BiquadWarning(biquads, origin.value_or(reads), writes))
        {
            song.result.warnings.push_back(*warning);
        }
    }

    return new_sequence;
}

// Adds the warnings about the sequence file of sound `sound` of `archive`, file `file_id`, on console `to`: its
// version, and the other sounds whose commands it contains.
void AddSequenceFileWarnings(Song& song, const sar::SoundArchive& archive, uint32_t sound, uint32_t file_id,
                             sar::Flavor from, sar::Flavor to)
{
    // The version of the sequence file, and the other sounds whose commands it holds.
    const uint32_t held = sar::SequenceFile::Read(archive.FileData(file_id)).version;
    const uint32_t version = song::ExportVersions(song::ItemKind::kSequence, to).file;
    if (song::VersionsCompare(from, to) && held > version)
    {
        song.result.warnings.push_back("The sequence file is written as version " + song::VersionText(to, version) +
                                       ", the oldest that " + song::ConsoleName(to) + " games read. It was version " +
                                       song::VersionText(from, held) + ". Its commands are unchanged.");
    }

    AddSharingWarning(song, song::SoundsSharingFile(archive, sound).size());
}

// The notes and channels that sound `sound` of `archive`, `source` in the tables, plays at once on the 3DS
// (OtherConsoleFrom). Sets the stereo waves of `song` and its mono setup, with `mono` the user's choice.
Held CtrNotesAtOnce(Song& song, const sar::SoundArchive& archive, uint32_t sound, const format::NativeSound& source,
                    MonoWaves mono)
{
    Held at_once = HeldOf(archive, sound);
    song.result.stereo_waves = at_once.stereo;
    song.mono.on =
        at_once.stereo && (mono == MonoWaves::kOn || (mono == MonoWaves::kAuto && at_once.channels > kChannels));
    song.mono.pan_mode = ValueOf(source, SoundValue::kPanMode, 0);
    song.mono.pan_info = ntdlib::nwsnd::PanInfoOf(ValueOf(source, SoundValue::kPanCurve, 0));
    song.mono.notes = std::move(at_once.stereo_notes);

    return at_once;
}

// Sets whether the waves of `song` went in as mono, and adds the warnings of the waves on console `to`: the mono
// waves, more notes or channels at once than the 3DS plays with `at_once`, and the waves of an older version
// (OtherConsoleFrom).
void AddWaveWarnings(Song& song, const Held& at_once, sar::Flavor to)
{
    song.result.mono_waves = song.mono.on && song.mono.count > 0;
    if (song.result.mono_waves)
    {
        song.result.warnings.push_back(MonoWarning(song, at_once));
    }

    const std::size_t channels = song.result.mono_waves ? at_once.mono_channels : at_once.channels;
    if (at_once.notes > kNotes || channels > kChannels)
    {
        song.result.warnings.push_back(
            "The 3DS plays at most " + std::to_string(kNotes) + " notes and " + std::to_string(kChannels) +
            " channels at once. The sound has up to " + std::to_string(at_once.notes) + " notes and " +
            std::to_string(channels) +
            " channels at once, not counting releases. The 3DS drops the notes with the lowest priority.");
    }

    if (!song.older_waves.empty())
    {
        song.result.warnings.push_back("The waves of " + ListOf(song.older_waves) + " are written as version " +
                                       song::VersionText(to, song::ExportVersions(song::ItemKind::kWave, to).file) +
                                       ", the oldest that " + song::ConsoleName(to) +
                                       " games read. That version has no original loop start. Playback is unchanged.");
    }
}

// The settings of sound `sound` in `bytes`, an archive of `from`, or nothing with a warning in `warnings` where they
// can't be read (OtherConsoleFrom).
std::optional<sar::SoundModel> SoundSettingsOf(std::span<const uint8_t> bytes, sar::Flavor from, uint32_t sound,
                                               std::vector<std::string>& warnings)
{
    try
    {
        return sar::ReadArchiveModel(bytes, from).sounds.at(sound);
    }
    catch (const std::exception& error)
    {
        warnings.push_back(std::string(kSettingsLeft) + "Can't read them from this archive. " +
                           format::AsSentence(error.what()));
        return std::nullopt;
    }
}

// ForOtherConsole, with the console whose sound the conversion started from as `origin` when it went to the Wii U
// first: the Wii. The warning about the biquad filters then names it, and not the Wii U.
OtherConsoleSong OtherConsoleFrom(const format::File& file, uint32_t sound, sar::Flavor to, MonoWaves mono,
                                  std::size_t ctr_wave_memory, const std::optional<song::Console>& origin)
{
    if (GoesThroughConverter(file))
    {
        // A song of the GBA's drivers, a sequence of the DS or a sound of DSE goes to the Wii U first, and on from
        // there to the 3DS or the Switch.
        OtherConsoleSong song = FromConverter(file, sound, false);
        if (to == sar::Flavor::kCafe)
        {
            return song;
        }

        return AfterStep(song, OtherConsoleFrom(song.file, 0, to, mono, ctr_wave_memory, std::nullopt));
    }

    RequireTables(file);

    if (file.native->engine == format::Engine::kWii)
    {
        // A sound of the Wii goes to the Wii U first, and on from there to the 3DS or the Switch.
        OtherConsoleSong song = FromWii(file, sound);
        if (to == sar::Flavor::kCafe)
        {
            return song;
        }

        return AfterStep(song, OtherConsoleFrom(song.file, 0, to, mono, ctr_wave_memory,
                                                song::Console{.engine = format::Engine::kWii,
                                                              .wii = format::BuildOfWiiArchive(*file.native)}));
    }

    const sar::Flavor from = FlavorOf(*file.native);
    if (to == from)
    {
        throw std::invalid_argument("The sound already plays on the " + song::ConsoleName(to) + ".");
    }

    const auto [source, bytes, archive, sequences, sequence, start, file_id] = SourceSequenceSound(file, sound);

    Song song;
    song.result.file = format::NewFile(format::EngineOf(to));

    // The 3DS's sound library plays 23 voices, and its DSP 24 channels of their waves. It drops the voice of the lowest
    // priority for a new one. The stereo waves go in as mono when the user asks, or when the sound holds more channels
    // at once than the 3DS plays.
    const Held at_once = to == sar::Flavor::kCtr ? CtrNotesAtOnce(song, *archive, sound, source, mono) : Held{};

    // The banks in the sound's slots, with their wave archives. The 3DS's model keeps the wave archives in its memory.
    const std::optional<Trim> trim = to == sar::Flavor::kCtr
                                         ? TrimFor(song, *archive, source.banks, sequence, start, ctr_wave_memory)
                                         : std::nullopt;
    const std::vector<uint32_t> slots = SlotBanks(song, *archive, source.banks, to, trim, ctr_wave_memory);
    AddWaveWarnings(song, at_once, to);

    const std::optional<uint32_t> new_sequence =
        ConvertSequence(song, *archive, sound, source, sequence, start, from, to, slots, origin);
    const auto added = static_cast<uint32_t>(song.result.file.native->tables->sounds.size() - 1);
    CopyValues(song, added, source);
    Settle(song, added, SoundSettingsOf(bytes, from, sound, song.result.warnings));

    AddSequenceFileWarnings(song, *archive, sound, file_id, from, to);

    CopyParts(song, file, sound, added, file_id < sequences.size() ? sequences[file_id] : std::nullopt, new_sequence);

    return std::move(song.result);
}

} // namespace

OtherConsoleSong ForOtherConsole(const format::File& file, uint32_t sound, sar::Flavor to, MonoWaves mono,
                                 std::size_t ctr_wave_memory)
{
    return OtherConsoleFrom(file, sound, to, mono, ctr_wave_memory, std::nullopt);
}

std::optional<std::string> EngineRefused(const format::File& file, format::Engine engine)
{
    if (engine == file.engine)
    {
        return std::nullopt;
    }

    if (!format::FactsOf(engine).hosts_other_consoles)
    {
        if (engine == format::Engine::kGba)
        {
            return "Can't play sounds of other consoles on the GBA engine.";
        }
        if (engine == format::Engine::kDse)
        {
            return "Can't play sounds of other consoles on the DSE engine.";
        }

        return "Can't play sounds of other consoles on the engine of " + std::string(format::FactsOf(engine).title) +
               ".";
    }

    if (file.engine == format::Engine::kGba || file.engine == format::Engine::kDs ||
        file.engine == format::Engine::kDse || format::IsDriverEngine(file.engine))
    {
        return std::nullopt;
    }

    if (!file.native || !file.native->tables)
    {
        return "Can't read the tables of this archive. Playing a sound on another console's engine requires them.";
    }

    return std::nullopt;
}

OtherConsoleSong ForEngine(const format::File& file, uint32_t sound, format::Engine engine)
{
    if (const std::optional<std::string> refused = EngineRefused(file, engine))
    {
        throw std::invalid_argument(*refused);
    }
    if (engine == file.engine)
    {
        throw std::invalid_argument("The sound already uses this engine.");
    }

    if (engine == format::Engine::kDs)
    {
        return ForDs(FoldTimelines(file), sound);
    }

    const std::optional<sar::Flavor> to = format::FlavorOf(engine);
    return to ? ForOtherConsole(FoldTimelines(file), sound, *to) : ForWii(FoldTimelines(file), sound);
}

} // namespace hokuto::edit
