// SPDX-License-Identifier: MIT

#include "song/brownie.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/brownie/archive_player.h"
#include "ntdlib/brownie/commands.h"
#include "ntdlib/brownie/sequencer.h"
#include "ntdlib/brownie/song.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/agb/sf2.h"
#include "ntdlib/sequence_export/brownie/convert.h"
#include "ntdlib/sequence_export/brownie/soundfont.h"
#include "ntdlib/sequence_export/convert.h"
#include "xsflib/binary.h"

#include "format/brownie_items.h"
#include "song/driver_player.h"
#include "song/gba_bytes.h"
#include "song/midi_export.h"
#include "song/sequence.h"

namespace hokuto::song
{

namespace
{

namespace brownie = ntdlib::brownie;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;

// The output's rate: the driver leaves the sound bias register as the BIOS does (ntdlib/brownie/archive_player.h).
constexpr int kBrownieRate = 32768;

// The largest value of command F2.
constexpr int kMostVolume = 0xFF;

// The sound that the queue of revision 0 selects without a request (ntdlib's sequencer).
constexpr int kNoRequest = 0;

// The samples of a sample set of revision 0: one for each semitone.
constexpr uint32_t kSetSemitones = 12;

// The items that a channel reads from item `entry` on, in the order that a walk of its commands first meets them. A
// call's target comes before the commands after the call. The walk follows jumps, and stops at a return and an end.
std::vector<uint32_t> ChannelItems(const ntdlib::commands::Sequence& sequence, uint32_t entry)
{
    std::vector<uint32_t> order;
    std::vector<bool> seen(sequence.items.size());
    std::vector<uint32_t> pending = {entry};
    while (!pending.empty())
    {
        uint32_t at = pending.back();
        pending.pop_back();
        while (at < sequence.items.size() && !seen[at])
        {
            seen[at] = true;
            order.push_back(at);
            const Command& item = sequence.items[at];
            if (format::BrownieIsWord(item) || item.kind == Kind::kData || item.kind == Kind::kReturn ||
                item.kind == Kind::kEnd)
            {
                break;
            }

            if (item.kind == Kind::kCall && !item.target_outside)
            {
                pending.push_back(at + 1);
                at = item.target;
                continue;
            }

            if (item.kind == Kind::kJump)
            {
                if (item.target_outside)
                {
                    break;
                }

                at = item.target;
                continue;
            }

            at++;
        }
    }

    return order;
}

// The rules of one revision of the driver.
class BrownieRevisionRules final : public DriverRules
{
public:
    explicit BrownieRevisionRules(brownie::Revision revision)
        : DriverRules({.engine = format::Engine::kBrownie,
                       .revision = static_cast<uint8_t>(revision),
                       // A channel starts with F2's value at 0: a PSG channel's envelope then loses nothing, and a
                       // sample channel plays at 1/256 of its level.
                       .start_volume = 0,
                       // A sample channel selects a sample set, or in revision 1 a sample (command E1).
                       .notes_name_samples = false,
                       .mixes_echo = false}),
          revision_(revision)
    {
    }

    std::vector<uint32_t> ItemOffsets(const ntdlib::commands::Sequence& sequence) const override
    {
        return format::BrownieItemOffsets(sequence, revision_);
    }

    ntdlib::commands::Scale ScaleOf(Kind kind) const override
    {
        return brownie::ScaleOf(kind, revision_);
    }

    // A note's bytes hold no velocity. Command F2 sets the channel's volume.
    int VelocityOf(int, bool) const override
    {
        return 127;
    }

    int VolumeOf(int, bool) const override
    {
        return 0;
    }

    int MostVolume(int) const override
    {
        return kMostVolume;
    }

    bool PsgTrack(int track) const override
    {
        return track >= 0 && track < brownie::kFirstSampleChannel;
    }

    std::optional<NoteVelocities> Velocities() const override
    {
        return NoteVelocities{
            std::nullopt, "A note of Brownie Brown's driver holds no velocity. Command F2 sets its channel's volume."};
    }

private:
    brownie::Revision revision_;
};

// The model of the driver and the GBA's sound hardware, for one ROM (ntdlib::brownie::ArchivePlayer).
class BrowniePlayer final : public ArchiveDriverPlayer<brownie::ArchivePlayer>
{
public:
    explicit BrowniePlayer(const BrownieRom& rom) : ArchiveDriverPlayer(rom.rom, rom.info), rom_(rom)
    {
    }

