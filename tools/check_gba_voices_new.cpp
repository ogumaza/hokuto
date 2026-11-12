// SPDX-License-Identifier: MIT

// The rounds of hokuto_check_gba_voices that add new voices, samples and key rows.

#include "check_gba_voices.h"
#include "check_support.h"

namespace hokuto::tools::gba_voices
{

namespace
{

// `frames` stereo frames of a held note of `program` at `key`, on the music player of `song`.
std::vector<float> NoteOutput(const std::shared_ptr<const hokuto::song::GbaRom>& rom, int song, int program, int key,
                              std::size_t frames)
{
    auto release = std::make_shared<std::atomic<bool>>(false);
    hokuto::song::GbaNotePreview preview(rom, song, program, key, 127, release);
    std::vector<float> out(frames * 2);
    out.resize(preview.Render(out.data(), frames) * 2);

    return out;
}

bool Sounds(std::span<const float> samples)
{
    return std::ranges::any_of(samples, [](float sample) { return std::abs(sample) > 1e-4f; });
}

} // namespace

void NewKinds(const Game& game, const std::map<uint32_t, Place>& places, Totals& totals)
{
    Game with_pattern{game.name, {}, {}, nullptr, game.groups, game.spans};
    File room = game.file;
    hokuto::edit::PlacedGbaBytes pattern;
    const auto pattern_bytes = hokuto::edit::GbaPatternBytes(hokuto::edit::NewGbaPattern());
    try
    {
        pattern = hokuto::edit::WithGbaNewBytes(room, pattern_bytes);
    }
    catch (const std::invalid_argument&)
    {
        room.native.Edit([](hokuto::format::NativeArchive& native) { native.gba_filler = true; });
        try
        {
            pattern = hokuto::edit::WithGbaNewBytes(room, pattern_bytes);
        }
        catch (const std::invalid_argument&)
        {
            totals.kinds_no_room++;
            return;
        }
    }
    with_pattern.file = pattern.file;
    with_pattern.bytes = hokuto::format::ExportArchive(pattern.file);
    with_pattern.rom = hokuto::song::LoadGbaRom(with_pattern.bytes);

    std::map<uint32_t, mp2k::Voice> voices;
    for (const auto& [address, place] : places)
    {
        const int psg = place.voice.PsgChannel();
        const int next = psg == 0 ? 2 : psg <= 2 ? 4 : psg == 3 ? 1 : 3;
        voices.emplace(address, hokuto::edit::GbaVoiceOfKind(place.voice, next, next == 3 ? pattern.address : 0));
    }
    const std::shared_ptr<const hokuto::song::GbaRom> rom = Round(with_pattern, "kinds", voices, {}, totals);
    if (rom == nullptr)
    {
        return;
    }

    std::size_t tried = 0;
    const std::size_t every = std::max<std::size_t>(places.size() / 64, 1);
    for (const auto& [address, place] : places)
    {
        if (tried++ % every != 0 || place.song < 0)
        {
            continue;
        }
        mp2k::Voice read;
        if (!mp2k::ReadVoice(rom->rom, address, read) || read.PsgChannel() == 0 || read.sustain < 8 ||
            read.sustain > 15 || read.attack > 7 || read.decay > 7 || read.release > 7 ||
            read.PsgChannel() == place.voice.PsgChannel())
        {
            continue; // a low sustain can round to silence, and other bytes that a table runs on into can stay silent
        }
        // A voice that a song selects can overlap a key split's key table. The round's edit of it then assigns this key
        // another row.
        mp2k::SongHeader header;
        mp2k::Voice entry;
        if (mp2k::ReadSong(game.rom->rom, game.rom->info, place.song, header) &&
            mp2k::ReadVoice(game.rom->rom, header.voices + 12 * place.program, entry) && entry.IsSplit() &&
            mp2k::SplitVoice(game.rom->rom, entry, place.key) != mp2k::SplitVoice(rom->rom, entry, place.key))
        {
            totals.kinds_other_rows++;
            continue;
        }

        // A slow attack on the wave channel, and the noise at its lowest rates, take a while to leave 0.
        if (!Sounds(NoteOutput(rom, place.song, static_cast<int>(place.program), place.key, 32768)))
        {
            std::printf("%s: kinds: %s doesn't sound as a voice of kind %d\n", game.name.c_str(), place.name.c_str(),
                        read.PsgChannel());
            totals.failed++;
            continue;
        }
        totals.kinds_sounded++;
    }
}

namespace
{

// A sample that the round put in: the voice that read it first, the sample as the game stores it, and its new address.
struct Made
{
    const Place* place = nullptr;
    mp2k::Wave wave;
    uint32_t address = 0;
};

// Puts the sample at `sample`, read first by `place`, in `edited` as a new sample from a WAV file, with a voice that
// plays it. Lists it in `made`. Returns false once the free space runs out.
bool AddSample(const Game& game, uint32_t sample, const Place* place, File& edited, std::vector<Made>& made,
               Totals& totals)
{
    mp2k::Wave wave;
    if (!mp2k::ReadWave(game.rom->rom, sample, wave) || wave.size == 0)
    {
        return true;
    }
    if (wave.type != 0)
    {
        totals.compressed++;
        return true;
    }
    if (wave.loops && wave.loop_start >= wave.size)
    {
        totals.odd_loops++;
        return true;
    }

    seqlib::Pcm pcm;
    pcm.channels.resize(1);
    pcm.channels[0].reserve(wave.size);
    const uint8_t* data = game.rom->rom.Ptr(wave.Data());
    for (uint32_t i = 0; i < wave.size; i++)
    {
        pcm.channels[0].push_back(static_cast<int16_t>(static_cast<int8_t>(data[i]) * 256));
    }
    pcm.loop = wave.loops;
    pcm.loop_start = wave.loops ? wave.loop_start : 0;
    pcm.loop_end = wave.loops ? wave.size : 0;
    const uint32_t rate = std::max<uint32_t>(wave.frequency / 1024, 1);

    // Only the free space can refuse the sample. Any other refusal fails.
    try
    {
        const hokuto::edit::IncomingWave incoming = hokuto::edit::ReadIncomingGbaSample(seqlib::WavFile(pcm, rate));
        hokuto::edit::PlacedGbaBytes placed;
        try
        {
            placed = hokuto::edit::WithGbaNewBytes(edited, incoming.bytes);
        }
        catch (const std::invalid_argument& error)
        {
            if (edited.native->gba_filler || !LacksRoom(error))
            {
                throw;
            }
            edited.native.Edit([](hokuto::format::NativeArchive& native) { native.gba_filler = true; });
            placed = hokuto::edit::WithGbaNewBytes(edited, incoming.bytes);
        }
        edited = hokuto::edit::WithGbaVoice(placed.file, hokuto::edit::GbaVoiceOfKind(place->voice, 0, placed.address));
        made.push_back({place, wave, placed.address});
        totals.in_filler += placed.address < game.rom->rom.End() && edited.native->gba_filler ? 1 : 0;
    }
    catch (const std::invalid_argument& error)
    {
        if (!LacksRoom(error))
        {
            std::printf("%s: new samples: the sample of 0x%08X doesn't come back: %s\n", game.name.c_str(), sample,
                        error.what());
            totals.failed++;
            return true;
        }
        // The inner catch has turned the filler runs on before a lack of room gets here.
        totals.no_room++;
        return false; // no run holds more
    }

    return true;
}

// Checks each sample of `made` as ntdlib reads it from `built_rom`, the ROM with the new samples, and compares a note
// of some of them with the samples of the game.
void CheckMadeSamples(const Game& game, const std::vector<Made>& made,
                      const std::shared_ptr<const hokuto::song::GbaRom>& built_rom, Totals& totals)
{
    const hokuto::song::GbaRom& built = *built_rom;
    std::size_t tried = 0;
    const std::size_t every = std::max<std::size_t>(made.size() / 64, 1);
    for (const Made& one : made)
    {
        mp2k::Wave read;
        const bool whole = one.wave.frequency % 1024 == 0;
        if (!mp2k::ReadWave(built.rom, one.address, read) || read.size != one.wave.size ||
            read.loops != one.wave.loops || (read.loops && read.loop_start != one.wave.loop_start) ||
            (whole && read.frequency != one.wave.frequency) || read.type != 0 ||
            !std::equal(game.rom->rom.Ptr(one.wave.Data()), game.rom->rom.Ptr(one.wave.Data()) + one.wave.size,
                        built.rom.Ptr(read.Data())))
        {
            std::printf("%s: new samples: ntdlib reads the sample of 0x%08X otherwise\n", game.name.c_str(),
                        one.wave.address);
            totals.failed++;
            continue;
        }
        totals.new_samples++;
        totals.odd_rates += whole ? 0 : 1;

        // The mixer reads one byte past the sample's last. The sample of a WAV file contains the first sample of the
        // loop there, or the last sample. The sample of a game contains what its tools wrote, and a note can then
        // differ by that byte.
        const bool same_end =
            game.rom->rom.Contains(one.wave.Data() + one.wave.size, 1) &&
            game.rom->rom.U8(one.wave.Data() + one.wave.size) == built.rom.U8(read.Data() + read.size);
        totals.other_ends += same_end ? 0 : 1;
        if (tried++ % every != 0 || one.place->song < 0 || !whole || !same_end)
        {
            continue;
        }
        const int program = static_cast<int>(one.place->program);

        // A row that reads other data as a voice can overlap a key table. The round's edit of it then assigns this key
        // another row, and the note can't compare.
        mp2k::SongHeader header;
        mp2k::Voice entry;
        if (mp2k::ReadSong(game.rom->rom, game.rom->info, one.place->song, header) &&
            mp2k::ReadVoice(game.rom->rom, header.voices + 12 * one.place->program, entry) && entry.IsSplit() &&
            mp2k::SplitVoice(game.rom->rom, entry, one.place->key) !=
                mp2k::SplitVoice(built.rom, entry, one.place->key))
        {
            totals.other_rows++;
            continue;
        }
        if (NoteOutput(game.rom, one.place->song, program, one.place->key, 16384) !=
            NoteOutput(built_rom, one.place->song, program, one.place->key, 16384))
        {
            std::printf("%s: new samples: %s plays other samples from its new sample\n", game.name.c_str(),
                        one.place->name.c_str());
            totals.failed++;
            continue;
        }
        totals.notes_compared++;
    }
}

} // namespace

void NewSamples(const Game& game, const std::map<uint32_t, Place>& places, Totals& totals)
{
    std::map<uint32_t, const Place*> first_voice;
    for (const auto& [address, place] : places)
    {
        if (place.voice.PsgChannel() == 0)
        {
            first_voice.try_emplace(place.voice.wave, &place);
        }
    }

    std::vector<Made> made;
    File edited = game.file;
    // The first sample without room ends the round. The samples after it count as untried.
    std::size_t tried = 0;
    for (const auto& [sample, place] : first_voice)
    {
        tried++;
        if (!AddSample(game, sample, place, edited, made, totals))
        {
            totals.samples_untried += first_voice.size() - tried;
            break;
        }
    }
    if (made.empty())
    {
        return;
    }

    const std::vector<uint8_t> exported = hokuto::format::ExportArchive(edited);
    const std::shared_ptr<const hokuto::song::GbaRom> rom = hokuto::song::LoadGbaRom(exported);
    if (hokuto::format::ExportArchive(hokuto::format::Read(hokuto::format::Write(edited))) != exported ||
        hokuto::format::ExportArchive(hokuto::format::ReadText(hokuto::format::WriteText(edited))) != exported)
    {
        std::printf("%s: new samples: a saved file gives another ROM\n", game.name.c_str());
        totals.failed++;
    }

    CheckMadeSamples(game, made, rom, totals);
}

namespace
{

// A sample's first samples can be silent. At a lower key than this, the check's note ends before the sample leaves
// them.
constexpr int kLeastKitKey = 24;

// Prints the problem `what` of the round of key rows on the voice group `name`, and counts it.
void KeyRowsFail(const Game& game, const std::string& name, Totals& totals, const std::string& what)
{
    std::printf("%s: key rows: %s: %s\n", game.name.c_str(), name.c_str(), what.c_str());
    totals.failed++;
}

// Checks the programs of `tabled`, whose key tables `edited` rewrites, in the ROM that `edited` exports: the voice
// group at `address`, `name`, shows the same rows, and some of the rows play song `song`'s notes as before.
void CheckTabledPrograms(const Game& game, uint32_t address, const std::string& name, int song, const File& edited,
                         const std::vector<const hokuto::app::GbaProgram*>& tabled, Totals& totals)
{
    const std::vector<uint8_t> exported = hokuto::format::ExportArchive(edited);
    const std::shared_ptr<const hokuto::song::GbaRom> rom = hokuto::song::LoadGbaRom(exported);
    if (hokuto::format::ExportArchive(hokuto::format::Read(hokuto::format::Write(edited))) != exported ||
        hokuto::format::ExportArchive(hokuto::format::ReadText(hokuto::format::WriteText(edited))) != exported)
    {
        KeyRowsFail(game, name, totals, "a saved file gives another ROM");
    }

    const hokuto::app::GbaBankView after = hokuto::app::MakeGbaBankView(*rom, address);
    for (const hokuto::app::GbaProgram* program : tabled)
    {
        const int number = static_cast<int>(program->program);
        const auto shown = std::ranges::find(after.programs, program->program, &hokuto::app::GbaProgram::program);
        std::vector<std::pair<int, int>> rows_before;
        std::vector<std::pair<int, int>> rows_after;
        for (const hokuto::app::GbaVoiceRow& row : program->rows)
        {
            rows_before.emplace_back(row.first_key, row.last_key);
        }
        if (shown != after.programs.end())
        {
            for (const hokuto::app::GbaVoiceRow& row : shown->rows)
            {
                rows_after.emplace_back(row.first_key, row.last_key);
            }
        }
        if (rows_before != rows_after)
        {
            KeyRowsFail(game, name, totals, "program " + std::to_string(number) + " shows other rows");
        }

        std::size_t tried = 0;
        const std::size_t every = std::max<std::size_t>(program->rows.size() / 8, 1);
        for (const hokuto::app::GbaVoiceRow& row : program->rows)
        {
            if (tried++ % every != 0)
            {
                continue;
            }
            if (NoteOutput(game.rom, song, number, row.first_key, 4096) !=
                NoteOutput(rom, song, number, row.first_key, 4096))
            {
                KeyRowsFail(
                    game, name, totals,
                    "program " + std::to_string(number) + " plays key " + std::to_string(row.first_key) + " otherwise");
                continue;
            }
            totals.rows_compared++;
        }
    }
}

// Turns the first single voice of `view`, the voice group `name`, into a key split, then a drum kit, and checks that
// song `song` plays its note as before.
void KeySplitAndDrumKit(const Game& game, const hokuto::app::GbaBankView& view, const std::string& name, int song,
                        Totals& totals)
{
    const auto is_single = [](const hokuto::app::GbaProgram& program)
    {
        return !program.voice.voice.IsSplit() && program.voice.kind != "Unknown";
    };
    const auto single = std::ranges::find_if(view.programs, is_single);
    if (single == view.programs.end())
    {
        return;
    }
    const mp2k::Voice& voice = single->voice.voice;
    const int number = static_cast<int>(single->program);
    const std::vector<float> played = NoteOutput(game.rom, song, number, 60, 4096);
    const auto with_rows = [&](uint8_t type, int first, int last)
    {
        const std::vector<hokuto::edit::GbaKeyRow> rows = {{first, last, voice}};
        return hokuto::song::LoadGbaRom(hokuto::format::ExportArchive(
            hokuto::edit::WithGbaKeyRows(game.file, hokuto::edit::GbaSplitOf(voice, type), rows)));
    };
    try
    {
        const std::size_t failed_before = totals.failed;
        if (NoteOutput(with_rows(mp2k::kVoiceKeySplit, 0, 127), song, number, 60, 4096) != played)
        {
            KeyRowsFail(game, name, totals, "program " + std::to_string(number) + " plays otherwise as a key split");
        }
        const std::vector<float> as_kit = NoteOutput(with_rows(mp2k::kVoiceDrumKit, 60, 60), song, number, 60, 4096);
        if ((voice.key == 60 && (voice.pan_sweep & 0x80) == 0 && as_kit != played) ||
            (voice.key >= kLeastKitKey && Sounds(played) && !Sounds(as_kit)))
        {
            KeyRowsFail(game, name, totals, "program " + std::to_string(number) + " plays otherwise as a drum kit");
        }
        totals.made_splits += totals.failed == failed_before ? 1 : 0;
    }
    catch (const std::invalid_argument& error)
    {
        if (LacksRoom(error))
        {
            totals.rows_no_room++;
        }
        else
        {
            KeyRowsFail(game, name, totals, "program " + std::to_string(number) + " refuses its rows: " + error.what());
        }
    }
}

// The round of key rows on the voice group `group`.
void NewKeyRows(const Game& game, const hokuto::song::GbaVoiceGroup& group, Totals& totals)
{
    const std::string name = hokuto::song::GbaVoiceGroupName(group.address);
    const int song = group.songs.front();
    const hokuto::app::GbaBankView view = hokuto::app::MakeGbaBankView(*game.rom, group.address);

    File edited = game.file;
    std::vector<const hokuto::app::GbaProgram*> tabled;
    for (const hokuto::app::GbaProgram& program : view.programs)
    {
        if (!program.voice.voice.IsSplit() || program.rows.empty())
        {
            continue;
        }
        try
        {
            edited = hokuto::edit::WithGbaKeyRows(edited, program.voice.voice, hokuto::app::KeyRowsOf(program));
            tabled.push_back(&program);
            totals.tables_rewritten++;
        }
        catch (const std::invalid_argument& error)
        {
            if (LacksRoom(error))
            {
                totals.rows_no_room++;
            }
            else
            {
                KeyRowsFail(game, name, totals,
                            "program " + std::to_string(program.program) + " refuses its rows: " + error.what());
            }
        }
    }
    if (!tabled.empty())
    {
        CheckTabledPrograms(game, group.address, name, song, edited, tabled, totals);
    }

    // The first single voice becomes a key split, then a drum kit.
    KeySplitAndDrumKit(game, view, name, song, totals);
}

// Prints the problem `what` of the round of new voices on the voice group `name`, and counts it.
void NewVoicesFail(const Game& game, const std::string& name, Totals& totals, const std::string& what)
{
    std::printf("%s: new voices: %s: %s\n", game.name.c_str(), name.c_str(), what.c_str());
    totals.failed++;
}

// The output of song `song` of the game's ROM, from `played` once a check has made it.
const std::vector<int16_t>& PlayedBefore(const Game& game, std::map<int, std::vector<int16_t>>& played, int song)
{
    const auto [known, added] = played.try_emplace(song);
    if (added)
    {
        known->second = SongOutput(*game.rom, song);
    }
    return known->second;
}

// Whether the sample at `address` of the game's ROM contains a sound. A game can hold a sample of zeros.
bool HoldsSound(const Game& game, uint32_t address)
{
    mp2k::Wave wave;
    if (!mp2k::ReadWave(game.rom->rom, address, wave) || wave.type != 0)
    {
        return false;
    }
    const uint8_t* data = game.rom->rom.Ptr(wave.Data());
    return std::any_of(data, data + wave.size, [](uint8_t b) { return b != 0; });
}

// Two programs of `group` for the new voices: free ones, or ones that no song selects.
std::vector<uint32_t> FreePrograms(const Game& game, const hokuto::song::GbaVoiceGroup& group, Totals& totals)
{
    // A game's table can fill its programs with voices that no song selects: a Delete in the Bank Editor frees
    // them.
    std::vector<uint32_t> free = hokuto::edit::FreeGbaPrograms(*game.rom, group.address);
    const bool fills_table = free.size() < 2;
    for (int program = 127; program >= 0 && free.size() < 2; program--)
    {
        const auto unsigned_program = static_cast<uint32_t>(program);
        if (std::ranges::find(group.programs, program) == group.programs.end() &&
            std::ranges::find(free, unsigned_program) == free.end())
        {
            free.push_back(unsigned_program);
        }
    }
    totals.tables_filled += fills_table ? 1 : 0;

    return free;
}

// Checks the bytes of `exported`, the ROM of `version`, and the ROM `rom` that it produces: only the table of the voice
// group and the fields of the headers of its songs differ, no song plays other voices, and a saved file produces the
// same ROM.
void CheckExportedBytes(const Game& game, const hokuto::song::GbaVoiceGroup& group, const std::string& name,
                        const hokuto::edit::GbaGroupVersion& version, const std::vector<uint8_t>& exported,
                        const hokuto::song::GbaRom& rom, Totals& totals)
{
    // The bytes that may differ: the table with the zeros that end it, and the fields of the headers.
    std::set<uint32_t> fields;
    for (const int song : group.songs)
    {
        mp2k::SongHeader header;
        mp2k::SongHeader read;
        if (!mp2k::ReadSong(game.rom->rom, game.rom->info, song, header) ||
            !mp2k::ReadSong(rom.rom, rom.info, song, read) || read.voices != version.group)
        {
            NewVoicesFail(game, name, totals, "song " + std::to_string(song) + "'s header names another table");
            continue;
        }
        for (uint32_t i = 0; i < 4; i++)
        {
            fields.insert(header.address + 4 + i);
        }
    }
    for (std::size_t i = 0; i < std::min(exported.size(), game.bytes.size()); i++)
    {
        const auto address = static_cast<uint32_t>(i + ntdlib::agb::kRomBase);
        const bool in_table = address >= version.group && address < version.group + 128 * 12 + 4;
        if (exported[i] != game.bytes[i] && !in_table && !fields.contains(address))
        {
            NewVoicesFail(game, name, totals,
                          "the byte at " + ntdlib::agb::Hex(address) + " differs. The table is at " +
                              ntdlib::agb::Hex(version.group) + ".");
            break;
        }
    }
    if (!hokuto::song::GbaSongsWithOtherVoices(*game.rom, rom).empty())
    {
        NewVoicesFail(game, name, totals, "the list of collisions isn't empty");
    }
    if (hokuto::format::ExportArchive(hokuto::format::Read(hokuto::format::Write(version.file))) != exported ||
        hokuto::format::ExportArchive(hokuto::format::ReadText(hokuto::format::WriteText(version.file))) != exported)
    {
        NewVoicesFail(game, name, totals, "a saved file gives another ROM");
    }
}

// Checks that songs of `group` and of `all_songs` play as before in `rom`, that the second program of `free` plays as
// program `source_program`, and that the first sounds unless it plays `sample`, a sample of zeros.
void CheckSongsPlay(const Game& game, const hokuto::song::GbaVoiceGroup& group, const std::string& name,
                    const std::vector<int>& all_songs, std::map<int, std::vector<int16_t>>& played,
                    const std::shared_ptr<const hokuto::song::GbaRom>& rom, int source_program,
                    const std::vector<uint32_t>& free, const std::optional<uint32_t>& sample, Totals& totals)
{
    std::set<int> songs(group.songs.begin(), group.songs.begin() + static_cast<std::ptrdiff_t>(
                                                                       std::min<std::size_t>(group.songs.size(), 4)));
    for (std::size_t i = 0; i < all_songs.size(); i += 32)
    {
        songs.insert(all_songs[i]);
    }
    for (const int song : songs)
    {
        if (SongOutput(*rom, song) != PlayedBefore(game, played, song))
        {
            NewVoicesFail(game, name, totals, "song " + std::to_string(song) + " plays other samples");
            continue;
        }
        totals.songs_compared++;
    }
    const int shown_song = group.songs.front();
    if (NoteOutput(rom, shown_song, source_program, 60, 8192) !=
        NoteOutput(rom, shown_song, static_cast<int>(free[1]), 60, 8192))
    {
        NewVoicesFail(game, name, totals,
                      "the copy of program " + std::to_string(source_program) + " plays other samples");
    }
    else
    {
        totals.copies_compared++;
    }
    if (!Sounds(NoteOutput(rom, shown_song, static_cast<int>(free[0]), 60, 8192)))
    {
        const bool silent_sample = sample && !HoldsSound(game, *sample);
        if (!silent_sample)
        {
            NewVoicesFail(game, name, totals, "the new voice doesn't sound");
        }
    }
}

// Checks that the Bank Editor lists the programs `free` of `version` in `rom`, and that a deleted voice frees its
// program.
void CheckListedAndDeleted(const Game& game, const std::string& name, const hokuto::song::GbaRom& rom,
                           const hokuto::edit::GbaGroupVersion& version, const std::vector<uint32_t>& free,
                           Totals& totals)
{
    const hokuto::app::GbaBankView view = hokuto::app::MakeGbaBankView(rom, version.group);
    for (const uint32_t program : {free[0], free[1]})
    {
        if (std::ranges::find(view.programs, program, &hokuto::app::GbaProgram::program) == view.programs.end() ||
            std::ranges::find(view.free_programs, program) != view.free_programs.end())
        {
            NewVoicesFail(game, name, totals, "the Bank Editor doesn't list program " + std::to_string(program));
        }
    }
    const std::shared_ptr<const hokuto::song::GbaRom> cleared = hokuto::song::LoadGbaRom(
        hokuto::format::ExportArchive(hokuto::edit::WithoutGbaVoice(version.file, version.group + 12 * free[0])));
    const hokuto::app::GbaBankView after = hokuto::app::MakeGbaBankView(*cleared, version.group);
    if (std::ranges::find(after.free_programs, free[0]) == after.free_programs.end())
    {
        NewVoicesFail(game, name, totals, "a deleted voice keeps its program");
    }
}

} // namespace

void NewVoices(const Game& game, const std::vector<uint32_t>& samples, Totals& totals)
{
    std::map<int, std::vector<int16_t>> played;

    // The new voice plays the first sample of the ROM that contains a sound. A game can hold a sample of zeros.
    const auto sounding =
        std::ranges::find_if(samples, [&game](uint32_t address) { return HoldsSound(game, address); });
    const std::optional<uint32_t> sample = sounding != samples.end() ? std::optional(*sounding)
                                           : samples.empty()         ? std::nullopt
                                                                     : std::optional(samples.front());

    const std::vector<int> all_songs = hokuto::song::GbaSongs(*game.rom);
    std::size_t tried = 0;
    const std::size_t every = std::max<std::size_t>(game.groups.size() / 16, 1);
    for (const hokuto::song::GbaVoiceGroup& group : game.groups)
    {
        if (tried++ % every != 0)
        {
            continue;
        }
        const std::string name = hokuto::song::GbaVoiceGroupName(group.address);
        NewKeyRows(game, group, totals);

        const std::vector<uint32_t> free = FreePrograms(game, group, totals);
        if (free.size() < 2 || group.programs.empty())
        {
            totals.groups_full++;
            continue;
        }
        mp2k::Voice source;
        const int source_program = group.programs.front();
        if (!mp2k::ReadVoice(game.rom->rom, group.address + 12 * static_cast<uint32_t>(source_program), source))
        {
            continue;
        }
        const std::vector<std::pair<uint32_t, mp2k::Voice>> voices = {
            {free[0], sample ? hokuto::edit::NewGbaSampleVoice(*sample) : hokuto::edit::NewGbaSquareVoice()},
            {free[1], source}};

        hokuto::edit::GbaGroupVersion version;
        try
        {
            version = hokuto::edit::WithGbaVoicesAt(game.file, *game.rom, group.address, voices);
        }
        catch (const std::invalid_argument& error)
        {
            if (LacksRoom(error))
            {
                totals.voices_no_room++;
            }
            else
            {
                std::printf("%s: new voices: %s: the group refuses its new voices: %s\n", game.name.c_str(),
                            name.c_str(), error.what());
                totals.failed++;
            }
            continue;
        }
        const bool moved = version.group != group.address;
        totals.groups_moved += moved ? 1 : 0;
        totals.groups_in_place += moved ? 0 : 1;

        const std::vector<uint8_t> exported = hokuto::format::ExportArchive(version.file);
        const std::shared_ptr<const hokuto::song::GbaRom> rom = hokuto::song::LoadGbaRom(exported);

        CheckExportedBytes(game, group, name, version, exported, *rom, totals);

        // The songs play as before, and the new voices play.
        CheckSongsPlay(game, group, name, all_songs, played, rom, source_program, free, sample, totals);

        // The Bank Editor lists both, and a deleted voice frees its program.
        CheckListedAndDeleted(game, name, *rom, version, free, totals);
    }
}

} // namespace hokuto::tools::gba_voices
