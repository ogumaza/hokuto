// SPDX-License-Identifier: MIT

// Checks the imports of MIDI files (src/edit/midi_imports.h) on sound archives of the Wii, the 3DS, the Wii U, the
// Switch and the DS. Each sequence sound is exported as File > Export writes it: a MIDI file and a SoundFont. The two
// then come back into a new file of the console the way File > Import > Bank… and File > Import > Sequence… take them.
// Each MIDI bank of the SoundFont is placed in the bank slot of its number. ntdlib's model of the console plays the new
// sound and the game's sound dry, for 10 s, and the audibility method's level measure compares them
// (song::CompareLevels).
//
// A failure is a conversion or an import that throws anything but a refusal, a new sound that ntdlib's performer can't
// read, a file that doesn't save and open again in both forms, a note of the MIDI file that the new sound doesn't play,
// and on a later console a new song that doesn't open as a timeline. A file of another kind, such as a GBA ROM, fails
// too. A note counts at the tick nearest its tick in the MIDI file, or a tick from that one, up to the file's `loopEnd`
// marker, the end of the new sound's loop or its last note. A note of a tick or less counts apart: the model can cut it
// in the tick that it starts, and such a note shows on neither side. The tool lists the findings of the level measure
// and counts them. It counts apart the sounds whose commands draw random numbers, and those whose MIDI file selects a
// bank past the sound's slots. With `--notes`, a line shows each missing note, and a sound with findings lists the
// measure's places and both renders' levels in each half second.
//
// A refusal fails too, except the imports that kExpectedImportRefusals lists. Those count as sounds that hold no notes.
// The conversions of these consoles give a MIDI file for a sound without notes, and the import refuses it.
//
// hokuto_check_midi_imports [--every n] [--seconds s] [--sound name] [--notes] <archive>...

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/convert.h"
#include "seqlib/midi/reader.h"
#include "xsflib/console.h"
#include "xsflib/files.h"

#include "checked_archive.h"
#include "checked_saves.h"
#include "edit/bytecode.h"
#include "edit/converter.h"
#include "edit/ds_new_items.h"
#include "edit/midi_imports.h"
#include "edit/new_items.h"
#include "edit/replace.h"
#include "edit/sound_font_imports.h"
#include "edit/timelines.h"
#include "format/file.h"
#include "format/model.h"
#include "song/audibility.h"
#include "song/dry_render.h"
#include "song/midi_export.h"

