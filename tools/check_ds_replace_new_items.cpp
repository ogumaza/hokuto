// SPDX-License-Identifier: MIT

// The rounds Plays and New items of hokuto_check_ds_replace.

#include "check_ds_replace.h"
#include "checked_saves.h"

namespace hokuto::tools::ds_replace
{

namespace
{

// The files of the wave archives that the banks of the sounds that Plays plays name.
std::set<uint32_t> PlayedWaveArchives(const Game& game, const std::vector<uint32_t>& sounds)
{
    const sdat::Archive& tables = game.archive->archive;
    std::set<uint32_t> files;
    for (std::size_t i = 0; i < sounds.size(); i += kPlayEvery)
    {
        uint16_t bank = 0;
        try
        {
            bank = hokuto::edit::DsSoundValuesOf(*game.archive, sounds[i]).bank;
        }
        catch (const std::invalid_argument&)
        {
            continue; // the render of such a sound fails too, and counts apart
        }
        if (bank >= tables.banks.size() || !tables.banks[bank])
        {
            continue;
        }

        for (const uint16_t wave_archive : tables.banks[bank]->wave_archives)
        {
            if (wave_archive < tables.wave_archives.size() && tables.wave_archives[wave_archive])
            {
                files.insert(tables.wave_archives[wave_archive]->file_id & sdat::kFileIdBits);
            }
        }
    }

    return files;
}

// The bytes of `wave` in 16-bit PCM, with its rate, timer and loop.
std::vector<uint8_t> Pcm16WaveBytes(const sdat::Wave& wave)
{
    // The sound unit starts IMA-ADPCM 8 of the wave's samples later than PCM. Zeros take those samples.
    std::vector<int16_t> samples = sdat::DecodeWave(wave);
    const std::size_t zeros = wave.encoding == sdat::WaveEncoding::kImaAdpcm ? kImaStartSamples : 0;
    const bool loops = wave.loop == 1 && wave.LoopStartSample() < samples.size();
    const uint32_t loop_start = wave.LoopStartSample() + static_cast<uint32_t>(zeros);
    if (!samples.empty())
    {
        samples.insert(samples.begin(), zeros, int16_t{0});
    }
    std::vector<uint8_t> bytes = samples.empty() ? sdat::WaveBytes(wave)
                                                 : sdat::EncodeWave(samples, std::max<uint16_t>(wave.sample_rate, 256),
                                                                    sdat::WaveEncoding::kPcm16,
                                                                    loops ? std::optional(loop_start) : std::nullopt);
    if (!samples.empty())
    {
        bytes[2] = static_cast<uint8_t>(wave.sample_rate & 0xFF);
        bytes[3] = static_cast<uint8_t>(wave.sample_rate >> 8);
        bytes[4] = static_cast<uint8_t>(wave.timer & 0xFF);
        bytes[5] = static_cast<uint8_t>(wave.timer >> 8);
    }

    return bytes;
}

// The game's file with each wave archive of `played` in 16-bit PCM.
File WidenedWaveArchives(const Game& game, const std::set<uint32_t>& played)
{
    const sdat::Archive& tables = game.archive->archive;
    File wide = game.file;
    for (const auto& [wave_archive, id] : WaveArchivesOf(game))
    {
        try
        {
            const sdat::WaveArchive parsed = sdat::WaveArchive::Parse(tables.File(id));
            std::vector<std::vector<uint8_t>> waves;
            for (const sdat::Wave& wave : parsed.waves)
            {
                waves.push_back(Pcm16WaveBytes(wave));
            }
            const std::vector<uint8_t> file = sdat::WaveArchiveFile(waves);
            if (played.contains(id))
            {
                wide = hokuto::edit::WithDsFile(wide, id, file);
            }
        }
        catch (const std::exception& error)
        {
            std::printf("%s: plays: %s stays apart: %s\n", game.name.c_str(),
                        song::DsWaveArchiveName(*game.archive, wave_archive).c_str(), error.what());
        }
    }

    return wide;
}

// What the renders of a sound gave before and after an edit (PlaysOf).
enum class Played
{
    kUnread,  // the sound can't render before the edit
    kRefused, // the render after the edit failed
    kOther,   // the renders differ
    kSilent,  // both renders hold only zeros
    kSame,
};

// Renders `sound` of `before_archive` and `edited` of `after_archive` for kPlaySeconds into `before` and `after`, and
// compares them. A render after the edit that the DS's memory can't hold prints a line of the round `round` for the
// sound `name`, and counts in `too_large`. Any other failure of that render fails `failed` in the round.
Played PlaysOf(const Game& game, const char* round, const std::string& name, const std::string& failed,
               const std::shared_ptr<const song::DsArchive>& before_archive, uint32_t sound,
               const std::shared_ptr<const song::DsArchive>& after_archive, uint32_t edited, std::vector<float>& before,
               std::vector<float>& after, std::size_t& too_large, Totals& totals)
{
    try
    {
        before = RenderedDs(before_archive, sound, kPlaySeconds);
    }
    catch (const std::exception&)
    {
        return Played::kUnread;
    }
    try
    {
        after = RenderedDs(after_archive, edited, kPlaySeconds);
    }
    catch (const std::exception& error)
    {
        if (HeapFull(error))
        {
            std::printf("%s: %s: %s stays apart: %s\n", game.name.c_str(), round, name.c_str(), error.what());
            too_large++;
        }
        else
        {
            Fail(game, round, failed + ": " + error.what(), totals);
        }
        return Played::kRefused;
    }
    if (after != before)
    {
        return Played::kOther;
    }

    return std::ranges::all_of(before, [](float sample) { return sample == 0.0f; }) ? Played::kSilent : Played::kSame;
}

// Checks that every kPlayEvery-th of `sounds` plays from `after`, the archive with its wave archives in 16-bit PCM,
// as it played before.
void CheckSoundsPlay(const Game& game, const std::vector<uint32_t>& sounds,
                     const std::shared_ptr<const song::DsArchive>& after, Totals& totals)
{
    for (std::size_t i = 0; i < sounds.size(); i += kPlayEvery)
    {
        const uint32_t sound = sounds[i];
        const std::string name = song::DsSoundName(*game.archive, sound);
        std::vector<float> before;
        std::vector<float> now;
        const Played played = PlaysOf(game, "plays", name, name, game.archive, sound, after, sound, before, now,
                                      totals.too_large, totals);
        if (played == Played::kUnread)
        {
            totals.sequence_apart++;
        }
        else if (played == Played::kOther)
        {
            const std::size_t first = FirstDifferingIndex(now, before);
            Fail(game, "plays",
                 name + " plays otherwise in 16-bit PCM, from output sample " + std::to_string(first / 2) + " of " +
                     std::to_string(before.size() / 2),
                 totals);
        }
        else if (played == Played::kSilent)
        {
            totals.silent++;
        }
        else if (played == Played::kSame)
        {
            totals.played++;
        }
    }
}

} // namespace

void Plays(const Game& game, Totals& totals)
{
    const std::vector<uint32_t> sounds = song::DsSounds(*game.archive);
    const std::set<uint32_t> played = PlayedWaveArchives(game, sounds);
    const File wide = WidenedWaveArchives(game, played);

    const std::shared_ptr<const song::DsArchive> after =
        song::LoadDsArchive(hokuto::format::ExportArchive(wide), game.archive->build);
    CheckSoundsPlay(game, sounds, after, totals);
}

namespace
{

// Whether two records of a table of the INFO block hold the same values.
bool SameRecord(const sdat::SequenceInfo& a, const sdat::SequenceInfo& b)
{
    return a.file_id == b.file_id && a.bank == b.bank && a.volume == b.volume &&
           a.channel_priority == b.channel_priority && a.player_priority == b.player_priority && a.player == b.player &&
           a.reserved == b.reserved && a.name == b.name;
}

bool SameRecord(const sdat::SequenceArchiveInfo& a, const sdat::SequenceArchiveInfo& b)
{
    return a.file_id == b.file_id && a.name == b.name && a.sequence_names == b.sequence_names;
}

bool SameRecord(const sdat::BankInfo& a, const sdat::BankInfo& b)
{
    return a.file_id == b.file_id && a.wave_archives == b.wave_archives && a.name == b.name;
}

bool SameRecord(const sdat::WaveArchiveInfo& a, const sdat::WaveArchiveInfo& b)
{
    return a.file_id == b.file_id && a.name == b.name;
}

bool SameRecord(const sdat::PlayerInfo& a, const sdat::PlayerInfo& b)
{
    return a.max_sequences == b.max_sequences && a.padding == b.padding && a.channels == b.channels &&
           a.heap_size == b.heap_size && a.name == b.name;
}

bool SameRecord(const sdat::GroupInfo& a, const sdat::GroupInfo& b)
{
    return a.items == b.items && a.name == b.name;
}

bool SameRecord(const sdat::StreamPlayerInfo& a, const sdat::StreamPlayerInfo& b)
{
    return a.channel_count == b.channel_count && a.channels == b.channels && a.reserved == b.reserved &&
           a.name == b.name;
}

bool SameRecord(const sdat::StreamInfo& a, const sdat::StreamInfo& b)
{
    return a.file_id == b.file_id && a.volume == b.volume && a.player_priority == b.player_priority &&
           a.player == b.player && a.flags == b.flags && a.reserved == b.reserved && a.name == b.name;
}

// Checks that `after`, a table of the INFO block of the new items' version, contains each record of `before` at its
// index, and `added` new records after them.
template <class Record>
void CheckRecords(const Game& game, const std::string& kind, const std::vector<std::optional<Record>>& before,
                  const std::vector<std::optional<Record>>& after, std::size_t added, Totals& totals)
{
    if (after.size() != before.size() + added)
    {
        Fail(game, "new items",
             "the table of " + kind + "s holds " + std::to_string(after.size()) + " records, not " +
                 std::to_string(before.size() + added),
             totals);
    }
    for (std::size_t i = 0; i < std::min(before.size(), after.size()); i++)
    {
        if (before[i].has_value() != after[i].has_value() || (before[i] && !SameRecord(*before[i], *after[i])))
        {
            Fail(game, "new items", kind + " " + std::to_string(i) + " holds another record", totals);
        }
    }
}

// The archive that `file`, a version of the game's file, exports.
std::shared_ptr<const song::DsArchive> LoadExported(const Game& game, const File& file)
{
    return song::LoadDsArchive(hokuto::format::ExportArchive(file), game.archive->build);
}

// Adds a copy of each wave archive of `bank`, the bank of the model `number`, to `edited`, and returns the slots that
// the copy of the bank uses. Counts the copies in `wave_archives_added` and `totals`.
std::array<uint16_t, 4> AddModelWaveArchives(const Game& game, const sdat::BankInfo& bank, const std::string& number,
                                             File& edited, std::size_t& wave_archives_added, Totals& totals)
{
    const sdat::Archive& tables = game.archive->archive;
    std::array<uint16_t, 4> slots = {kNoWaveArchive, kNoWaveArchive, kNoWaveArchive, kNoWaveArchive};
    for (std::size_t slot = 0; slot < slots.size(); slot++)
    {
        const uint16_t wave_archive = bank.wave_archives[slot];
        if (wave_archive >= tables.wave_archives.size() || !tables.wave_archives[wave_archive])
        {
            continue;
        }
        slots[slot] = static_cast<uint16_t>(LoadExported(game, edited)->archive.wave_archives.size());
        edited = hokuto::edit::AddDsWaveArchive(
            edited, "NEW_WAVES_" + number + "_" + std::to_string(slot),
            song::DsItemBytes(*game.archive, {song::ItemKind::kWaveArchive, wave_archive}));
        wave_archives_added++;
        totals.new_wave_archives++;
    }

    return slots;
}

// Checks that `after`, the archive of `edited` with `sounds_added` new sounds, `banks_added` new banks and
// `wave_archives_added` new wave archives, keeps every other item, and that a saved `edited` exports `exported` again.
void CheckOtherItemsKept(const Game& game, const File& edited, const std::vector<uint8_t>& exported,
                         const song::DsArchive& after, std::size_t sounds_added, std::size_t banks_added,
                         std::size_t wave_archives_added, Totals& totals)
{
    const sdat::Archive& tables = game.archive->archive;
    const sdat::Archive& now = after.archive;
    for (uint32_t id = 0; id < tables.files.size(); id++)
    {
        if (id >= now.files.size() || !std::ranges::equal(now.File(id), tables.File(id)))
        {
            Fail(game, "new items", "file " + std::to_string(id) + " holds other bytes", totals);
        }
    }
    CheckRecords(game, "sequence", tables.sequences, now.sequences, sounds_added, totals);
    CheckRecords(game, "sequence archive", tables.sequence_archives, now.sequence_archives, 0, totals);
    CheckRecords(game, "bank", tables.banks, now.banks, banks_added, totals);
    CheckRecords(game, "wave archive", tables.wave_archives, now.wave_archives, wave_archives_added, totals);
    CheckRecords(game, "player", tables.players, now.players, 0, totals);
    CheckRecords(game, "group", tables.groups, now.groups, 0, totals);
    CheckRecords(game, "stream player", tables.stream_players, now.stream_players, 0, totals);
    CheckRecords(game, "stream", tables.streams, now.streams, 0, totals);
    const std::vector<std::optional<uint32_t>> old_sequences = hokuto::format::DsSoundSequences(game.file, game.bytes);
    const std::vector<std::optional<uint32_t>> new_sequences = hokuto::format::DsSoundSequences(edited, exported);
    for (const uint32_t sound : song::DsSounds(*game.archive))
    {
        if (song::DsSoundName(after, sound) != song::DsSoundName(*game.archive, sound))
        {
            Fail(game, "new items", "sound " + std::to_string(sound) + " takes another name", totals);
        }
        if (sound < old_sequences.size() && old_sequences[sound] != new_sequences[sound])
        {
            Fail(game, "new items", "sound " + std::to_string(sound) + " takes another sequence", totals);
        }
    }
    if (hokuto::format::ExportArchive(hokuto::format::Read(hokuto::format::Write(edited))) != exported ||
        hokuto::format::ExportArchive(hokuto::format::ReadText(hokuto::format::WriteText(edited))) != exported)
    {
        Fail(game, "new items", "a saved file gives another archive", totals);
    }
}

// Checks that each new sound of `added`, with its model, plays from `after` as its model played before.
void CheckNewSoundsPlay(const Game& game, const std::shared_ptr<const song::DsArchive>& after,
                        const std::vector<std::pair<uint32_t, uint32_t>>& added, Totals& totals)
{
    for (const auto& [model, sound] : added)
    {
        const std::string name = song::DsSoundName(*game.archive, model);
        std::vector<float> before;
        std::vector<float> now;
        const Played played = PlaysOf(game, "new items", name, "the copy of " + name, game.archive, model, after, sound,
                                      before, now, totals.new_too_large, totals);
        if (played == Played::kUnread)
        {
            totals.new_unread++;
        }
        else if (played == Played::kOther)
        {
            Fail(game, "new items", "the copy of " + name + " plays otherwise", totals);
        }
        else if (played == Played::kSilent)
        {
            totals.new_silent++;
        }
        else if (played == Played::kSame)
        {
            totals.new_played++;
        }
    }
}

} // namespace

void NewItems(const Game& game, Totals& totals)
{
    if (const std::optional<std::string> refused = hokuto::edit::DsAdditionsRefused(game.file))
    {
        std::printf("%s: new items stay apart: %s\n", game.name.c_str(), refused->c_str());
        totals.new_apart++;
        return;
    }

    const sdat::Archive& tables = game.archive->archive;

    // The models are spread across the sounds. The name of a new item contains the number of its model.
    const std::vector<uint32_t> sounds = SequenceSounds(game);
    File edited = game.file;
    std::vector<std::pair<uint32_t, uint32_t>> added;
    std::size_t banks_added = 0;
    std::size_t wave_archives_added = 0;
    for (std::size_t i = 0; i < sounds.size() && added.size() < kNewModels;
         i += std::max<std::size_t>(sounds.size() / kNewModels, 1))
    {
        const uint32_t model = sounds[i];
        const std::string number = std::to_string(model);
        try
        {
            hokuto::edit::DsSoundValues values = hokuto::edit::DsSoundValuesOf(*game.archive, model);
            const uint32_t model_bank = values.bank;
            if (model_bank >= tables.banks.size() || !tables.banks[model_bank])
            {
                continue;
            }
            const sdat::BankInfo& bank = *tables.banks[model_bank];
            const std::array<uint16_t, 4> slots =
                AddModelWaveArchives(game, bank, number, edited, wave_archives_added, totals);
            values.bank = static_cast<uint16_t>(LoadExported(game, edited)->archive.banks.size());
            edited =
                hokuto::edit::AddDsBank(edited, "NEW_BANK_" + number,
                                        song::DsItemBytes(*game.archive, {song::ItemKind::kBank, model_bank}), slots);
            banks_added++;
            totals.new_banks++;
            const auto sound = static_cast<uint32_t>(LoadExported(game, edited)->archive.sequences.size());
            edited = hokuto::edit::AddDsSequenceSound(
                edited, "NEW_SOUND_" + number, song::DsItemBytes(*game.archive, {song::ItemKind::kSequence, model}),
                values);
            added.emplace_back(model, sound);
            totals.new_sounds++;
        }
        catch (const std::exception& error)
        {
            Fail(game, "new items", song::DsSoundName(*game.archive, model) + ": " + error.what(), totals);
        }
    }

    // Every other item stays.
    const std::vector<uint8_t> exported = hokuto::format::ExportArchive(edited);
    const std::shared_ptr<const song::DsArchive> after = song::LoadDsArchive(exported, game.archive->build);
    CheckOtherItemsKept(game, edited, exported, *after, added.size(), banks_added, wave_archives_added, totals);

    // Each new sound plays the samples of its model.
    CheckNewSoundsPlay(game, after, added, totals);
}

} // namespace hokuto::tools::ds_replace
