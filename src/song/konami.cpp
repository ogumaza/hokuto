// SPDX-License-Identifier: MIT

#include "song/konami.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/units.h"
#include "ntdlib/konami/archive_player.h"
#include "ntdlib/konami/commands.h"
#include "ntdlib/konami/sequencer.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/agb/sf2.h"
#include "ntdlib/sequence_export/agb/sfz.h"
#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/sequence_export/konami/convert.h"
#include "ntdlib/sequence_export/konami/soundfont.h"
#include "seqlib/pcm.h"
#include "seqlib/wav.h"
#include "xsflib/binary.h"

#include "song/driver_player.h"
#include "song/gba_bytes.h"
#include "song/midi_export.h"
#include "song/sequence.h"

namespace hokuto::song
{

namespace
{

// The rules of one revision of Konami's driver.
class KonamiRevisionRules final : public DriverRules
{
public:
    explicit KonamiRevisionRules(ntdlib::konami::Revision revision)
        : DriverRules({.engine = format::Engine::kKonami,
                       .revision = static_cast<uint8_t>(revision),
                       .start_volume = 0,
                       // In the revision without a mixer, a track selects its sample.
                       .notes_name_samples = revision != ntdlib::konami::Revision::kDungeonDiceMonsters,
                       .mixes_echo = true}),
          revision_(revision)
    {
    }

    std::vector<uint32_t> ItemOffsets(const ntdlib::commands::Sequence& sequence) const override
    {
        return ntdlib::konami::ItemOffsets(sequence, revision_);
    }

    ntdlib::commands::Scale ScaleOf(ntdlib::commands::Kind kind) const override
    {
        return ntdlib::konami::ScaleOf(kind, revision_);
    }

    int VelocityOf(int volume, bool sample_note) const override
    {
        return KonamiVelocityOf(volume, revision_, sample_note);
    }

    int VolumeOf(int velocity, bool sample_note) const override
    {
        return KonamiVolumeOf(velocity, revision_, sample_note);
    }

    int MostVolume(int track) const override
    {
        return KonamiMostVolume(revision_, track);
    }

    bool PsgTrack(int track) const override
    {
        return ntdlib::konami::IsPsgTrack(track);
    }

    // The command of a note contains the driver volume.
    std::optional<NoteVelocities> Velocities() const override
    {
        const int most = ntdlib::konami::UltimateMasters(revision_) ? 127 : 15;
        return NoteVelocities{most, "Note volume must be between 0 and " + std::to_string(most) + "."};
    }

private:
    ntdlib::konami::Revision revision_;
};

// The model of Konami's driver and the GBA's sound hardware, for one ROM (ntdlib::konami::ArchivePlayer).
class KonamiPlayer final : public ArchiveDriverPlayer<ntdlib::konami::ArchivePlayer>
{
public:
    explicit KonamiPlayer(const KonamiRom& rom) : ArchiveDriverPlayer(rom.rom, rom.info), rom_(rom)
    {
    }

    void Start(uint32_t sound, bool enhanced) override
    {
        ntdlib::konami::ArchivePlayerOptions options;
        options.enhanced = enhanced;
        options.live = KonamiSoundIsLive(rom_, sound);
        options.echo = echo_;
        player_.Start(KonamiSongOf(rom_, sound), options);
    }

    void SetEcho(bool on) override
    {
        echo_ = on;
    }

    int Tracks() const override
    {
        return ntdlib::konami::kTracks;
    }

    uint32_t Tick() const override
    {
        return static_cast<uint32_t>(player_.Frames());
    }

    bool Finished() const override
    {
        const ntdlib::konami::Sequencer* song = player_.Driver().Song();
        return song == nullptr || song->Stopped() || !song->AnyTrackActive();
    }

private:
    const KonamiRom& rom_;
    bool echo_ = true;
};

// What the output stage of ntdlib::konami::Output reads of the records of the sample tracks, frame by frame: the
// samples whose headers the note starts read, and the samples whose headers give a voice's step at a pitch change while
// the voice plays another sample.
class StageReads
{
public:
    explicit StageReads(ntdlib::konami::Revision revision)
        : ultimate_(ntdlib::konami::UltimateMasters(revision)),
          ring_(ntdlib::konami::EternalDuelistLayout(revision)),
          any_flag_(revision == ntdlib::konami::Revision::kEternalDuelist),
          playing_(static_cast<std::size_t>(ntdlib::konami::TrackCount(revision) - ntdlib::konami::kPsgTracks), -1)
    {
    }

