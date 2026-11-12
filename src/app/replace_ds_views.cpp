// SPDX-License-Identifier: MIT

// The views of the DS's Replace and Import windows: a sequence, a bank, a wave archive, a wave and a stream.

#include "app/replace_ds_views.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sdat/cut.h"
#include "ntdlib/sdat/formats.h"
#include "xsflib/files.h"

#include "app/export_warnings.h"
#include "app/item_views.h"
#include "app/replace_common.h"
#include "app/replace_view.h"
#include "edit/converter.h"
#include "edit/ds_files.h"
#include "edit/ds_new_items.h"
#include "edit/ds_streams.h"
#include "edit/ds_waves.h"
#include "edit/files.h"
#include "edit/midi_imports.h"
#include "edit/replace.h"
#include "format/file.h"
#include "song/ds.h"
#include "song/items.h"

namespace hokuto::app
{
namespace
{

using replace_common::AddProgramWarnings;
using replace_common::AddWaveLoopWarning;
using replace_common::DsSizeLine;
using replace_common::DsWaveSourceLine;
using replace_common::EncodingText;
using replace_common::FillDrumRows;
using replace_common::kWaveMemory;
using replace_common::LengthText;
using replace_common::LoopText;
using replace_common::MidiSource;
using replace_common::NameRefusal;
using replace_common::OfferDsRates;
using replace_common::OfferPresentLoop;
using replace_common::PairedDrumNotes;
using replace_common::PairedProgram;
using replace_common::RateChangeText;
using replace_common::WavePlayersText;
using replace_common::WaveSizeLine;
using replace_common::WavSourceLine;
using replace_common::WavWarnings;

// Play plays each of `sounds`: their names go in the menu beside it.
void OfferDsSounds(ReplaceView& view, const song::DsArchive& archive, std::vector<uint32_t> sounds)
{
    view.playable = true;
    view.play_sounds = std::move(sounds);
    for (const uint32_t sound : view.play_sounds)
    {
        view.plays.push_back(song::DsSoundName(archive, sound));
    }
}

// The file that becomes a DS stream, for the window's first line.
std::string DsStreamSourceLine(const std::string& file_name, const edit::IncomingWave& incoming, bool wav)
{
    if (wav)
    {
        return WavSourceLine(file_name, incoming);
    }

    std::string line = file_name + ": DS stream file, " + EncodingText(incoming.encoding) +
                       (incoming.channels == 1 ? ", mono, " : ", stereo, ") + Grouped(incoming.rate) + " Hz, " +
                       LengthText(incoming.frames, incoming.rate);
    if (incoming.loop)
    {
        line += LoopText(incoming.loop_start, incoming.frames);
    }

    return line;
}

} // namespace

ReplaceView MakeDsFileReplaceView(const ReplaceSource& source, edit::FileKind kind, std::span<const uint8_t> bytes)
{
    const song::DsArchive& archive = *source.played.Ds();
    const ntdlib::sdat::Archive& sdat_archive = archive.archive;
    const bool bank = kind == edit::FileKind::kBank;
    const uint32_t item = source.sound;
    const std::size_t count = bank ? edit::DsBankInstruments(bytes) : edit::DsWaveArchiveWaves(bytes);

    ReplaceView view;
    view.sequence = false;
    view.title = "Replace " + (bank ? song::DsBankName(archive, item) : song::DsWaveArchiveName(archive, item));
    view.heading = bank ? "Bank file" : "Wave archive file";
    view.source = source.file_name + (bank ? ": DS bank file, " : ": DS wave archive file, ") + Grouped(count) +
                  (bank ? (count == 1 ? " instrument" : " instruments") : (count == 1 ? " wave" : " waves"));
    view.warnings =
        bank ? edit::DsBankWarnings(archive, item, bytes) : edit::DsWaveArchiveWarnings(archive, item, bytes);
    OfferDsSounds(view, archive,
                  bank ? edit::DsSoundsOfBank(archive, item) : edit::DsSoundsOfWaveArchive(archive, item));

    const uint32_t file_id = bank ? sdat_archive.banks.at(item)->file_id
                                  : sdat_archive.wave_archives.at(item)->file_id & ntdlib::sdat::kFileIdBits;
    view.warnings.push_back(DsSizeLine(sdat_archive.File(file_id).size(), bytes.size(), "file"));
    try
    {
        view.result = bank ? edit::ReplaceDsBank(*source.file, archive, item, bytes)
                           : edit::ReplaceDsWaveArchive(*source.file, archive, item, bytes);
    }
    catch (const std::invalid_argument& error)
    {
        view.error = error.what();
    }

    return view;
}

namespace
{

// The rows of a MIDI file's programs and drum notes on the DS bank `bank`, and the warnings about the programs that are
// missing from the bank.
void FillDsMidiRows(ReplaceView& view, const ReplaceSource& source, uint32_t bank)
{
    const song::DsArchive& archive = *source.played.Ds();
    const std::string name = song::DsBankName(archive, bank);
    const std::vector<uint32_t> choices = song::DsBankPrograms(archive, bank);
    const edit::Selections selections = edit::ProgramsSelected(source.incoming.sequence, edit::kEmptySongStart);
    for (const edit::Program& program : selections.programs)
    {
        ProgramRow& row = view.programs.emplace_back();
        row.program = program;
        const auto found = source.midi_programs.find(program);
        row.target = found != source.midi_programs.end() ? found->second : program.number;
        row.bank = name;
        row.choices = choices;
    }
    FillDrumRows(view, source, name, choices);
    AddProgramWarnings(view.programs, selections.undecided, view.warnings, false);
}

} // namespace

ReplaceView MakeDsSequenceReplaceView(const ReplaceSource& source, std::span<const uint8_t> bytes)
{
    const song::DsArchive& archive = *source.played.Ds();
    const std::string name = song::DsSoundName(archive, source.sound);
    ReplaceView view;
    view.sequence = false;
    view.title = "Replace " + name;
    view.heading = "Sequence file";
    view.source =
        source.incoming.midi ? MidiSource(source) : source.file_name + ": DS sequence file, " + SizeText(bytes.size());
    view.warnings = source.incoming.warnings;

    // Every sound that plays the sequence gets the new commands.
    std::vector<uint32_t> sounds = edit::DsSoundsOfSequence(*source.file, archive, source.sound);
    if (sounds.size() > 1)
    {
        std::vector<std::string> others;
        for (std::size_t i = 1; i < sounds.size(); i++)
        {
            others.push_back(song::DsSoundName(archive, sounds[i]));
        }
        const bool one = others.size() == 1;
        view.warnings.push_back(NameList(others) + (one ? " uses" : " use") + " the same sequence and also " +
                                (one ? "gets" : "get") + " the new commands.");
    }
    if (const std::optional<uint32_t> bank = song::DsBankOf(archive, source.sound))
    {
        view.warnings.push_back(name + " keeps using bank " + song::DsBankName(archive, *bank) +
                                ". Programs missing from that bank will be silent.");
        if (!source.midi.empty())
        {
            FillDsMidiRows(view, source, *bank);
        }
    }
    if (!sounds.empty())
    {
        OfferDsSounds(view, archive, std::move(sounds));
    }

    try
    {
        view.result = edit::ReplaceDsSequence(*source.file, archive, source.sound, bytes);
    }
    catch (const std::invalid_argument& error)
    {
        view.error = error.what();
    }

    return view;
}

std::string FirstDsName(const song::DsArchive& archive, const std::string& file_name)
{
    if (!archive.archive.has_symbols)
    {
        return {};
    }

    const std::string stem = xsflib::Utf8(xsflib::PathFromUtf8(file_name).stem()).substr(0, 200);
    const std::vector<std::string> taken = edit::DsTakenNames(archive);
    std::string name = stem;
    for (int number = 2; std::ranges::binary_search(taken, name); number++)
    {
        name = stem + "_" + std::to_string(number);
    }

    return name;
}

std::vector<int> FirstDsRows(const ReplaceSource& source, edit::DsNewItem kind, std::span<const uint8_t> bytes)
{
    const song::DsArchive& archive = *source.played.Ds();
    std::vector<int> rows;
    if (kind == edit::DsNewItem::kSequence)
    {
        const std::vector<uint32_t> banks = song::DsBanks(archive);
        const std::optional<uint32_t> bank = song::DsBankOf(archive, source.sound);
        const auto found = bank ? std::ranges::find(banks, *bank) : banks.end();
        if (found != banks.end())
        {
            rows.push_back(static_cast<int>(found - banks.begin()));
        }
        else
        {
            // A new sound without a sound to copy uses the first bank with instruments. A new file's first bank is
            // empty. A SoundFont's import adds its melodic bank before its drums.
            int row = banks.empty() ? -1 : 0;
            for (std::size_t i = 0; i < banks.size(); i++)
            {
                if (!song::DsBankPrograms(archive, banks[i]).empty())
                {
                    row = static_cast<int>(i);
                    break;
                }
            }
            rows.push_back(row);
        }
    }
    else if (kind == edit::DsNewItem::kBank)
    {
        const std::vector<uint32_t> wave_archives = song::DsWaveArchives(archive);
        for (const edit::DsBankSlot& slot : edit::DsBankSlots(bytes))
        {
            int row = -1;
            for (std::size_t i = wave_archives.size(); i > 0 && row < 0; i--)
            {
                if (song::DsWaveCount(archive, wave_archives[i - 1]) >= slot.waves)
                {
                    row = static_cast<int>(i - 1);
                }
            }
            rows.push_back(row);
        }
    }

    return rows;
}

namespace
{

// Fills `view` for the import of a sequence file, `bytes`, `size` large. Returns the table entry that the new sound
// uses.
std::string FillDsSequenceImport(ReplaceView& view, const ReplaceSource& source, std::span<const uint8_t> bytes,
                                 const std::vector<int>& rows, const std::string& size)
{
    const song::DsArchive& archive = *source.played.Ds();
    const ntdlib::sdat::Archive& tables = archive.archive;
    std::string number;
    if (!ntdlib::sdat::ReadSequenceFile(bytes))
    {
        throw std::invalid_argument("Can't read the file as a DS sequence file.");
    }
    view.source = source.new_song        ? "An empty song to write in the editors."
                  : source.incoming.midi ? MidiSource(source)
                                         : source.file_name + ": DS sequence file, " + size;
    view.warnings = source.incoming.warnings;
    number = "SEQ_" + std::to_string(tables.sequences.size());
    for (const uint32_t model : source.models)
    {
        view.models.push_back(song::DsSoundName(archive, model));
    }
    view.model = static_cast<std::size_t>(std::ranges::find(source.models, source.sound) - source.models.begin());

    view.rows_heading = "Bank";
    const std::vector<uint32_t> banks = song::DsBanks(archive);
    for (const uint32_t each : banks)
    {
        view.wave_archives.push_back(song::DsBankName(archive, each));
    }
    const int chosen = rows.empty() ? -1 : rows.front();
    view.wave_archive_rows.push_back({"Bank", chosen});
    if (chosen >= 0 && static_cast<std::size_t>(chosen) < banks.size())
    {
        if (source.midi.empty())
        {
            const std::vector<std::string> warnings =
                edit::DsSequenceWarnings(archive, banks[static_cast<std::size_t>(chosen)], bytes);
            view.warnings.insert(view.warnings.end(), warnings.begin(), warnings.end());
        }
        else
        {
            FillDsMidiRows(view, source, banks[static_cast<std::size_t>(chosen)]);
        }
        if (source.new_song && song::DsBankPrograms(archive, banks[static_cast<std::size_t>(chosen)]).empty())
        {
            view.warnings.push_back(
                "The bank has no instruments. Notes stay silent until the bank has an instrument for their "
                "program. The editors show only notes that sound.");
        }
    }
    else
    {
        view.error = "The new sound requires a bank, but the archive has none that Hokuto can read.";
    }

    return number;
}

// Fills `view` for the import of a bank file, `bytes`, `size` large. Returns the table entry that the new bank uses.
std::string FillDsBankImport(ReplaceView& view, const ReplaceSource& source, std::span<const uint8_t> bytes,
                             const std::vector<int>& rows, const std::string& size)
{
    const song::DsArchive& archive = *source.played.Ds();
    const ntdlib::sdat::Archive& tables = archive.archive;
    std::string number;
    const std::vector<edit::DsBankSlot> slots = edit::DsBankSlots(bytes);
    const std::size_t instruments = edit::DsBankInstruments(bytes);
    view.source = source.new_bank ? "An empty bank to fill in the Bank Editor. Use Wave Archives… there to choose "
                                    "the wave archives it uses."
                                  : source.file_name + ": DS bank file, " + Grouped(instruments) +
                                        (instruments == 1 ? " instrument, " : " instruments, ") + size;
    number = "BANK_" + std::to_string(tables.banks.size());

    const std::vector<uint32_t> wave_archives = song::DsWaveArchives(archive);
    for (const uint32_t each : wave_archives)
    {
        view.wave_archives.push_back(song::DsWaveArchiveName(archive, each));
    }
    for (std::size_t row = 0; row < slots.size(); row++)
    {
        const int target = row < rows.size() ? rows[row] : -1;
        view.wave_archive_rows.push_back({"Slot " + std::to_string(slots[row].slot) + " (" + Grouped(slots[row].waves) +
                                              (slots[row].waves == 1 ? " wave)" : " waves)"),
                                          target});
    }

    view.warnings = edit::DsNewBankWarnings(archive, bytes, ChosenDsSlots(slots, rows, wave_archives));

    return number;
}

} // namespace

std::array<uint16_t, ntdlib::sdat::kWaveArchiveSlots> ChosenDsSlots(std::span<const edit::DsBankSlot> slots,
                                                                    std::span<const int> rows,
                                                                    std::span<const uint32_t> wave_archives)
{
    std::array<uint16_t, ntdlib::sdat::kWaveArchiveSlots> chosen;
    chosen.fill(ntdlib::sdat::kNoWaveArchive);
    for (std::size_t row = 0; row < slots.size() && row < rows.size(); row++)
    {
        const int target = rows[row];
        if (target >= 0 && static_cast<std::size_t>(target) < wave_archives.size() && slots[row].slot < chosen.size())
        {
            chosen[slots[row].slot] = static_cast<uint16_t>(wave_archives[static_cast<std::size_t>(target)]);
        }
    }

    return chosen;
}

namespace
{

// Sets the error of `view` for a refused import or a name that the new item can't take.
void CheckDsImportName(ReplaceView& view, const ReplaceSource& source, const std::string& name)
{
    const song::DsArchive& archive = *source.played.Ds();
    if (source.refused)
    {
        view.error = *source.refused;
    }
    else if (view.error.empty() && view.names)
    {
        const std::vector<std::string> names = edit::DsTakenNames(archive);
        const auto taken = [&names](const std::string& each)
        {
            return std::ranges::binary_search(names, each);
        };
        view.error = NameRefusal(name, "item", taken).value_or(view.error);
    }
}

} // namespace

ReplaceView MakeDsImportView(const ReplaceSource& source, edit::DsNewItem kind, std::span<const uint8_t> bytes,
                             const std::string& name, const std::vector<int>& rows)
{
    // The file, the name, the sounds to copy and the rows.
    const song::DsArchive& archive = *source.played.Ds();
    const bool sequence = kind == edit::DsNewItem::kSequence;
    const bool bank = kind == edit::DsNewItem::kBank;
    ReplaceView view;
    view.sequence = false;
    view.import = true;
    view.action = source.new_song || source.new_bank ? "Add" : "Import";
    view.title = source.new_song   ? "Add Sequence Sound"
                 : source.new_bank ? "Add Bank"
                 : sequence        ? "Import Sequence"
                 : bank            ? "Import Bank"
                                   : "Import Wave Archive";
    view.heading = sequence ? "Sequence file" : bank ? "Bank file" : "Wave archive file";
    name.copy(view.name.data(), view.name.size() - 1);
    view.names = archive.archive.has_symbols;

    // The numbers in the tables that the new item uses.
    const ntdlib::sdat::Archive& tables = archive.archive;
    std::string number;
    std::string size = SizeText(bytes.size());
    if (sequence)
    {
        number = FillDsSequenceImport(view, source, bytes, rows, size);
    }
    else if (bank)
    {
        number = FillDsBankImport(view, source, bytes, rows, size);
    }
    else
    {
        const std::size_t waves = edit::DsWaveArchiveWaves(bytes);
        view.source = source.file_name + ": DS wave archive file, " + Grouped(waves) +
                      (waves == 1 ? " wave, " : " waves, ") + size;
        number = "WAVEARC_" + std::to_string(tables.wave_archives.size());
    }

    // The name. The import makes the same checks.
    CheckDsImportName(view, source, name);

    // What the new item does. Play plays a new sound, a note of a new bank's first instrument or a new wave archive's
    // first wave.
    view.playable = view.error.empty() && !source.new_song && !source.new_bank;
    view.ready = view.error.empty();
    if (!view.names)
    {
        view.warnings.push_back("The archive has no names. The new item appears as " + number + ".");
    }
    if (sequence)
    {
        view.warnings.push_back(
            "Games play sounds by number. The new sound plays only if the game code is changed to play it.");
    }
    else
    {
        view.warnings.push_back(std::string("No sound uses the new ") + (bank ? "bank" : "wave archive") + " yet.");
    }

    return view;
}

ReplaceView MakeDsNewWaveView(const ReplaceSource& source, const edit::IncomingWave& incoming, bool wav,
                              std::optional<format::File> result, std::string error)
{
    // The file, and the menus of the wave archives and of a WAV file's encodings.
    const song::DsArchive& archive = *source.played.Ds();
    ReplaceView view;
    view.sequence = false;
    view.action = "Import";
    view.title = wav ? "Import Audio File" : "Import Wave";
    view.heading = wav ? "WAV file" : "Wave file";
    view.source = DsWaveSourceLine(source.file_name, incoming, wav);
    if (wav)
    {
        OfferDsRates(view, incoming, 0, edit::kHighestDsRate);
    }
    view.wave = incoming.bytes;
    const std::vector<uint32_t> wave_archives = song::DsWaveArchives(archive);
    view.wave_archive = static_cast<int>(std::ranges::find(wave_archives, source.sound) - wave_archives.begin());
    for (const uint32_t each : wave_archives)
    {
        view.wave_archives.push_back(song::DsWaveArchiveName(archive, each));
    }
    view.result = std::move(result);
    view.error = std::move(error);
    if (!view.result)
    {
        return view;
    }

    // The place of the new wave.
    const std::size_t wave = song::DsWaveCount(archive, source.sound);
    const std::string target = song::DsWaveArchiveName(archive, source.sound);
    view.warnings.push_back("The new wave becomes wave " + std::to_string(wave) + " of " + target +
                            (wave == 0 ? " (the first wave)." : " (after the existing waves).") +
                            " No bank uses it yet.");
    if (wav)
    {
        WavWarnings(incoming, view.warnings, false, true);
        if (incoming.channels == 2)
        {
            view.warnings.push_back("The WAV file is stereo, but DS waves are mono. The two channels are mixed down.");
        }
    }

    // The size. A game loads its wave archives into memory that it sets aside.
    view.warnings.push_back(std::string("The new wave is ") + EncodingText(incoming.encoding) + " (" +
                            SizeText(incoming.bytes.size()) +
                            "). Games load wave archives into a fixed memory area. A larger wave archive may not fit.");

    return view;
}

ReplaceView MakeDsWaveReplaceView(const ReplaceSource& source, uint32_t wave, const edit::IncomingWave& incoming,
                                  bool wav, const edit::WaveChanges& changes, std::optional<format::File> result,
                                  std::string error)
{
    // The wave, and the WAV file or the wave file of the DS that goes in its place.
    const song::DsArchive& archive = *source.played.Ds();
    ReplaceView view;
    view.sequence = false;
    view.title = "Replace " + song::DsItemName(archive, {song::ItemKind::kWave, source.sound, wave});
    view.heading = wav ? "WAV file" : "Wave file";
    view.source = DsWaveSourceLine(source.file_name, incoming, wav);
    if (wav)
    {
        OfferDsRates(view, incoming, changes.rate, edit::kHighestDsRate);
    }
    OfferPresentLoop(view, incoming, wav, changes);
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
            banks.push_back(song::DsBankName(archive, bank));
        }
        view.warnings.push_back(WavePlayersText(banks));
    }

    // The loop, the channels, the rate and the sizes.
    AddWaveLoopWarning(changes.loop, incoming.loop, view.warnings);
    if (wav)
    {
        WavWarnings(incoming, view.warnings, false, true);
        if (incoming.channels == 2)
        {
            view.warnings.push_back("The WAV file is stereo, but DS waves are mono. The two channels are mixed down.");
        }
    }
    if (!edit::SameDsTimer(changes.rate, incoming.rate))
    {
        view.warnings.push_back(RateChangeText(changes.rate, incoming.rate));
    }
    view.warnings.push_back(
        WaveSizeLine(changes.encoding, changes.size, incoming.encoding, incoming.bytes.size(), kWaveMemory));

    return view;
}

