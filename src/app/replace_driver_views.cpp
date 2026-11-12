// SPDX-License-Identifier: MIT

// The views of the Replace windows on DSE for a sound, a bank and a wave, and of the imports of a MIDI file and a
// SoundFont on the GBA.

#include "app/replace_driver_views.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/dse/formats.h"
#include "ntdlib/mp2k/song.h"
#include "ntdlib/sequence_export/dse/sequence_input.h"

#include "app/export_warnings.h"
#include "app/item_views.h"
#include "app/replace_common.h"
#include "app/replace_view.h"
#include "edit/bytecode.h"
#include "edit/ds_waves.h"
#include "edit/dse_files.h"
#include "edit/files.h"
#include "edit/gba_midi.h"
#include "edit/gba_sound_fonts.h"
#include "edit/gba_voices.h"
#include "edit/replace.h"
#include "edit/sfz_imports.h"
#include "format/archive.h"
#include "format/file.h"
#include "format/kinds.h"
#include "song/archive.h"
#include "song/dse.h"
#include "song/gba.h"
#include "song/items.h"
#include "song/sequence.h"

namespace hokuto::app
{
namespace
{

using replace_common::AddWaveLoopWarning;
using replace_common::DsSizeLine;
using replace_common::DsWaveSourceLine;
using replace_common::FillDrumRows;
using replace_common::kWaveMemory;
using replace_common::MidiSource;
using replace_common::OfferDsRates;
using replace_common::OfferPresentLoop;
using replace_common::PairedDrumNotes;
using replace_common::PairedProgram;
using replace_common::RateChangeText;
using replace_common::RatesOf;
using replace_common::WavePlayersText;
using replace_common::WaveSizeLine;
using replace_common::WavWarnings;

// Play plays each of `sounds` of the set `set`: their names go in the menu beside it.
void OfferDseSounds(ReplaceView& view, const song::DseSet& set, std::vector<uint32_t> sounds)
{
    view.playable = true;
    view.play_sounds = std::move(sounds);
    for (const uint32_t sound : view.play_sounds)
    {
        view.plays.push_back(song::DseSoundName(set, sound));
    }
}

// The programs that contain an instrument in `bank`, a DSE bank, and the waves that it contains or names.
std::pair<std::size_t, std::size_t> DseBankCounts(const ntdlib::dse::WaveBank& bank)
{
    const std::size_t programs =
        bank.instruments ? static_cast<std::size_t>(std::ranges::count_if(
                               bank.instruments->programs, [](const auto& program) { return program.has_value(); }))
                         : 0;
    const auto waves =
        static_cast<std::size_t>(std::ranges::count_if(bank.waves, [](const auto& wave) { return wave.has_value(); }));
    return {programs, waves};
}

// The programs that the presets of an import take in a voice group: the list of the presets and their programs, the
// count of the presets left out, and the count of the programs whose voices no song played.
struct GbaPresetPlaces
{
    std::string places;
    std::size_t without = 0;
    std::size_t given_way = 0;
};

// Places the presets of `voices` in the voice group at `group`.
GbaPresetPlaces PlaceGbaPresets(const song::GbaRom& rom, uint32_t group, const edit::GbaSoundFontVoices& voices)
{
    const std::size_t presets = voices.presets.size();
    // The program that each preset gets.
    const std::vector<uint32_t> free = edit::FreeGbaPrograms(rom, group);
    const std::vector<uint32_t> unplayed = edit::UnplayedGbaPrograms(rom, group);
    const std::vector<std::optional<uint32_t>> programs = edit::GbaSoundFontPrograms(voices, free, unplayed);
    std::string places;
    std::size_t without = 0;
    std::size_t given_way = 0;
    for (std::size_t i = 0; i < presets; i++)
    {
        if (!programs[i])
        {
            ++without;
            continue;
        }
        places +=
            (places.empty() ? "" : ", ") + voices.presets[i].name + " (program " + std::to_string(*programs[i]) + ")";
        given_way += std::ranges::find(unplayed, *programs[i]) != unplayed.end();
    }

    return {std::move(places), without, given_way};
}

// Adds the warning that the voice group at `from` moves to `to`, with the songs other than song `song_number` that
// play it.
void WarnMovedGroup(const song::GbaRom& rom, int song_number, uint32_t from, uint32_t to,
                    std::vector<std::string>& warnings)
{
    const std::vector<song::GbaVoiceGroup> groups = song::GbaVoiceGroups(rom);
    const auto played = std::ranges::find(groups, from, &song::GbaVoiceGroup::address);
    std::vector<std::string> others;
    for (std::size_t i = 0; played != groups.end() && i < played->songs.size(); i++)
    {
        if (played->songs[i] != song_number)
        {
            others.push_back(song::GbaSongName(rom, played->songs[i]));
        }
    }
    warnings.push_back(song::GbaVoiceGroupName(from) + " moves to free space and becomes " +
                       song::GbaVoiceGroupName(to) + "." +
                       (others.empty() ? "" : " Other songs that play it: " + NameList(others) + "."));
}

// The voices of a voice group: one for each program.
constexpr std::size_t kGroupVoices = 128;

// The 128 voices of the voice group at `group`, none where there's no group. Names each voice in `view`.
std::vector<ntdlib::mp2k::Voice> GbaGroupVoices(const song::GbaRom& rom, std::optional<uint32_t> group,
                                                ReplaceView& view)
{
    std::vector<ntdlib::mp2k::Voice> group_voices(kGroupVoices);
    for (std::size_t i = 0; i < group_voices.size(); i++)
    {
        const bool read =
            group && ntdlib::mp2k::ReadVoice(rom.rom, *group + static_cast<uint32_t>(i) * ntdlib::mp2k::kVoiceSize,
                                             group_voices[i]);
        view.gba_voices.push_back("Voice " + std::to_string(i) +
                                  (read ? ": " + song::GbaVoiceKind(group_voices[i]) : ""));
    }

    return group_voices;
}

// Adds the warnings about song `number` after the import: a song that moves where the ROM has no room, and the notes
// that lose their channel.
void AddGbaImportWarnings(ReplaceView& view, const ReplaceSource& source, const song::GbaRom& rom, uint32_t number)
{
    // A song that outgrows its place moves after the ROM's data. Where the ROM has no room there, the export's
    // choice decides.
    const std::vector<uint32_t> moved = format::MovedSongs(*view.result);
    if (number < source.places.size() && source.places[number] &&
        std::ranges::find(moved, source.places[number]->sequence) != moved.end() && format::LacksRoom(*view.result))
    {
        view.warnings.push_back(
            "The new commands no longer fit in the original space, and there's no free space after the ROM data. "
            "File > Export places such songs in runs of filler bytes inside the ROM, or leaves out their edits.");
    }

    // The notes that lose their channel. The Export window and the Key Editor show the same notes.
    const std::optional<song::Collisions> collisions =
        edit::GbaSongCollisions(*view.result, rom, static_cast<int>(number));
    if (collisions && (collisions->cut > 0 || collisions->unstarted > 0))
    {
        view.warnings.push_back("The new song has " + CollisionText(*collisions, "the GBA") + ".");
        view.warnings.push_back(ChannelsNote("the GBA"));
    }
}

// Warns in `view` where the presets of an import go in the voice group at `group` (`placed`). With `sfz`, the lines
// name instruments, and otherwise presets.
void WarnPlacedPresets(ReplaceView& view, const GbaPresetPlaces& placed, uint32_t group, bool sfz)
{
    const std::string one = sfz ? "instrument" : "preset";
    const std::string many = one + "s";
    if (!placed.places.empty())
    {
        view.warnings.push_back(std::string(sfz ? "Instruments" : "Presets") + " added to " +
                                song::GbaVoiceGroupName(group) + ": " + placed.places + ".");
    }
    if (placed.without > 0)
    {
        view.warnings.push_back(std::to_string(placed.without) + " " +
                                (placed.without == 1 ? one + " is" : many + " are") +
                                " left out because the songs use all the other programs of the voice group.");
    }
    if (placed.given_way > 0)
    {
        view.warnings.push_back(placed.given_way == 1
                                    ? std::string("1 program had a voice that no song uses. A new voice replaces it.")
                                    : std::to_string(placed.given_way) +
                                          " programs had voices that no song uses. New voices replace them.");
    }
}

} // namespace

ReplaceView MakeDseSoundReplaceView(const ReplaceSource& source)
{
    const song::DseSet& set = *source.played.Dse();
    const edit::DseIncomingSong& song = *source.dse_song;
    const std::string name = song::DseSoundName(set, source.sound);
    ReplaceView view;
    view.sequence = false;
    view.title = "Replace " + name;
    view.heading = source.midi.empty() ? "Song file" : "MIDI file";
    const std::size_t tracks = edit::DseSongTracks(song);
    view.source = source.midi.empty() ? source.file_name + ": DSE song file, " + Grouped(tracks) +
                                            (tracks == 1 ? " track, " : " tracks, ") + SizeText(source.incoming.size)
                                      : MidiSource(source);

    // A row for each program that the tracks select, with the programs of the sound's first bank.
    const ntdlib::sequence_export::dse::SequenceInput& input = set.sounds.at(source.sound);
    const std::string bank = input.banks.empty() ? std::string() : song::DseBankName(*input.banks.front());
    const std::vector<uint32_t> choices =
        input.banks.empty() ? std::vector<uint32_t>() : song::DseBankPrograms(*input.banks.front());
    for (const uint32_t program : edit::DseSongPrograms(song))
    {
        ProgramRow& row = view.programs.emplace_back();
        row.program = {0, static_cast<int>(program)};
        const auto found = source.midi_programs.find(row.program);
        row.target = found != source.midi_programs.end() ? found->second : static_cast<int>(program);
        row.bank = bank;
        row.choices = choices;
    }
    if (!source.midi.empty() && !input.banks.empty())
    {
        FillDrumRows(view, source, bank, choices);
    }

    const edit::ProgramMap programs = ChosenPrograms(view);
    view.warnings = edit::DseSoundWarnings(set, source.sound, song, programs);
    OfferDseSounds(view, set, {source.sound});
    try
    {
        view.result = edit::ReplaceDseSound(*source.file, set, source.sound, song, programs);
    }
    catch (const std::invalid_argument& error)
    {
        view.error = error.what();
    }

    return view;
}

ReplaceView MakeDseBankReplaceView(const ReplaceSource& source)
{
    const song::DseSet& set = *source.played.Dse();
    const ntdlib::dse::WaveBank& incoming = *source.dse_bank;
    const ntdlib::dse::WaveBank& present = *set.banks.at(source.sound).bank;
    const auto [programs, waves] = DseBankCounts(incoming);
    ReplaceView view;
    view.sequence = false;
    view.title = "Replace " + song::DseBankName(present);
    view.heading = "Bank file";
    view.source = source.file_name + ": DSE wave bank file, " + Grouped(programs) +
                  (programs == 1 ? " program, " : " programs, ") + Grouped(waves) + (waves == 1 ? " wave" : " waves") +
                  (incoming.samples ? ", with samples" : "");

    // The sounds that play the bank, what the new bank changes for them, and the sizes. Every file of the set that
    // contains the bank gets the new one.
    const std::vector<uint32_t> sounds = edit::DseSoundsOfBank(set, source.sound);
    if (!sounds.empty())
    {
        std::vector<std::string> names;
        for (const uint32_t sound : sounds)
        {
            names.push_back(song::DseSoundName(set, sound));
        }
        view.warnings.push_back(NameList(names) + (names.size() == 1 ? " uses" : " use") +
                                " this bank and will play the new one.");
    }
    const std::vector<std::string> warnings = edit::DseBankWarnings(set, source.sound, incoming);
    view.warnings.insert(view.warnings.end(), warnings.begin(), warnings.end());
    view.warnings.push_back(DsSizeLine(present.Write().size(), incoming.Write().size(), "bank"));
    OfferDseSounds(view, set, sounds);
    try
    {
        view.result = edit::ReplaceDseBank(*source.file, set, source.sound, incoming);
    }
    catch (const std::invalid_argument& error)
    {
        view.error = error.what();
    }

    return view;
}

ReplaceView MakeDseWaveReplaceView(const ReplaceSource& source, uint32_t wave, const edit::IncomingWave& incoming,
                                   const edit::WaveChanges& changes, std::optional<format::File> result,
                                   std::string error)
{
    // The wave, and the WAV file that replaces it, in one of the DS's encodings.
    const song::DseSet& set = *source.played.Dse();
    ReplaceView view;
    view.sequence = false;
    view.title =
        "Replace " + song::DseItemName(set, {.kind = song::ItemKind::kWave, .index = source.sound, .wave = wave});
    view.heading = "WAV file";
    view.source = DsWaveSourceLine(source.file_name, incoming, true);
    OfferDsRates(view, incoming, changes.rate, edit::kHighestDsRate);
    OfferPresentLoop(view, incoming, true, changes);
    view.wave = incoming.bytes;
    view.result = std::move(result);
    view.error = std::move(error);

    // What plays the wave.
    if (changes.banks.empty())
    {
        view.warnings.push_back("No bank uses the wave.");
    }
    else
    {
        std::vector<std::string> banks;
        for (const uint32_t bank : changes.banks)
        {
            banks.push_back(song::DseBankName(*set.banks.at(bank).bank));
        }
        view.warnings.push_back(WavePlayersText(banks));
    }

    // The loop, the channels, the rate and the sizes.
    AddWaveLoopWarning(changes.loop, incoming.loop, view.warnings);
    WavWarnings(incoming, view.warnings, false, true);
    if (incoming.channels == 2)
    {
        view.warnings.push_back("The WAV file is stereo, but DS waves are mono. The two channels are mixed down.");
    }
    if (changes.rate != incoming.rate)
    {
        view.warnings.push_back(RateChangeText(changes.rate, incoming.rate) +
                                " The tuning of each split that plays the wave changes to keep its pitch.");
    }

    // A bank keeps a wave's samples alone. Its table contains the wave's parameters.
    view.warnings.push_back(WaveSizeLine(changes.encoding, changes.size, incoming.encoding,
                                         edit::DseWaveSize(incoming.bytes), kWaveMemory));

    return view;
}

ReplaceView MakeGbaMidiImportView(const ReplaceSource& source, std::span<const uint8_t> midi, const song::GbaRom& rom,
                                  std::span<const int> songs, std::size_t song, const edit::GbaVoiceMap& voices)
{
    ReplaceView view;
    const bool assembly = edit::IsGbaAssembly(midi);
    view.gba_assembly = assembly;
    view.title = assembly ? "Import Assembly File" : "Import MIDI File";
    view.action = "Import";
    view.heading = assembly ? "Assembly file" : "MIDI file";
    view.source = source.file_name + (assembly ? ": song in assembly, " : ": MIDI file, ") + SizeText(midi.size());
    view.sequence = false;
    view.playable = true;
    for (const int number : songs)
    {
        view.gba_songs.push_back(song::GbaSongName(rom, number));
    }
    view.gba_song = std::min(song, songs.size() - 1);
    const auto number = static_cast<uint32_t>(songs[view.gba_song]);

    // The 128 voices of the song's voice group, by their numbers and kinds.
    const std::optional<uint32_t> group = edit::GbaVoiceGroupOf(*source.file, source.places, number);
    const std::vector<ntdlib::mp2k::Voice> group_voices = GbaGroupVoices(rom, group, view);

    // Each instrument gets the chosen voice, or else the proposed one. The import then reads the MIDI file again with
    // the voices.
    try
    {
        const std::vector<edit::GbaMidiInstrument> instruments =
            assembly ? std::vector<edit::GbaMidiInstrument>() : edit::GbaSongOfMidi(midi, {}).instruments;
        edit::GbaVoiceMap map =
            group ? edit::ProposedGbaVoices(*source.file, *group, instruments, group_voices) : edit::GbaVoiceMap();
        for (const edit::GbaMidiInstrument& instrument : instruments)
        {
            const auto chosen = voices.find(instrument);
            if (chosen != voices.end())
            {
                map[instrument] = chosen->second;
            }
            view.gba_rows.push_back({.instrument = instrument, .voice = map[instrument]});
        }

        const int note_arguments = edit::ConsoleOf(*source.file).note_arguments;
        const edit::GbaMidiSong imported =
            assembly ? edit::GbaSongOfAssembly(
                           std::string_view(reinterpret_cast<const char*>(midi.data()), midi.size()), note_arguments)
                     : edit::GbaSongOfMidi(midi, map, note_arguments);
        view.result = edit::WithGbaMidiSong(*source.file, source.places, number, imported);
        view.warnings = imported.warnings;

        AddGbaImportWarnings(view, source, rom, number);
    }
    catch (const std::exception& error)
    {
        view.error = format::AsSentence(error.what());
    }

    return view;
}

ReplaceView MakeGbaSoundFontImportView(const ReplaceSource& source, const edit::GbaSoundFontVoices& voices,
                                       const song::GbaRom& rom, std::span<const int> songs, std::size_t song,
                                       std::size_t rate)
{
    ReplaceView view;
    const bool sfz = edit::IsSfzName(source.file_name);
    view.title = sfz ? "Import SFZ Files" : "Import SoundFont";
    view.action = "Import";
    view.heading = sfz ? "SFZ files" : "SoundFont 2 file";

    // Every line names what the files contain: an SFZ file is an instrument, and a SoundFont contains presets.
    const std::string one = sfz ? "instrument" : "preset";
    const std::string many = one + "s";
    const std::size_t presets = voices.presets.size();
    view.source = source.file_name + ": " + std::to_string(presets) + " " + (presets == 1 ? one : many) +
                  ". Samples and tables take up " + SizeText(voices.bytes) + " of ROM.";
    view.sequence = false;
    for (const int number : songs)
    {
        view.gba_songs.push_back(song::GbaSongName(rom, number));
    }
    view.gba_song = std::min(song, songs.size() - 1);
    view.rates = RatesOf(voices.highest_rate);
    view.rate = std::min(rate, view.rates.size() - 1);
    view.first_rate_lower = true;

    const auto number = static_cast<uint32_t>(songs[view.gba_song]);
    const std::optional<uint32_t> group = edit::GbaVoiceGroupOf(*source.file, source.places, number);
    if (!group)
    {
        view.error =
            "Can't find the voice group for this song because the project keeps its commands as raw ROM "
            "bytes.";
        return view;
    }

    try
    {
        const GbaPresetPlaces placed = PlaceGbaPresets(rom, *group, voices);
        const edit::GbaGroupVersion version = edit::WithGbaSoundFont(*source.file, rom, *group, voices);
        view.result = version.file;
        view.gba_group = version.group;

        // Every song that plays a voice group whose table moves uses the new address of the table. Like the Undo
        // History, the line of the presets names the group where the import puts it.
        if (version.group != *group)
        {
            WarnMovedGroup(rom, songs[view.gba_song], *group, version.group, view.warnings);
        }
        WarnPlacedPresets(view, placed, version.group, sfz);
        view.warnings.insert(view.warnings.end(), voices.warnings.begin(), voices.warnings.end());
        view.warnings.push_back(
            "A new voice plays once a song selects its program. File > Import > Sequence… maps MIDI "
            "instruments to voices.");
        if (format::LacksRoom(*view.result))
        {
            view.warnings.push_back(
                "There's no free space after the ROM data for the new bytes. File > Export places "
                "them in runs of filler bytes inside the ROM, or leaves them out.");
        }
    }
    catch (const std::exception& error)
    {
        view.error = format::AsSentence(error.what());
    }

    return view;
}

void ReadDseMidi(ReplaceSource& source)
{
    edit::DseIncomingSong song = edit::DseSongOfMidi(source.midi, source.file_name, {}, source.drum_notes);
    source.incoming.midi = true;
    source.incoming.size = source.midi.size();
    source.incoming.warnings = song.warnings;
    source.dse_song = std::move(song);
}

void ProposeDseMidiMap(ReplaceSource& source, uint32_t bank)
{
    source.midi_bank = bank;
    source.midi_programs.clear();
    const auto map = source.file->bank_maps.find(bank);
    source.drum_notes = map != source.file->bank_maps.end() ? PairedDrumNotes(map->second, source.midi_drums.notes)
                                                            : format::MidiDrumNotes();

    // The programs' rows come from the tracks with the drum notes' pairings.
    ReadDseMidi(source);
    if (map == source.file->bank_maps.end())
    {
        return;
    }
    for (const uint32_t number : edit::DseSongPrograms(*source.dse_song))
    {
        const auto program = static_cast<int>(number);
        if (const std::optional<int> paired = PairedProgram(map->second, program))
        {
            source.midi_programs[edit::Program{0, program}] = *paired;
        }
    }
}

std::optional<uint32_t> DseFirstBank(const song::DseSet& set, uint32_t sound)
{
    if (sound >= set.sounds.size() || set.sounds[sound].banks.empty())
    {
        return std::nullopt;
    }

    const std::shared_ptr<const ntdlib::dse::WaveBank>& first = set.sounds[sound].banks.front();
    for (std::size_t place = 0; place < set.banks.size(); place++)
    {
        if (set.banks[place].bank == first)
        {
            return static_cast<uint32_t>(place);
        }
    }

    return std::nullopt;
}

} // namespace hokuto::app