namespace
{

namespace export_ = ntdlib::sequence_export;
namespace edit = hokuto::edit;
namespace format = hokuto::format;
namespace sar = ntdlib::sar;
namespace song = hokuto::song;
namespace tools = hokuto::tools;

// The bank slots of a sound of the later consoles.
constexpr uint32_t kSlots = 4;

// The refusals of an import that the check expects, for a sound that plays no notes. src/edit writes these messages
// without a constant or a function to call. The table copies them, and each entry has to follow its source.
constexpr std::array<std::string_view, 2> kExpectedImportRefusals = {
    "The MIDI file has no notes.",   // edit::SongOfMidi (src/edit/midi_import.cpp)
    "The SoundFont has no presets.", // edit::ReadSoundFontImport, edit::AddSoundFont (src/edit/sound_font_imports.cpp)
};

struct Totals
{
    std::size_t archives = 0;
    std::size_t sounds = 0;
    std::size_t no_notes = 0;
    std::size_t timelines = 0;
    std::size_t notes = 0;
    std::size_t missing = 0;
    std::size_t in_first_tick = 0;
    std::size_t other_banks = 0;
    std::size_t silent = 0;
    std::size_t no_finding = 0;
    std::size_t spans = 0;
    std::size_t beyond_3db = 0;
    std::size_t runs = 0;
    std::size_t random = 0;
    std::size_t random_findings = 0;
    std::size_t gains = 0;
    std::size_t moved = 0;
    std::size_t failed = 0;
    std::map<std::string, std::size_t> refusals;
    std::map<std::string, std::size_t> warnings;
};

// Whether a command that the player reaches from item `start` of `sequence` draws a random number.
bool DrawsRandomNumbers(const ntdlib::commands::Sequence& sequence, uint32_t start)
{
    using ntdlib::commands::Kind;
    using ntdlib::commands::Source;
    const std::vector<bool> reached = edit::Reached(sequence, start, true);
    for (std::size_t item = 0; item < sequence.items.size(); item++)
    {
        const ntdlib::commands::Command& command = sequence.items[item];
        if (reached[item] && (command.kind == Kind::kRandomVariable || command.value.source == Source::kRandom ||
                              (command.time && command.time->source == Source::kRandom)))
        {
            return true;
        }
    }

    return false;
}

export_::Conversion ConversionOf(const tools::CheckedArchive& archive, uint32_t sound)
{
    return archive.ds    ? song::ConvertToMidi(*archive.ds, sound, {}, "Hokuto")
           : archive.wii ? song::ConvertToMidi(*archive.wii, sound, {}, {}, "Hokuto", archive.build)
                         : song::ConvertToMidi(*archive.sar, sound, {}, {}, "Hokuto");
}

song::DryRender RenderOf(const tools::CheckedArchive& archive, const std::shared_ptr<const song::Sequence>& sequence,
                         double seconds)
{
    const std::atomic<bool> cancel = false;
    const song::Played played = archive.ds    ? song::Played(archive.ds)
                                : archive.wii ? song::Played(archive.wii)
                                              : song::Played(archive.sar);
    return song::RenderDry(played, sequence, song::Mixing::kStrict, seconds, cancel);
}

// The notes that a MIDI file starts and the new sound doesn't.
struct Missing
{
    std::size_t total = 0;
    std::size_t missing = 0;
    std::size_t in_first_tick = 0;
    std::string first;
};

// The level of `frames` frames of `samples`, interleaved stereo, from frame `from`, in dB below full scale.
double Level(const std::vector<float>& samples, std::size_t from, std::size_t frames)
{
    double sum = 0.0;
    for (std::size_t i = from * 2; i < (from + frames) * 2 && i < samples.size(); i++)
    {
        sum += static_cast<double>(samples[i]) * samples[i];
    }

    return sum > 0.0 ? 10.0 * std::log10(sum / static_cast<double>(frames * 2)) : -200.0;
}

// Removes from `played` a note of `key` that starts at `tick`, a tick later or a tick earlier, the first found in that
// order. Returns whether there was one.
bool TakePlayed(std::multiset<std::pair<uint32_t, uint8_t>>& played, uint32_t tick, uint8_t key)
{
    for (const uint32_t near : {tick, tick + 1, tick > 0 ? tick - 1 : tick})
    {
        const auto note = played.find({near, key});
        if (note != played.end())
        {
            played.erase(note);
            return true;
        }
    }

    return false;
}

// Prints each note of `played` that starts within 8 ticks of `tick`, as its start and key.
void PrintPlayedNear(const std::multiset<std::pair<uint32_t, uint8_t>>& played, uint32_t tick)
{
    for (const auto& [start, key] : played)
    {
        if (start + 8 >= tick && start <= tick + 8)
        {
            std::printf(" %u/%d", start, key);
        }
    }
}

// The tick of `read` at tick `midi_tick` of the MIDI file `file`, to the nearest tick.
uint32_t TicksOf(const song::Sequence& read, const seqlib::midi::File& file, uint64_t midi_tick)
{
    return static_cast<uint32_t>((midi_tick * static_cast<uint64_t>(read.timebase) + file.division / 2) /
                                 file.division);
}

// The earliest `loopEnd` marker of the MIDI file `file`, in the ticks of `read`, or `end` where no marker comes sooner.
uint32_t EndAtLoopMarkers(const seqlib::midi::File& file, const song::Sequence& read, uint32_t end)
{
    for (const seqlib::midi::Track& track : file.tracks)
    {
        for (const seqlib::midi::Event& event : track.events)
        {
            static constexpr std::string_view kLoopEnd = "loopEnd";
            if (event.bytes.size() >= 3 + kLoopEnd.size() && event.bytes[0] == 0xFF && event.bytes[1] == 0x06 &&
                std::string_view(
                    reinterpret_cast<const char*>(event.bytes.data()) + event.bytes.size() - kLoopEnd.size(),
                    kLoopEnd.size()) == kLoopEnd)
            {
                end = std::min(end, TicksOf(read, file, event.tick));
            }
        }
    }

    return end;
}

// The notes that the MIDI file `midi` starts before the end of `read`, read by itself, that `read` doesn't start at the
// nearest tick or a tick from it. The file's `loopEnd` marker ends them too: past it, the file plays on as the game's
// sound did, and the new sound goes round its loop. A note that lasts a tick at most counts apart: the model can cut it
// in the tick that it starts, and such a note shows on neither side. With `list`, a line shows each note that counts as
// missing.
Missing MissingNotes(const std::vector<uint8_t>& midi, const song::Sequence& read, bool list)
{
    const seqlib::midi::File file = seqlib::midi::Read(midi);
    std::multiset<std::pair<uint32_t, uint8_t>> played;
    uint32_t end = read.length;
    for (const song::Track& track : read.tracks)
    {
        for (const song::Note& note : track.notes)
        {
            played.emplace(note.start, note.key);
            end = std::max(end, note.start + 1);
        }
    }

    end = read.loop ? read.loop->end : end;
    end = EndAtLoopMarkers(file, read, end);

    Missing result;
    for (const seqlib::midi::Track& track : file.tracks)
    {
        for (auto event = track.events.begin(); event != track.events.end(); ++event)
        {
            if (event->bytes.size() < 3 || (event->bytes[0] & 0xF0) != 0x90 || event->bytes[2] == 0)
            {
                continue;
            }
            const uint32_t tick = TicksOf(read, file, event->tick);
            if (tick >= end)
            {
                continue;
            }
            result.total++;
            if (TakePlayed(played, tick, event->bytes[1]))
            {
                continue;
            }

            // The note's end: the next note-off of its channel and key.
            const auto ends_note = [&event](const seqlib::midi::Event& next)
            {
                const uint8_t type = next.bytes.empty() ? 0 : next.bytes[0] & 0xF0;
                return next.bytes.size() >= 3 && (next.bytes[0] & 0x0F) == (event->bytes[0] & 0x0F) &&
                       next.bytes[1] == event->bytes[1] && (type == 0x80 || (type == 0x90 && next.bytes[2] == 0));
            };
            const auto off = std::find_if(event + 1, track.events.end(), ends_note);
            if (off != track.events.end() && TicksOf(read, file, off->tick) <= tick + 1)
            {
                result.in_first_tick++;
                continue;
            }
            if (result.first.empty())
            {
                result.first = "key " + std::to_string(event->bytes[1]) + " at tick " + std::to_string(tick);
            }
            if (list)
            {
                std::printf("  key %d, velocity %d, channel %d, tick %u, with", event->bytes[1], event->bytes[2],
                            event->bytes[0] & 0x0F, tick);
                PrintPlayedNear(played, tick);
                std::printf("\n");
            }
            result.missing++;
        }
    }

    return result;
}

// Imports `conversion`, the conversion of sound `name` of `archive`: the banks of its SoundFont into a new file of
// `engine`, then its MIDI file as a sequence sound that uses them. Returns the saved form of the file, or nothing when
// the import refuses the sound. `label` names the archive in the lines.
std::optional<format::File> ImportSound(const std::string& label, const tools::CheckedArchive& archive,
                                        format::Engine engine, const std::string& name,
                                        const export_::Conversion& conversion, Totals& totals)
{
    format::File saved;
    try
    {
        // The SoundFont's banks, then the sound in their slots.
        format::File file = format::NewFile(engine);
        const edit::SoundFontImport font = edit::ReadSoundFontImport(file, conversion.sf2);
        file = edit::AddSoundFont(file, conversion.sf2, "BANK_CHECK");
        const uint32_t first_bank = archive.ds ? 1 : 0;
        std::vector<uint32_t> slots;
        std::optional<uint32_t> bank_0;
        for (std::size_t i = 0; i < font.midi_banks.size(); i++)
        {
            const auto midi_bank = static_cast<uint32_t>(font.midi_banks[i]);
            const uint32_t number = first_bank + static_cast<uint32_t>(i);
            if (midi_bank == 0)
            {
                bank_0 = number;
            }
            if (midi_bank < kSlots)
            {
                slots.resize(std::max<std::size_t>(slots.size(), midi_bank + 1), sar::kNoItem);
                slots[midi_bank] = sar::ItemId(sar::kBankItem, number);
            }
        }

        const edit::MidiSong song_of_file =
            edit::ImportedSong(conversion.midi, "check", archive.ds ? 48 : 96, {}, archive.ds ? "DS" : "NintendoWare");
        totals.moved += song_of_file.moved;
        for (const std::string& warning : song_of_file.warnings)
        {
            // The count that a warning starts with stays out of its kind.
            const std::size_t words = warning.find_first_not_of("0123456789");
            totals.warnings[warning.substr(words == std::string::npos ? 0 : words)]++;
        }
        const auto selects_bank = [](const std::string& warning)
        {
            return warning.find("control change 0.") != std::string::npos;
        };
        if (std::ranges::any_of(song_of_file.warnings, selects_bank))
        {
            totals.other_banks++;
        }

        if (archive.ds)
        {
            const edit::DsMidiFile midi = edit::DsFileOfMidi(conversion.midi, "check.mid");
            saved = edit::AddDsSequenceSound(file, "SEQ_CHECK", midi.file,
                                             {.bank = static_cast<uint16_t>(bank_0.value_or(0))});
        }
        else
        {
            const edit::Incoming incoming = edit::ReadIncomingMidi(conversion.midi, "check.mid", edit::ConsoleOf(file));
            const format::File added =
                archive.wii ? edit::AddWiiSequenceSound(
                                  file, std::nullopt, "SEQ_CHECK", incoming.sequence, edit::kEmptySongStart, {},
                                  std::vector<uint32_t>{sar::ItemId(sar::kBankItem, bank_0.value_or(0))})
                            : edit::AddSequenceSound(file, std::nullopt, "SEQ_CHECK", incoming.sequence,
                                                     edit::kEmptySongStart, {}, slots);
            const format::File opened = edit::WithNewModes(added, std::vector<uint32_t>{0});
            const bool timeline = edit::IsTimeline(opened, 0);
            totals.timelines += timeline ? 1 : 0;
            if (!timeline && !archive.wii)
            {
                std::printf("%s: %s doesn't open as a timeline\n", label.c_str(), name.c_str());
                totals.failed++;
            }
            saved = edit::FoldTimelines(opened);
        }
    }
    catch (const std::invalid_argument& error)
    {
        const std::string message = error.what();
        if (std::ranges::find(kExpectedImportRefusals, message) != kExpectedImportRefusals.end())
        {
            totals.no_notes++;
        }
        else
        {
            std::printf("%s: %s doesn't come in. %s\n", label.c_str(), name.c_str(), message.c_str());
            totals.refusals[message]++;
            totals.failed++;
        }
        return std::nullopt;
    }

    return saved;
}

// Counts the places where the levels of `old_render` and `new_render`, the game's sound and its import, differ
// (`comparison`), and prints a line for a run or a difference above 3 dB. With `list`, a line shows each place and the
// levels of each half second. `draws` says whether the sound draws random values.
void CountLevelPlaces(const song::LevelComparison& comparison, const song::DryRender& old_render,
                      const song::DryRender& new_render, const std::string& label, const std::string& name, bool draws,
                      bool list, Totals& totals)
{
    bool run = false;
    double worst = 0.0;
    for (const song::LevelPlace& place : comparison.places)
    {
        run = run || place.run;
        worst = std::abs(place.db) > std::abs(worst) ? place.db : worst;
        if (list)
        {
            std::printf("  %+.1f dB%s at %.2f s for %.2f s\n", place.db, place.run ? ", a run," : "", place.start,
                        place.seconds);
        }
    }
    if (list)
    {
        // Each half second's level of both renders, to show where the import differs.
        const auto half = static_cast<std::size_t>(old_render.rate / 2);
        const std::size_t frames = std::max(old_render.samples.size(), new_render.samples.size()) / 2;
        for (std::size_t from = 0; from < frames; from += half)
        {
            std::printf("  %6.1f s: the game's %6.1f dBFS, the import's %6.1f dBFS\n",
                        static_cast<double>(from) / old_render.rate, Level(old_render.samples, from, half),
                        Level(new_render.samples, from, half));
        }
    }
    totals.random_findings += draws ? 1 : 0;
    if (run)
    {
        totals.runs++;
    }
    else if (std::abs(worst) > 3.0)
    {
        totals.beyond_3db++;
    }
    else
    {
        totals.spans++;
    }
    if (!draws && (run || std::abs(worst) > 3.0))
    {
        std::printf("%s: %s is %+.1f dB off at worst%s, gain %+.1f dB\n", label.c_str(), name.c_str(), worst,
                    run ? ", in a run" : "", comparison.gain_db);
    }
}

// Checks `saved`, the import of sound `sound` of `archive` from the MIDI file `midi`: it saves and opens again, plays
// every note of the MIDI file, and plays at the levels of the game's sound for `seconds`.
void CheckImport(const std::string& label, const tools::CheckedArchive& archive, uint32_t sound,
                 const std::string& name, const std::vector<uint8_t>& midi, const format::File& saved, bool draws,
                 double seconds, bool list, Totals& totals)
{
    // The new sound, and what it plays.
    totals.sounds++;
    totals.random += draws ? 1 : 0;
    if (!tools::SavesAndOpens(saved))
    {
        std::printf("%s: %s doesn't save and open again\n", label.c_str(), name.c_str());
        totals.failed++;
    }
    const tools::CheckedArchive built = tools::BuildChecked(saved, "check");
    const auto after = std::make_shared<const song::Sequence>(built.Read(0));
    const Missing missing = MissingNotes(midi, *after, list);
    totals.notes += missing.total;
    totals.missing += missing.missing;
    totals.in_first_tick += missing.in_first_tick;
    if (missing.missing > 0)
    {
        std::printf("%s: %s is missing %zu of the %zu notes of the MIDI file. The first: %s\n", label.c_str(),
                    name.c_str(), missing.missing, missing.total, missing.first.c_str());
        totals.failed++;
    }

    const auto before = std::make_shared<const song::Sequence>(archive.Read(sound));
    const song::DryRender old_render = RenderOf(archive, before, seconds);
    const song::DryRender new_render = RenderOf(built, after, seconds);
    const song::LevelComparison comparison =
        song::CompareLevels(old_render.samples, new_render.samples, old_render.rate);
    if (comparison.windows == 0)
    {
        totals.silent++;
        return;
    }
    if (comparison.places.empty())
    {
        totals.no_finding++;
    }
    else
    {
        CountLevelPlaces(comparison, old_render, new_render, label, name, draws, list, totals);
    }
    totals.gains += std::abs(comparison.gain_db) > 3.0 ? 1 : 0;
}

void CheckSound(const std::string& label, const tools::CheckedArchive& archive, format::Engine engine, uint32_t sound,
                bool draws, double seconds, bool list, Totals& totals)
{
    const std::string name = archive.NameOf(sound);

    // The conversion throws std::runtime_error for a sound that it refuses. A refusal fails, with a line. Anything else
    // fails too.
    export_::Conversion conversion;
    try
    {
        conversion = ConversionOf(archive, sound);
    }
    catch (const std::runtime_error& error)
    {
        std::printf("%s: %s doesn't convert. %s\n", label.c_str(), name.c_str(), error.what());
        totals.refusals[std::string("conversion: ") + error.what()]++;
        totals.failed++;
        return;
    }

    const std::optional<format::File> saved = ImportSound(label, archive, engine, name, conversion, totals);
    if (!saved)
    {
        return;
    }

    CheckImport(label, archive, sound, name, conversion.midi, *saved, draws, seconds, list, totals);
}

void CheckArchive(const std::filesystem::path& path, std::size_t every, double seconds,
                  const std::optional<std::string>& only, bool list, Totals& totals)
{
    std::vector<uint8_t> bytes;
    if (!xsflib::ReadFile(path, bytes))
    {
        throw std::runtime_error("can't read the archive");
    }
    const std::string label = xsflib::Utf8(path.filename());
    const tools::CheckedArchive archive = tools::OpenChecked(path, bytes);
    if (!archive.sar && !archive.wii && !archive.ds)
    {
        throw std::runtime_error("not a sound archive of the 3DS, the Wii U, the Switch, the Wii or the DS");
    }

    const format::File game = tools::ImportChecked(bytes, archive);
    const std::vector<std::optional<edit::SoundPlace>> places = tools::PlacesOfChecked(game, bytes, archive);

    totals.archives++;
    std::size_t counted = 0;
    for (uint32_t sound = 0; sound < archive.SoundCount(); sound++)
    {
        if (!archive.IsSequence(sound) || (only && archive.NameOf(sound) != *only) || (!only && counted++ % every != 0))
        {
            continue;
        }

        // A sound that fails leaves the others to run.
        try
        {
            bool draws = false;
            if (sound < places.size() && places[sound])
            {
                const edit::SoundPlace& where = *places[sound];
                const uint32_t start = where.start_field ? game.native->fields.at(*where.start_field).to.offset : 0;
                draws = DrawsRandomNumbers(*game.sequences.at(where.sequence), start);
            }
            CheckSound(label, archive, game.engine, sound, draws, seconds, list, totals);
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s: %s\n", label.c_str(), archive.NameOf(sound).c_str(), error.what());
            totals.failed++;
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    const std::vector<std::string> arguments = xsflib::console::Arguments(argc, argv);
    std::size_t every = 1;
    double seconds = 10.0;
    std::optional<std::string> only;
    bool list = false;
    std::vector<std::filesystem::path> paths;
    for (std::size_t i = 1; i < arguments.size(); i++)
    {
        const std::string& argument = arguments[i];
        if (argument == "--every" && i + 1 < arguments.size())
        {
            every = std::max<std::size_t>(std::stoul(arguments[++i]), 1);
        }
        else if (argument == "--seconds" && i + 1 < arguments.size())
        {
            seconds = std::stod(arguments[++i]);
        }
        else if (argument == "--sound" && i + 1 < arguments.size())
        {
            only = arguments[++i];
        }
        else if (argument == "--notes")
        {
            list = true;
        }
        else
        {
            paths.push_back(xsflib::PathFromUtf8(argument));
        }
    }
    if (paths.empty())
    {
        std::fprintf(stderr,
                     "usage: hokuto_check_midi_imports [--every n] [--seconds s] [--sound name] [--notes] "
                     "<archive>...\n");
        return 2;
    }

    Totals totals;
    for (const std::filesystem::path& path : paths)
    {
        try
        {
            CheckArchive(path, every, seconds, only, list, totals);
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s\n", xsflib::Utf8(path.filename()).c_str(), error.what());
            totals.failed++;
        }
    }

    std::printf(
        "%zu archives: %zu sounds came back, %zu hold no notes. %zu timelines. %zu notes, %zu missing, %zu more "
        "of a tick or less. %zu select a bank past the slots. %zu silent, %zu without a finding, %zu spans of 1 to 3 "
        "dB, %zu beyond 3 dB, %zu runs. %zu draw random numbers, %zu of them with findings. %zu overall gains beyond 3 "
        "dB. %zu events moved. %zu failed.\n",
        totals.archives, totals.sounds, totals.no_notes, totals.timelines, totals.notes, totals.missing,
        totals.in_first_tick, totals.other_banks, totals.silent, totals.no_finding, totals.spans, totals.beyond_3db,
        totals.runs, totals.random, totals.random_findings, totals.gains, totals.moved, totals.failed);
    for (const auto& [text, count] : totals.refusals)
    {
        std::printf("refused %zu, unexpectedly: %s\n", count, text.c_str());
    }
    for (const auto& [text, count] : totals.warnings)
    {
        std::printf("warned %zu: %s\n", count, text.c_str());
    }

    return totals.failed == 0 ? 0 : 1;
}