ReplaceView MakeDsStreamReplaceView(const ReplaceSource& source, const edit::IncomingWave& incoming, bool wav,
                                    const edit::WaveChanges& changes, std::optional<format::File> result,
                                    std::string error)
{
    // The stream, and the WAV file or the stream file of the DS that replaces it.
    const song::DsArchive& archive = *source.played.Ds();
    ReplaceView view;
    view.sequence = false;
    view.title = "Replace " + song::DsStreamName(archive, source.sound);
    view.heading = wav ? "WAV file" : "Stream file";
    view.source = DsStreamSourceLine(source.file_name, incoming, wav);
    if (wav)
    {
        OfferDsRates(view, incoming, changes.rate, edit::kHighestDsStreamRate);
    }
    OfferPresentLoop(view, incoming, wav, changes);
    view.wave = incoming.bytes;
    view.result = std::move(result);
    view.error = std::move(error);

    if (changes.loop && !incoming.loop)
    {
        view.warnings.push_back(
            "The current stream loops, but the new one doesn't. The sound stops at the end of the "
            "stream.");
    }
    else if (!changes.loop && incoming.loop)
    {
        view.warnings.push_back(
            "The new stream loops, but the current one doesn't. The sound no longer stops by "
            "itself.");
    }
    if (wav)
    {
        WavWarnings(incoming, view.warnings, true);
    }
    if (changes.channels != incoming.channels)
    {
        view.warnings.push_back("The current stream has " + std::to_string(changes.channels) +
                                (changes.channels == 1 ? " channel" : " channels") + ". The new one has " +
                                std::to_string(incoming.channels) + ".");
    }
    if (changes.rate != incoming.rate)
    {
        view.warnings.push_back(RateChangeText(changes.rate, incoming.rate) +
                                " The DS mixes at about 32,728 Hz. Streams at other rates haven't been tested in a "
                                "game.");
    }
    const auto [present, added] = SizeTexts(changes.size, incoming.bytes.size());
    view.warnings.push_back(std::string("The current stream is ") + EncodingText(changes.encoding) + " (" + present +
                            "). The new one is " + EncodingText(incoming.encoding) + " (" + added + ").");

    return view;
}