    void Start(uint32_t sound, bool enhanced) override
    {
        brownie::ArchivePlayerOptions options;
        options.enhanced = enhanced;
        player_.Start(rom_.SongOf(sound), options);
    }

    int Tracks() const override
    {
        return brownie::kChannelCount;
    }

    // A tick is a frame: each frame runs the channels' commands of its tick.
    uint32_t Tick() const override
    {
        return static_cast<uint32_t>(player_.Frames());
    }

    // The driver starts the sound at the end of the first frame. The sound has ended once none of its channels is in
    // use.
    bool Finished() const override
    {
        if (player_.Frames() == 0)
        {
            return false;
        }

        const brownie::Sequencer& driver = player_.Driver();
        for (int channel = 0; channel < driver.ChannelCount(); channel++)
        {
            if (driver.GetChannel(channel).flags & brownie::kFlagOn)
            {
                return false;
            }
        }

        return true;
    }

private:
    const BrownieRom& rom_;
};

// A sample's entry in the driver's tables: three words with its start, its end and its loop.
BrownieSample SampleAt(const ntdlib::agb::Rom& rom, uint32_t entry)
{
    return {rom.U32(entry), rom.U32(entry + 4), rom.U32(entry + 8)};
}

// Whether `sample`'s points fall within the ROM, with a loop among them.
bool Readable(const ntdlib::agb::Rom& rom, const BrownieSample& sample)
{
    return sample.end > sample.start && rom.Contains(sample.start, sample.end - sample.start) &&
           sample.loop >= sample.start && sample.loop <= sample.end;
}

// The address of entry `semitone` of sample set `set` of revision 0, or of sample `set` of revision 1.
uint32_t EntryOf(const BrownieRom& rom, uint32_t set, uint32_t semitone)
{
    const uint32_t word = rom.rom.U32(rom.info.sample_table + 4 * set);
    return rom.info.revision == brownie::Revision::kMagicalVacation ? word : rom.rom.U32(word + 4 * semitone);
}

// The settings of the driver's commands that sound with a note and that an audition keeps: the programs, the
// envelopes, the volume, the pans, the transpose, the tune and the sweep, by kind and index.
bool AuditionKeeps(const Command& command)
{
    switch (command.kind)
    {
    case Kind::kProgram:
    case Kind::kVolume:
    case Kind::kPan:
    case Kind::kTranspose:
    case Kind::kTune:
    case Kind::kSweep:
        return true;
    default:
        return false;
    }
}

// The length byte that selects the longest length in length set 0: 0 stands for 256 frames.
uint8_t LongestLengthByte(const BrownieRom& rom)
{
    uint8_t best = 0x80;
    int longest = -1;
    for (int b = 0x80; b < 0xE0; b++)
    {
        const int frames = brownie::NoteLength(rom.rom, rom.info, static_cast<uint8_t>(b), 0);
        const int length = frames == 0 ? 256 : frames;
        if (length > longest)
        {
            longest = length;
            best = static_cast<uint8_t>(b);
        }
    }

    return best;
}

// The first sound that the queue starts without a condition: any sound but 0 in revision 0.
int FreeSound(const BrownieRom& rom)
{
    return rom.info.revision == brownie::Revision::kMagicalVacation ? 0 : 1;
}

} // namespace

const DriverRules& BrownieRules(brownie::Revision revision)
{
    static const std::array<BrownieRevisionRules, 2> rules = {
        BrownieRevisionRules(brownie::Revision::kSwordOfMana),
        BrownieRevisionRules(brownie::Revision::kMagicalVacation),
    };
    const auto index = static_cast<std::size_t>(revision);
    if (index >= rules.size())
    {
        throw std::invalid_argument("Brownie Brown's driver has no revision " + std::to_string(index));
    }

    return rules[index];
}

const BrownieRom* BrownieOf(const DriverRom* rom)
{
    return dynamic_cast<const BrownieRom*>(rom);
}

std::optional<uint8_t> BrownieProgramIndex(brownie::Revision, int track)
{
    if (track >= brownie::kFirstSampleChannel)
    {
        return uint8_t{0};
    }
    if (track % 4 == brownie::kNoiseChannel)
    {
        return std::nullopt;
    }

    return track % 4 == brownie::kWaveChannel ? uint8_t{3} : uint8_t{2};
}

std::optional<std::vector<Command>> BrownieSettingsAt(brownie::Revision revision,
                                                      const ntdlib::commands::Sequence& commands, const Track& track,
                                                      const Note& note)
{
    std::vector<uint32_t> offsets;
    try
    {
        offsets = format::BrownieItemOffsets(commands, revision);
    }
    catch (const std::invalid_argument&)
    {
        return std::nullopt;
    }

    const std::map<uint32_t, uint32_t> items = ItemsByOffset(offsets);

    std::map<std::pair<Kind, uint8_t>, Command> settings;
    bool reached = false;
    for (std::size_t s = 0; s < track.stretches.size() && s <= note.stretch && !reached; s++)
    {
        const auto first = items.find(track.stretches[s].first);
        const auto last = items.find(track.stretches[s].last);
        if (first == items.end() || last == items.end())
        {
            continue;
        }

        for (uint32_t i = first->second; i <= last->second; i++)
        {
            if (s == note.stretch && offsets[i] == note.command)
            {
                reached = true;
                break;
            }

            const Command& command = commands.items[i];
            if (AuditionKeeps(command))
            {
                Command kept = command;
                kept.target = 0;
                kept.target_outside = false;
                settings[{command.kind, command.index}] = kept;
            }
        }
    }

    std::vector<Command> held;
    for (const auto& [kind, command] : settings)
    {
        held.push_back(command);
    }

    return held;
}

int BrownieShownKey(brownie::Revision revision, int track, int base, int transpose, uint8_t byte)
{
    if (track < brownie::kFirstSampleChannel && track % 4 == brownie::kNoiseChannel)
    {
        return byte;
    }

    const int key = (byte + transpose) & 0x7F;
    if (track >= brownie::kFirstSampleChannel && revision == brownie::Revision::kSwordOfMana)
    {
        return base + brownie::SampleKey(key);
    }

    return base + key;
}

const DriverRules& BrownieRom::Rules() const
{
    return BrownieRules(info.revision);
}

std::span<const uint8_t> BrownieRom::Bytes() const
{
    return {rom.Ptr(ntdlib::agb::kRomBase), rom.Size()};
}

int BrownieRom::SongOf(uint32_t sound) const
{
    return sound < sounds.size() ? sounds[sound] : -1;
}

std::optional<BrownieRom::ListPlace> BrownieRom::ListOf(uint32_t sound) const
{
    const auto found = lists.find(SongOf(sound));
    return found != lists.end() ? std::optional(found->second) : std::nullopt;
}

std::vector<uint32_t> BrownieRom::Sounds() const
{
    // A sound plays when its list falls within the ROM. The queue of revision 0 selects sound 0 without a request.
    std::vector<uint32_t> playing;
    for (uint32_t sound = 0; sound < sounds.size(); ++sound)
    {
        std::vector<brownie::SongChannel> channels;
        const int song = sounds[sound];
        if ((song != kNoRequest || info.revision == brownie::Revision::kMagicalVacation) &&
            brownie::ReadSong(rom, info, song, channels))
        {
            playing.push_back(sound);
        }
    }

    return playing;
}

uint32_t BrownieRom::SoundCount() const
{
    return static_cast<uint32_t>(sounds.size());
}

std::string BrownieRom::SoundName(uint32_t sound) const
{
    return "Sound " + ntdlib::sequence_export::agb::SongNumber(SongOf(sound), info.song_count);
}

SoundFields BrownieRom::FieldsOf(uint32_t sound) const
{
    // Entry `song` of the song table names the sound's list. The list starts each channel of the sound.
    SoundFields fields;
    const int song = SongOf(sound);
    if (song < 0 || song >= info.song_count)
    {
        return fields;
    }

    fields.sequence = uint64_t{info.song_table} + 4 * static_cast<uint64_t>(song) - ntdlib::agb::kRomBase;
    fields.starts = true;

    return fields;
}

int BrownieRom::Rate() const
{
    return kBrownieRate;
}

std::vector<uint32_t> BrownieRom::SampleSelections() const
{
    std::set<uint32_t> selected;
    for (const ntdlib::brownie::Decoded& region : regions)
    {
        for (const Command& item : region.sequence.items)
        {
            if (item.kind == Kind::kProgram && item.index == 0)
            {
                selected.insert(static_cast<uint32_t>(item.value.value / kOne));
            }
        }
    }

    return {selected.begin(), selected.end()};
}

std::vector<int> BrownieRom::Samples() const
{
    std::vector<int> numbers;
    for (const auto& [number, sample] : samples)
    {
        numbers.push_back(number);
    }

    return numbers;
}

std::string BrownieRom::SampleName(int sample) const
{
    return DriverSampleName(sample);
}

void BrownieRom::CheckSample(int sample) const
{
    if (!samples.contains(sample))
    {
        throw std::invalid_argument("The ROM holds no " + SampleName(sample) + ".");
    }
}

double BrownieRom::SampleRate(int sample) const
{
    CheckSample(sample);
    return info.mix_rate;
}

std::vector<uint8_t> BrownieRom::SampleWav(int sample) const
{
    const double rate = SampleRate(sample);
    const BrownieSample& entry = samples.at(sample);
    const std::optional<WavLoop> loop = entry.loop != entry.end
                                            ? std::optional(WavLoop{entry.loop - entry.start, entry.end - entry.start})
                                            : std::nullopt;
    return Pcm8Wav(rom, entry.start, entry.end - entry.start, false, loop, WavRate(rate));
}

SampleShape BrownieRom::ShapeOf(int sample) const
{
    CheckSample(sample);
    const BrownieSample& entry = samples.at(sample);
    return {entry.end - entry.start, entry.loop != entry.end ? std::optional(entry.loop - entry.start) : std::nullopt};
}

namespace
{

// Adds to `played` the numbers of the samples of sample set `set`, at each of its first `semitones` semitones.
void AddSetSamples(const BrownieRom& brownie_rom, uint32_t set, uint32_t semitones, std::set<int>& played)
{
    for (uint32_t semitone = 0; semitone < semitones; semitone++)
    {
        const BrownieSample entry = SampleAt(brownie_rom.rom, EntryOf(brownie_rom, set, semitone));
        for (const auto& [number, sample] : brownie_rom.samples)
        {
            if (sample == entry)
            {
                played.insert(number);
            }
        }
    }
}

} // namespace

std::vector<int> BrownieRom::SamplesOfSound(uint32_t sound) const
{
    // The samples of each sample set, or each sample, that the sound's sample channels select.
    const std::optional<ListPlace> place = ListOf(sound);
    if (!place)
    {
        return {};
    }

    const ntdlib::commands::Sequence& sequence = regions[place->region].sequence;
    std::set<int> played;
    for (const brownie::ListTrack& track : brownie::TracksOfList(sequence, place->list))
    {
        if (track.channel < brownie::kFirstSampleChannel)
        {
            continue;
        }

        for (const uint32_t item : ChannelItems(sequence, track.entry))
        {
            const Command& command = sequence.items[item];
            if (command.kind != Kind::kProgram || command.index != 0)
            {
                continue;
            }

            const auto set = static_cast<uint32_t>(command.value.value / kOne);
            const uint32_t semitones = info.revision == brownie::Revision::kMagicalVacation ? 1 : kSetSemitones;
            AddSetSamples(*this, set, semitones, played);
        }
    }

    return {played.begin(), played.end()};
}

std::vector<uint32_t> BrownieRom::TrackPrograms(int track) const
{
    // The duties of the square channels' command F1, and the waves of the wave channel's command FC, that the sounds
    // select.
    const std::optional<uint8_t> index = BrownieProgramIndex(info.revision, track);
    if (!index || track >= brownie::kFirstSampleChannel)
    {
        return {};
    }

    std::set<uint32_t> programs;
    for (const auto& [song, place] : lists)
    {
        const ntdlib::commands::Sequence& sequence = regions[place.region].sequence;
        for (const brownie::ListTrack& listed : brownie::TracksOfList(sequence, place.list))
        {
            if (listed.channel != track)
            {
                continue;
            }

            for (const uint32_t item : ChannelItems(sequence, listed.entry))
            {
                const Command& command = sequence.items[item];
                if (command.kind == Kind::kProgram && command.index == *index)
                {
                    programs.insert(static_cast<uint32_t>(command.value.value / kOne));
                }
            }
        }
    }

    return {programs.begin(), programs.end()};
}

std::vector<DriverBankSlot> BrownieRom::BankSlots(uint32_t) const
{
    // The sample channels select one of the driver's sample sets, or samples (command E1).
    DriverBankSlot slot;
    slot.bank = info.revision == brownie::Revision::kMagicalVacation ? "Samples" : "Sample sets";
    slot.programs = SampleSelections();

    return {slot};
}

Sequence BrownieRom::ReadSequence(uint32_t sound) const
{
    return song::ReadSequence(*this, sound);
}

ntdlib::sequence_export::Conversion BrownieRom::ConvertToMidi(uint32_t sound, const std::string& program) const
{
    ntdlib::sequence_export::agb::ConvertSettings settings;
    settings.program = program;
    ntdlib::sequence_export::brownie::SongFiles files =
        ntdlib::sequence_export::brownie::ConvertSongFiles(rom, info, SongOf(sound), settings);

    return AgbConversion(SoundName(sound), std::move(files));
}

ntdlib::sequence_export::BankConversion BrownieRom::ConvertSamples(std::span<const int> numbers,
                                                                   const std::string& program) const
{
    namespace agb = ntdlib::sequence_export::agb;
    ntdlib::sequence_export::brownie::SoundfontBuilder builder(rom, static_cast<uint32_t>(info.mix_rate));
    agb::Sf2File& file = builder.File();
    ntdlib::sequence_export::BankConversion conversion;
    for (const int sample : numbers)
    {
        const auto found = samples.find(sample);
        const int index = found != samples.end()
                              ? builder.GameSampleIndex({found->second.start, found->second.end, found->second.loop})
                              : -1;
        if (index < 0)
        {
            conversion.warnings.push_back(SampleName(sample) + " holds no samples.");
            continue;
        }

        const std::string name = SampleName(sample);
        const bool looped = file.samples[static_cast<std::size_t>(index)].loop_end != 0;
        file.instruments.push_back(agb::MakeInstrument(name, {agb::RootKeyZone(index, looped, 0, 127, 60)}));
        file.presets.push_back(agb::MakePreset(name, static_cast<uint16_t>(sample / 128),
                                               static_cast<uint16_t>(sample % 128),
                                               static_cast<int>(file.instruments.size()) - 1));
    }

    return SamplesConversion(std::move(conversion), file, numbers, program, rom.Title());
}

std::unique_ptr<DriverPlayer> BrownieRom::MakePlayer() const
{
    return std::make_unique<BrowniePlayer>(*this);
}

DriverAudition BrownieRom::SampleAudition(int sample, int key, int) const
{
    CheckSample(sample);
    const BrownieSample& wanted = samples.at(sample);

    // The sample set and the semitone that hold the sample, or the sample's number in the driver's table.
    const bool vacation = info.revision == brownie::Revision::kMagicalVacation;
    std::optional<std::pair<uint32_t, uint32_t>> place;
    for (const uint32_t set : SampleSelections())
    {
        for (uint32_t semitone = 0; semitone < (vacation ? 1 : kSetSemitones) && !place; semitone++)
        {
            if (SampleAt(rom, EntryOf(*this, set, semitone)) == wanted)
            {
                place = std::pair(set, semitone);
            }
        }
    }
    if (!place)
    {
        throw std::invalid_argument("No sample set of the driver's plays " + SampleName(sample) + ".");
    }

    // The settings with which a channel first plays the set or the sample. Without them, a sample channel's volume
    // stays at 0.
    DriverAudition audition;
    audition.track = brownie::kFirstSampleChannel;
    const auto setup = audition_setups.find(place->first);
    if (setup != audition_setups.end())
    {
        audition.setup = setup->second;
    }
    else
    {
        audition.setup = {ValueCommand(Kind::kProgram, place->first), ValueCommand(Kind::kVolume, kMostVolume)};
    }

    std::erase_if(audition.setup, [](const Command& command) { return command.kind == Kind::kTranspose; });

    // Revision 0's mixer plays a semitone's sample at its rate in the octave from key 36, and an octave lower at half
    // its rate. In revision 1, the timer plays a key at the rate of its entry in the table.
    int byte = 0;
    if (!vacation)
    {
        const int octave = std::clamp(3 + static_cast<int>(std::floor((key - 60) / 12.0)), 0, 3);
        byte = 12 * octave + static_cast<int>(place->second);
    }
    else
    {
        int nearest = 0;
        double distance = std::numeric_limits<double>::infinity();
        for (int k = 0; k < 0x80; k++)
        {
            const uint32_t at = info.rate_table + 2 * static_cast<uint32_t>(k * brownie::kRateSteps);
            if (!rom.Contains(at, 2))
            {
                break;
            }

            const int reload = rom.U16(at);
            if (reload == 0)
            {
                continue;
            }

            const double rate = static_cast<double>(ntdlib::agb::kSecondCycles) / (0x10000 - reload);
            const double off = std::abs(std::log2(rate / info.mix_rate));
            if (off < distance)
            {
                distance = off;
                nearest = k;
            }
        }

        byte = std::clamp(nearest + key - 60, 0, 0x7F);
    }

    audition.note.kind = Kind::kNote;
    audition.note.key = static_cast<uint8_t>(byte);

    return audition;
}

std::optional<DriverAudition> BrownieRom::NoteAudition(const ntdlib::commands::Sequence& commands,
                                                       const Sequence& sequence, int track, int program, int key,
                                                       int) const
{
    // The settings that the track's commands have set at its first note of the program, and the byte that plays the key
    // with the track's transpose there.
    const auto found = std::ranges::find(sequence.tracks, track, &Track::number);
    if (found == sequence.tracks.end() || track >= brownie::kChannelCount)
    {
        return std::nullopt;
    }

    const auto note = std::ranges::find_if(
        found->notes, [program](const Note& note) { return note.program == static_cast<uint32_t>(program); });
    if (note == found->notes.end())
    {
        return std::nullopt;
    }

    const std::optional<std::vector<Command>> settings = BrownieSettingsAt(info.revision, commands, *found, *note);
    if (!settings)
    {
        return std::nullopt;
    }

    DriverAudition audition;
    audition.track = track;
    int transpose = 0;
    for (const Command& command : *settings)
    {
        audition.setup.push_back(command);
        if (command.kind == Kind::kTranspose)
        {
            transpose = static_cast<int>(
                ntdlib::commands::FromUnit(command.value.value, brownie::ScaleOf(Kind::kTranspose, info.revision)));
        }
    }

    // A noise note shows at its byte. For another note, the key is the byte with the transpose added.
    int byte = key;
    const int base = brownie::PsgTuning(rom, info).base;
    if (track >= brownie::kFirstSampleChannel || track % 4 != brownie::kNoiseChannel)
    {
        byte = key - base - transpose;
    }
    if (byte < 0 || byte >= 0x80 ||
        BrownieShownKey(info.revision, track, base, transpose, static_cast<uint8_t>(byte)) != key)
    {
        return std::nullopt;
    }

    audition.note.kind = Kind::kNote;
    audition.note.key = static_cast<uint8_t>(byte);

    return audition;
}

std::shared_ptr<const DriverRom> BrownieRom::AuditionRom(const DriverAudition& audition) const
{
    if (audition.track < 0 || audition.track >= brownie::LayoutOf(info.revision).channels)
    {
        throw std::invalid_argument("Brownie Brown's driver has no channel " + std::to_string(audition.track) + ".");
    }

    // A list of one channel. The commands of the channel are the setup, the longest length, the note and three holds.
    const int sound = FreeSound(*this);
    if (sound >= info.song_count)
    {
        throw std::invalid_argument("The ROM holds no sound that a note can take the place of.");
    }

    ntdlib::commands::Sequence sequence;
    Command channel;
    channel.kind = Kind::kOpenTrack;
    channel.index = static_cast<uint32_t>(audition.track);
    channel.target = 3;
    sequence.items.push_back(channel);
    Command table;
    table.kind = Kind::kJump;
    table.encoding.opcode = brownie::kAddressWord;
    table.target_outside = true;
    table.target = ntdlib::agb::kRomBase;
    sequence.items.push_back(table);
    sequence.items.push_back(Command{.kind = Kind::kEnd, .index = 1});
    for (const Command& setup : audition.setup)
    {
        sequence.items.push_back(setup);
    }

    Command length;
    length.kind = Kind::kOther;
    length.encoding.opcode = LongestLengthByte(*this);
    sequence.items.push_back(length);
    sequence.items.push_back(audition.note);
    for (int hold = 0; hold < 3; hold++)
    {
        sequence.items.push_back(Command{.kind = Kind::kWait});
    }

    sequence.items.push_back(Command{.kind = Kind::kEnd});

    std::vector<uint8_t> bytes = PaddedRomCopy(rom);
    const uint32_t base = EndAddress(bytes);
    const brownie::Encoded encoded = brownie::EncodeSequence(sequence, info.revision, base);
    AppendWordAligned(bytes, encoded.bytes);
    xsflib::StoreLe32(&bytes[info.song_table + 4 * static_cast<uint32_t>(sound) - ntdlib::agb::kRomBase], base);

    auto copy = std::make_shared<BrownieRom>();
    copy->rom.Assign(std::move(bytes));
    copy->info = info;
    copy->sounds = {sound};
    copy->samples = samples;

    return copy;
}

namespace
{

// Adds to `rom` the samples of each sample set or sample that the commands select, once each.
void AddSampleSelections(BrownieRom& rom)
{
    const bool vacation = rom.info.revision == brownie::Revision::kMagicalVacation;
    for (const uint32_t set : rom.SampleSelections())
    {
        for (uint32_t semitone = 0; semitone < (vacation ? 1 : kSetSemitones); semitone++)
        {
            const BrownieSample sample = SampleAt(rom.rom, EntryOf(rom, set, semitone));
            const bool known =
                std::ranges::any_of(rom.samples, [&sample](const auto& entry) { return entry.second == sample; });
            if (!known && Readable(rom.rom, sample))
            {
                const int number = vacation ? static_cast<int>(set) : static_cast<int>(rom.samples.size());
                rom.samples.emplace(number, sample);
            }
        }
    }
}

// Adds to `rom` the settings with which the channel whose data starts at `entry` first plays each sample set or sample
// that no other channel has played yet.
void AddChannelSetups(BrownieRom& rom, const ntdlib::commands::Sequence& sequence, uint32_t entry)
{
    std::map<std::pair<Kind, uint8_t>, Command> settings;
    for (const uint32_t item : ChannelItems(sequence, entry))
    {
        const Command& command = sequence.items[item];
        if (AuditionKeeps(command))
        {
            settings[{command.kind, command.index}] = command;
        }

        const auto selected = settings.find({Kind::kProgram, uint8_t{0}});
        if (command.kind == Kind::kNote && selected != settings.end())
        {
            const auto set = static_cast<uint32_t>(selected->second.value.value / kOne);
            if (!rom.audition_setups.contains(set))
            {
                std::vector<Command>& setup = rom.audition_setups[set];
                for (const auto& [kind, kept] : settings)
                {
                    setup.push_back(kept);
                }
            }
        }
    }
}

// Adds to `rom` the settings with which a channel first plays each sample set or sample.
void AddAuditionSetups(BrownieRom& rom)
{
    for (const auto& [song, place] : rom.lists)
    {
        const ntdlib::commands::Sequence& sequence = rom.regions[place.region].sequence;
        for (const brownie::ListTrack& listed : brownie::TracksOfList(sequence, place.list))
        {
            if (listed.channel >= brownie::kFirstSampleChannel)
            {
                AddChannelSetups(rom, sequence, listed.entry);
            }
        }
    }
}

} // namespace

std::shared_ptr<const BrownieRom> LoadBrownieRom(std::vector<uint8_t> bytes)
{
    auto rom = std::make_shared<BrownieRom>();
    rom->rom.Assign(std::move(bytes));
    std::string error;
    if (!brownie::DetectDriver(rom->rom, {}, rom->info, error))
    {
        throw xsflib::FormatError(error.empty() ? "the ROM shows no sign of Brownie Brown's driver"
                                                : "Hokuto can't read the tables of Brownie Brown's driver: " + error);
    }

    // The sounds keep their numbers.
    for (int song = 0; song < rom->info.song_count; ++song)
    {
        rom->sounds.push_back(song);
    }

    // The regions of the sounds' commands, and where each sound's list starts.
    for (const brownie::Region& region : brownie::FindRegions(rom->rom, rom->info))
    {
        brownie::Decoded decoded = brownie::DecodeRegion(rom->rom, rom->info, region);
        if (decoded.opaque)
        {
            continue;
        }

        const auto index = static_cast<uint32_t>(rom->regions.size());
        for (std::size_t s = 0; s < decoded.sounds.size() && s < decoded.lists.size(); s++)
        {
            rom->lists[decoded.sounds[s]] = {index, decoded.lists[s]};
        }

        rom->regions.push_back(std::move(decoded));
    }

    // The samples of each sample set or sample that the commands select, once each, and the settings with which a
    // channel first plays each one.
    AddSampleSelections(*rom);
    AddAuditionSetups(*rom);
    if (rom->Sounds().empty())
    {
        throw xsflib::FormatError("the driver's song table names no sound within the ROM");
    }

    return rom;
}

} // namespace hokuto::song
