// SPDX-License-Identifier: MIT

// Checks the import of MIDI files into the songs of GBA ROMs (src/edit/gba_midi.h). The tool imports each ROM into a
// Hokuto file the way the program does (format::ImportGbaRom). It reads each song that plays (song::GbaSongs) with
// ntdlib's performer, once for each sequence, and writes a MIDI file of what the performer read, at the song's 24
// ticks to a quarter note:
//
// - Each track that runs a note command after its first voice gets a MIDI channel of its own, in the order of the
//   tracks. The performer reports no note of a track whose notes never sound, such as quiet notes of a PSG channel, but
//   such a track still takes channels from the tracks after it. A note is written at its tick, with its velocity, and
//   lasts as long as its command says. A tie lasts as long as the arrangement shows it. The arrangement shows a tie
//   that the driver never started without a length. Such a tie lasts up to the track's next end of a tie of its key: it
//   holds a channel until then, and a later note can take the channel from it. The key of a note is the key that
//   sounds: the key of its command and the transposition of its track. The import's notes have to sound at those keys.
// - Each command that sets a track's voice, volume, pan, pitch bend, bend range, LFO, tuning, priority or echo is
//   written at its tick in the form that the GBA community's converters use: a program change, a control change or a
//   pitch bend. A program change between two notes of a tick stays between them. The tool counts the commands that a
//   MIDI file doesn't hold. A bend range above 127 semitones is one: the track's notes then play at other pitches, and
//   the comparison leaves out their lengths. A sample that plays once ends sooner at a higher pitch. A pan 64 right of
//   the centre is another. It is written as 127, and the comparison reads it as 63.
// - Each tempo is written at its tick as the beats a minute that the song's command sets: twice its value. A song
//   without a tempo at tick 0 starts at MusicPlayer2000's 150 beats a minute.
// - The loop's start and end go in as the markers "[" and "]". The commands from the loop's end on stay out. A track
//   without a loop ends where its command to end runs, and the file where the arrangement ends. The arrangement ends a
//   song without a loop at its last sound when the song's end comes long after it (ntdlib's kMaxSilentEndSeconds). A
//   track whose command to end doesn't run by then ends where the arrangement ends.
//
// The import puts each MIDI file in the place of its song, every song in one version of the file, with each program's
// voice. On Metroid Fusion's sequencer, the import makes a tie of each note whose length no code encodes. The tool then
// exports the ROM, and the performer has to read each song again as it read it before: each track's notes, started or
// not, with their ticks, lengths, keys, velocities and voices, the track's volumes and pans, the tempos and the loop.
// The import's warnings have to stay empty but for what the tool leaves out of the MIDI file.
//
// The tool counts some notes apart. The comparison leaves out the first three kinds, and the lengths of the others:
//
// - A note of velocity 0. MIDI has no such note-on. The import sets its velocity to 1.
// - A tie that the driver never started. The arrangement shows it without a length.
// - A note that the loop's end cuts to less than a frame's ticks. It can then start and end in one of the driver's
//   frames, and never sound. Before the import, its command carried it on into the next pass.
// - A note that a later note of its key cuts short in the MIDI file. The driver's channels can then come free sooner,
//   and a note that the driver never started for want of a channel can start. In such a song, the comparison uses the
//   started notes and the others together, without their lengths and voices.
// - A note that ends sooner than its command says, such as one that the track's end or another note cuts. A note that
//   the driver never started lasts the length of its command. When the loop's end or the track's end cuts it in the
//   MIDI file, its new command gets the cut length.
//
// The tool also counts the notes that sound past the loop's end. The arrangement and the MIDI file cut them there.
//
// A song without notes counts apart too: the import refuses a MIDI file without notes.
//
// The performer finds a song's loop where its first two passes play alike. A song whose passes differ, such as by the
// phase of an LFO, can then have its loop found a pass later after the import: the intro of the MIDI file contains the
// first pass. The tool counts such a loop apart, and compares the reads up to the earlier loop's end.
//
// A ROM without free space has no room for a song that the import makes longer. The tool then leaves those songs out,
// and counts them apart. With --filler, the songs go into the ROM's filler runs instead
// (format::NativeArchive::gba_filler).
//
// The tool prints a line for each song that differs, and a summary. It returns 1 when a song differs, or when an
// import, a read or the export fails.
//
// usage: hokuto_check_gba_midi [--filler] <rom.gba>...

