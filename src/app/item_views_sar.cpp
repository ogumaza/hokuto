// SPDX-License-Identifier: MIT

// The views of the banks, waves, streams and wave sounds of the 3DS, the Wii U, the Switch and the Wii, with the
// edits of a bank's instruments and the waves that a drop places.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/stream_file.h"
#include "ntdlib/rsar/wave_file.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/rvl/snd/stream_sound.h"
#include "ntdlib/sar/bank_file.h"
#include "ntdlib/sar/decode.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/wave_file.h"
#include "ntdlib/sound_build.h"

#include "app/item_views.h"
#include "app/item_views_parts.h"
#include "app/key_edits.h"
#include "edit/banks.h"
#include "edit/files.h"
#include "edit/groups.h"
#include "edit/streams.h"
#include "edit/timelines.h"
#include "edit/waves.h"
#include "format/archive.h"
#include "format/file.h"
#include "format/kinds.h"
#include "song/archive.h"
#include "song/items.h"
#include "song/streams.h"

namespace hokuto::app
{
namespace
{

namespace rsar = ntdlib::rsar;
namespace sar = ntdlib::sar;
using edit::Region;
using item_views_parts::kHighest;
using item_views_parts::ListOf;

// A count of things as a sentence gives it, such as "1 bank" or "3 banks".
std::string CountOf(std::size_t count, const std::string& thing)
{
    return std::to_string(count) + " " + thing + (count == 1 ? "" : "s");
}

// The line of the other items that an edit also reaches: `others`, each a `noun`, or `none` without any.
std::string OthersLine(const std::vector<std::string>& others, const std::string& noun, const char* none)
{
    return others.empty() ? std::string(none)
                          : "Edits also affect " + CountOf(others.size(), noun) + ": " + ListOf(others) + ".";
}

// A size in bytes as a sentence gives it, such as "1 byte" or "12,345 bytes".
std::string BytesText(uint64_t bytes)
{
    return Grouped(bytes) + (bytes == 1 ? " byte" : " bytes");
}

// The size of the file `file_id` of `archive` as a sentence about `what` gives it, or nothing where the archive holds
// none of its bytes.
std::optional<std::string> SizeLine(const sar::SoundArchive& archive, uint32_t file_id, const std::string& what)
{
    const std::size_t size = archive.FileData(file_id).size();
    if (size == 0)
    {
        return std::nullopt;
    }

    return what + ": " + BytesText(size);
}

// Every wave of the wave archives of `archive` that Hokuto reads, and the waves' names, for the menus of a wave.
void ListWaves(const sar::SoundArchive& archive, std::vector<sar::WaveId>& waves, std::vector<std::string>& names)
{
    for (std::size_t wave_archive = 0; wave_archive < archive.WaveArchives().size(); ++wave_archive)
    {
        std::size_t count = 0;
        try
        {
            count = song::WaveCountOf(archive, wave_archive);
        }
        catch (const std::exception&)
        {
            continue;
        }
        for (std::size_t wave = 0; wave < count; ++wave)
        {
            const auto index = static_cast<uint32_t>(wave_archive);
            waves.push_back({sar::ItemId(sar::kWaveArchiveItem, index), static_cast<uint32_t>(wave)});
            names.push_back(song::ItemName(archive, {song::ItemKind::kWave, index, static_cast<uint32_t>(wave)}));
        }
    }
}

// The waves of the wave data of file `file_id` of the Wii's `archive` that Hokuto reads, and the waves' names.
void ListWaves(const rsar::SoundArchive& archive, uint32_t file_id, std::vector<sar::WaveId>& waves,
               std::vector<std::string>& names)
{
    std::size_t count = 0;
    try
    {
        count = song::WaveCountOf(archive, file_id);
    }
    catch (const std::exception&)
    {
        return;
    }
    for (std::size_t wave = 0; wave < count; ++wave)
    {
        const auto index = static_cast<uint32_t>(wave);
        waves.push_back({sar::ItemId(sar::kWaveArchiveItem, file_id), index});
        names.push_back(song::ItemName(archive, {song::ItemKind::kWave, file_id, index}));
    }
}

// Every wave of the wave archives of the Wii's `archive` that Hokuto reads, and the waves' names.
void ListWaves(const rsar::SoundArchive& archive, std::vector<sar::WaveId>& waves, std::vector<std::string>& names)
{
    for (const song::WiiWaveArchive& wave_archive : song::WaveArchivesOf(archive))
    {
        ListWaves(archive, wave_archive.file_id, waves, names);
    }
}

// The sizes of file `file_id` of the Wii's `archive` and of its wave data in sentences about `what`, after `lines`.
void WiiSizeLines(const rsar::SoundArchive& archive, uint32_t file_id, const std::string& what,
                  std::vector<std::string>& lines)
{
    if (const std::size_t size = archive.FileData(file_id).size(); size > 0)
    {
        lines.push_back(what + ": " + BytesText(size));
    }
    if (const std::size_t size = archive.WaveData(file_id).size(); size > 0)
    {
        lines.push_back("Wave data: " + BytesText(size));
    }
}

// Whether the build of NW4R SND that plays the Wii's `file` reads a region's or a note's hold, key group and note-off.
bool ReadsHold(const format::File& file)
{
    return !file.native || format::BuildOfWiiArchive(*file.native).SinceMarch2008();
}

// The parts of the view of bank `bank` that the archives of every console store the same way: its name, `model`, the
// names of its wave entries, and every wave of the archive that Hokuto reads, for the menu of a region's wave.
template <class Archive>
BankView BankViewOf(const Archive& archive, uint32_t bank, sar::BankFile model)
{
    BankView view;
    view.bank = bank;
    view.name = archive.Banks().at(bank).name;
    view.model = std::move(model);
    for (uint32_t entry = 0; entry < view.model.waves.size(); ++entry)
    {
        view.entries.push_back(edit::WaveEntryName(archive, view.model, entry));
    }
    ListWaves(archive, view.waves, view.wave_names);

    return view;
}

// The parts of the view of wave `wave` of `wave_archive` that the archives of every console store the same way: its
// name, its values, and the samples that its end can move over. `present` is the wave as the archive contains it.
template <class Archive, class WaveFile>
WaveView WaveViewOf(const format::File& file, const Archive& archive, uint32_t wave_archive, uint32_t wave,
                    const WaveFile& present)
{
    const format::WaveModel model = edit::WaveModelOf(file, archive, wave_archive, wave);
    WaveView view;
    view.name = song::ItemName(archive, {song::ItemKind::kWave, wave_archive, wave});
    view.values = {model.encoding, model.rate, model.loop, model.loop ? model.loop_start : 0, model.loop_end};
    view.held = std::min(model.length, edit::HeldSamples(present));
    view.samples = edit::DecodedSamples(present, view.held);
    view.peaks = PeaksOf(view.samples);
    view.encodings = {sar::WaveEncoding::kDspAdpcm, sar::WaveEncoding::kPcm16, sar::WaveEncoding::kPcm8};

    return view;
}

// The line of a wave's view that says what else plays the wave: `banks` and the wave sounds `sounds`.
std::string WaveReach(const std::vector<std::string>& banks, const std::vector<std::string>& sounds)
{
    if (banks.empty() && sounds.empty())
    {
        return "No bank or wave sound uses this wave.";
    }

    std::string players;
    if (!banks.empty())
    {
        players = CountOf(banks.size(), "bank") + " (" + ListOf(banks) + ")";
    }
    if (!sounds.empty())
    {
        players +=
            (players.empty() ? "" : " and ") + CountOf(sounds.size(), "wave sound") + " (" + ListOf(sounds) + ")";
    }

    return "Edits affect " + players + ".";
}

// The samples of a stream's view: the channels of `pcm`, held up to `length` samples, and their peaks. A stream can
// take DSP-ADPCM, PCM16 and its `encoding`.
void FillStreamSamples(WaveView& view, sar::Pcm pcm, uint32_t length, sar::WaveEncoding encoding)
{
    view.samples = std::move(pcm.channels);
    view.held = length;
    for (const std::vector<int16_t>& channel : view.samples)
    {
        view.held = std::min(view.held, static_cast<uint32_t>(channel.size()));
    }
    view.peaks = PeaksOf(view.samples);
    view.encodings = {sar::WaveEncoding::kDspAdpcm, sar::WaveEncoding::kPcm16};
    if (encoding != sar::WaveEncoding::kDspAdpcm && encoding != sar::WaveEncoding::kPcm16)
    {
        view.encodings.push_back(encoding);
    }
}

} // namespace

BankView MakeBankView(const format::File& file, const sar::SoundArchive& archive, uint32_t bank)
{
    BankView view = BankViewOf(archive, bank, edit::BankModelOf(file, archive, bank));

    // The Wii U's AX and the 3DS's DSP read a region's interpolation. The Switch's renderer doesn't.
    const sar::Flavor flavor = archive.ReadOptions().flavor;
    view.interpolation = flavor == sar::Flavor::kCafe || flavor == sar::Flavor::kCtr;
    view.engine = format::EngineOf(flavor);
    for (const uint32_t sound : edit::SoundsPlaying(archive, edit::FileKind::kBank, bank))
    {
        view.sounds.push_back(archive.Sounds()[sound].name);
    }
    for (const uint32_t group : edit::GroupsLackingWaves(file, archive, bank))
    {
        view.lacking.push_back(group < archive.Groups().size() ? archive.Groups()[group].name
                                                               : "Group " + std::to_string(group));
    }
    if (const std::optional<std::string> size = SizeLine(archive, archive.Banks()[bank].file_id, "Bank file"))
    {
        view.sizes.push_back(*size);
    }

    return view;
}

BankView MakeBankView(const format::File& file, const rsar::SoundArchive& archive, uint32_t bank)
{
    BankView view = BankViewOf(archive, bank, edit::BankModelOf(archive, bank));
    view.interpolation = false;
    view.engine = format::Engine::kWii;
    view.hold_and_groups = ReadsHold(file);

    // The sequence sounds that play the bank's file change with an edit, whichever bank of the file they name.
    const uint32_t file_id = archive.Banks()[bank].file_id;
    for (const rsar::SoundInfo& sound : archive.Sounds())
    {
        if (sound.type == rsar::SoundType::kSequence && sound.bank < archive.Banks().size() &&
            archive.Banks()[sound.bank].file_id == file_id)
        {
            view.sounds.push_back(sound.name);
        }
    }
    WiiSizeLines(archive, file_id, "Bank file", view.sizes);

    return view;
}

std::optional<uint32_t> FreeProgram(const sar::BankFile& bank, uint32_t from)
{
    for (uint32_t program = from; program <= edit::kHighestProgram; ++program)
    {
        if (program >= bank.instruments.size() || !bank.instruments[program])
        {
            return program;
        }
    }

    return std::nullopt;
}

AddedInstrument WithAddedInstrument(sar::BankFile bank, uint32_t from, bool wii)
{
    const std::optional<uint32_t> free = FreeProgram(bank, from);
    if (!free)
    {
        throw std::invalid_argument("The bank has no free program after the selected one.");
    }
    if (wii && bank.waves.empty())
    {
        throw std::invalid_argument(
            "This bank has no waves, and a Wii bank can't keep an instrument without one. Drag WAV files or waves "
            "from the browser onto the list.");
    }

    std::optional<sar::WaveId> wave;
    if (wii)
    {
        wave = bank.waves.front();
    }

    return {edit::WithNewInstrument(std::move(bank), *free, wave), *free};
}

DroppedWaves WithDroppedWaves(sar::BankFile bank, const BankDrop& drop, const std::vector<sar::WaveId>& waves)
{
    const bool instrument =
        drop.program && *drop.program < bank.instruments.size() && bank.instruments[*drop.program].has_value();

    // On the list, or without an instrument at the program, each wave makes an instrument at the next free program.
    // Each instrument's region covers every key and velocity.
    if (!instrument)
    {
        uint32_t program = drop.program.value_or(0);
        std::vector<uint32_t> programs;
        for (const sar::WaveId& wave : waves)
        {
            const std::optional<uint32_t> free = FreeProgram(bank, program);
            if (!free)
            {
                throw std::invalid_argument("The bank has no free program for another instrument.");
            }
            bank = edit::WithNewInstrument(std::move(bank), *free, wave);
            programs.push_back(*free);
            program = *free + 1;
        }
        return {std::move(bank), waves.size() == 1 ? "Add Instrument" : "Add Instruments", std::move(programs), {0, 0}};
    }

    // One wave on a region gives the region the wave.
    std::vector<Region> regions = edit::RegionsOf(*bank.instruments[*drop.program]);
    const std::optional<std::size_t> under = edit::RegionAt(*bank.instruments[*drop.program], drop.key, drop.velocity);
    if (waves.size() == 1 && under)
    {
        const std::pair<int, int> region(regions[*under].first_key, regions[*under].first_velocity);
        regions[*under].values.wave_id_index = edit::WaveEntry(bank, waves.front());
        return {edit::WithRegions(std::move(bank), *drop.program, std::move(regions)),
                "Change Wave",
                {*drop.program},
                region};
    }

    // Other drops make a region of one key at each wave, from the key under the drop up, over every velocity.
    for (std::size_t w = 0; w < waves.size(); ++w)
    {
        const int key = drop.key + static_cast<int>(w);
        if (key > kHighest)
        {
            throw std::invalid_argument("The waves run past key 127. Drop them on a lower key.");
        }
        const auto covers = [key](const Region& region)
        {
            return key >= region.first_key && key <= region.last_key;
        };
        if (std::ranges::any_of(regions, covers))
        {
            throw std::invalid_argument("Key " + KeyName(key) +
                                        " already has a region. To change the wave of a region, drop a single wave on "
                                        "it. Otherwise, drop the waves on free keys.");
        }

        Region region;
        region.first_key = static_cast<uint8_t>(key);
        region.last_key = static_cast<uint8_t>(key);
        region.values.wave_id_index = edit::WaveEntry(bank, waves[w]);
        region.values.original_key = static_cast<uint8_t>(key);
        regions.push_back(region);
    }

    return {edit::WithRegions(std::move(bank), *drop.program, std::move(regions)),
            waves.size() == 1 ? "Add Region" : "Add Regions",
            {*drop.program},
            {drop.key, 0}};
}

WaveView MakeWaveView(const format::File& file, const sar::SoundArchive& archive, uint32_t wave_archive, uint32_t wave)
{
    // The wave's values, and the samples that its end can move over.
    const sar::WaveFile present = edit::PresentWave(archive, wave_archive, wave);
    WaveView view = WaveViewOf(file, archive, wave_archive, wave, present);

    // What else plays the wave: the banks that name it, and the wave sounds whose notes play it.
    const edit::WaveChanges changes = edit::ChangesOfWave(archive, wave_archive, wave);
    std::vector<std::string> banks;
    for (const uint32_t bank : changes.banks)
    {
        banks.push_back(archive.Banks()[bank].name);
    }
    std::vector<std::string> sounds;
    for (const uint32_t sound : changes.sounds)
    {
        sounds.push_back(archive.Sounds()[sound].name);
    }
    view.reach = WaveReach(banks, sounds);

    // The wave's size, and its wave archive's.
    view.sizes.push_back("Wave: " + BytesText(changes.size));
    if (const std::optional<std::string> size =
            SizeLine(archive, archive.WaveArchives()[wave_archive].file_id, "Wave archive file"))
    {
        view.sizes.push_back(*size);
    }

    return view;
}

WaveView MakeWaveView(const format::File& file, const rsar::SoundArchive& archive, uint32_t file_id, uint32_t wave)
{
    // The wave's values, and the samples that its end can move over.
    const rsar::WaveFile present = edit::PresentWave(archive, file_id, wave);
    WaveView view = WaveViewOf(file, archive, file_id, wave, present);

    // What else plays the wave: the banks of its file, and the wave sounds of its file whose notes play it.
    std::vector<std::string> banks;
    for (const rsar::BankInfo& bank : archive.Banks())
    {
        if (bank.file_id == file_id)
        {
            banks.push_back(bank.name);
        }
    }
    std::vector<std::string> sounds;
    std::optional<rsar::WaveSoundFile> read;
    for (const rsar::SoundInfo& sound : archive.Sounds())
    {
        if (sound.type != rsar::SoundType::kWaveSound || sound.file_id != file_id)
        {
            continue;
        }

        if (!read)
        {
            try
            {
                read = rsar::WaveSoundFile::Parse(archive.FileData(file_id));
            }
            catch (const std::exception&)
            {
                break;
            }
        }
        const bool plays = sound.sub_index >= 0 && static_cast<std::size_t>(sound.sub_index) < read->Sounds().size() &&
                           !read->Sounds()[static_cast<std::size_t>(sound.sub_index)].notes.empty() &&
                           read->Sounds()[static_cast<std::size_t>(sound.sub_index)].notes.front().wave_index ==
                               static_cast<int32_t>(wave);
        if (plays)
        {
            sounds.push_back(sound.name);
        }
    }
    view.reach = WaveReach(banks, sounds);

    // The wave's size, and its file's and its wave data's.
    std::size_t bytes = 0;
    for (const rsar::WaveFile::Channel& channel : present.channels)
    {
        bytes += channel.samples.size();
    }
    view.sizes.push_back("Wave samples: " + BytesText(bytes));
    WiiSizeLines(archive, file_id, banks.empty() ? "Wave sound file" : "Bank file", view.sizes);

    return view;
}

std::string MissingStreamText(const std::string& sound, const std::string& file)
{
    return "The stream for " + sound + " is missing: " + file +
           " wasn't next to the archive at import. Put the file next to the archive and import the archive again.";
}

WaveView MakeStreamView(const format::File& file, const sar::SoundArchive& archive, uint32_t sound)
{
    // The stream that the file keeps for the sound.
    const edit::StreamChanges changes = edit::ChangesOfStream(file, archive, sound);
    const std::string& name = archive.Sounds()[sound].name;
    if (!changes.present || !file.native || !file.native->streams.contains(changes.file))
    {
        throw std::invalid_argument(MissingStreamText(name, song::StreamFileName(archive, sound)));
    }
    const std::vector<uint8_t>& bytes = *file.native->streams.at(changes.file).bytes;

    // The stream's values, and its samples.
    const edit::StreamValues values = edit::StreamValuesOf(bytes);
    WaveView view;
    view.name = name;
    view.stream = true;
    view.values = {values.encoding, values.rate, values.loop, values.loop ? values.loop_start : 0, values.length};
    FillStreamSamples(view, song::DecodeStreamFile(bytes), values.length, values.encoding);

    // The tracks that the sound plays, from the archive's table, and the regions.
    const sar::StreamSoundInfo& info = archive.Sounds()[sound].stream;
    for (std::size_t track = 0; track < info.tracks.size(); ++track)
    {
        const sar::StreamTrackInfo& track_info = info.tracks[track];
        const bool starts = ((info.valid_tracks >> track) & 1U) != 0;
        view.tracks.push_back("Track " + std::to_string(track + 1) + ": " + ChannelsOf(track_info.channels) +
                              ", volume " + std::to_string(track_info.volume) + ", pan " + PanName(track_info.pan) +
                              (starts ? "" : " (off at start)"));
    }
    view.regions = "None";

    // What else plays the stream: the other stream sounds that play its file.
    std::vector<std::string> others;
    for (const uint32_t other : changes.sounds)
    {
        others.push_back(archive.Sounds()[other].name);
    }
    view.reach = OthersLine(others, "other stream sound", "No other stream sound uses this stream.");
    view.sizes.push_back("Stream file: " + BytesText(changes.size));

    return view;
}

namespace
{

// The lines of the tracks of `stream`, the stream of Wii sound `sound`, and of those that the sound plays.
std::vector<std::string> WiiStreamTrackLines(const format::File& file, const rsar::SoundArchive& archive,
                                             uint32_t sound, const rsar::StreamFile& stream)
{
    std::vector<std::string> tracks;
    const ntdlib::SndBuild build = format::BuildOfWiiArchive(*file.native);
    const rsar::SoundInfo& info = archive.Sounds()[sound];
    if (!build.SinceMarch2008())
    {
        const std::vector<uint8_t> first =
            stream.channels.size() > 1 ? std::vector<uint8_t>{0, 1} : std::vector<uint8_t>{0};
        tracks.push_back("This build of NW4R SND plays only " + ChannelsOf(first) +
                         ", at full volume and centred. The track settings are ignored.");
    }
    else
    {
        const std::optional<std::vector<ntdlib::rvl::snd::StrmPlayer::Track>> played =
            ntdlib::rvl::snd::StrmPlayer::TracksOf(build, info, archive.Version(), stream);
        for (std::size_t track = 0; track < stream.tracks.size(); ++track)
        {
            const rsar::StreamFile::Track& track_info = stream.tracks[track];
            const bool plays = played && track < played->size() && (*played)[track].active;
            tracks.push_back("Track " + std::to_string(track + 1) + ": " + ChannelsOf(track_info.channels) +
                             ", volume " + std::to_string(track_info.volume) + ", pan " + PanName(track_info.pan) +
                             (plays ? "" : " (not used by this sound)"));
        }
        if (!played)
        {
            tracks.emplace_back(
                "No track plays. The sound entry refers to a track or channel that the stream doesn't have.");
        }
    }

    return tracks;
}

// The line that names the other stream sounds that play stream file `id`, the file of `sound`.
std::string OtherStreamSounds(const rsar::SoundArchive& archive, uint32_t sound, std::optional<uint32_t> id)
{
    std::vector<std::string> others;
    for (uint32_t other = 0; other < archive.Sounds().size(); ++other)
    {
        if (other != sound && song::StreamFileOf(archive, other) == id)
        {
            others.push_back(archive.Sounds()[other].name);
        }
    }

    return OthersLine(others, "other stream sound", "No other stream sound uses this stream.");
}

} // namespace

WaveView MakeStreamView(const format::File& file, const rsar::SoundArchive& archive, uint32_t sound)
{
    // The stream that the file keeps for the sound.
    const std::string& name = archive.Sounds().at(sound).name;
    const std::optional<uint32_t> id = song::StreamFileOf(archive, sound);
    if (!id || !file.native || !file.native->streams.contains(*id))
    {
        throw std::invalid_argument(MissingStreamText(name, song::StreamFileName(archive, sound)));
    }
    const std::vector<uint8_t>& bytes = *file.native->streams.at(*id).bytes;
    rsar::StreamFile stream;
    try
    {
        stream = rsar::StreamFile::Read(bytes);
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument("Can't read the stream for " + name + ". " + format::AsSentence(error.what()));
    }

    // The stream's values, and its samples.
    WaveView view;
    view.name = name;
    view.stream = true;
    const auto encoding = static_cast<sar::WaveEncoding>(stream.encoding);
    view.values = {encoding, stream.sample_rate, stream.loop, stream.loop ? stream.loop_start : 0, stream.sample_count};
    sar::Pcm pcm;
    try
    {
        pcm = song::DecodeStreamFile(bytes);
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(error.what());
    }
    FillStreamSamples(view, std::move(pcm), stream.sample_count, encoding);

    // The stream file's tracks, and those that the sound plays. The builds before March 2008 play the first two
    // channels alone.
    view.tracks = WiiStreamTrackLines(file, archive, sound, stream);
    view.regions = "None";

    // What else plays the stream: the other stream sounds that play its file.
    view.reach = OtherStreamSounds(archive, sound, id);
    view.sizes.push_back("Stream file: " + BytesText(bytes.size()));

    return view;
}

std::vector<std::string> GroupSizeLines(const format::File& file, const sar::SoundArchive& archive, uint32_t file_id)
{
    // The groups that hold the file.
    std::vector<edit::Group> loading;
    for (edit::Group& group : edit::GroupsOf(file, archive))
    {
        if (std::ranges::find(group.files, file_id) != group.files.end())
        {
            loading.push_back(std::move(group));
        }
    }
    if (loading.empty() || !file.native)
    {
        return {};
    }

    // Each group's load in the archive that an export writes, beside the game's. The Export window measures the same
    // archive. The archive that plays lays the timelines out with every bar in its track, and an export folds them back
    // into the converter's subroutines.
    const format::File folded = edit::FoldTimelines(file);
    const format::GameSizes now =
        format::SizesOf(format::ExportArchive(folded), FlavorOf(*file.native), format::GroupFilesBeside(folded));
    const std::optional<format::GameSizes>& game = file.native->game_sizes;
    std::vector<std::string> lines;
    for (const edit::Group& group : loading)
    {
        if (group.group >= now.groups.size())
        {
            continue;
        }

        const uint64_t size = now.groups[group.group];
        std::string line = "Group " + group.name + " loads " + BytesText(size);
        if (game && group.group < game->groups.size())
        {
            const uint64_t before = game->groups[group.group];
            line += size > before   ? ", " + Grouped(size - before) + " more than the original"
                    : size < before ? ", " + Grouped(before - size) + " fewer than the original"
                                    : ", the same as the original";
        }
        lines.push_back(line);
    }

    return lines;
}

std::vector<std::string> WiiGroupSizeLines(const format::File& file, uint32_t file_id)
{
    // The groups of the archive that an export writes, with the timelines folded back. The other consoles' groups come
    // from such an archive too.
    const std::shared_ptr<const rsar::SoundArchive> exported =
        song::LoadWiiArchive(format::ExportArchive(edit::FoldTimelines(file)), "", ntdlib::rvl::snd::kRsarOptions);
    const rsar::SoundArchive& archive = *exported;
    std::vector<std::string> lines;
    for (std::size_t g = 0; g < archive.Groups().size(); ++g)
    {
        const rsar::SoundArchive::Group& group = archive.Groups()[g];
        const auto holds = [file_id](const rsar::SoundArchive::Group::Item& item)
        {
            return item.file_id == file_id;
        };
        if (std::ranges::any_of(group.items, holds))
        {
            const uint64_t size = static_cast<uint64_t>(group.size) + group.wave_data_size;
            lines.push_back("Group " + std::to_string(g) + " loads " + BytesText(size));
        }
    }

    return lines;
}

std::optional<WaveSoundView> MakeWaveSoundView(const format::File& file, const rsar::SoundArchive& archive,
                                               uint32_t sound)
{
    if (sound >= archive.Sounds().size() || archive.Sounds()[sound].type != rsar::SoundType::kWaveSound)
    {
        return std::nullopt;
    }

    WaveSoundView view;
    try
    {
        view.fields = edit::WaveSoundFieldsOf(archive, sound);
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
    const rsar::SoundInfo& info = archive.Sounds()[sound];
    ListWaves(archive, info.file_id, view.waves, view.wave_names);
    view.hold = ReadsHold(file);
    view.engine = format::Engine::kWii;

    // What else plays the wave sound: the other sounds that name the same wave sound of the same file.
    std::vector<std::string> others;
    for (uint32_t other = 0; other < archive.Sounds().size(); ++other)
    {
        const rsar::SoundInfo& candidate = archive.Sounds()[other];
        if (other != sound && candidate.type == rsar::SoundType::kWaveSound && candidate.file_id == info.file_id &&
            candidate.sub_index == info.sub_index)
        {
            others.push_back(candidate.name);
        }
    }
    view.reach = OthersLine(others, "other sound", "No other sound uses this wave sound.");
    WiiSizeLines(archive, info.file_id, "Wave sound file", view.sizes);

    return view;
}

std::optional<WaveSoundView> MakeWaveSoundView(const sar::SoundArchive& archive, uint32_t sound)
{
    if (sound >= archive.Sounds().size() || archive.Sounds()[sound].type != sar::SoundType::kWaveSound)
    {
        return std::nullopt;
    }

    WaveSoundView view;
    try
    {
        view.fields = edit::WaveSoundFieldsOf(archive, sound);
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
    ListWaves(archive, view.waves, view.wave_names);
    view.biquad_types = archive.ReadOptions().flavor == sar::Flavor::kNx ? 10 : 5;
    view.engine = format::EngineOf(archive.ReadOptions().flavor);

    // What else plays the wave sound: the other sounds that name the same wave sound of the same file.
    const sar::SoundInfo& info = archive.Sounds()[sound];
    std::vector<std::string> others;
    for (uint32_t other = 0; other < archive.Sounds().size(); ++other)
    {
        const sar::SoundInfo& candidate = archive.Sounds()[other];
        if (other != sound && candidate.type == sar::SoundType::kWaveSound && candidate.file_id == info.file_id &&
            candidate.wave_sound_index == info.wave_sound_index)
        {
            others.push_back(candidate.name);
        }
    }
    view.reach = OthersLine(others, "other sound", "No other sound uses this wave sound.");
    if (const std::optional<std::string> size = SizeLine(archive, info.file_id, "Wave sound file"))
    {
        view.sizes.push_back(*size);
    }

    return view;
}

} // namespace hokuto::app