    // Follows the stage through a frame's records. Adds the samples whose notes start in the frame to `started`, and
    // those whose rates the frame borrows to `borrowed`.
    void Frame(std::array<ntdlib::konami::TrackOutput, ntdlib::konami::kTracks> records, std::set<int>& started,
               std::set<int>& borrowed)
    {
        namespace konami = ntdlib::konami;

        // The stage of the older revisions plays the noise track's left side on the next record's channel while its
        // pan is off the centre. The first voice then reads that record.
        constexpr int kNoise = konami::kPsgTracks - 1;
        const uint8_t pan = records[kNoise].pan;
        if (!ultimate_ && !ring_ && pan != 0xFF && pan != 0x0F && pan != 0xF0)
        {
            records[konami::kPsgTracks] = records[kNoise];
        }

        for (std::size_t voice = 0; voice < playing_.size(); voice++)
        {
            const konami::TrackOutput& record = records[konami::kPsgTracks + voice];
            if (record.flags == 0)
            {
                continue;
            }

            // The Eternal Duelist revision's stage checks for a note's start before a stop, and the others check for a
            // stop first. In that revision, every other flag changes the pitch.
            const bool starts = (record.flags & konami::kOutNoteOn) != 0;
            if ((record.flags & konami::kOutStop) && !(any_flag_ && starts))
            {
                playing_[voice] = -1;
                continue;
            }

            if (starts)
            {
                playing_[voice] = SampleOf(record.key);
                started.insert(playing_[voice]);
            }
            // A voice that doesn't play gets its step from the start of its next note.
            const bool pitch = any_flag_ ? !starts : (record.flags & konami::kOutPitch) != 0;
            if (pitch && playing_[voice] >= 0 && SampleOf(record.key) != playing_[voice])
            {
                borrowed.insert(SampleOf(record.key));
            }
        }
    }

private:
    // A record's key as a sample number (KonamiSampleName). Bit 15 picks the second table, and the low bits the entry.
    int SampleOf(uint16_t key) const
    {
        if (key & 0x8000)
        {
            return 0x8000 | (key & (ring_ ? 0x3FFF : 0xFFF));
        }

        return ring_ ? int{key} : key & 0xFFF;
    }