#include "check_gba_midi.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ntdlib/mp2k/driver.h"
#include "xsflib/console.h"
#include "xsflib/files.h"

#include "checked_archive.h"
#include "checked_saves.h"
#include "edit/gba_midi.h"
#include "edit/replace.h"
#include "format/archive.h"
#include "format/file.h"
#include "loop_passes.h"
#include "song/gba.h"
#include "song/sequence.h"

namespace
{

using hokuto::format::File;
using hokuto::song::Note;
using hokuto::song::Sequence;
using hokuto::song::Track;
using hokuto::tools::gba_midi::LeftOut;
using hokuto::tools::gba_midi::MidiOf;
using hokuto::tools::gba_midi::Shifts;
using hokuto::tools::gba_midi::Totals;
using hokuto::tools::gba_midi::Written;

// A note as the comparison uses it: its tick, length, key, velocity and voice.
using Played = std::tuple<uint32_t, uint32_t, uint8_t, uint8_t, uint32_t>;

// What the comparison uses of a track: its started notes and its unstarted notes, in order, and its volumes and pans.
struct TrackPlay
{
    std::vector<Played> notes;
    std::vector<Played> unstarted;
    std::vector<hokuto::song::Setting> volume;
    std::vector<hokuto::song::Setting> pan;
};

// The highest pan that control change 10 of a MIDI file can store: 127, 63 right of the centre. A pan command of
// MusicPlayer2000 can hold 64.
constexpr int kHighestMidiPan = 63;

// What the comparison uses of the tracks of `song` with notes, without the notes of `left_out`, and without the lengths
// of the notes of `loose`. A pan further right than MIDI can store counts as the highest MIDI pan. With `until`, it
// keeps only the notes and settings before that tick, and cuts the notes there. With `shifts`, each note adds the
// transposition of its track to its key. `left_out` and `loose` hold the keys that sound. With `together`, the started
// notes and the notes that the driver never started are in one list, without their lengths and voices: the performer
// reports neither for a note that never started.
std::vector<TrackPlay> PlayOf(const Sequence& song, const LeftOut& left_out, const LeftOut& loose,
                              std::optional<uint32_t> until, const Shifts* shifts, bool together)
{
    std::vector<TrackPlay> plays;
    for (const Track& track : song.tracks)
    {
        if (track.notes.empty() && track.unstarted.empty())
        {
            continue;
        }
        const std::size_t index = plays.size();
        TrackPlay& play = plays.emplace_back();
        const auto take = [&](const std::vector<Note>& notes, std::vector<Played>& to)
        {
            for (const Note& note : notes)
            {
                uint8_t key = note.key;
                if (const auto track_shifts = shifts ? shifts->find(index) : Shifts::const_iterator{};
                    shifts && track_shifts != shifts->end())
                {
                    const auto shift = track_shifts->second.find({note.start, note.key});
                    key = static_cast<uint8_t>(key + (shift != track_shifts->second.end() ? shift->second : 0));
                }
                const auto in = [&note, key, index](const LeftOut& out)
                {
                    const auto found = out.find(index);
                    return found != out.end() && found->second.contains({note.start, key});
                };
                if (!in(left_out) && (!until || note.start < *until))
                {
                    const uint32_t length = until ? std::min(note.length, *until - note.start) : note.length;
                    to.emplace_back(note.start, in(loose) || together ? 0 : length, key, note.velocity,
                                    together ? 0 : note.program);
                }
            }
            std::sort(to.begin(), to.end());
        };
        take(track.notes, play.notes);
        take(track.unstarted, together ? play.notes : play.unstarted);
        const auto before_until = [until](const std::vector<hokuto::song::Setting>& settings)
        {
            std::vector<hokuto::song::Setting> kept;
            std::copy_if(settings.begin(), settings.end(), std::back_inserter(kept),
                         [until](const hokuto::song::Setting& s) { return !until || s.tick < *until; });
            return kept;
        };
        play.volume = before_until(track.volume);
        play.pan = before_until(track.pan);
        for (hokuto::song::Setting& pan : play.pan)
        {
            pan.value = std::min(pan.value, kHighestMidiPan);
        }
    }

    return plays;
}

// Text that names the first note of `then` and of `now` that differ.
std::string FirstDifference(const std::vector<Played>& then, const std::vector<Played>& now)
{
    const auto text = [](const std::vector<Played>& notes, std::size_t i)
    {
        if (i >= notes.size())
        {
            return std::string("none");
        }
        const auto& [tick, length, key, velocity, voice] = notes[i];
        return "tick " + std::to_string(tick) + ", length " + std::to_string(length) + ", key " + std::to_string(key) +
               ", velocity " + std::to_string(velocity) + ", voice " + std::to_string(voice);
    };
    const std::size_t i = hokuto::tools::FirstDifferingIndex(then, now);
    return ": " + text(then, i) + " became " + text(now, i);
}

// The import of a song: its sequence, what the performer read before, and the notes that the comparison leaves out.
struct Imported
{
    int song = 0;
    uint32_t sequence = 0;
    Sequence before;
    LeftOut left_out;
    LeftOut loose;
    Shifts shifts;
    bool cut_short = false;
};

// Imports each song of `songs` whose sequence `sequences` lists into `current` from the MIDI file that MidiOf writes,
// once for each sequence, and returns the imports. `file` is the ROM's Hokuto file, `rom` the ROM and `places` where
// its songs play.
std::vector<Imported> ImportSongs(const File& file, const hokuto::song::GbaRom& rom, const std::vector<int>& songs,
                                  const std::vector<std::optional<uint32_t>>& sequences,
                                  std::span<const std::optional<hokuto::edit::SoundPlace>> places,
                                  const std::string& rom_name, File& current, Totals& totals)
{
    std::vector<Imported> imported;
    std::set<uint32_t> seen;
    for (const int song : songs)
    {
        const std::optional<uint32_t> sequence = sequences[static_cast<std::size_t>(song)];
        if (!sequence || !seen.insert(*sequence).second)
        {
            continue;
        }

        const std::string name = rom_name + ": " + hokuto::song::GbaSongName(rom, song);
        try
        {
            Imported entry{song, *sequence, hokuto::song::ReadSequence(rom, song), {}, {}, {}};
            if (std::all_of(entry.before.tracks.begin(), entry.before.tracks.end(),
                            [](const Track& track) { return track.notes.empty() && track.unstarted.empty(); }))
            {
                totals.without_notes++;
                continue;
            }
            Written written = MidiOf(file, *sequence, entry.before, totals);
            const hokuto::edit::GbaMidiSong midi =
                hokuto::edit::GbaSongOfMidi(written.midi, {}, ntdlib::mp2k::NoteArguments(rom.info));
            if (!midi.warnings.empty())
            {
                std::printf("%s: %s\n", name.c_str(), midi.warnings.front().c_str());
                totals.warned++;
            }
            current = hokuto::edit::WithGbaMidiSong(current, places, static_cast<uint32_t>(song), midi);
            entry.left_out = std::move(written.left_out);
            entry.loose = std::move(written.loose);
            entry.shifts = std::move(written.shifts);
            entry.cut_short = written.cut_short;
            imported.push_back(std::move(entry));
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s\n", name.c_str(), error.what());
            totals.failed++;
        }
    }

    return imported;
}

// Exports `current`, `file` with the songs of `imported`, and returns the ROM that the export produces, or nothing when
// it fails.
std::shared_ptr<const hokuto::song::GbaRom> ExportImports(
    const File& file, const hokuto::song::GbaRom& rom, std::span<const std::optional<hokuto::edit::SoundPlace>> places,
    const std::string& rom_name, File current, std::vector<Imported>& imported, Totals& totals)
{
    // A ROM without free space has no room for the songs that the import makes longer. They then stay out, and the
    // others go in again.
    std::shared_ptr<const hokuto::song::GbaRom> played;
    try
    {
        std::vector<uint8_t> exported;
        try
        {
            exported = hokuto::format::ExportArchive(current);
        }
        catch (const std::invalid_argument& error)
        {
            const std::vector<uint32_t> moved = hokuto::format::MovedSongs(current);
            if (moved.empty() || std::string(error.what()).find("32 MiB") == std::string::npos)
            {
                throw;
            }

            const std::set<uint32_t> outgrown(moved.begin(), moved.end());
            std::printf("%s: %zu songs outgrow their places, and the ROM has no room for them. They stay out.\n",
                        rom_name.c_str(), outgrown.size());
            totals.outgrown += outgrown.size();
            std::erase_if(imported, [&outgrown](const Imported& entry) { return outgrown.contains(entry.sequence); });
            current = file;
            for (const Imported& entry : imported)
            {
                Totals ignored;
                const Written written = MidiOf(file, entry.sequence, entry.before, ignored);
                current = hokuto::edit::WithGbaMidiSong(
                    current, places, static_cast<uint32_t>(entry.song),
                    hokuto::edit::GbaSongOfMidi(written.midi, {}, ntdlib::mp2k::NoteArguments(rom.info)));
            }
            exported = hokuto::format::ExportArchive(current);
        }
        played = hokuto::song::LoadGbaRom(std::move(exported));
    }
    catch (const std::exception& error)
    {
        std::printf("%s: the export fails: %s\n", rom_name.c_str(), error.what());
        totals.failed++;
        return nullptr;
    }

    return played;
}

// Compares each song of `imported` as `played`, the ROM after the import, plays it with how the song played before.
void CheckImported(const hokuto::song::GbaRom& rom, const hokuto::song::GbaRom& played,
                   const std::vector<Imported>& imported, const std::string& rom_name, Totals& totals)
{
    for (const Imported& entry : imported)
    {
        const std::string name = rom_name + ": " + hokuto::song::GbaSongName(rom, entry.song);
        totals.songs++;
        try
        {
            const Sequence after = hokuto::song::ReadSequence(played, entry.song);

            // The performer can find the loop a whole number of passes away. The comparison then ends at the earlier
            // loop's end.
            const std::optional<uint32_t> until = hokuto::tools::EarlierEnd(entry.before, after);
            totals.passes_away += until ? 1 : 0;
            const auto tempos_until = [until](const std::vector<hokuto::song::Tempo>& tempos)
            {
                std::vector<hokuto::song::Tempo> kept;
                std::copy_if(tempos.begin(), tempos.end(), std::back_inserter(kept),
                             [until](const hokuto::song::Tempo& t) { return !until || t.tick < *until; });
                return kept;
            };
            const std::vector<hokuto::song::Tempo> tempos_then = tempos_until(entry.before.tempo);
            const std::vector<hokuto::song::Tempo> tempos_now = tempos_until(after.tempo);

            const std::vector<TrackPlay> then =
                PlayOf(entry.before, entry.left_out, entry.loose, until, &entry.shifts, entry.cut_short);
            const std::vector<TrackPlay> now =
                PlayOf(after, entry.left_out, entry.loose, until, nullptr, entry.cut_short);
            std::string what;
            const auto loop_text = [](const std::optional<hokuto::song::Loop>& loop)
            {
                return loop ? std::to_string(loop->start) + "-" + std::to_string(loop->end) : std::string("none");
            };
            if (entry.before.loop != after.loop && !until)
            {
                what = "the loop moved: " + loop_text(entry.before.loop) + " became " + loop_text(after.loop);
            }
            else if (tempos_then != tempos_now)
            {
                const std::size_t i = hokuto::tools::FirstDifferingIndex(tempos_then, tempos_now);
                const auto tempo_text = [](const std::vector<hokuto::song::Tempo>& tempos, std::size_t at)
                {
                    return at < tempos.size()
                               ? std::to_string(tempos[at].bpm) + " BPM at tick " + std::to_string(tempos[at].tick)
                               : std::string("none");
                };
                what = "the tempos differ: " + tempo_text(tempos_then, i) + " became " + tempo_text(tempos_now, i);
            }
            else if (then.size() != now.size())
            {
                what = std::to_string(then.size()) + " tracks with notes became " + std::to_string(now.size());
            }
            else
            {
                for (std::size_t t = 0; t < then.size() && what.empty(); t++)
                {
                    const std::string track = "track " + std::to_string(t) + "'s ";
                    what = then[t].notes != now[t].notes
                               ? track + "notes differ" + FirstDifference(then[t].notes, now[t].notes)
                           : then[t].unstarted != now[t].unstarted
                               ? track + "unstarted notes differ" + FirstDifference(then[t].unstarted, now[t].unstarted)
                           : then[t].volume != now[t].volume ? track + "volumes differ"
                           : then[t].pan != now[t].pan       ? track + "pans differ"
                                                             : "";
                }
            }
            if (!what.empty())
            {
                std::printf("%s: %s\n", name.c_str(), what.c_str());
                totals.differ++;
            }
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s\n", name.c_str(), error.what());
            totals.failed++;
        }
    }
}

void CheckRom(const std::filesystem::path& path, Totals& totals)
{
    const std::string rom_name = xsflib::Utf8(path);
    std::vector<uint8_t> bytes;
    if (!xsflib::ReadFile(path, bytes))
    {
        std::printf("%s: can't be read\n", rom_name.c_str());
        totals.failed++;
        return;
    }

    File file;
    std::shared_ptr<const hokuto::song::GbaRom> rom;
    try
    {
        file = hokuto::tools::WithFillerChoice(hokuto::format::ImportGbaRom(bytes));
        rom = hokuto::song::LoadGbaRom(bytes);
    }
    catch (const xsflib::FormatError&)
    {
        return; // no MP2K, or no song that plays
    }
    totals.roms++;

    // Where each song plays. The program finds the songs the same way.
    const std::vector<int> songs = hokuto::song::GbaSongs(*rom);
    std::vector<std::optional<uint32_t>> sequences;
    for (int song = 0; song <= (songs.empty() ? -1 : songs.back()); song++)
    {
        sequences.push_back(hokuto::format::GbaSongSequence(file, rom->info.song_table, song));
    }
    const auto places = hokuto::edit::GbaSoundPlaces(
        sequences, hokuto::format::GbaTrackFields(file, rom->info.song_table, sequences.size()));

    File current = file;
    std::vector<Imported> imported = ImportSongs(file, *rom, songs, sequences, places, rom_name, current, totals);
    const std::shared_ptr<const hokuto::song::GbaRom> played =
        ExportImports(file, *rom, places, rom_name, std::move(current), imported, totals);
    if (!played)
    {
        return;
    }

    CheckImported(*rom, *played, imported, rom_name, totals);
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> arguments = xsflib::console::Arguments(argc, argv);
    hokuto::tools::TakeFillerOption(arguments);
    if (arguments.size() < 2)
    {
        std::fprintf(stderr, "usage: hokuto_check_gba_midi [--filler] <rom.gba>...\n");
        return 2;
    }

    Totals totals;
    for (std::size_t i = 1; i < arguments.size(); i++)
    {
        CheckRom(xsflib::PathFromUtf8(arguments[i]), totals);
    }

    std::printf(
        "%zu ROMs, %zu songs, %zu tracks, %zu notes, %zu of them ties. %zu songs differ, %zu imports gave "
        "warnings, %zu failures. Apart: %zu notes of velocity 0, %zu unstarted ties, %zu notes that a later "
        "note of their key cuts short, %zu notes past the loop's end, %zu notes that end sooner than their commands, "
        "%zu notes that the loop's end cuts to less than a frame, %zu songs without notes, %zu songs without room, %zu "
        "commands that the MIDI files don't hold, %zu notes "
        "transposed, %zu note commands that the performer doesn't report, %zu loops a whole number of passes away.\n",
        totals.roms, totals.songs, totals.tracks, totals.notes, totals.ties, totals.differ, totals.warned,
        totals.failed, totals.silent, totals.unstarted_ties, totals.crossing, totals.past_loop, totals.sooner,
        totals.brief, totals.without_notes, totals.outgrown, totals.commands_left_out, totals.transposed,
        totals.unreported, totals.passes_away);

    return totals.differ + totals.failed + totals.warned > 0 ? 1 : 0;
}