ReplaceView MakeDsStreamImportView(const ReplaceSource& source, const edit::IncomingWave& incoming, bool wav,
                                   const std::string& name)
{
    const song::DsArchive& archive = *source.played.Ds();
    ReplaceView view;
    view.sequence = false;
    view.import = true;
    view.action = "Import";
    view.title = "Import Stream";
    view.heading = wav ? "WAV file" : "Stream file";
    view.source = DsStreamSourceLine(source.file_name, incoming, wav);
    name.copy(view.name.data(), view.name.size() - 1);
    view.names = archive.archive.has_symbols;
    if (wav)
    {
        OfferDsRates(view, incoming, 0, edit::kHighestDsStreamRate);
    }
    view.wave = incoming.bytes;
    for (const uint32_t model : source.models)
    {
        view.models.push_back(song::DsStreamName(archive, model));
    }
    view.model = static_cast<std::size_t>(std::ranges::find(source.models, source.sound) - source.models.begin());

    const bool player = !archive.archive.stream_players.empty() && archive.archive.stream_players.front().has_value();
    if (source.refused)
    {
        view.error = *source.refused;
    }
    else if (!player && source.models.empty())
    {
        view.error = "The archive has no stream player. Streams require one.";
    }
    else if (view.names)
    {
        const std::vector<std::string> names = edit::DsTakenNames(archive);
        const auto taken = [&names](const std::string& each)
        {
            return std::ranges::binary_search(names, each);
        };
        view.error = NameRefusal(name, "item", taken).value_or(view.error);
    }
    view.ready = view.error.empty();
    if (!view.names)
    {
        view.warnings.push_back("The archive has no names. The new item appears as STRM_" +
                                std::to_string(archive.archive.streams.size()) + ".");
    }
    if (wav)
    {
        WavWarnings(incoming, view.warnings, true);
    }
    view.warnings.push_back("The new stream is " + std::string(EncodingText(incoming.encoding)) + " (" +
                            SizeText(incoming.bytes.size()) + ").");
    view.warnings.push_back(
        "Games play streams by number. The new stream plays only if the game code is changed to play it.");

    return view;
}