    bool ultimate_ = false;
    bool ring_ = false;
    bool any_flag_ = false;
    std::vector<int> playing_; // the sample of each voice's note, or -1 before a note and after a stop
};

// The samples whose headers the output stage reads for a sound: at its note starts, and for the rates that it borrows.
struct HeaderReads
{
    std::set<int> started;
    std::set<int> borrowed;
};

// The reads of sound `sound`, over two passes of the loop of the song on ntdlib's sequencer (StageReads::Frame).
// Nothing for the revision without a mixer.
HeaderReads ReadsOf(const KonamiRom& rom, uint32_t sound)
{
    namespace konami = ntdlib::konami;
    HeaderReads reads;
    if (rom.info.revision == konami::Revision::kDungeonDiceMonsters)
    {
        return reads;
    }

    const std::unique_ptr<konami::Sequencer> sequencer =
        konami::Sequencer::Create(rom.rom, rom.info, KonamiSongOf(rom, sound), KonamiRequestOf(rom, sound));
    if (!sequencer->Valid())
    {
        return reads;
    }

    StageReads stage(rom.info.revision);
    for (uint32_t frame = 0; frame < konami::kMaxFrames; frame++)
    {
        const std::array<konami::TrackOutput, konami::kTracks>& records = sequencer->Step();
        if (sequencer->LoopsDone() >= 2 || sequencer->Stopped() || !sequencer->AnyTrackActive())
        {
            break;
        }

        stage.Frame(records, reads.started, reads.borrowed);
    }

    return reads;
}

} // namespace

const DriverRules& KonamiRules(ntdlib::konami::Revision revision)
{
    static const std::array<KonamiRevisionRules, 11> rules = {
        KonamiRevisionRules(ntdlib::konami::Revision::kUltimateMasters),
        KonamiRevisionRules(ntdlib::konami::Revision{1}),
        KonamiRevisionRules(ntdlib::konami::Revision{2}),
        KonamiRevisionRules(ntdlib::konami::Revision{3}),
        KonamiRevisionRules(ntdlib::konami::Revision{4}),
        KonamiRevisionRules(ntdlib::konami::Revision{5}),
        KonamiRevisionRules(ntdlib::konami::Revision{6}),
        KonamiRevisionRules(ntdlib::konami::Revision{7}),
        KonamiRevisionRules(ntdlib::konami::Revision{8}),
        KonamiRevisionRules(ntdlib::konami::Revision{9}),
        KonamiRevisionRules(ntdlib::konami::Revision::kDungeonDiceMonsters),
    };
    const auto index = static_cast<std::size_t>(revision);
    if (index >= rules.size())
    {
        throw std::invalid_argument("Konami's driver has no revision " + std::to_string(index));
    }

    return rules[index];
}

std::optional<ntdlib::konami::Revision> KonamiRevisionOf(const DriverRules* rules)
{
    if (rules == nullptr || rules->DriverEngine() != format::Engine::kKonami)
    {
        return std::nullopt;
    }

    return static_cast<ntdlib::konami::Revision>(rules->Revision());
}

const KonamiRom* KonamiOf(const DriverRom* rom)
{
    return dynamic_cast<const KonamiRom*>(rom);
}

const DriverRules& KonamiRom::Rules() const
{
    return KonamiRules(info.revision);
}

std::span<const uint8_t> KonamiRom::Bytes() const
{
    return {rom.Ptr(ntdlib::agb::kRomBase), rom.Size()};
}

std::vector<uint32_t> KonamiRom::Sounds() const
{
    return KonamiSounds(*this);
}

uint32_t KonamiRom::SoundCount() const
{
    return static_cast<uint32_t>(2 * std::max(info.song_count, 0));
}

std::string KonamiRom::SoundName(uint32_t sound) const
{
    return KonamiSoundName(*this, sound);
}

SoundFields KonamiRom::FieldsOf(uint32_t sound) const
{
    // The entry of a song in the song table contains a field of 32 bits that names the bytes of the song, and a field
    // of 16 bits for the start of each track (ntdlib::konami::Cut).
    SoundFields fields;
    const int song = KonamiSongOf(*this, sound);
    if (song < 0 || song >= info.song_count)
    {
        return fields;
    }

    const uint64_t entry = uint64_t{info.song_table} - ntdlib::agb::kRomBase +
                           ntdlib::konami::SongEntrySize(info.revision) * static_cast<uint64_t>(song);
    fields.sequence = entry;
    for (int track = 0; track < ntdlib::konami::TrackCount(info.revision); track++)
    {
        fields.tracks.push_back(entry + 4 + 2 * static_cast<uint64_t>(track));
    }

    return fields;
}

int KonamiRom::Rate() const
{
    return KonamiRate(*this);
}

std::vector<int> KonamiRom::Samples() const
{
    return KonamiSamples(*this);
}

std::string KonamiRom::SampleName(int sample) const
{
    return KonamiSampleName(sample);
}

double KonamiRom::SampleRate(int sample) const
{
    return KonamiSampleRate(*this, sample);
}

std::vector<uint8_t> KonamiRom::SampleWav(int sample) const
{
    return KonamiSampleWav(*this, sample);
}

SampleShape KonamiRom::ShapeOf(int sample) const
{
    const ntdlib::konami::SampleInfo shape = info.Sample(rom, sample);
    return {static_cast<uint32_t>(shape.length),
            shape.Looped() ? std::optional(static_cast<uint32_t>(shape.loop_start)) : std::nullopt};
}

std::vector<int> KonamiRom::SamplesOfSound(uint32_t sound) const
{
    using ntdlib::commands::Kind;

    // A live stage's table of samples can replace the samples of the notes, and a song that the decoder keeps as data
    // contains no notes. The note starts of the output stage give their samples.
    const ntdlib::konami::Decoded decoded = ntdlib::konami::DecodeSong(rom, info, KonamiSongOf(*this, sound));
    if (KonamiSoundIsLive(*this, sound) || decoded.opaque)
    {
        const std::set<int> started = ReadsOf(*this, sound).started;
        return {started.begin(), started.end()};
    }

    // A track of the revision without a mixer can select its sample too.
    const bool dungeon = info.revision == ntdlib::konami::Revision::kDungeonDiceMonsters;
    std::set<int> played;
    for (const ntdlib::commands::Command& command : decoded.sequence.items)
    {
        if (command.kind == Kind::kNote && command.sample)
        {
            played.insert(*command.sample);
        }
        else if (dungeon && command.kind == Kind::kProgram && command.index == 0)
        {
            played.insert(static_cast<int>(command.value.value / ntdlib::commands::kOne));
        }
    }

    return {played.begin(), played.end()};
}

std::vector<uint32_t> KonamiRom::TrackPrograms(int track) const
{
    // The noise channel selects neither a duty nor a wave.
    constexpr int kNoiseTrack = ntdlib::konami::kPsgTracks - 1;
    return track == kNoiseTrack ? std::vector<uint32_t>() : KonamiTrackPrograms(*this, track);
}

Sequence KonamiRom::ReadSequence(uint32_t sound) const
{
    return song::ReadSequence(*this, sound);
}

ntdlib::sequence_export::Conversion KonamiRom::ConvertToMidi(uint32_t sound, const std::string& program) const
{
    ntdlib::sequence_export::agb::ConvertSettings settings;
    settings.program = program;
    settings.version = KonamiSoundIsLive(*this, sound) ? 1 : 0;
    ntdlib::sequence_export::konami::SongFiles files =
        ntdlib::sequence_export::konami::ConvertSongFiles(rom, info, KonamiSongOf(*this, sound), settings);

    return AgbConversion(KonamiSoundName(*this, sound), std::move(files));
}

ntdlib::sequence_export::BankConversion KonamiRom::ConvertSamples(std::span<const int> samples,
                                                                  const std::string& program) const
{
    namespace agb = ntdlib::sequence_export::agb;
    ntdlib::sequence_export::konami::SoundfontBuilder builder(rom, info);
    agb::Sf2File& file = builder.File();
    ntdlib::sequence_export::BankConversion conversion;
    for (const int sample : samples)
    {
        const int index = builder.DsSample(sample);
        const ntdlib::konami::SampleInfo sample_info = info.Sample(rom, sample);
        if (index < 0 || !sample_info.valid)
        {
            conversion.warnings.push_back(KonamiSampleName(sample) + " is empty.");
            continue;
        }

        const std::string name = KonamiSampleName(sample);
        file.instruments.push_back(agb::MakeInstrument(
            name, {agb::RootKeyZone(index, sample_info.Looped(), 0, 127, builder.DsRootKey(sample))}));
        file.presets.push_back(agb::MakePreset(name, static_cast<uint16_t>(sample / 128),
                                               static_cast<uint16_t>(sample % 128),
                                               static_cast<int>(file.instruments.size()) - 1));
    }

    // Each sample's SFZ file plays the sample by itself, like its preset: "Samples_0012.sfz" for sample 12.
    conversion = SamplesConversion(std::move(conversion), file, samples, program, rom.Title());
    conversion.sfz = agb::PresetSfz(file, "Samples", program);

    return conversion;
}

bool KonamiRom::EditsSamples() const
{
    return true;
}

bool KonamiRom::WritesSfz() const
{
    return true;
}

std::unique_ptr<DriverPlayer> KonamiRom::MakePlayer() const
{
    return std::make_unique<KonamiPlayer>(*this);
}

DriverAudition KonamiRom::SampleAudition(int sample, int key, int velocity) const
{
    return KonamiSampleAudition(*this, sample, key, velocity);
}

std::optional<DriverAudition> KonamiRom::NoteAudition(const ntdlib::commands::Sequence& commands,
                                                      const Sequence& sequence, int track, int program, int key,
                                                      int velocity) const
{
    return KonamiNoteAudition(*this, commands, sequence, track, program, key, velocity);
}

std::shared_ptr<const DriverRom> KonamiRom::AuditionRom(const DriverAudition& audition) const
{
    return KonamiAuditionRom(*this, audition);
}

std::shared_ptr<const KonamiRom> LoadKonamiRom(std::vector<uint8_t> bytes)
{
    auto rom = std::make_shared<KonamiRom>();
    rom->rom.Assign(std::move(bytes));
    std::string error;
    if (!ntdlib::konami::DetectDriver(rom->rom, {}, rom->info, error))
    {
        throw xsflib::FormatError(error.empty()
                                      ? "the ROM doesn't use Konami's driver"
                                      : "the song table of Konami's driver has no song that Hokuto can read: " + error);
    }
    if (KonamiSounds(*rom).empty())
    {
        throw xsflib::FormatError("the song table points outside the ROM");
    }

    return rom;
}

std::vector<uint32_t> KonamiSounds(const KonamiRom& rom)
{
    std::vector<uint32_t> sounds;
    for (int song = 0; song < rom.info.song_count; song++)
    {
        ntdlib::konami::SongHeader header;
        if (!ntdlib::konami::ReadSongHeader(rom.rom, rom.info.song_table, song, rom.info.revision, header) ||
            !rom.rom.Contains(header.base))
        {
            continue;
        }

        sounds.push_back(static_cast<uint32_t>(song));
        if (std::ranges::find(rom.info.live_songs, song) != rom.info.live_songs.end())
        {
            sounds.push_back(static_cast<uint32_t>(rom.info.song_count + song));
        }
    }

    return sounds;
}

int KonamiSongOf(const KonamiRom& rom, uint32_t sound)
{
    const auto song = static_cast<int>(sound);
    return song >= rom.info.song_count ? song - rom.info.song_count : song;
}

bool KonamiSoundIsLive(const KonamiRom& rom, uint32_t sound)
{
    return static_cast<int>(sound) >= rom.info.song_count;
}

ntdlib::konami::Request KonamiRequestOf(const KonamiRom& rom, uint32_t sound)
{
    ntdlib::konami::Request request;
    if (!KonamiSoundIsLive(rom, sound))
    {
        return request;
    }

    request.setting = ntdlib::konami::kLiveStageSetting;
    if (const auto table = rom.info.live_tables.find(KonamiSongOf(rom, sound)); table != rom.info.live_tables.end())
    {
        request.samples = table->second;
    }

    return request;
}

std::string KonamiSoundName(const KonamiRom& rom, uint32_t sound)
{
    return "Song " + ntdlib::sequence_export::agb::SongNumber(KonamiSongOf(rom, sound), rom.info.song_count) +
           (KonamiSoundIsLive(rom, sound) ? " (live)" : "");
}

int KonamiRate(const KonamiRom& rom)
{
    return rom.info.revision == ntdlib::konami::Revision::kDungeonDiceMonsters ? 32768 : 65536;
}

int KonamiVelocityOf(int volume, ntdlib::konami::Revision revision, bool sample_note)
{
    if (sample_note && ntdlib::konami::UltimateMasters(revision))
    {
        return std::clamp(volume, 0, 127);
    }

    return static_cast<int>(std::lround(std::clamp(volume, 0, 15) * 127.0 / 15.0));
}

bool KonamiNoteStartsOn(const ntdlib::commands::Command& command, int holder, int number,
                        ntdlib::konami::Revision revision)
{
    const bool on_next = command.sample && command.index == 1;
    return holder == number ? !on_next : on_next || revision != ntdlib::konami::Revision::kDungeonDiceMonsters;
}

std::optional<uint32_t> KonamiNoteCommandIn(std::span<const ntdlib::commands::Command> items, uint32_t first,
                                            uint32_t end, int holder, int number, ntdlib::konami::Revision revision)
{
    for (uint32_t i = std::min<uint32_t>(end, static_cast<uint32_t>(items.size())); i > first; --i)
    {
        const ntdlib::commands::Command& command = items[i - 1];
        if (command.kind == ntdlib::commands::Kind::kNote && KonamiNoteStartsOn(command, holder, number, revision))
        {
            return i - 1;
        }
    }

    return std::nullopt;
}

std::optional<std::pair<std::size_t, uint32_t>> KonamiNoteCommandBefore(
    std::span<const ntdlib::commands::Command> items, std::span<const KonamiRun> runs, std::size_t run, uint32_t item,
    int holder, int number, ntdlib::konami::Revision revision)
{
    for (std::size_t r = std::min(run + 1, runs.size()); r > 0; --r)
    {
        const auto& [first, end] = runs[r - 1];
        const uint32_t stop = r - 1 == run ? std::min(item, end) : end;
        if (const std::optional<uint32_t> found = KonamiNoteCommandIn(items, first, stop, holder, number, revision))
        {
            return std::pair(r - 1, *found);
        }
    }

    return std::nullopt;
}

std::vector<KonamiRunStart> KonamiVolumeStartsAlong(std::span<const ntdlib::commands::Command> items,
                                                    std::span<const KonamiRun> runs, ntdlib::konami::Revision revision)
{
    std::vector<KonamiRunStart> starts;
    std::array<std::optional<uint32_t>, 2> last;
    for (const auto& [first, end] : runs)
    {
        for (uint32_t i = first; i < end && i < items.size(); ++i)
        {
            const ntdlib::commands::Command& command = items[i];
            if (command.kind == ntdlib::commands::Kind::kVolume && command.index == 0)
            {
                starts.push_back({i, last});
            }
            else if (command.kind == ntdlib::commands::Kind::kNote)
            {
                for (const int number : {0, 1})
                {
                    last[static_cast<std::size_t>(number)] = KonamiNoteStartsOn(command, 0, number, revision)
                                                                 ? std::optional(i)
                                                                 : last[static_cast<std::size_t>(number)];
                }
            }
        }
    }

    return starts;
}

int KonamiVolumeOf(int velocity, ntdlib::konami::Revision revision, bool sample_note)
{
    if (sample_note && ntdlib::konami::UltimateMasters(revision))
    {
        return std::clamp(velocity, 0, 127);
    }
    if (velocity <= 0)
    {
        return 0;
    }

    return std::clamp(static_cast<int>(std::lround(velocity * 15.0 / 127.0)), 1, 15);
}

int KonamiMostVolume(ntdlib::konami::Revision revision, int track)
{
    return ntdlib::konami::UltimateMasters(revision) && !ntdlib::konami::IsPsgTrack(track) ? 127 : 15;
}

std::vector<int> KonamiSamples(const KonamiRom& rom)
{
    using ntdlib::commands::Kind;
    const bool dungeon = rom.info.revision == ntdlib::konami::Revision::kDungeonDiceMonsters;
    std::set<int> samples;
    for (int song = 0; song < rom.info.song_count; song++)
    {
        const ntdlib::konami::Decoded decoded = ntdlib::konami::DecodeSong(rom.rom, rom.info, song);
        if (decoded.opaque)
        {
            continue;
        }

        for (const ntdlib::commands::Command& command : decoded.sequence.items)
        {
            if (command.kind == Kind::kNote && command.sample)
            {
                samples.insert(*command.sample);
            }
            else if (dungeon && command.kind == Kind::kProgram && command.index == 0)
            {
                samples.insert(static_cast<int>(command.value.value / ntdlib::commands::kOne));
            }
        }
    }

    std::vector<int> valid;
    for (const int sample : samples)
    {
        if (const ntdlib::konami::SampleInfo info = rom.info.Sample(rom.rom, sample); info.valid && info.length > 0)
        {
            valid.push_back(sample);
        }
    }

    return valid;
}

std::vector<int> KonamiBorrowedRates(const KonamiRom& rom, uint32_t sound)
{
    const std::set<int> borrowed = ReadsOf(rom, sound).borrowed;
    return {borrowed.begin(), borrowed.end()};
}

std::vector<uint32_t> KonamiTrackPrograms(const KonamiRom& rom, int track)
{
    using ntdlib::commands::Kind;
    std::set<uint32_t> programs;
    for (int song = 0; song < rom.info.song_count; song++)
    {
        const ntdlib::konami::Decoded decoded = ntdlib::konami::DecodeSong(rom.rom, rom.info, song);
        if (decoded.opaque || static_cast<std::size_t>(track) >= decoded.entries.size())
        {
            continue;
        }

        // A track's commands run up to the next track's start.
        const uint32_t first = decoded.entries[static_cast<std::size_t>(track)];
        uint32_t end = static_cast<uint32_t>(decoded.sequence.items.size());
        for (const uint32_t entry : decoded.entries)
        {
            end = entry > first ? std::min(end, entry) : end;
        }

        for (uint32_t item = first; item < end; item++)
        {
            const ntdlib::commands::Command& command = decoded.sequence.items[item];
            if (command.kind == Kind::kProgram && command.index < 2)
            {
                programs.insert(static_cast<uint32_t>(command.value.value / ntdlib::commands::kOne));
            }
        }
    }

    return {programs.begin(), programs.end()};
}

std::string KonamiSampleName(int sample)
{
    return DriverSampleName(sample);
}

std::vector<uint8_t> KonamiSampleWav(const KonamiRom& rom, int sample)
{
    const ntdlib::konami::SampleInfo info = rom.info.Sample(rom.rom, sample);
    if (!info.valid || info.length <= 0)
    {
        throw std::invalid_argument(KonamiSampleName(sample) + " is empty.");
    }

    const std::vector<int8_t> pcm = ntdlib::konami::SamplePcm(rom.rom, info);
    seqlib::Pcm out;
    out.channels.emplace_back(pcm.size());
    std::ranges::transform(pcm, out.channels[0].begin(),
                           [](int8_t value) { return static_cast<int16_t>(value * 256); });
    if (info.Looped())
    {
        out.loop = true;
        out.loop_start = static_cast<uint32_t>(info.loop_start);
        out.loop_end = static_cast<uint32_t>(pcm.size());
    }

    return seqlib::WavFile(out, WavRate(KonamiSampleRate(rom, sample)));
}

double KonamiSampleRate(const KonamiRom& rom, int sample)
{
    const ntdlib::konami::SampleInfo info = rom.info.Sample(rom.rom, sample);
    if (!info.valid)
    {
        throw std::invalid_argument("The ROM has no " + KonamiSampleName(sample) + ".");
    }

    return info.Rate(rom.info.mix_rate);
}

DriverAudition KonamiSampleAudition(const KonamiRom& rom, int sample, int key, int velocity)
{
    using namespace ntdlib::commands;
    DriverAudition audition;
    audition.track = ntdlib::konami::kPsgTracks;
    audition.note.kind = Kind::kNote;
    audition.note.sample = static_cast<uint16_t>(sample);
    audition.note.velocity = std::max(KonamiVolumeOf(velocity, rom.info.revision, true), 1) * kOne;

    // A note of the revision without a mixer has no semitones.
    if (rom.info.revision != ntdlib::konami::Revision::kDungeonDiceMonsters)
    {
        audition.note.key = static_cast<uint8_t>(static_cast<int8_t>(std::clamp(key - 60, -128, 127)));
    }

    return audition;
}

std::optional<DriverAudition> KonamiNoteAudition(const KonamiRom& rom, const ntdlib::commands::Sequence& commands,
                                                 const Sequence& sequence, int track, int program, int key,
                                                 int velocity)
{
    using namespace ntdlib::commands;
    const auto found = std::ranges::find(sequence.tracks, track, &Track::number);
    if (found == sequence.tracks.end())
    {
        return std::nullopt;
    }

    // A note's command is the offset of its item.
    const std::vector<uint32_t> offsets = ntdlib::konami::ItemOffsets(commands, rom.info.revision);
    const auto item_at = [&](uint32_t offset) -> std::optional<uint32_t>
    {
        const auto place = std::ranges::lower_bound(offsets, offset);
        return place != offsets.end() && *place == offset && place + 1 != offsets.end()
                   ? std::optional(static_cast<uint32_t>(place - offsets.begin()))
                   : std::nullopt;
    };
    const Note* nearest = nullptr;
    uint32_t nearest_item = 0;
    for (const Note& note : found->notes)
    {
        if (note.program != static_cast<uint32_t>(program) || note.command == kNoCommand ||
            (nearest != nullptr && std::abs(note.key - key) >= std::abs(nearest->key - key)))
        {
            continue;
        }

        // A note that a volume command starts takes its key from the note command before it (ReadSequence). The
        // search stays in the stretch that ran the volume command, among the commands that the note's track ran.
        std::optional<uint32_t> item = item_at(note.command);
        if (item && commands.items[*item].kind == Kind::kVolume)
        {
            const auto holder = note.command_track >= 0
                                    ? std::ranges::find(sequence.tracks, int{note.command_track}, &Track::number)
                                    : found;
            const std::optional<uint32_t> first =
                holder != sequence.tracks.end() && note.stretch < holder->stretches.size()
                    ? item_at(holder->stretches[note.stretch].first)
                    : std::nullopt;
            while (first && *item > *first && commands.items[*item].kind != Kind::kNote)
            {
                --*item;
            }
        }

        if (item && commands.items[*item].kind == Kind::kNote)
        {
            nearest = &note;
            nearest_item = *item;
        }
    }
    if (nearest == nullptr)
    {
        return std::nullopt;
    }

    // The driver's next note can play on the next track. The preview plays it where the command runs.
    DriverAudition audition;
    audition.track = nearest->command_track >= 0 ? nearest->command_track : track;
    audition.note = commands.items[nearest_item];
    audition.note.bytes.clear();
    audition.note.encoding = {};
    audition.note.index = 0;

    // A DDM sample note has no semitone. Its rate follows the sample.
    const bool dungeon = rom.info.revision == ntdlib::konami::Revision::kDungeonDiceMonsters;
    if (!dungeon || !audition.note.sample)
    {
        const int moved = audition.note.sample ? static_cast<int8_t>(audition.note.key) + key - nearest->key
                                               : audition.note.key + key - nearest->key;
        audition.note.key = audition.note.sample ? static_cast<uint8_t>(std::clamp(moved, -128, 127))
                                                 : static_cast<uint8_t>(std::clamp(moved, 0, 255));
    }

    audition.note.velocity =
        std::max(KonamiVolumeOf(velocity, rom.info.revision, audition.note.sample.has_value()), 1) *
        ntdlib::commands::kOne;

    // The track's nearest program before the note selects its duty, its wave or its sample.
    for (uint32_t item = nearest_item; item > 0; item--)
    {
        const Command& before = commands.items[item - 1];
        if (before.kind == Kind::kEnd || before.kind == Kind::kLoopEnd || before.kind == Kind::kData)
        {
            break;
        }

        if (before.kind == Kind::kProgram && before.index < 2)
        {
            Command setup = before;
            setup.bytes.clear();
            setup.encoding = {};
            audition.setup.push_back(setup);
            break;
        }
    }

    return audition;
}

std::shared_ptr<const KonamiRom> KonamiAuditionRom(const KonamiRom& rom, const DriverAudition& audition)
{
    using namespace ntdlib::commands;
    const ntdlib::konami::Revision revision = rom.info.revision;
    const int tracks = ntdlib::konami::TrackCount(revision);
    const auto wait = [](int64_t ticks)
    {
        return ValueCommand(Kind::kWait, ticks);
    };

    // Each other track waits, then ends. The audition's track holds the note for over a minute. Every revision's delays
    // reach this far. A track that ends leaves the song playing while another track runs.
    constexpr int64_t kLongDelay = 0xFF0;
    ntdlib::commands::Sequence sequence;
    std::vector<uint32_t> entries;
    for (int track = 0; track < tracks; track++)
    {
        entries.push_back(static_cast<uint32_t>(sequence.items.size()));
        if (track != audition.track)
        {
            sequence.items.push_back(wait(1));
            sequence.items.push_back(Command{.kind = Kind::kEnd});
            continue;
        }

        sequence.items.push_back(wait(0));
        for (const Command& setup : audition.setup)
        {
            sequence.items.push_back(setup);
            sequence.items.push_back(wait(0));
        }

        sequence.items.push_back(audition.note);
        sequence.items.push_back(wait(kLongDelay));
        sequence.items.push_back(Command{.kind = Kind::kEnd});
    }

    const ntdlib::konami::Encoded encoded = ntdlib::konami::EncodeSequence(sequence, entries, revision);

    std::vector<uint8_t> bytes = PaddedRomCopy(rom.rom);
    const uint32_t base = AppendWordAligned(bytes, encoded.bytes);
    const uint32_t table = EndAddress(bytes);
    for (int shift = 0; shift < 32; shift += 8)
    {
        bytes.push_back(static_cast<uint8_t>(base >> shift));
    }

    for (const uint32_t entry : entries)
    {
        const uint32_t offset = encoded.offsets[entry];
        bytes.push_back(static_cast<uint8_t>(offset));
        bytes.push_back(static_cast<uint8_t>(offset >> 8));
    }

    PadToWord(bytes);

    auto copy = std::make_shared<KonamiRom>();
    copy->rom.Assign(std::move(bytes));
    copy->info = rom.info;
    copy->info.song_table = table;
    copy->info.song_count = 1;
    copy->info.live_songs.clear();
    copy->info.live_tables.clear();

    return copy;
}

} // namespace hokuto::song
