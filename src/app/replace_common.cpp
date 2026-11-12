// SPDX-License-Identifier: MIT

// The parts that the views of the Replace window share: the archive's sounds, banks and slots, the names that an import
// may use, the groups that load new files, and the waves and rates of a wave or a bank.

#include "app/replace_common.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/sar/formats.h"

#include "app/export_warnings.h"
#include "app/item_views.h"
#include "app/replace_view.h"
#include "audio/resampler.h"
#include "edit/ds_waves.h"
#include "edit/files.h"
#include "edit/groups.h"
#include "edit/new_items.h"
#include "format/file.h"
#include "song/items.h"

namespace hokuto::app
{

namespace replace_common
{

std::string TrackList(uint32_t tracks)
{
    std::vector<std::string> numbers;
    for (int track = 0; track < 32; track++)
    {
        if ((tracks >> track) & 1)
        {
            numbers.push_back(std::to_string(track));
        }
    }

    return (numbers.size() == 1 ? "track " : "tracks ") + NameList(numbers);
}

std::vector<uint32_t> SoundBanks(const ReplaceSource& source, uint32_t sound)
{
    if (!source.played.Wii())
    {
        return source.played.Sar()->Sounds()[sound].banks;
    }

    const uint32_t bank = source.played.Wii()->Sounds()[sound].bank;
    return {bank < source.played.Wii()->Banks().size() ? ntdlib::sar::ItemId(ntdlib::sar::kBankItem, bank)
                                                       : ntdlib::sar::kNoItem};
}

std::vector<std::string> BankNames(const ReplaceSource& source)
{
    std::vector<std::string> names;
    const auto add = [&names](const std::string& name)
    {
        names.push_back(!name.empty() ? name : "BANK_" + std::to_string(names.size()));
    };
    if (source.played.Wii())
    {
        for (const ntdlib::rsar::BankInfo& bank : source.played.Wii()->Banks())
        {
            add(bank.name);
        }
        return names;
    }

    for (const ntdlib::sar::BankInfo& bank : source.played.Sar()->Banks())
    {
        add(bank.name);
    }

    return names;
}

std::vector<uint32_t> ProgramsOfBank(const ReplaceSource& source, uint32_t bank)
{
    return source.played.Wii() ? song::ProgramsOf(*source.played.Wii(), bank)
                               : song::ProgramsOf(*source.played.Sar(), bank);
}

std::string MidiSource(const ReplaceSource& source)
{
    return source.file_name + ": MIDI file, " + SizeText(source.incoming.size);
}

void FillDrumRows(ReplaceView& view, const ReplaceSource& source, const std::string& bank,
                  const std::vector<uint32_t>& choices)
{
    for (const int note : source.midi_drums.notes)
    {
        DrumRow& row = view.drums.emplace_back();
        row.note = note;
        row.key = note;
        row.bank = bank;
        row.choices = choices;
        if (const auto paired = source.drum_notes.find(note); paired != source.drum_notes.end())
        {
            row.program = paired->second.program;
            row.key = paired->second.key;
        }
    }
}

void FillDrumRows(ReplaceView& view, const ReplaceSource& source)
{
    if (source.midi.empty() || source.midi_drums.notes.empty())
    {
        return;
    }

    const std::vector<uint32_t> banks = source.banks ? *source.banks : SoundBanks(source, source.sound);
    const std::vector<std::string> bank_names = BankNames(source);
    const auto slot = static_cast<std::size_t>(source.midi_drums.slot);
    const uint32_t id = slot < banks.size() ? banks[slot] : ntdlib::sar::kNoItem;
    std::string name;
    std::vector<uint32_t> choices;
    if (id != ntdlib::sar::kNoItem && ntdlib::sar::ItemNumber(id) < bank_names.size())
    {
        name = bank_names[ntdlib::sar::ItemNumber(id)];
        try
        {
            choices = ProgramsOfBank(source, ntdlib::sar::ItemNumber(id));
        }
        catch (const std::exception&)
        {
            // The bank offers no programs.
        }
    }
    FillDrumRows(view, source, name, choices);
}

bool Holds(const ProgramRow& row)
{
    return std::ranges::find(row.choices, static_cast<uint32_t>(row.target)) != row.choices.end();
}

void AddProgramWarnings(const std::vector<ProgramRow>& rows, std::size_t undecided, std::vector<std::string>& warnings,
                        bool slots)
{
    if (undecided > 0)
    {
        warnings.push_back(
            std::to_string(undecided) +
            (undecided == 1
                 ? " program change sets its program from a variable or a random range. It can't be remapped."
                 : " program changes set their programs from a variable or a random range. They can't be "
                   "remapped."));
    }
    for (const ProgramRow& row : rows)
    {
        const std::string program = "program " + std::to_string(row.target);
        const std::string slot = "slot " + std::to_string(row.program.slot);
        if (row.bank.empty())
        {
            warnings.push_back("The sound has no bank in " + slot + ". Notes using " + program +
                               " will be silent there.");
        }
        else if (!Holds(row))
        {
            warnings.push_back(row.bank + " has no " + program + ". Notes using it" + (slots ? " in " + slot : "") +
                               " will be silent.");
        }
    }
}

std::string NumberList(const std::vector<uint32_t>& numbers)
{
    std::vector<std::string> texts;
    for (const uint32_t number : numbers)
    {
        texts.push_back(std::to_string(number));
    }

    return NameList(texts);
}

std::string SamplesText(const edit::IncomingWave& incoming)
{
    return std::to_string(incoming.bits) + (incoming.floating ? "-bit floating point" : "-bit PCM");
}

const char* EncodingText(ntdlib::sar::WaveEncoding encoding)
{
    switch (encoding)
    {
    case ntdlib::sar::WaveEncoding::kPcm8:
        return "8-bit PCM";
    case ntdlib::sar::WaveEncoding::kPcm16:
        return "16-bit PCM";
    case ntdlib::sar::WaveEncoding::kDspAdpcm:
        return "DSP-ADPCM";
    case ntdlib::sar::WaveEncoding::kImaAdpcm:
        break;
    }

    return "IMA-ADPCM";
}

std::string WaveSizeLine(ntdlib::sar::WaveEncoding present, std::size_t present_size, ntdlib::sar::WaveEncoding added,
                         std::size_t added_size, const char* memory)
{
    const auto [present_text, added_text] = SizeTexts(present_size, added_size);
    std::string line = std::string("The current wave is ") + EncodingText(present) + " (" + present_text +
                       "). The new one is " + EncodingText(added) + " (" + added_text + ").";
    if (added_size > present_size && memory != nullptr)
    {
        line += " ";
        line += memory;
    }

    return line;
}

std::string ChannelsText(std::size_t channels)
{
    return channels == 1 ? "mono" : channels == 2 ? "stereo" : std::to_string(channels) + " channels";
}

std::string LengthText(std::size_t frames, uint32_t rate)
{
    std::array<char, 32> seconds{};
    std::snprintf(seconds.data(), seconds.size(), "%.2f", rate == 0 ? 0.0 : static_cast<double>(frames) / rate);

    return Grouped(frames) + (frames == 1 ? " sample (" : " samples (") + seconds.data() + " s)";
}

bool HoldsNames(const ReplaceSource& source)
{
    return source.played.Sar() == nullptr ||
           edit::HoldsNames(source.played.Sar()->Bytes(), source.played.Sar()->ReadOptions().flavor);
}

std::string Placed(std::size_t number, const std::string& items)
{
    return number == 0 ? " (the first in the archive)" : " (after the existing " + items + ")";
}

std::vector<std::string> TakenNames(const format::NativeTables& tables, const ntdlib::sar::SoundArchive* archive)
{
    std::vector<std::string> names(tables.banks.begin(), tables.banks.end());
    names.insert(names.end(), tables.wave_archives.begin(), tables.wave_archives.end());
    names.insert(names.end(), tables.groups.begin(), tables.groups.end());

    const auto add = [&names](const auto& items)
    {
        for (const auto& item : items)
        {
            names.push_back(item.name);
        }
    };
    add(tables.sounds);
    add(tables.players);
    if (archive != nullptr)
    {
        add(archive->Sounds());
        add(archive->SoundGroups());
        add(archive->Banks());
        add(archive->WaveArchives());
    }
    std::ranges::sort(names);
    names.erase(std::ranges::unique(names).begin(), names.end());

    return names;
}

bool NameTaken(std::span<const std::string> taken, const std::string& name)
{
    return std::ranges::binary_search(taken, name);
}

std::string NoNameRefusal(std::string_view items)
{
    return "Enter a name for the new " + std::string(items) + ".";
}

std::string NameInUse(const std::string& name)
{
    return "The name " + name + " is already in use.";
}

std::string_view NewItemsWord(bool bank, std::size_t sounds)
{
    if (bank)
    {
        return "bank";
    }

    return sounds == 1 ? "sound" : "sounds";
}

format::MidiDrumNotes PairedDrumNotes(const format::BankMidiMap& map, std::span<const int> notes)
{
    format::MidiDrumNotes paired;
    for (const int note : notes)
    {
        if (const auto found = map.drum_notes.find(note); found != map.drum_notes.end())
        {
            paired.insert(*found);
        }
    }

    return paired;
}

std::optional<int> PairedProgram(const format::BankMidiMap& map, int number)
{
    const auto paired = map.programs.find(number);
    if (paired == map.programs.end() || paired->second == number)
    {
        return std::nullopt;
    }

    return paired->second;
}

std::string WavePlayersText(const std::vector<std::string>& players)
{
    const bool one = players.size() == 1;
    return NameList(players) + (one ? " uses this wave and will play" : " use this wave and will play") +
           " the new one.";
}

void AddWaveLoopWarning(bool present_loop, bool new_loop, std::vector<std::string>& warnings)
{
    if (present_loop && !new_loop)
    {
        warnings.push_back(
            "The current wave loops, but the new one doesn't. Notes longer than the new wave go silent when it "
            "ends.");
    }
    else if (!present_loop && new_loop)
    {
        warnings.push_back(
            "The new wave loops, but the current one doesn't. Notes no longer go silent at the end of the wave.");
    }
}

std::string RateChangeText(uint32_t from, uint32_t to)
{
    return "The sample rate changes from " + Grouped(from) + " Hz to " + Grouped(to) + " Hz.";
}

std::string WavesText(std::size_t count)
{
    return Grouped(count) + (count == 1 ? " wave" : " waves");
}

} // namespace replace_common

namespace
{

// The name of file `id` of `source`'s archive for a warning: the name of the wave archive or the bank that it holds, or
// "the file of" a sound that plays it. Otherwise its number.
std::string FileNameOf(const ReplaceSource& source, uint32_t id)
{
    const auto name_in = [id](const auto& items) -> std::optional<std::string>
    {
        const auto found = std::ranges::find(items, id, [](const auto& item) { return item.file_id; });
        return found != items.end() && !found->name.empty() ? std::optional<std::string>(found->name) : std::nullopt;
    };

    std::optional<std::string> name;
    std::optional<std::string> sound;
    if (source.played.Wii())
    {
        name = name_in(source.played.Wii()->Banks());
        sound = name_in(source.played.Wii()->Sounds());
    }
    else if (source.played.Sar())
    {
        name = name_in(source.played.Sar()->WaveArchives());
        name = name ? name : name_in(source.played.Sar()->Banks());
        sound = name_in(source.played.Sar()->Sounds());
    }
    if (name)
    {
        return *name;
    }

    return sound ? "the file of " + *sound : "file " + std::to_string(id);
}

} // namespace

namespace replace_common
{

void SizeGroups(ReplaceView& view, const ReplaceSource& source, const std::map<uint32_t, std::size_t>& sizes)
{
    const std::size_t files = source.played.Wii()   ? source.played.Wii()->FileCount()
                              : source.played.Sar() ? source.played.Sar()->FileCount()
                                                    : 0;
    const auto present = [&source](uint32_t id) -> std::size_t
    {
        if (id < source.file_sizes.size())
        {
            return source.file_sizes[id];
        }
        if (source.played.Wii())
        {
            return source.played.Wii()->FileData(id).size() + source.played.Wii()->WaveData(id).size();
        }

        return source.played.Sar() ? source.played.Sar()->FileData(id).size() : 0;
    };
    const auto size = [&sizes, &present](uint32_t id)
    {
        const auto found = sizes.find(id);
        return found != sizes.end() ? found->second : present(id);
    };

    view.new_group_size = 0;
    for (const uint32_t id : view.item_files)
    {
        view.new_group_size += size(id);
    }

    view.group_growth.assign(view.groups.size(), "");
    for (std::size_t i = 0; i < view.groups.size() && i < source.groups.size(); i++)
    {
        const edit::GroupOption& option = view.groups[i];
        if (option.holds || !option.unavailable.empty() || option.taken.empty())
        {
            continue;
        }

        // A file that shrinks makes the sum fall. The unsigned sum wraps around and still comes out exact.
        const edit::Group& group = source.groups[i];
        std::size_t before = 0;
        if (group.group < source.group_sizes.size() && !source.played.Wii())
        {
            before = static_cast<std::size_t>(source.group_sizes[group.group]);
        }
        else
        {
            for (const uint32_t id : group.files)
            {
                before += present(id);
            }
        }
        std::size_t after = before;
        for (const uint32_t id : group.files)
        {
            after += size(id);
            after -= present(id);
        }
        std::vector<std::string> others;
        for (const uint32_t id : option.taken)
        {
            after += size(id);
            if (id != view.item_files.front() && id < files)
            {
                others.push_back(FileNameOf(source, id));
            }
        }
        view.group_growth[i] = "Group " + option.name + " " + SizeChangeText(before, after, false) + ".";
        if (!others.empty())
        {
            view.group_growth[i] +=
                " " + NameList(others) + (others.size() == 1 ? " is" : " are") + " also added to the group.";
        }
    }
}

namespace
{

// The end of FillGroups and FillWiiGroups, once `view.groups` offers the groups: the files of the new items, the
// groups that they join, the note and the sizes.
void JoinGroups(ReplaceView& view, const ReplaceSource& source, std::span<const uint32_t> after, std::string note,
                std::string none, const std::map<uint32_t, std::size_t>& sizes)
{
    view.item_files.assign(after.begin(), after.end());
    for (const edit::GroupOption& option : view.groups)
    {
        const bool chosen =
            source.joined ? std::ranges::find(*source.joined, option.group) != source.joined->end() : option.proposed;
        if (chosen && !option.holds && option.unavailable.empty())
        {
            view.joined.push_back(option.group);
        }
    }
    if (std::ranges::any_of(view.groups, &edit::GroupOption::proposed))
    {
        view.groups_note = std::move(note);
    }
    view.groups_none = std::move(none);
    SizeGroups(view, source, sizes);
}

} // namespace

void FillGroups(ReplaceView& view, const ReplaceSource& source, std::span<const uint32_t> before,
                std::span<const uint32_t> after, std::string note, std::string none,
                const std::map<uint32_t, std::size_t>& sizes)
{
    if (!source.file || !source.file->native || !source.file->native->tables || !source.played.Sar())
    {
        return;
    }

    view.offers_groups = true;
    view.makes_groups = HoldsNames(source);
    view.new_groups = source.new_groups;
    view.taken_names = TakenNames(*source.file->native->tables, source.played.Sar().get());
    std::vector<uint32_t> held;
    std::ranges::copy_if(after, std::back_inserter(held),
                         [&source](uint32_t id) { return id < source.played.Sar()->FileCount(); });
    view.groups = edit::GroupOptions(source.groups, before, after, edit::UnreadableFiles(*source.played.Sar(), held));
    JoinGroups(view, source, after, std::move(note), std::move(none), sizes);
}

std::vector<std::optional<std::size_t>> WaveArchiveMenu(const ntdlib::sar::SoundArchive& archive,
                                                        std::vector<std::string>& names)
{
    std::vector<std::optional<std::size_t>> counts;
    for (std::size_t i = 0; i < archive.WaveArchives().size(); i++)
    {
        std::optional<std::size_t>& count = counts.emplace_back();
        try
        {
            if (!archive.FileData(archive.WaveArchives()[i].file_id).empty())
            {
                count = song::WaveCountOf(archive, i);
            }
        }
        catch (const std::exception&)
        {
            // The menu shows the wave archive without its waves.
        }
        const std::string& name = archive.WaveArchives()[i].name;
        names.push_back((!name.empty() ? name : "WARC_" + std::to_string(i)) +
                        (count ? " (" + WavesText(*count) + ")" : ""));
    }

    return counts;
}

void WavWarnings(const edit::IncomingWave& incoming, std::vector<std::string>& warnings, bool stream, bool ds)
{
    const std::string item = stream ? "stream" : "wave";
    if (incoming.loops > 0 && !incoming.loop)
    {
        warnings.push_back("The WAV file has loop points outside its sample data. The new " + item + " won't loop.");
    }
    if (incoming.loops > 1)
    {
        warnings.push_back("The WAV file has " + std::to_string(incoming.loops) + " loops. Only the first is used.");
    }
    if (incoming.loop && !incoming.loop_forward)
    {
        warnings.push_back("The WAV file has a reverse or ping-pong loop. The new " + item + " loops forward only.");
    }
    if (incoming.loop && !incoming.loop_endless)
    {
        warnings.push_back("The loop in the WAV file has a fixed repeat count. The new " + item +
                           (stream ? " loops until the game stops the sound." : " loops until the note ends."));
    }
    if (incoming.after_loop > 0 && incoming.loop_from_present)
    {
        warnings.push_back("The WAV file has " + LengthText(incoming.after_loop, incoming.wav_rate) +
                           " after the current loop end. This audio is dropped because a looped " + item +
                           " ends at its loop end.");
    }
    else if (incoming.after_loop > 0)
    {
        warnings.push_back("The WAV file has " + LengthText(incoming.after_loop, incoming.wav_rate) +
                           " after its loop end. This audio is dropped because a looped " + item +
                           " ends at its loop end.");
    }
    if (incoming.rate != incoming.wav_rate)
    {
        warnings.push_back("The WAV file is resampled from " + Grouped(incoming.wav_rate) + " Hz to " +
                           Grouped(incoming.rate) + " Hz. Frequencies above " + Grouped(incoming.rate / 2) +
                           " Hz are filtered out first. The pitch doesn't change.");
    }
    if (incoming.loop_moved > 0)
    {
        warnings.push_back(
            LoopMoveLine(stream, ds, incoming.loop_start, incoming.loop_start + incoming.loop_moved, true));
    }

    // The samples.
    if (incoming.bits != 16 || incoming.floating)
    {
        warnings.push_back("The WAV file has " + SamplesText(incoming) + " samples. They're converted to 16-bit.");
    }
    // Samples of more than 16 bits lose detail in 16-bit PCM too. Those of 8 bits keep theirs. DSP-ADPCM gives some WAV
    // files back exactly, such as one of 8-bit samples.
    const bool deeper = incoming.bits > 16 || incoming.floating;
    const std::string samples = deeper ? "16-bit samples" : "samples";
    if (incoming.signal_to_noise)
    {
        std::array<char, 32> ratio{};
        std::snprintf(ratio.data(), ratio.size(), "%.1f", *incoming.signal_to_noise);
        warnings.push_back(std::string(EncodingText(incoming.encoding)) + " is lossy (signal-to-noise ratio " +
                           ratio.data() + " dB). 16-bit PCM keeps the " + samples + " exactly.");
    }
    else if (incoming.encoding != ntdlib::sar::WaveEncoding::kPcm16)
    {
        warnings.push_back(std::string(EncodingText(incoming.encoding)) + " keeps the " + samples + " exactly.");
    }
    if (incoming.dropped > 0)
    {
        warnings.push_back("The WAV data ends with " + Grouped(incoming.dropped) +
                           (incoming.dropped == 1
                                ? " byte that doesn't form a complete sample frame. It's ignored."
                                : " bytes that don't form a complete sample frame. They're ignored."));
    }
}

std::string LoopText(uint64_t start, uint64_t end)
{
    return ", loop from sample " + Grouped(start) + " to " + Grouped(end);
}

std::string WavSourceLine(const std::string& file_name, const edit::IncomingWave& incoming)
{
    const uint32_t rate = incoming.wav_rate;
    const std::size_t frames = incoming.wav_frames;
    std::string line = file_name + ": " + SamplesText(incoming) + ", " + ChannelsText(incoming.channels) + ", " +
                       Grouped(rate) + " Hz, " + LengthText(frames + incoming.after_loop, rate);
    if (incoming.loop && !incoming.loop_from_present)
    {
        line += LoopText(incoming.wav_loop_start, frames);
    }

    return line;
}

std::string WaveSourceLine(const std::string& file_name, const edit::IncomingWave& incoming, ntdlib::sar::Flavor flavor)
{
    const std::optional<edit::ConvertedFile>& converted = incoming.converted;
    const std::string format =
        song::ConsoleName(converted ? converted->flavor : flavor) + " wave, version " +
        song::VersionText(converted ? converted->flavor : flavor, converted ? converted->version : incoming.version) +
        ", " + EncodingText(incoming.encoding);
    const uint32_t rate = incoming.rate;
    const std::size_t frames = incoming.frames;
    std::string line = file_name + ": " + format + ", " + ChannelsText(incoming.channels) + ", " + Grouped(rate) +
                       " Hz, " + LengthText(frames + incoming.after_loop, rate);
    if (incoming.loop && !incoming.loop_from_present)
    {
        line += LoopText(incoming.loop_start, frames);
    }

    return line;
}

std::vector<uint32_t> RatesOf(uint32_t wav_rate, std::optional<uint32_t> present)
{
    std::vector<uint32_t> lower = {44100, 32000, 24000, 22050, 16000, 11025, 8000};
    if (present)
    {
        lower.push_back(*present);
    }
    std::erase_if(lower, [wav_rate](uint32_t rate) { return rate == 0 || rate >= wav_rate; });
    std::ranges::sort(lower, std::greater<>());
    lower.erase(std::ranges::unique(lower).begin(), lower.end());
    lower.insert(lower.begin(), wav_rate);

    return lower;
}

std::string WiiVersionText(uint32_t version)
{
    return std::to_string(version >> 8 & 0xFF) + "." + std::to_string(version & 0xFF);
}

std::vector<uint32_t> WiiFilesOfSound(const ntdlib::rsar::SoundArchive& archive, uint32_t sound)
{
    if (sound >= archive.Sounds().size())
    {
        return {};
    }

    const ntdlib::rsar::SoundInfo& info = archive.Sounds()[sound];
    std::vector<uint32_t> files{info.file_id};
    if (info.type == ntdlib::rsar::SoundType::kSequence && info.bank < archive.Banks().size() &&
        archive.Banks()[info.bank].file_id != info.file_id)
    {
        files.push_back(archive.Banks()[info.bank].file_id);
    }

    return files;
}

std::vector<std::string> WiiTakenNames(const ReplaceSource& source)
{
    std::vector<std::string> names;
    const auto add = [&names](const auto& items)
    {
        for (const auto& item : items)
        {
            names.push_back(item.name);
        }
    };
    add(source.played.Wii()->Sounds());
    add(source.played.Wii()->Banks());
    add(source.played.Wii()->Players());
    add(source.groups);
    std::ranges::sort(names);
    names.erase(std::ranges::unique(names).begin(), names.end());

    return names;
}

void FillWiiGroups(ReplaceView& view, const ReplaceSource& source, std::span<const uint32_t> before,
                   std::span<const uint32_t> after, std::string note, std::string none,
                   const std::map<uint32_t, std::size_t>& sizes)
{
    if (source.groups.empty())
    {
        return;
    }

    view.offers_groups = true;
    view.makes_groups = false;
    view.groups = edit::GroupOptions(source.groups, before, after);
    JoinGroups(view, source, after, std::move(note), std::move(none), sizes);
}

void AddWiiGroupWarning(ReplaceView& view)
{
    if (!view.offers_groups)
    {
        view.warnings.emplace_back(kWiiLastGroup);
    }
}

std::vector<std::optional<std::size_t>> WiiWaveDataMenu(const ReplaceSource& source, bool beside,
                                                        std::vector<std::string>& names)
{
    std::vector<std::optional<std::size_t>> counts;
    if (beside && !source.beside.empty())
    {
        std::optional<std::size_t>& count = counts.emplace_back();
        try
        {
            count = edit::ReadWiiWaveData(source.beside).waves.size();
        }
        catch (const std::invalid_argument&)
        {
            // The menu shows the file without its waves. The import says why it can't take them.
        }
        names.push_back(source.beside_name + " (next to the file" + (count ? ", " + WavesText(*count) : "") + ")");
    }
    for (const song::WiiWaveArchive& wave_archive : song::WaveArchivesOf(*source.played.Wii()))
    {
        std::optional<std::size_t>& count = counts.emplace_back();
        try
        {
            count = song::WaveCountOf(*source.played.Wii(), wave_archive.file_id);
        }
        catch (const std::exception&)
        {
            // The menu shows the wave data without its waves.
        }
        names.push_back((beside ? "Copy of " : "") + wave_archive.name + (count ? " (" + WavesText(*count) + ")" : ""));
    }

    return counts;
}

ReplaceView Refused(const ReplaceSource& source, ReplaceView view)
{
    if (source.refused)
    {
        view.error = *source.refused;
        view.ready = false;
    }

    return view;
}

void OfferPresentLoop(ReplaceView& view, const edit::IncomingWave& incoming, bool wav, const edit::WaveChanges& changes)
{
    if (!wav || incoming.loops != 0 || !changes.loop || changes.rate == 0)
    {
        return;
    }

    const edit::WaveLoop loop{
        static_cast<uint32_t>(audio::ResampledPlace(changes.loop_start, changes.rate, incoming.wav_rate)),
        static_cast<uint32_t>(audio::ResampledPlace(changes.loop_end, changes.rate, incoming.wav_rate))};
    if (loop.start < loop.end && loop.end <= incoming.wav_frames + incoming.after_loop)
    {
        view.present_loop = loop;
        view.take_loop = incoming.loop_from_present;
    }
    else
    {
        view.warnings.push_back("The current loop (samples " + Grouped(loop.start) + " to " + Grouped(loop.end) +
                                " at the sample rate of the WAV file) ends past the end of the WAV file. The new wave "
                                "can't use it.");
    }
}

std::string DsSizeLine(std::size_t present, std::size_t added, const char* item)
{
    if (present == added)
    {
        return std::string("Both ") + item + "s are " + SizeText(present) + ".";
    }

    const auto [before, after] = SizeTexts(present, added);
    std::string line = std::string("The current ") + item + " is " + before + ". The new one is " + after + ".";
    if (added > present)
    {
        line += " Games load the files a sound requires into a fixed memory area. Larger files may not fit.";
    }

    return line;
}

std::string DsWaveSourceLine(const std::string& file_name, const edit::IncomingWave& incoming, bool wav)
{
    if (wav)
    {
        return WavSourceLine(file_name, incoming);
    }

    std::string line = file_name + ": DS wave file, " + EncodingText(incoming.encoding) + ", mono, " +
                       Grouped(incoming.rate) + " Hz, " + LengthText(incoming.frames, incoming.rate);
    if (incoming.loop)
    {
        line += LoopText(incoming.loop_start, incoming.frames);
    }

    return line;
}

std::vector<uint32_t> DsRatesOf(const edit::IncomingWave& incoming, uint32_t present, uint32_t highest)
{
    std::vector<uint32_t> rates = RatesOf(incoming.wav_rate, present);
    std::erase_if(rates, [highest](uint32_t rate) { return rate > highest; });
    if (incoming.wav_rate > highest)
    {
        std::erase(rates, kDsFallbackRate);
        rates.insert(rates.begin(), kDsFallbackRate);
    }

    return rates;
}

void OfferDsRates(ReplaceView& view, const edit::IncomingWave& incoming, uint32_t present, uint32_t highest)
{
    view.encoding = incoming.encoding;
    view.ds_encodings = true;
    view.rates = DsRatesOf(incoming, present, highest);
    view.rate = static_cast<std::size_t>(std::ranges::find(view.rates, incoming.rate) - view.rates.begin());
    view.first_rate_lower = incoming.wav_rate > highest;
}

} // namespace replace_common
} // namespace hokuto::app
