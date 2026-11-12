// SPDX-License-Identifier: MIT

// The rounds of hokuto_check_dse_banks on the song files, the sounds and the MIDI files.

#include "check_dse_banks.h"
#include "checked_saves.h"

namespace hokuto::tools::dse_banks
{

namespace
{

// The files of a set's bytes, by their paths.
std::map<std::string, std::span<const uint8_t>> FilesOf(std::span<const uint8_t> set)
{
    std::map<std::string, std::span<const uint8_t>> files;
    for (const dse::NamedFile& named : dse::ReadSet(set))
    {
        files.emplace(named.path, named.bytes);
    }

    return files;
}

} // namespace

std::vector<std::string> PathsAt(std::span<const uint8_t> set, const std::vector<uint64_t>& offsets)
{
    std::vector<std::string> paths;
    for (const dse::NamedFile& named : dse::ReadSet(set))
    {
        const auto offset = static_cast<uint64_t>(named.bytes.data() - set.data());
        if (std::ranges::find(offsets, offset) != offsets.end())
        {
            paths.push_back(named.path);
        }
    }

    return paths;
}

bool OthersKept(std::span<const uint8_t> before, std::span<const uint8_t> after,
                const std::vector<std::string>& changed)
{
    const auto old_files = FilesOf(before);
    const auto new_files = FilesOf(after);
    if (old_files.size() != new_files.size())
    {
        return false;
    }
    for (const auto& [path, bytes] : new_files)
    {
        const auto found = old_files.find(path);
        if (found == old_files.end())
        {
            return false;
        }
        if (std::ranges::find(changed, path) == changed.end() && !std::ranges::equal(bytes, found->second))
        {
            return false;
        }
    }

    return true;
}

namespace
{

// The first file of `after` whose bytes differ from those of the same path in `before`, and the offset there, in
// words.
std::string FirstChange(std::span<const uint8_t> before, std::span<const uint8_t> after)
{
    const auto old_files = FilesOf(before);
    for (const auto& [path, bytes] : FilesOf(after))
    {
        const auto found = old_files.find(path);
        if (found == old_files.end())
        {
            return "a new file " + path;
        }
        const std::span<const uint8_t> old = found->second;
        if (std::ranges::equal(bytes, old))
        {
            continue;
        }
        const std::size_t at = FirstDifferingIndex(bytes, old);
        char offset[64];
        if (at < bytes.size() && at < old.size())
        {
            std::snprintf(offset, sizeof offset, "0x%zX (0x%02X in place of 0x%02X)", at, bytes[at], old[at]);
        }
        else
        {
            std::snprintf(offset, sizeof offset, "0x%zX", at);
        }
        return path + " at " + offset + ", " + std::to_string(bytes.size()) + " bytes in place of " +
               std::to_string(old.size());
    }

    return before.size() == after.size() ? "the set's directory" : "the set's size";
}

// A song file (smdl) of sound `sound`: a song's file, or a sound effect's settings and tracks after `header`, the
// header of one of the set's songs.
std::vector<uint8_t> SongFileOf(const DseSet& set, uint32_t sound,
                                const std::array<uint8_t, dse::Song::kHeaderSize>& header)
{
    const ntdlib::sequence_export::dse::SequenceInput& input = set.sounds.at(sound);
    if (input.song)
    {
        return input.song->Write();
    }

    const dse::EffectSequence& effect = *input.effects->sequences.at(input.effect);
    dse::Song song;
    song.header = header;
    song.info = effect.info;
    song.tracks = effect.tracks;
    return song.Write();
}

// The notes of `sequence` by its tracks, those that the driver never started among them, in the order of their starts
// and keys. Another bank starts other notes.
std::vector<std::vector<hokuto::song::Note>> NotesOf(const hokuto::song::Sequence& sequence)
{
    std::vector<std::vector<hokuto::song::Note>> notes;
    for (const hokuto::song::Track& track : sequence.tracks)
    {
        std::vector<hokuto::song::Note>& all = notes.emplace_back(track.notes);
        all.insert(all.end(), track.unstarted.begin(), track.unstarted.end());
        std::ranges::sort(all,
                          [](const hokuto::song::Note& first, const hokuto::song::Note& second)
                          {
                              return std::tie(first.start, first.key, first.velocity, first.length) <
                                     std::tie(second.start, second.key, second.velocity, second.length);
                          });
    }

    return notes;
}

// The tracks of sound `sound` of `set`: the tracks of a song, or those of a sound effect.
const std::vector<dse::Track>& TracksOf(const DseSet& set, uint32_t sound)
{
    const ntdlib::sequence_export::dse::SequenceInput& input = set.sounds.at(sound);
    return input.song ? input.song->tracks : input.effects->sequences.at(input.effect)->tracks;
}

// The events of `track` without its bank selects (0xA8, 0xA9 and 0xAA), and the count of the runs of bank selects
// with nothing between them.
std::pair<std::vector<dse::Event>, std::size_t> WithoutSelects(const dse::Track& track)
{
    std::vector<dse::Event> events;
    std::size_t runs = 0;
    bool in_run = false;
    for (const dse::Event& event : track.events)
    {
        const bool select = event.code == 0xA8 || event.code == 0xA9 || event.code == 0xAA;
        runs += select && !in_run ? 1 : 0;
        in_run = select;
        if (!select)
        {
            events.push_back(event);
        }
    }

    return {std::move(events), runs};
}

// Whether `got` contains the events of `want`, but for the bank selects. The edit replaces a run of bank selects of a
// bank that the sound doesn't play with one select of the first bank of the sound.
bool SameEvents(const std::vector<dse::Track>& got, const std::vector<dse::Track>& want)
{
    return std::ranges::equal(got, want,
                              [](const dse::Track& first, const dse::Track& second)
                              {
                                  return first.number == second.number && first.channel == second.channel &&
                                         WithoutSelects(first) == WithoutSelects(second);
                              });
}

// How the notes of `played` compare with those of `wanted`, track by track: the same starts, keys and velocities over
// the notes that both arrangements hold. The voices of another bank can end a note elsewhere, and the performer can
// find the loop a pass away. `lengths` counts the notes of other lengths, and `counts` the tracks with more or fewer
// notes.
struct NoteComparison
{
    bool same = true;
    std::string difference;
    std::size_t lengths = 0;
    std::size_t counts = 0;
};

NoteComparison CompareNotes(const hokuto::song::Sequence& played, const hokuto::song::Sequence& wanted)
{
    NoteComparison comparison;
    if (played.timebase != wanted.timebase || played.tracks.size() != wanted.tracks.size())
    {
        comparison.same = false;
        comparison.difference = std::to_string(played.tracks.size()) + " tracks at " + std::to_string(played.timebase) +
                                " ticks a quarter note in place of " + std::to_string(wanted.tracks.size()) + " at " +
                                std::to_string(wanted.timebase);
        return comparison;
    }

    const auto played_notes = NotesOf(played);
    const auto wanted_notes = NotesOf(wanted);
    for (std::size_t track = 0; track < wanted_notes.size() && comparison.same; ++track)
    {
        const auto& got = played_notes[track];
        const auto& want = wanted_notes[track];
        comparison.counts += got.size() != want.size() ? 1 : 0;
        for (std::size_t i = 0; i < std::min(got.size(), want.size()); ++i)
        {
            if (got[i].start != want[i].start || got[i].key != want[i].key || got[i].velocity != want[i].velocity)
            {
                const auto text = [](const hokuto::song::Note& note)
                {
                    return std::to_string(note.key) + " at " + std::to_string(note.start) + " at velocity " +
                           std::to_string(note.velocity);
                };
                comparison.same = false;
                comparison.difference = "note " + std::to_string(i) + " of track " + std::to_string(track) + " is " +
                                        text(got[i]) + " in place of " + text(want[i]);
                break;
            }
            comparison.lengths += got[i].length != want[i].length ? 1 : 0;
        }
    }

    return comparison;
}

// The next sound of the same kind, after the sound in the list. `place` is the sound's place in `sounds`.
std::optional<uint32_t> NextOfKind(const DseSet& set, std::span<const uint32_t> sounds, std::size_t place)
{
    const bool effects = set.sounds[sounds[place]].effects != nullptr;
    for (std::size_t step = 1; step < sounds.size(); ++step)
    {
        const uint32_t other = sounds[(place + step) % sounds.size()];
        if ((set.sounds[other].effects != nullptr) == effects)
        {
            return std::optional(other);
        }
    }
    return std::optional<uint32_t>();
}

// Replaces sound `sound`, named `name`, with its file at `path` or, for a sound effect, its settings and tracks as a
// song file with `header`. Returns whether the sound took the file.
bool PutSoundInPlace(const Game& game, uint32_t sound, const std::string& name, const std::string& path,
                     const std::array<uint8_t, dse::Song::kHeaderSize>& header, Totals& totals)
{
    const DseSet& set = *game.set;
    try
    {
        const std::vector<uint8_t> file =
            set.sounds[sound].song
                ? std::vector<uint8_t>(FilesOf(game.bytes).at(path).begin(), FilesOf(game.bytes).at(path).end())
                : SongFileOf(set, sound, header);
        const edit::DseIncomingSong incoming = edit::ReadIncomingDseSong(file);
        const File replaced = edit::ReplaceDseSound(game.file, set, sound, incoming);
        const std::vector<uint8_t> exported = hokuto::format::ExportArchive(replaced);
        const std::vector<std::string> warnings = edit::DseSoundWarnings(set, sound, incoming);
        const bool moved = std::ranges::any_of(
            warnings, [](const std::string& line) { return line.find("They play") != std::string::npos; });
        if (exported == game.bytes)
        {
            totals.same_sounds++;
        }
        else if (set.sounds[sound].song && dse::Song::Parse(file).Write() != file)
        {
            std::printf("%s: %s counts apart: the reader leaves out %zd bytes of its file\n", game.name.c_str(),
                        name.c_str(),
                        static_cast<std::ptrdiff_t>(file.size()) -
                            static_cast<std::ptrdiff_t>(dse::Song::Parse(file).Write().size()));
            totals.sounds_apart++;
        }
        else if (moved)
        {
            std::printf("%s: %s counts apart: its tracks select none of its banks\n", game.name.c_str(), name.c_str());
            totals.sounds_apart++;
        }
        else
        {
            Fail(game, totals, name + ": its song file gives another set: " + FirstChange(game.bytes, exported));
        }
    }
    catch (const std::invalid_argument& error)
    {
        std::printf("%s: %s refused its song file: %s\n", game.name.c_str(), name.c_str(), error.what());
        totals.sounds_refused++;
        return false;
    }

    return true;
}

// Whether the sound effects of `read`, the set with a new file for sound effect `sound`, keep the sequences of `set`
// but for the sound's slot.
bool EffectSlotsKept(const DseSet& set, const DseSet& read, uint32_t sound)
{
    bool kept = true;
    const dse::SoundEffects& before = *set.sounds[sound].effects;
    const dse::SoundEffects& after = *read.sounds.at(sound).effects;
    for (std::size_t slot = 0; slot < before.sequences.size() && kept; ++slot)
    {
        kept = slot == set.sounds[sound].effect ||
               (slot < after.sequences.size() && before.sequences[slot] == after.sequences[slot]);
    }

    return kept;
}

// Replaces sound `sound`, named `name` with its file at `path`, with the file of sound `next`, made with `header`. The
// sound plays the next sound's notes, and only the sound's file changes. A sound effect's file keeps the other
// effects' tracks.
void CheckNextSound(const Game& game, uint32_t sound, uint32_t next, const std::string& name, const std::string& path,
                    const std::array<uint8_t, dse::Song::kHeaderSize>& header, Totals& totals)
{
    const DseSet& set = *game.set;
    try
    {
        const edit::DseIncomingSong incoming = edit::ReadIncomingDseSong(SongFileOf(set, next, header));
        const File replaced = edit::ReplaceDseSound(game.file, set, sound, incoming);
        const std::vector<uint8_t> exported = hokuto::format::ExportArchive(replaced);
        const std::shared_ptr<const DseSet> read = hokuto::song::LoadDseSet(exported, game.build);
        const hokuto::song::Sequence played = hokuto::song::ReadSequence(*read, sound);
        const hokuto::song::Sequence wanted = hokuto::song::ReadSequence(set, next);
        const bool same_events = SameEvents(TracksOf(*read, sound), TracksOf(set, next));
        const NoteComparison notes = CompareNotes(played, wanted);
        totals.next_lengths += notes.lengths;
        totals.next_counts += notes.counts;
        const bool kept = OthersKept(game.bytes, exported, {path}) &&
                          (!set.sounds[sound].effects || EffectSlotsKept(set, *read, sound));
        if (!same_events)
        {
            Fail(game, totals, name + ": the tracks of " + hokuto::song::DseSoundName(set, next) + " don't read back");
        }
        else if (!notes.same)
        {
            Fail(game, totals,
                 name + ": the notes of " + hokuto::song::DseSoundName(set, next) +
                     " don't play in its place: " + notes.difference);
        }
        else if (!kept)
        {
            Fail(game, totals, name + ": the next sound's file changes other files or sound effects");
        }
        else
        {
            totals.next_sounds++;
        }
    }
    catch (const std::invalid_argument& error)
    {
        std::printf("%s: %s refused %s: %s\n", game.name.c_str(), name.c_str(),
                    hokuto::song::DseSoundName(set, next).c_str(), error.what());
        totals.next_refused++;
    }
}

// Replaces sound `sound`, named `name`, with its MIDI file. Its notes come back near their places, in ticks of 48 to
// the quarter note.
void CheckSoundMidi(const Game& game, uint32_t sound, const std::string& name, Totals& totals)
{
    const DseSet& set = *game.set;
    ntdlib::sequence_export::dse::Conversion conversion;
    try
    {
        conversion = ntdlib::sequence_export::dse::ConvertSequence(set.sounds[sound].Files(), name,
                                                                   {.build = set.build}, "hokuto_check_dse_banks");
    }
    catch (const std::exception&)
    {
        return; // the converter refuses the sound, and Export… offers no MIDI file
    }
    edit::ProgramMap programs;
    for (std::size_t preset = 0; preset < conversion.performance.presets.size() && preset < 128; ++preset)
    {
        programs[edit::Program{0, static_cast<int>(preset)}] = conversion.performance.presets[preset].program;
    }
    try
    {
        totals.midi_sounds++;
        const edit::DseIncomingSong incoming = edit::DseSongOfMidi(conversion.midi, name + ".mid");
        const File replaced = edit::ReplaceDseSound(game.file, set, sound, incoming, programs);
        const std::shared_ptr<const DseSet> read =
            hokuto::song::LoadDseSet(hokuto::format::ExportArchive(replaced), game.build);
        const hokuto::song::Sequence played = hokuto::song::ReadSequence(*read, sound);
        const hokuto::song::Sequence wanted = hokuto::song::ReadSequence(set, sound);
        for (const std::vector<hokuto::song::Note>& track : NotesOf(wanted))
        {
            totals.midi_arranged += track.size();
        }
        std::vector<hokuto::song::Note> notes;
        for (const std::vector<hokuto::song::Note>& track : NotesOf(played))
        {
            notes.insert(notes.end(), track.begin(), track.end());
        }

        // The MIDI file's notes up to its loopEnd marker, or the end of the new sound's first pass. Past them, the
        // file plays on into the next pass.
        const seqlib::midi::File file = seqlib::midi::Read(conversion.midi);
        const auto ticks = [](uint64_t at, uint32_t timebase)
        {
            return static_cast<int64_t>(std::llround(static_cast<double>(at) * kMidiTimebase / timebase));
        };
        int64_t end = ticks(played.loop ? played.loop->end : played.length, played.timebase);
        for (const seqlib::midi::Text& marker : seqlib::midi::Markers(file))
        {
            if (marker.text == "loopEnd")
            {
                end = std::min(end, ticks(marker.tick, file.division));
            }
        }
        std::vector<bool> taken(notes.size());
        std::size_t matched = 0;
        std::size_t count = 0;
        for (const seqlib::midi::Note& note : seqlib::midi::Notes(file))
        {
            const int64_t start = ticks(note.on, file.division);
            if (start >= end)
            {
                continue;
            }
            count++;
            for (std::size_t i = 0; i < notes.size(); ++i)
            {
                if (!taken[i] && notes[i].key == note.key &&
                    std::abs(ticks(notes[i].start, played.timebase) - start) <= kMidiTicks)
                {
                    taken[i] = true;
                    matched++;
                    const int64_t length = ticks(note.off - note.on, file.division);
                    totals.midi_lengths +=
                        std::abs(ticks(notes[i].length, played.timebase) - length) > kMidiTicks ? 1 : 0;
                    break;
                }
            }
        }
        totals.midi_notes += count;
        totals.midi_matched += matched;
        totals.midi_whole += count > 0 && matched == count ? 1 : 0;
        totals.midi_noteless += count == 0 ? 1 : 0;
        if (matched != count)
        {
            std::printf("%s: %s: %zu of the %zu notes of its MIDI file came back\n", game.name.c_str(), name.c_str(),
                        matched, count);
        }
    }
    catch (const std::invalid_argument& error)
    {
        std::printf("%s: %s refused its MIDI file: %s\n", game.name.c_str(), name.c_str(), error.what());
        totals.midi_refused++;
    }
}

} // namespace

void SoundRounds(const Game& game, Totals& totals)
{
    const DseSet& set = *game.set;
    const std::vector<uint32_t> sounds = hokuto::song::DseSounds(set);
    std::array<uint8_t, dse::Song::kHeaderSize> header = {};
    for (const uint32_t sound : sounds)
    {
        if (set.sounds[sound].song)
        {
            header = set.sounds[sound].song->header;
            break;
        }
    }

    for (std::size_t place = 0; place < sounds.size(); ++place)
    {
        const uint32_t sound = sounds[place];
        const std::string name = hokuto::song::DseSoundName(set, sound);
        const std::string path = set.sounds[sound].ref.path;
        totals.sounds++;

        // The sound's file in its place reproduces the image: a song's file in the image, or a sound effect's settings
        // and tracks as a song file. Two kinds of sound count apart. A file can hold bytes after a track's end. No
        // track reaches them, and the reader leaves them out. Tracks can select none of the sound's banks. The window
        // then says that they play the sound's first bank, and the replacement selects it.
        if (!PutSoundInPlace(game, sound, name, path, header, totals))
        {
            continue;
        }

        // The sample of the sounds that play: every nth, with n the count of sounds divided by kSampledSounds and
        // rounded down.
        if (place % std::max<std::size_t>(sounds.size() / kSampledSounds, 1) != 0)
        {
            continue;
        }
        totals.sampled_sounds++;

        // The next sound's file: the sound plays its notes, and only the sound's file changes. A sound effect's file
        // keeps the other effects' tracks.
        if (const std::optional<uint32_t> next = NextOfKind(set, sounds, place))
        {
            CheckNextSound(game, sound, *next, name, path, header, totals);
        }

        // The sound's MIDI file: its notes come back near their places, in ticks of 48 to the quarter note.
        CheckSoundMidi(game, sound, name, totals);
    }
    std::fflush(stdout);
}

} // namespace hokuto::tools::dse_banks