std::vector<uint8_t> ReadDsMidi(ReplaceSource& source)
{
    edit::DsMidiFile midi = edit::DsFileOfMidi(source.midi, source.file_name, source.midi_programs, source.drum_notes);
    source.incoming.midi = true;
    source.incoming.size = source.midi.size();
    source.incoming.sequence = std::move(midi.song);
    source.incoming.warnings = std::move(midi.warnings);

    return std::move(midi.file);
}

void ProposeDsMidiMap(ReplaceSource& source, uint32_t bank)
{
    source.midi_bank = bank;
    source.drum_notes.clear();
    source.midi_programs.clear();
    const auto map = source.file->bank_maps.find(bank);
    if (map == source.file->bank_maps.end())
    {
        return;
    }
    source.drum_notes = PairedDrumNotes(map->second, source.midi_drums.notes);

    // The programs' rows come from the commands with the drum notes' pairings.
    const edit::DsMidiFile midi = edit::DsFileOfMidi(source.midi, source.file_name, {}, source.drum_notes);
    for (const edit::Program& program : edit::ProgramsSelected(midi.song, edit::kEmptySongStart).programs)
    {
        if (const std::optional<int> paired = PairedProgram(map->second, program.number))
        {
            source.midi_programs[program] = *paired;
        }
    }
}

} // namespace hokuto::app
