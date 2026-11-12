// SPDX-License-Identifier: MIT

#include "song/rd2.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/rd2/archive_player.h"
#include "ntdlib/rd2/commands.h"
#include "ntdlib/rd2/output.h"
#include "ntdlib/rd2/sequencer.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/agb/sf2.h"
#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/sequence_export/rd2/convert.h"
#include "ntdlib/sequence_export/rd2/soundfont.h"
#include "xsflib/binary.h"

#include "song/driver_player.h"
#include "song/gba_bytes.h"
#include "song/midi_export.h"
#include "song/sequence.h"

namespace hokuto::song
{

namespace
{

namespace rd2 = ntdlib::rd2;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;

// The output's rate: the driver's init sets 8 bits at 65,536 Hz in the sound bias register.
constexpr int kRd2Rate = 65536;

// The driver's largest volume of a track (command E0).
constexpr int kMostTrackVolume = 0xFF;

// The longest length that the two bytes of a note or a wait can store, in ticks.
constexpr int64_t kLongestLength = 0x7FFF;

// The keys that a note's command contains.
constexpr int kHighestKey = 0x5F;

// The address of the list of banks of sequence `sequence` of `info`: the list of a song, or else of a sound effect.
uint32_t BankListAddress(const rd2::DriverInfo& info, int sequence)
{
    const auto music = static_cast<int>(info.sequence_addresses.size());
    return sequence < music ? info.bank_list_addresses[static_cast<std::size_t>(sequence)]
                            : info.effect_bank_list_addresses[static_cast<std::size_t>(sequence - music)];
}

// The entries of a table of offsets from its start: the first entry points past the table.
uint32_t TableEntries(const ntdlib::agb::Rom& rom, uint32_t table)
{
    if (table == 0 || !rom.Contains(table, 4))
    {
        return 0;
    }

    const uint32_t first = rom.U32(table);
    return first % 4 == 0 && rom.Contains(table, first) ? first / 4 : 0;
}

// The number of instruments of bank `bank`. A bank starts with a halfword for each instrument: the instrument's offset
// from the bank's start, or 0 for none. The table ends where the first instrument's settings start. A note's
// instrument has 7 bits: at most 128.
uint32_t InstrumentCount(const ntdlib::agb::Rom& rom, const rd2::DriverInfo& info, uint16_t bank)
{
    const uint32_t base = rd2::BankAddress(rom, info, bank);
    uint32_t end = 2 * 256;
    uint32_t instrument = 0;
    for (; 2 * instrument < end && rom.Contains(base + 2 * instrument, 2); ++instrument)
    {
        const uint16_t offset = rom.U16(base + 2 * instrument);
        if (offset != 0)
        {
            end = std::min<uint32_t>(end, offset);
        }
    }

    return std::min<uint32_t>(instrument, 128);
}

// A sample's header: its length, rate, loop start and loop end, then its 8-bit points. The driver plays up to the
// loop's end and jumps back to its start, or without a loop up to the length.
struct SampleHeader
{
    uint32_t length = 0;
    uint32_t rate = 0;
    uint32_t loop_start = 0;
    uint32_t loop_end = 0;

    uint32_t Points() const
    {
        return loop_end != 0 ? loop_end : length;
    }
};

SampleHeader HeaderAt(const ntdlib::agb::Rom& rom, uint32_t address)
{
    return {rom.U32(address), rom.U32(address + 4), rom.U32(address + 8), rom.U32(address + 12)};
}

// The rules of one revision of the driver.
class Rd2RevisionRules final : public DriverRules
{
public:
    explicit Rd2RevisionRules(rd2::Revision revision)
        : DriverRules({.engine = format::Engine::kRd2,
                       .revision = static_cast<uint8_t>(revision),
                       .start_volume = kRd2StartVolume,
                       // A track selects its instrument, and the instrument's regions select the samples.
                       .notes_name_samples = false,
                       // A track sends to the driver's echo (command E3), but the game's code sets the echo's level.
                       // The model keeps the level that the driver's init leaves, and the echo then adds nothing.
                       .mixes_echo = false}),
          revision_(revision)
    {
    }

    std::vector<uint32_t> ItemOffsets(const ntdlib::commands::Sequence& sequence) const override
    {
        return rd2::ItemOffsets(sequence, revision_);
    }

