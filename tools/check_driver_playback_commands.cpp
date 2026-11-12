// SPDX-License-Identifier: MIT

// hokuto_check_driver_playback: the counts of the note commands that the tracks run, and of those without a note of
// the arrangement.

#include "check_driver_playback.h"

namespace hokuto::tools::driver_playback
{

namespace
{

// The tracks that sound `sound` mutes, a bit for each: on Nintendo R&D2's driver, those that the game's code mutes in
// the sound's version of its song.
uint32_t MutedTracks(const song::DriverRom& rom, uint32_t sound)
{
    if (const song::Rd2Rom* rd2 = song::Rd2Of(&rom))
    {
        return ntdlib::rd2::VersionSettings(rd2->info, rd2->SongOf(sound), rd2->VersionOf(sound)).muted_tracks;
    }

    return 0;
}

// The channels that the game silences in sound `sound`, a version of a tune of Rare's driver, by bits: some channels of
// the arrangement under water.
uint16_t SilencedChannels(const song::DriverRom& rom, uint32_t sound)
{
    if (const song::RareRom* rare = song::RareOf(&rom))
    {
        return ntdlib::rare::SilencedChannels(rare->info, rare->SongOf(sound), rare->VersionOf(sound) == 1);
    }

    return 0;
}

// Krawall's effects that change what a row's note does (Player::PlayEntry in ntdlib): the slides to a note, and the
// note delay.
constexpr int kKrawallPortaNote = 19;
constexpr int kKrawallVolumeSlidePorta = 24;
constexpr int kKrawallNoteDelay = 45;

// What ntdlib's Player of Krawall does with the entry of a channel in a pass of a row.
struct KrawallEntry
{
    bool started = false; // whether a voice starts on the channel in the row
    uint32_t sample = 0;  // the channel's sample at the row's first tick, or 0
    int speed = 0;        // the row's ticks
    int level = 0;        // the highest level on either side of the voice that the row starts, over the voice's life
};

// The entries of the rows of sound `sound` as ntdlib's Player of Krawall plays them, by the address of the row, its
// pass from 0 and the channel. A voice starts where the mixer channel of the channel starts to sound, gets another
// handle or moves its position. The converter starts a note there. The player plays `rows` rows and a few more.
using KrawallEntries = std::map<std::tuple<uint32_t, int, int>, KrawallEntry>;
KrawallEntries PlayedEntries(const song::KrawallRom& krawall, uint32_t sound, std::size_t rows)
{
    KrawallEntries entries;
    ntdlib::krawall::Player player(krawall.ModulesRom(), krawall.info, krawall.info.songs[sound]);
    if (!player.Valid())
    {
        return entries;
    }

    const auto channels = static_cast<std::size_t>(player.Module().channels);
    std::map<uint32_t, int> passes; // the passes of each row so far
    uint32_t row = 0;
    int pass = 0;
    std::size_t played = 0;
    std::vector<bool> sounding(channels);
    std::vector<uint32_t> handles(channels);
    std::vector<uint32_t> moves(channels);
    std::vector<KrawallEntry*> voices(channels); // the entry that started each channel's voice, while it sounds
    player.OnTick(
        [&](uint64_t)
        {
            const bool first = player.TickInRow() == 0;
            if (first)
            {
                row = player.RowData();
                pass = passes[row]++;
                played++;
            }
            for (std::size_t c = 0; c < channels; c++)
            {
                const ntdlib::krawall::Channel& channel = player.Channels()[c];
                const bool now = player.Sounding(channel);
                const uint32_t moved = now ? player.MixerOf(channel).moves : 0;
                KrawallEntry& entry = entries[{row, pass, static_cast<int>(c)}];
                if (first)
                {
                    entry.sample = channel.sample;
                    entry.speed = player.Speed();
                }
                if (now && (!sounding[c] || channel.handle != handles[c] || moved != moves[c]))
                {
                    entry.started = true;
                    voices[c] = &entry;
                }
                if (!now)
                {
                    voices[c] = nullptr;
                }
                else if (voices[c] != nullptr)
                {
                    const ntdlib::krawall::MixChannel& mixer = player.MixerOf(channel);
                    voices[c]->level = std::max({voices[c]->level, int{mixer.left}, int{mixer.right}});
                }
                sounding[c] = now;
                handles[c] = now ? channel.handle : 0;
                moves[c] = moved;
            }
        });
    while (player.Playing() && played <= rows + 4)
    {
        player.Frame();
    }

    return entries;
}

// The state of CountCommands on Krawall: what ntdlib's Player does with the entries, and the commands without a note
// whose voice reaches a level.
struct KrawallCounts
{
    KrawallEntries entries;
    // On Krawall, the address where the sequence starts. The offsets of stretches count from it.
    uint32_t rows_start = 0;
    std::size_t heard = 0; // Krawall's commands without a note whose voice reaches a level
    std::optional<std::pair<int, uint32_t>> first_heard; // the track and the tick of the first
};

// Counts Krawall's note command `i` of `commands`, in stretch `s` of `track` on the row's pass `pass`, when the
// arrangement shows no note of it: by what ntdlib's Player does with its entry, in `krawall`, `lost` and `totals`.
void CountKrawallCommand(const ntdlib::commands::Sequence& commands, std::span<const uint32_t> offsets, uint32_t i,
                         const song::Track& track, std::size_t s, int pass, KrawallCounts& krawall,
                         std::map<int, std::size_t>& lost, Totals& totals)
{
    using ntdlib::commands::Kind;
    const ntdlib::commands::Command& command = commands.items[i];
    const song::Stretch& stretch = track.stretches[s];
    const uint32_t offset = offsets[i];
    const bool named = std::ranges::any_of(
        track.notes, [offset, s](const song::Note& note) { return note.command == offset && note.stretch == s; });
    if (named)
    {
        return;
    }

    // The entry's effect follows its note and its volume.
    int effect = 0;
    int operand = 0;
    for (uint32_t j = i + 1; j < commands.items.size() && commands.items[j].index == command.index; j++)
    {
        const ntdlib::commands::Command& next = commands.items[j];
        if (next.kind == Kind::kVolume)
        {
            continue;
        }
        if (next.kind == Kind::kOther && next.encoding.opcode >= ntdlib::krawall::kEffectOpcode &&
            next.encoding.opcode < ntdlib::krawall::kPatternOpcode)
        {
            effect = next.encoding.opcode - ntdlib::krawall::kEffectOpcode;
            operand = next.bytes.empty() ? 0 : next.bytes[0];
        }
        break;
    }
    const auto entry = krawall.entries.find({krawall.rows_start + stretch.first, pass, track.number});
    if (entry != krawall.entries.end() && entry->second.started && entry->second.level == 0)
    {
        totals.unheard++;
        return;
    }
    if (entry != krawall.entries.end() && entry->second.started)
    {
        krawall.heard++;
        if (!krawall.first_heard)
        {
            krawall.first_heard = std::pair<int, uint32_t>(track.number, stretch.tick);
        }
    }
    else if (entry != krawall.entries.end() && (effect == kKrawallPortaNote || effect == kKrawallVolumeSlidePorta))
    {
        totals.slides++;
        return;
    }
    else if (entry != krawall.entries.end() && entry->second.sample == 0)
    {
        totals.unsampled++;
        return;
    }
    else if (entry != krawall.entries.end() && effect == kKrawallNoteDelay && operand >= entry->second.speed)
    {
        totals.delayed++;
        return;
    }
    lost[track.number]++;
}

// The state of CountCommands for `sequence` of `rom` before its first command: on Krawall, what ntdlib's Player does
// with the entries and where the sequence starts. Counts a sequence of Krawall in `totals`.
KrawallCounts KrawallCountsOf(const song::DriverRom& rom, const song::Sequence& sequence, Totals& totals)
{
    KrawallCounts krawall_counts;
    if (const song::KrawallRom* k = song::KrawallOf(&rom))
    {
        std::size_t rows = 0;
        for (const song::Track& track : sequence.tracks)
        {
            rows = std::max(rows, track.stretches.size());
        }
        krawall_counts.entries = PlayedEntries(*k, sequence.sound, rows);
        krawall_counts.rows_start = ntdlib::krawall::SequenceAddress(
            k->ModulesRom(), k->info, ntdlib::krawall::SequenceOfSong(k->info, static_cast<int>(sequence.sound)));
        totals.krawall++;
    }

    return krawall_counts;
}

// Krawall's pass of the row of each stretch of `track`, from 0.
std::vector<int> KrawallPasses(const song::Track& track)
{
    std::vector<int> passes;
    std::map<uint32_t, int> seen;
    for (const song::Stretch& stretch : track.stretches)
    {
        passes.push_back(seen[stretch.first]++);
    }

    return passes;
}

// Prints and counts the commands without a note: those of `krawall_counts` whose voice sounds, and those of `lost`, by
// track. With `brownie`, the commands of a track whose meter stays at 0 in the reference count apart.
void CountLostCommands(const std::string& name, const song::DriverRom& rom, const song::Sequence& sequence,
                       bool enhanced, uint32_t sample_rate, bool brownie, const KrawallCounts& krawall_counts,
                       const std::map<int, std::size_t>& lost, Totals& totals)
{
    const std::size_t heard = krawall_counts.heard;
    if (heard > 0)
    {
        std::printf(
            "%s: %zu note %s a voice that sounds, and the arrangement shows no note of %s. The first is on "
            "track %d at tick %u.\n",
            name.c_str(), heard, heard == 1 ? "command starts" : "commands start", heard == 1 ? "it" : "them",
            krawall_counts.first_heard->first, krawall_counts.first_heard->second);
        totals.heard += heard;
        totals.failed++;
    }
    if (lost.empty())
    {
        return;
    }

    Peaks peaks;
    if (brownie)
    {
        const double seconds = static_cast<double>(sequence.length + 2) / ntdlib::agb::kFrameRate;
        const auto samples = static_cast<std::size_t>(std::ceil(seconds * sample_rate));
        peaks = ReferencePeaks(rom, sequence.sound, enhanced, samples, rom.MakePlayer()->Tracks());
    }
    for (const auto& [number, count] : lost)
    {
        const bool silent = number >= 0 && static_cast<std::size_t>(number) < peaks.size() &&
                            peaks[static_cast<std::size_t>(number)] == std::array<int32_t, 2>{0, 0};
        (silent ? totals.silent : totals.lost) += count;
    }
}

// Whether the arrangement of `sequence` shows a note of the note command at `offset`, at tick `at` of `track`, or the
// command counts apart in `totals`. With `next_track`, a note of the next track counts. With `notes_wait`, a note
// within a frame counts, and a command at `volume` 0 counts apart. With `brownie`, a note that sounds in a later frame
// counts.
bool ShowsNote(const song::Sequence& sequence, const song::Track& track, uint64_t at, uint32_t offset, bool next_track,
               bool notes_wait, bool brownie, const std::optional<int64_t>& volume, Totals& totals)
{
    bool found = false;
    for (const song::Track& other : sequence.tracks)
    {
        if (other.number != track.number && (!next_track || other.number != track.number + 1))
        {
            continue;
        }
        found = found || std::ranges::any_of(other.notes, [at](const song::Note& note) { return note.start == at; });
    }
    if (!found && notes_wait)
    {
        // A frame at the tempo of the tick: the performer's ticks to a quarter note, times the quarter
        // notes a frame.
        double bpm = 120.0;
        for (const song::Tempo& tempo : sequence.tempo)
        {
            bpm = tempo.tick <= at ? tempo.bpm : bpm;
        }
        const double frame = 96.0 * hokuto::format::kQuintetSongTicks * bpm / 60.0 / ntdlib::agb::kFrameRate;
        const bool near = std::ranges::any_of(
            track.notes, [at, frame](const song::Note& note)
            { return std::abs(static_cast<double>(note.start) - static_cast<double>(at)) <= frame + 1.0; });
        if (near)
        {
            totals.framed++;
            return true;
        }
        if (volume && *volume <= 0)
        {
            // The note plays at volume 0 and makes no sound: the converter leaves it out.
            totals.zero_volume++;
            return true;
        }
    }
    // On Brownie Brown's driver, a note can sound in a frame after its command: the arrangement shows
    // it where it sounds, and its command leads to it.
    if (!found && brownie)
    {
        const auto late = [&](const song::Note& note)
        {
            return note.command == offset && note.start > at;
        };
        if (std::ranges::any_of(track.notes, late))
        {
            totals.late++;
            return true;
        }
    }

    return found;
}

} // namespace

void CountCommands(const std::string& name, const song::DriverRom& rom, const song::Sequence& sequence, bool enhanced,
                   uint32_t sample_rate, Totals& totals)
{
    using ntdlib::commands::Kind;
    const auto [commands, offsets] = DecodedCommands(rom, sequence.sound);
    const uint32_t muted = MutedTracks(rom, sequence.sound);
    const uint16_t silenced = SilencedChannels(rom, sequence.sound);
    const bool next_track = song::KonamiOf(&rom) != nullptr;
    const bool brownie = song::BrownieOf(&rom) != nullptr;
    const bool channels = song::UbiMilanOf(&rom) != nullptr;
    const bool notes_wait = song::QuintetOf(&rom) != nullptr;
    const bool krawall = song::KrawallOf(&rom) != nullptr;
    std::map<int, std::size_t> lost; // the commands without a note, by track
    std::map<uint32_t, uint32_t> items;
    for (std::size_t i = 0; i + 1 < offsets.size(); i++)
    {
        items.emplace(offsets[i], static_cast<uint32_t>(i));
    }
    KrawallCounts krawall_counts = KrawallCountsOf(rom, sequence, totals);
    for (const song::Track& track : sequence.tracks)
    {
        std::optional<int64_t> volume; // Quintet's volume of the channel, once a command sets it
        std::vector<int> passes;       // Krawall's pass of each stretch's row, from 0
        if (krawall)
        {
            passes = KrawallPasses(track);
        }
        for (std::size_t s = 0; s < track.stretches.size(); s++)
        {
            const song::Stretch& stretch = track.stretches[s];
            const auto first = items.find(stretch.first);
            const auto last = items.find(stretch.last);
            if (sequence.loop && stretch.tick >= sequence.loop->end)
            {
                continue;
            }
            if (first == items.end() || last == items.end())
            {
                // The decoded offsets and the program's stretches disagree. The run fails.
                std::printf("%s: a stretch of track %d at tick %u has no commands at its offsets.\n", name.c_str(),
                            track.number, stretch.tick);
                totals.unmatched++;
                continue;
            }

            // On Brownie Brown's driver, a note, a hold, a rest and a release wait after they start, for the frames
            // that the file sets for them (format::BrownieWaitOf).
            uint64_t tick = stretch.tick;
            uint64_t waited = 0;
            const ntdlib::commands::Command* last_timed = nullptr; // on Quintet's driver: the last note, rest or wait
            for (uint32_t i = first->second; i <= last->second; i++)
            {
                const ntdlib::commands::Command& command = commands.items[i];
                tick += waited;
                waited = 0;
                const uint64_t at = tick;
                const ntdlib::commands::Command* tied_to =
                    last_timed != nullptr && last_timed->kind == Kind::kNote ? last_timed : nullptr;
                if (command.kind == Kind::kNote || command.kind == Kind::kNoteOff || command.kind == Kind::kWait)
                {
                    last_timed = &command;
                }
                if (notes_wait && command.kind == Kind::kVolume)
                {
                    // Index 0 sets the volume, and index 1 steps it.
                    const int64_t value = command.value.value / ntdlib::commands::kOne;
                    volume = command.index == 0 ? std::optional<int64_t>(value)
                             : volume           ? std::optional<int64_t>(*volume + value)
                                                : std::nullopt;
                }
                if (brownie && hokuto::format::BrownieWaits(command))
                {
                    waited = hokuto::format::BrownieWaitOf(command);
                }
                else if (command.kind == Kind::kWait ||
                         (notes_wait && (command.kind == Kind::kNote || command.kind == Kind::kNoteOff)))
                {
                    tick += static_cast<uint64_t>(command.value.value / ntdlib::commands::kOne);
                }
                // On Ubisoft Milan's driver, the tracks share one track of commands, and a note plays on the track of
                // its channel. On Krawall, every track contains the rows of the module, and a note counts on the track
                // of its channel.
                if (command.kind != Kind::kNote || at >= sequence.length ||
                    ((channels || krawall) && command.index != track.number))
                {
                    continue;
                }
                if ((track.number >= 0 && track.number < 32 && (muted >> track.number & 1) != 0) ||
                    (command.index < 16 && (silenced >> command.index & 1) != 0))
                {
                    totals.muted++;
                    continue;
                }
                if (notes_wait && i > first->second && commands.items[i - 1].kind == Kind::kTie && tied_to &&
                    tied_to->key == command.key && tied_to->index == command.index)
                {
                    totals.tied++;
                    continue;
                }

                totals.commands++;
                if (krawall)
                {
                    CountKrawallCommand(commands, offsets, i, track, s, passes[s], krawall_counts, lost, totals);
                    continue;
                }

                if (!ShowsNote(sequence, track, at, offsets[i], next_track, notes_wait, brownie, volume, totals))
                {
                    lost[track.number]++;
                }
            }
        }
    }

    CountLostCommands(name, rom, sequence, enhanced, sample_rate, brownie, krawall_counts, lost, totals);
}

} // namespace hokuto::tools::driver_playback