    ntdlib::commands::Scale ScaleOf(Kind kind) const override
    {
        return rd2::ScaleOf(kind);
    }

    int VelocityOf(int volume, bool) const override
    {
        return Rd2VelocityOf(volume);
    }

    int VolumeOf(int velocity, bool) const override
    {
        return Rd2DriverVelocity(velocity);
    }

    int MostVolume(int) const override
    {
        return kMostTrackVolume;
    }

    // A track plays any of the driver's voices.
    bool PsgTrack(int) const override
    {
        return false;
    }

    // A song starts at the tempo of a tick a frame.
    std::optional<double> StartTempoBpm() const override
    {
        return static_cast<double>(rd2::kStartTempo);
    }

    // A note's command contains the driver's velocity, a byte.
    std::optional<NoteVelocities> Velocities() const override
    {
        return NoteVelocities{UINT8_MAX, "A note's velocity runs from 0 to 255."};
    }

private:
    rd2::Revision revision_;
};

// The model of the driver and the GBA's sound hardware, for one ROM (ntdlib::rd2::ArchivePlayer).
class Rd2Player final : public ArchiveDriverPlayer<rd2::ArchivePlayer>
{
public:
    explicit Rd2Player(const Rd2Rom& rom) : ArchiveDriverPlayer(rom.rom, rom.info), rom_(rom)
    {
    }

    void Start(uint32_t sound, bool enhanced) override
    {
        rd2::ArchivePlayerOptions options;
        options.enhanced = enhanced;
        options.version = rom_.VersionOf(sound);
        player_.Start(rom_.SongOf(sound), options);
    }

    int Tracks() const override
    {
        return rd2::kPlayerTracks;
    }

    // The ticks whose commands have run. The driver counts kTickUnits of time in a tick. A frame runs the commands up
    // to the time at its start, and then adds the frame's tempo to the time. At a tempo below kTickUnits, the time
    // after the first frame still falls in tick 0. The commands of tick 0 have already run by then.
    uint32_t Tick() const override
    {
        const rd2::Sequencer* song = player_.Driver().Song();
        if (song == nullptr || player_.Frames() == 0)
        {
            return 0;
        }

        const uint64_t last_start = song->Elapsed() - static_cast<uint64_t>(song->FrameTempo());
        return static_cast<uint32_t>(last_start / rd2::kTickUnits + 1);
    }

    bool Finished() const override
    {
        const rd2::Sequencer* song = player_.Driver().Song();
        return song == nullptr || song->Ended();
    }

private:
    const Rd2Rom& rom_;
};

// The bank, the instrument and the note with which an instrument of the driver plays a sample. A note that the
// region's tuning gives plays the sample at its rate. With `fixed`, a drum kit or an instrument with a sample for each
// key plays the sample: the note selects it, at one pitch.
struct SamplePlace
{
    uint16_t bank = 0;
    uint16_t instrument = 0;
    int note = 0;
    bool fixed = false;
};

// The address of the sample that `lookup`, an instrument's settings for a note of bank `bank`, plays. Nothing for a
// note that plays no sample: a PSG voice, the wave voice, or a region that is missing from the bank. An instrument with
// a sample for each key plays the sample of the note's entry (ntdlib's Sequencer::SetVoiceSound).
std::optional<uint32_t> SampleOfLookup(const ntdlib::agb::Rom& rom, const rd2::DriverInfo& info, uint16_t bank,
                                       const rd2::Lookup& lookup)
{
    if (!lookup.found || lookup.missing)
    {
        return std::nullopt;
    }
    if (lookup.key_sample)
    {
        return rd2::SampleAddress(rom, info, bank, lookup.key_sample_index);
    }
    if (lookup.region == 0 || rd2::RegionType(rom, lookup.region) != rd2::kSampleType)
    {
        return std::nullopt;
    }

    return rd2::SampleAddress(rom, info, bank, rom.U16(lookup.region + 2));
}

// A sample that a note of a sound plays, by its number, and the note's bank, instrument and key.
struct NoteSample
{
    int sample = 0;
    SamplePlace place;
};

// The samples that the notes of sound `sound` play, in the order of the notes. Nothing for a sound whose sequence
// Hokuto can't read.
std::vector<NoteSample> NoteSamples(const Rd2Rom& rom, uint32_t sound)
{
    Sequence sequence;
    try
    {
        sequence = rom.ReadSequence(sound);
    }
    catch (const std::runtime_error&)
    {
        return {};
    }

    const std::vector<uint16_t> banks = rom.BankListOf(sound);
    std::vector<NoteSample> samples;
    for (const Track& track : sequence.tracks)
    {
        for (const Note& note : track.notes)
        {
            // The driver plays the bank in the slot's entry of the list (ntdlib's Sequencer::BankOf). BankListOf
            // reads every slot up to the last that the sequence selects. It stops early only at an entry past the
            // table of banks. A note's bank is missing from the list only at or after such an entry. The reader then
            // assigns the note slot 0. The note counts against the first bank.
            if (note.bank_slot >= banks.size())
            {
                continue;
            }

            const uint16_t bank = banks[note.bank_slot];
            const auto instrument = static_cast<uint16_t>(note.program);
            const rd2::Lookup lookup = rd2::LookUpInstrument(rom.rom, rom.info, bank, instrument, note.key);
            const std::optional<uint32_t> address = SampleOfLookup(rom.rom, rom.info, bank, lookup);
            const auto found = address ? std::ranges::find(rom.sample_addresses, *address) : rom.sample_addresses.end();
            if (found != rom.sample_addresses.end())
            {
                samples.push_back({static_cast<int>(found - rom.sample_addresses.begin()),
                                   SamplePlace{bank, instrument, note.key, lookup.drum || lookup.key_sample}});
            }
        }
    }

    return samples;
}

} // namespace

// The samples that the notes of a ROM's sounds play (NoteSamples): each sound's samples, and the place of the first
// note of the sounds that plays each sample. `read` guards the fill.
struct Rd2Uses
{
    std::once_flag read;
    std::map<uint32_t, std::vector<int>> samples;
    std::map<int, SamplePlace> places;
};

std::shared_ptr<Rd2Uses> MakeRd2Uses()
{
    return std::make_shared<Rd2Uses>();
}

namespace
{

// The table of the samples that the notes of `rom` play, read on the first call.
const Rd2Uses& UsesOf(const Rd2Rom& rom)
{
    Rd2Uses& uses = *rom.uses;
    std::call_once(uses.read,
                   [&rom, &uses]
                   {
                       for (const uint32_t sound : rom.Sounds())
                       {
                           std::set<int> played;
                           for (const NoteSample& note : NoteSamples(rom, sound))
                           {
                               played.insert(note.sample);
                               uses.places.try_emplace(note.sample, note.place);
                           }
                           uses.samples[sound] = {played.begin(), played.end()};
                       }
                   });
    return uses;
}

// The place of sample `sample` (SamplePlace), or nothing when no instrument plays it. A sample instrument of the
// driver's banks comes first. The place of a sample that only drum kits and instruments with a sample for each key play
// is the first note of the sounds that plays it.
std::optional<SamplePlace> PlaceOfSample(const Rd2Rom& rom, int sample)
{
    const uint32_t address = rom.sample_addresses[static_cast<std::size_t>(sample)];
    const uint32_t banks = TableEntries(rom.rom, rom.info.banks);
    std::optional<SamplePlace> found;
    for (uint32_t bank = 0; bank < banks; ++bank)
    {
        const auto instruments = static_cast<int>(InstrumentCount(rom.rom, rom.info, static_cast<uint16_t>(bank)));
        for (int instrument = 0; instrument < instruments; ++instrument)
        {
            for (int note = 0; note <= kHighestKey; ++note)
            {
                const rd2::Lookup lookup =
                    rd2::LookUpInstrument(rom.rom, rom.info, static_cast<uint16_t>(bank),
                                          static_cast<uint16_t>(instrument), static_cast<uint8_t>(note));
                if (!lookup.found || lookup.region == 0 || lookup.missing || lookup.drum || lookup.key_sample ||
                    rd2::RegionType(rom.rom, lookup.region) != rd2::kSampleType ||
                    rd2::SampleAddress(rom.rom, rom.info, static_cast<uint16_t>(bank),
                                       rom.rom.U16(lookup.region + 2)) != address)
                {
                    continue;
                }

                const int tuned = rd2::RegionTuning(rom.rom, lookup.region);
                if (note == tuned)
                {
                    return SamplePlace{static_cast<uint16_t>(bank), static_cast<uint16_t>(instrument), note};
                }

                if (!found)
                {
                    found = SamplePlace{static_cast<uint16_t>(bank), static_cast<uint16_t>(instrument), note};
                }
            }
            if (found)
            {
                return found;
            }
        }
    }

    const Rd2Uses& uses = UsesOf(rom);
    const auto played = uses.places.find(sample);
    return played != uses.places.end() ? std::optional(played->second) : std::nullopt;
}

} // namespace

const DriverRules& Rd2Rules(rd2::Revision revision)
{
    static const std::array<Rd2RevisionRules, 3> rules = {
        Rd2RevisionRules(rd2::Revision::kLinkToThePast),
        Rd2RevisionRules(rd2::Revision::kSuperMarioAdvance2),
        Rd2RevisionRules(rd2::Revision::kSuperMarioAdvance3),
    };
    const auto index = static_cast<std::size_t>(revision);
    if (index >= rules.size())
    {
        throw std::invalid_argument("Nintendo R&D2's driver has no revision " + std::to_string(index));
    }

    return rules[index];
}

const Rd2Rom* Rd2Of(const DriverRom* rom)
{
    return dynamic_cast<const Rd2Rom*>(rom);
}

int Rd2VelocityOf(int velocity)
{
    return static_cast<int>(std::lround(std::clamp(velocity, 0, 255) * 127.0 / 255.0));
}

int Rd2DriverVelocity(int velocity)
{
    if (velocity <= 0)
    {
        return 0;
    }

    return std::clamp(static_cast<int>(std::lround(velocity * 255.0 / 127.0)), 1, 255);
}

const DriverRules& Rd2Rom::Rules() const
{
    return Rd2Rules(info.revision);
}

std::span<const uint8_t> Rd2Rom::Bytes() const
{
    return {rom.Ptr(ntdlib::agb::kRomBase), rom.Size()};
}

int Rd2Rom::SongOf(uint32_t sound) const
{
    return sound < sounds.size() ? sounds[sound].first : -1;
}

int Rd2Rom::VersionOf(uint32_t sound) const
{
    return sound < sounds.size() ? sounds[sound].second : 0;
}

std::vector<uint32_t> Rd2Rom::Sounds() const
{
    // A song plays when its sequence falls within the ROM, and a sound effect when its entry names a track.
    std::vector<uint32_t> playing;
    for (uint32_t sound = 0; sound < sounds.size(); ++sound)
    {
        const int song = sounds[sound].first;
        const rd2::Effect* effect = rd2::SongEffect(info, song);
        const uint32_t address = rd2::SequenceAddress(info, rd2::SequenceOfSong(info, song));
        if (rom.Contains(address, 4) && (effect == nullptr || effect->address != 0))
        {
            playing.push_back(sound);
        }
    }

    return playing;
}

uint32_t Rd2Rom::SoundCount() const
{
    return static_cast<uint32_t>(sounds.size());
}

std::string Rd2Rom::SoundName(uint32_t sound) const
{
    const int song = SongOf(sound);
    return VersionedSoundName(song, rd2::SongCount(info), ntdlib::sequence_export::rd2::SongVersions(info, song),
                              static_cast<std::size_t>(VersionOf(sound)));
}

SoundFields Rd2Rom::FieldsOf(uint32_t sound) const
{
    // A field of the table of sequences, or of the sound effects' sequences, names a song's sequence. Its header names
    // the tracks' starts (ntdlib::rd2::Cut): a song plays each of them, and a sound effect one.
    SoundFields fields;
    const int song = SongOf(sound);
    if (song < 0 || song >= rd2::SongCount(info))
    {
        return fields;
    }

    const auto sequence = static_cast<uint32_t>(rd2::SequenceOfSong(info, song));
    const auto music = static_cast<uint32_t>(info.sequence_addresses.size());
    const uint32_t entry = sequence < music ? info.sequences + 4 * sequence : info.effects + 4 * (sequence - music);
    fields.sequence = uint64_t{entry} - ntdlib::agb::kRomBase;
    if (const rd2::Effect* effect = rd2::SongEffect(info, song))
    {
        fields.entries.push_back(effect->track);
    }
    else
    {
        const uint32_t address = info.sequence_addresses[sequence];
        const int count = rom.Contains(address, 1) ? std::clamp(int{rom.S8(address)}, 0, rd2::kPlayerTracks) : 0;
        for (int track = 0; track < count; track++)
        {
            fields.entries.push_back(track);
        }
    }

    return fields;
}

int Rd2Rom::Rate() const
{
    return kRd2Rate;
}

std::vector<int> Rd2Rom::Samples() const
{
    std::vector<int> numbers;
    for (std::size_t sample = 0; sample < sample_addresses.size(); ++sample)
    {
        if (ntdlib::sequence_export::rd2::SampleReadable(rom, sample_addresses[sample]))
        {
            numbers.push_back(static_cast<int>(sample));
        }
    }

    return numbers;
}

std::string Rd2BankName(uint16_t bank)
{
    std::array<char, 16> name{};
    std::snprintf(name.data(), name.size(), "Bank %03d", bank);
    return name.data();
}

std::string Rd2Rom::SampleName(int sample) const
{
    return DriverSampleName(sample);
}

void Rd2Rom::CheckSample(int sample) const
{
    if (sample < 0 || static_cast<std::size_t>(sample) >= sample_addresses.size() ||
        !ntdlib::sequence_export::rd2::SampleReadable(rom, sample_addresses[static_cast<std::size_t>(sample)]))
    {
        throw std::invalid_argument("The ROM holds no " + SampleName(sample) + ".");
    }
}

double Rd2Rom::SampleRate(int sample) const
{
    CheckSample(sample);
    return HeaderAt(rom, sample_addresses[static_cast<std::size_t>(sample)]).rate;
}

std::vector<uint8_t> Rd2Rom::SampleWav(int sample) const
{
    const double rate = SampleRate(sample);
    const uint32_t address = sample_addresses[static_cast<std::size_t>(sample)];
    const SampleHeader header = HeaderAt(rom, address);
    const std::optional<WavLoop> loop =
        header.loop_end != 0 ? std::optional(WavLoop{header.loop_start, header.loop_end}) : std::nullopt;
    return Pcm8Wav(rom, address + 16, header.Points(), false, loop, WavRate(rate));
}

SampleShape Rd2Rom::ShapeOf(int sample) const
{
    CheckSample(sample);
    const SampleHeader header = HeaderAt(rom, sample_addresses[static_cast<std::size_t>(sample)]);
    return {header.Points(), header.loop_end != 0 ? std::optional(header.loop_start) : std::nullopt};
}

std::vector<int> Rd2Rom::SamplesOfSound(uint32_t sound) const
{
    // The samples that the sound's notes play. A sample of a bank's sample set that no note plays doesn't count.
    const Rd2Uses& uses = UsesOf(*this);
    const auto found = uses.samples.find(sound);
    if (found != uses.samples.end())
    {
        return found->second;
    }

    std::set<int> played;
    for (const NoteSample& note : NoteSamples(*this, sound))
    {
        played.insert(note.sample);
    }

    return {played.begin(), played.end()};
}

std::vector<uint16_t> Rd2Rom::BankListOf(uint32_t sound) const
{
    const int song = SongOf(sound);
    if (song < 0 || song >= rd2::SongCount(info))
    {
        return {};
    }

    // A track starts on slot 0, and its commands of C7 select the others.
    const int sequence = rd2::SequenceOfSong(info, song);
    int last = 0;
    try
    {
        for (const Command& item : rd2::DecodeRomSequence(rom, info, sequence).sequence.items)
        {
            if (item.kind == Kind::kBankSelect)
            {
                last = std::max(last, static_cast<int>(item.value.value / kOne));
            }
        }
    }
    catch (const std::exception&)
    {
        last = 0;
    }

    const uint32_t list = BankListAddress(info, sequence);
    const uint32_t banks = TableEntries(rom, info.banks);
    std::vector<uint16_t> slots;
    for (int slot = 0; slot <= last && rom.Contains(list + 2 * static_cast<uint32_t>(slot), 2); ++slot)
    {
        const uint16_t bank = rom.U16(list + 2 * static_cast<uint32_t>(slot));
        if (bank >= banks)
        {
            break;
        }

        slots.push_back(bank);
    }

    return slots;
}

std::vector<DriverBankSlot> Rd2Rom::BankSlots(uint32_t sound) const
{
    // An instrument whose offset is 0 is missing (InstrumentCount).
    std::vector<DriverBankSlot> slots;
    for (const uint16_t bank : BankListOf(sound))
    {
        DriverBankSlot slot{Rd2BankName(bank), {}};
        const uint32_t base = rd2::BankAddress(rom, info, bank);
        const uint32_t instruments = InstrumentCount(rom, info, bank);
        for (uint32_t instrument = 0; instrument < instruments; ++instrument)
        {
            if (rom.U16(base + 2 * instrument) != 0 &&
                rd2::LookUpInstrument(rom, info, bank, static_cast<uint16_t>(instrument), 60).found)
            {
                slot.programs.push_back(instrument);
            }
        }

        slots.push_back(std::move(slot));
    }

    return slots;
}

Sequence Rd2Rom::ReadSequence(uint32_t sound) const
{
    return song::ReadSequence(*this, sound);
}

ntdlib::sequence_export::Conversion Rd2Rom::ConvertToMidi(uint32_t sound, const std::string& program) const
{
    ntdlib::sequence_export::agb::ConvertSettings settings;
    settings.program = program;
    settings.version = VersionOf(sound);
    ntdlib::sequence_export::rd2::SongFiles files =
        ntdlib::sequence_export::rd2::ConvertSongFiles(rom, info, SongOf(sound), settings);

    return AgbConversion(SoundName(sound), std::move(files));
}

ntdlib::sequence_export::BankConversion Rd2Rom::ConvertSamples(std::span<const int> samples,
                                                               const std::string& program) const
{
    namespace agb = ntdlib::sequence_export::agb;
    ntdlib::sequence_export::rd2::SoundfontBuilder builder(rom, ntdlib::sequence_export::rd2::PsgShare(info.revision));
    agb::Sf2File& file = builder.File();
    ntdlib::sequence_export::BankConversion conversion;
    for (const int sample : samples)
    {
        const int index = sample >= 0 && static_cast<std::size_t>(sample) < sample_addresses.size()
                              ? builder.GameSample(sample_addresses[static_cast<std::size_t>(sample)])
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

    return SamplesConversion(std::move(conversion), file, samples, program, rom.Title());
}

std::unique_ptr<DriverPlayer> Rd2Rom::MakePlayer() const
{
    return std::make_unique<Rd2Player>(*this);
}

DriverAudition Rd2Rom::SampleAudition(int sample, int key, int velocity) const
{
    CheckSample(sample);
    const std::optional<SamplePlace> place = PlaceOfSample(*this, sample);
    if (!place)
    {
        throw std::invalid_argument("No instrument of the driver's banks plays " + SampleName(sample) + ".");
    }

    // The setup selects the bank by its number among the driver's banks: AuditionRom makes the list of banks of the
    // song contain the bank.
    DriverAudition audition;
    audition.track = 0;
    audition.setup = {ValueCommand(Kind::kBankSelect, place->bank), ValueCommand(Kind::kProgram, place->instrument)};
    audition.note.kind = Kind::kNote;
    // The note of a drum kit or of an instrument with a sample for each key selects the sample. It plays at one pitch.
    audition.note.key =
        static_cast<uint8_t>(place->fixed ? place->note : std::clamp(place->note + key - 60, 0, kHighestKey));
    audition.note.velocity = std::max(Rd2DriverVelocity(velocity), 1) * kOne;
    audition.note.value = ntdlib::commands::Argument{ntdlib::commands::Source::kValue, kLongestLength * kOne, 0};

    return audition;
}

std::optional<DriverAudition> Rd2Rom::NoteAudition(const ntdlib::commands::Sequence&, const Sequence& sequence,
                                                   int track, int program, int key, int velocity) const
{
    // A note's program is its instrument, and its bank slot names a bank of the sound's list of banks.
    const auto found = std::ranges::find(sequence.tracks, track, &Track::number);
    if (found == sequence.tracks.end())
    {
        return std::nullopt;
    }

    const auto note = std::ranges::find_if(
        found->notes, [program](const Note& note) { return note.program == static_cast<uint32_t>(program); });
    const std::vector<uint16_t> banks = BankListOf(sequence.sound);
    if (note == found->notes.end() || note->bank_slot >= banks.size())
    {
        return std::nullopt;
    }

    // The setup selects the bank by its number among the driver's banks: AuditionRom makes the list of banks of the
    // song contain the bank.
    DriverAudition audition;
    audition.track = 0;
    audition.setup = {ValueCommand(Kind::kBankSelect, banks[note->bank_slot]), ValueCommand(Kind::kProgram, program)};
    audition.note.kind = Kind::kNote;
    audition.note.key = static_cast<uint8_t>(std::clamp(key, 0, kHighestKey));
    audition.note.velocity = std::max(Rd2DriverVelocity(velocity), 1) * kOne;
    audition.note.value = ntdlib::commands::Argument{ntdlib::commands::Source::kValue, kLongestLength * kOne, 0};

    return audition;
}

std::shared_ptr<const DriverRom> Rd2Rom::AuditionRom(const DriverAudition& audition) const
{
    if (info.sequence_addresses.empty())
    {
        throw std::invalid_argument("The ROM holds no sequence that a note can take the place of.");
    }

    // A sequence of one track: the setup, the note, a wait as long as the note and the end. Its list of banks contains
    // the bank of the setup's bank select. The bank select then selects slot 0.
    std::vector<uint16_t> banks;
    ntdlib::commands::Sequence sequence;
    Command count;
    count.kind = Kind::kData;
    count.bytes = {1, 0};
    sequence.items.push_back(count);
    Command header;
    header.kind = Kind::kOpenTrack;
    header.index = 0;
    header.target = 2;
    header.encoding.opcode = rd2::kHeaderEntry;
    sequence.items.push_back(header);
    for (Command setup : audition.setup)
    {
        if (setup.kind == Kind::kBankSelect)
        {
            banks.push_back(static_cast<uint16_t>(setup.value.value / kOne));
            setup.value.value = 0;
        }

        sequence.items.push_back(setup);
    }

    sequence.items.push_back(audition.note);
    sequence.items.push_back(ValueCommand(Kind::kWait, kLongestLength));
    sequence.items.push_back(Command{.kind = Kind::kEnd});
    if (banks.empty())
    {
        banks.push_back(0);
    }

    const rd2::Encoded encoded = rd2::EncodeSequence(sequence, info.revision);
    std::vector<uint8_t> bytes = PaddedRomCopy(rom);
    const uint32_t base = AppendWordAligned(bytes, encoded.bytes);
    const uint32_t list = EndAddress(bytes);
    for (const uint16_t bank : banks)
    {
        bytes.push_back(static_cast<uint8_t>(bank));
        bytes.push_back(static_cast<uint8_t>(bank >> 8));
    }

    PadToWord(bytes);

    auto copy = std::make_shared<Rd2Rom>();
    copy->rom.Assign(std::move(bytes));
    copy->info = info;
    copy->info.sequence_addresses[0] = base;
    copy->info.bank_list_addresses[0] = list;
    if (!copy->info.music_settings.empty())
    {
        copy->info.music_settings[0] = {rd2::MusicSettings()};
    }

    copy->sounds = {{0, 0}};
    copy->sample_addresses = sample_addresses;

    return copy;
}

std::shared_ptr<const Rd2Rom> LoadRd2Rom(std::vector<uint8_t> bytes)
{
    auto rom = std::make_shared<Rd2Rom>();
    rom->rom.Assign(std::move(bytes));
    std::string error;
    if (!rd2::DetectDriver(rom->rom, {}, rom->info, error))
    {
        throw xsflib::FormatError(error.empty() ? "the ROM shows no sign of Nintendo R&D2's driver"
                                                : "Hokuto can't read the tables of Nintendo R&D2's driver: " + error);
    }

    // Each song's first version, then the other versions.
    rom->sounds = VersionedSounds(rd2::SongCount(rom->info), [&rom](int song)
                                  { return ntdlib::sequence_export::rd2::SongVersions(rom->info, song).size(); });

    // The samples of each sample set, once each.
    const uint32_t sets = TableEntries(rom->rom, rom->info.sample_sets);
    for (uint32_t set = 0; set < sets; ++set)
    {
        const uint32_t table = rom->info.sample_sets + rom->rom.U32(rom->info.sample_sets + 4 * set);
        const uint32_t count = TableEntries(rom->rom, table);
        for (uint32_t index = 0; index < count; ++index)
        {
            const uint32_t address = table + rom->rom.U32(table + 4 * index);
            if (std::ranges::find(rom->sample_addresses, address) == rom->sample_addresses.end())
            {
                rom->sample_addresses.push_back(address);
            }
        }
    }

    if (rom->Sounds().empty())
    {
        throw xsflib::FormatError("the driver's tables name no song within the ROM");
    }

    return rom;
}

} // namespace hokuto::song
