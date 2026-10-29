// SPDX-License-Identifier: MIT

// Tests a sequence sound as a file of another console (src/edit/other_console.h). A small archive of the Switch, and
// the same archive of the Wii U and of the 3DS, laid out as NintendoWare's tools lay one out, hold two sequence sounds
// of one file, three banks and two wave archives. The song has 3D settings, a single play and a user parameter. Its
// slots hold two banks. Their waves lie in both wave archives, in 8-bit and 16-bit PCM and DSP-ADPCM. One wave holds
// the original start of its loop: version 1.2.0, or 2.1.1.0 on the 3DS. The other sound's bank names a wave archive
// that the archive lacks. The file holds tags, scenes, a track's colour and the volumes of the aux buses. A file of the
// Switch or the Wii U also holds the enhanced mixing for the song and an effect of each of the two consoles. Each file
// holds a reverb of the 3DS and an AXFX chorus of the Wii. Each console keeps the effects that it runs. The song goes
// to each other console and back. ntdlib's performer has to play its notes, and ntdlib has to decode its waves as
// before. The file has to come back from both forms. The commands that the 3DS's player skips stay out of a song that
// goes to the 3DS or comes from it. A song whose wave archives don't fit in the memory of the 3DS's model goes there
// with only the instruments that it selects, and with only their waves. A song with more notes at once than the 3DS
// plays gets a warning. A song whose stereo waves take more channels at once than the 3DS plays takes them as mono. The
// biquad filters of a song take the types and values that come nearest on the other console. The song also goes to the
// Wii from each of the three. The Wii's player skips a bank select, and an end after a comparison becomes a jump to an
// end. ntdlib's performer has to play the song's notes on the Wii as before, and ntdlib has to decode the bank's waves
// as before. The file has to come back from both forms. A song that plays both of its banks goes to the Wii with them
// as one bank, and the Wii has to play each note with its instrument there. A bank whose waves don't fit in the memory
// of the Wii's model takes only the instruments that the notes select.

#include "edit/other_console.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/nwsnd/console.h"
#include "ntdlib/nwsnd/util.h"
#include "ntdlib/rsar/bank_file.h"
#include "ntdlib/rsar/decode.h"
#include "ntdlib/rsar/sequence_file.h"
#include "ntdlib/rsar/wave_file.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/archive_model.h"
#include "ntdlib/sar/bank_file.h"
#include "ntdlib/sar/decode.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/wave_file.h"
#include "ntdlib/sound_build.h"
#include "ntdlib/testing/rsar_files.h"
#include "ntdlib/testing/sar_files.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "edit/effects.h"
#include "edit/replace.h"
#include "edit/tables.h"
#include "edit/waves.h"
#include "format/archive.h"
#include "format/file.h"
#include "format/text.h"
#include "song/archive.h"
#include "song/biquads.h"
#include "song/items.h"
#include "song/sequence.h"

namespace
{

namespace format = hokuto::format;
namespace sar = ntdlib::sar;
namespace sar_files = ntdlib::testing::sar_files;
namespace song = hokuto::song;

using format::EffectKind;
using format::File;

// The archive's sounds, banks and wave archives.
constexpr uint32_t kSong = 0;
constexpr uint32_t kOther = 1;
constexpr uint32_t kWavesA = 0;
constexpr uint32_t kWavesB = 1;
constexpr uint32_t kWavesAId = 0x05000000;
constexpr uint32_t kWavesBId = 0x05000001;
constexpr uint32_t kLackedWavesId = 0x05000007;

// The sequence file. The song picks program 0 and plays a note, then program 1 and another. The other sound starts at
// offset 13 and plays a note of program 0.
const std::vector<uint8_t> kBytecode = {
    0xB0, 48,      // 0: timebase 48
    0x81, 0,       // 2: program 0
    60,   100, 24, // 4: key 60 for 24 ticks
    0x81, 1,       // 7: program 1
    62,   90,  12, // 9: key 62 for 12 ticks
    0xFF,          // 12: the end
    0x81, 0,       // 13: program 0
    64,   80,  24, // 15: key 64 for 24 ticks
    0xFF,          // 18: the end
};

// The message of the std::invalid_argument that `call` throws, or nothing when it throws none.
template <class Call>
std::string RefusalOf(const Call& call)
{
    try
    {
        call();
    }
    catch (const std::invalid_argument& error)
    {
        return error.what();
    }

    return "";
}

// The version of a wave of `flavor` without the original start of its loop.
uint32_t OldWaveVersion(sar::Flavor flavor)
{
    return flavor == sar::Flavor::kCtr ? 0x02010000 : 0x00010100;
}

// The version of a wave of `flavor` with the original start of its loop.
uint32_t NewWaveVersion(sar::Flavor flavor)
{
    return flavor == sar::Flavor::kCtr ? 0x02010100 : 0x00010200;
}

// A looped wave of the console's 16-bit PCM samples with the version `version`. The loop lets a note last its length on
// both consoles. A wave that ends sooner ends the note in another tick on each.
std::vector<uint8_t> PcmWave(sar::Flavor flavor, uint8_t value, uint32_t version)
{
    sar_files::WaveSpec spec;
    spec.encoding = 1;
    spec.loop = true;
    spec.loop_end = 4;
    spec.channels = {{value, value, 0, value, value, 0, 0, value}};
    std::vector<uint8_t> wave = sar_files::Wave(flavor, spec);
    (flavor == sar::Flavor::kCafe ? xsflib::StoreBe32 : xsflib::StoreLe32)(&wave[0x08], version);

    return wave;
}

// A looped wave of 8-bit PCM samples.
std::vector<uint8_t> Pcm8Wave(sar::Flavor flavor)
{
    sar_files::WaveSpec spec;
    spec.encoding = 0;
    spec.loop = true;
    spec.loop_end = 8;
    spec.channels = {{0x10, 0x20, 0x7F, 0x80, 0xF0, 0x00, 0x01, 0xFF}};

    return sar_files::Wave(flavor, spec);
}

// A DSP-ADPCM wave of two frames, 28 samples, with a loop from sample 14.
std::vector<uint8_t> AdpcmWave(sar::Flavor flavor)
{
    sar_files::WaveSpec spec;
    spec.encoding = 2;
    spec.loop = true;
    spec.loop_start = 14;
    spec.loop_end = 28;
    spec.channels = {{0x21, 0x13, 0x57, 0x9B, 0xDF, 0x02, 0x46, 0x8A, 0x12, 0xF1, 0xE2, 0xD3, 0xC4, 0xB5, 0xA6, 0x97}};

    return sar_files::Wave(flavor, spec);
}

// An instrument whose one region plays wave `wave` of its bank.
sar_files::KeyTable Instrument(uint32_t wave)
{
    sar_files::Region region;
    region.wave = wave;
    region.original_key = 60;
    region.adshr = {127, 127, 127, 0, 127};
    const sar_files::VelocityTable velocities{sar_files::VelocityTable::kDirect, 0, {{0, region}}};
    return sar_files::KeyTable{sar_files::KeyTable::kDirect, 0, {{0, velocities}}};
}

// A bank that names `waves`, with an instrument for each wave in turn.
std::vector<uint8_t> Bank(sar::Flavor flavor, const std::vector<std::pair<uint32_t, uint32_t>>& waves)
{
    sar_files::BankSpec bank;
    bank.waves = waves;
    for (uint32_t i = 0; i < waves.size(); ++i)
    {
        bank.instruments.push_back(Instrument(i));
    }

    return sar_files::Bank(flavor, bank);
}

// The same bank, laid out as NintendoWare's tools lay one out. Each region holds all of its values.
std::vector<uint8_t> ToolsBank(sar::Flavor flavor, const std::vector<std::pair<uint32_t, uint32_t>>& waves)
{
    sar::BankFile bank;
    bank.flavor = flavor;
    for (uint32_t i = 0; i < waves.size(); ++i)
    {
        bank.waves.push_back({waves[i].first, waves[i].second});
        sar::BankFile::Region region;
        region.values.wave_id_index = i;
        region.values.adshr = {127, 127, 127, 0, 127};
        bank.instruments.emplace_back().emplace().key_regions = {{0, 127, {region}}};
    }

    return bank.Write();
}

// The test archive of `flavor`, laid out as the tools lay one out, with `bytecode` in its sequence file. The song plays
// banks A and B. Bank A names both waves of wave archive A. Bank B names wave archive B's first wave and wave archive
// A's second wave. Wave archive B's first wave holds the original start of its loop, and its second 8-bit PCM. The
// other sound starts at offset `other_start` and plays bank C. Bank C names a wave archive that the archive lacks.
std::vector<uint8_t> TestArchive(sar::Flavor flavor, const std::vector<uint8_t>& bytecode = kBytecode,
                                 uint32_t other_start = 13)
{
    sar::ArchiveModel model;
    model.flavor = flavor;
    model.version = flavor == sar::Flavor::kCtr ? 0x02030000 : 0x00020200;
    model.players = {{.name = "PLAYER_TEST", .playable_sound_max = 1}};

    sar::SoundModel song;
    song.name = "SEQ_SONG";
    song.volume = 90;
    song.pan_mode = 1;
    song.pan_curve = 2;
    song.player_priority = 48;
    song.actor_player_id = 2;
    song.single_play = 0x00010005;
    song.settings_3d = {.flags = 0x7, .decay_ratio = 0.25f, .decay_curve = 2, .doppler_factor = 3};
    song.user_options = {0x12345678, std::nullopt, std::nullopt, std::nullopt};
    song.banks = {sar::ItemId(sar::kBankItem, 0), sar::ItemId(sar::kBankItem, 1)};
    song.allocate_track_flags = 3;
    song.channel_priority = 40;
    song.release_priority_fix = true;
    sar::SoundModel other;
    other.name = "SEQ_OTHER";
    other.banks = {sar::ItemId(sar::kBankItem, 2)};
    other.start_offset = other_start;
    model.sounds = {song, other};
    model.banks = {
        {.name = "BANK_A", .file_id = 1}, {.name = "BANK_B", .file_id = 2}, {.name = "BANK_C", .file_id = 3}};
    model.wave_archives = {{.name = "WARC_A", .file_id = 4}, {.name = "WARC_B", .file_id = 5}};
    model.files = {
        {.bytes = sar_files::Sequence(flavor, bytecode, {{"Song", 0}, {"Other", other_start}})},
        {.bytes = Bank(flavor, {{kWavesAId, 0}, {kWavesAId, 1}})},
        {.bytes = Bank(flavor, {{kWavesBId, 0}, {kWavesAId, 1}})},
        {.bytes = Bank(flavor, {{kLackedWavesId, 0}})},
        {.bytes = sar_files::WaveArchive(flavor, {PcmWave(flavor, 0x30, OldWaveVersion(flavor)), AdpcmWave(flavor)})},
        {.bytes = sar_files::WaveArchive(flavor, {PcmWave(flavor, 0x50, NewWaveVersion(flavor)), Pcm8Wave(flavor)})},
    };

    return sar::BuildArchive(model);
}

// The test file of `flavor` with `bytecode` in its sequence file, and with its tags, scenes, colours and buses'
// volumes. A file of the Switch or the Wii U holds the enhanced mixing for the song, an I3DL2 reverb of the Switch on
// bus A and a chorus of the Wii U on bus B. Each file holds a reverb of the 3DS on bus A and an AXFX chorus of the Wii
// on bus B. Bus C holds none: the 3DS lacks it.
File TestFile(sar::Flavor flavor, const std::vector<uint8_t>& bytecode = kBytecode, uint32_t other_start = 13)
{
    File file = format::ImportArchive(TestArchive(flavor, bytecode, other_start), flavor);
    file.engine = format::EngineOf(flavor);
    file.tags = {{"title", "Test songs"}};
    file.sound_tags = {{kSong, {{"title", "The song"}}}, {kOther, {{"title", "The other"}}}};
    file.scenes = {{.sound = kSong, .name = "Loud", .values = {{3, 5}}, .changes = {{.time = 96, .variable = 3}}},
                   {.sound = kOther, .name = "Quiet", .values = {{1, 0}}, .changes = {}}};
    file.track_colours = {{{kSong, 0}, {200, 100, 50}}, {{kOther, 0}, {1, 2, 3}}};
    file.aux_volumes = {0.5f, 1.0f, 1.5f};
    if (flavor != sar::Flavor::kCtr)
    {
        file.sound_mixing = {{kSong, format::Mixing::kEnhanced}};
        file.aux_effects[0] = {{EffectKind::kI3dl2Reverb, std::vector<float>(12, 0.0f)}};
        file.aux_effects[1] = {{EffectKind::kChorus, {0.1f, 0.2f, 0.3f, 0.4f, 0.5f}}};
    }
    file.aux_effects[0].push_back(
        {EffectKind::kCtrReverb, hokuto::edit::CtrStartingParameters(EffectKind::kCtrReverb)});
    file.aux_effects[1].push_back({EffectKind::kWiiChorus, {10.0f, 0.5f, 1.0f}});

    return file;
}

// The effects of `effects` that a file of the console of `to` keeps. Of the test file's four effects, the Switch runs
// the I3DL2 reverb, the Wii U nw::snd's chorus and the 3DS the reverb of the 3DS's nw::snd. The AXFX chorus runs on the
// Wii alone.
std::vector<format::Effect> KeptEffects(const std::vector<format::Effect>& effects, sar::Flavor to)
{
    const EffectKind runs = to == sar::Flavor::kNx     ? EffectKind::kI3dl2Reverb
                            : to == sar::Flavor::kCafe ? EffectKind::kChorus
                                                       : EffectKind::kCtrReverb;
    std::vector<format::Effect> kept;
    for (const format::Effect& effect : effects)
    {
        if (effect.kind == runs)
        {
            kept.push_back(effect);
        }
    }

    return kept;
}

// The archive that `file` exports, opened.
std::shared_ptr<const sar::SoundArchive> ArchiveOf(const File& file)
{
    return song::LoadArchive(format::ExportArchive(file), "test", format::ExportGroupFiles(file));
}

// The notes that ntdlib's performer plays of sound `sound` of `archive`, track by track.
std::vector<std::vector<song::Note>> NotesOf(const sar::SoundArchive& archive, uint32_t sound)
{
    std::vector<std::vector<song::Note>> notes;
    for (const song::Track& track : song::ReadSequence(archive, sound).tracks)
    {
        notes.push_back(track.notes);
    }

    return notes;
}

// `notes` without the offsets of their note commands. The offsets move when commands leave the sequence.
std::vector<std::vector<song::Note>> WithoutOffsets(std::vector<std::vector<song::Note>> notes)
{
    for (std::vector<song::Note>& track : notes)
    {
        for (song::Note& note : track)
        {
            note.command = 0;
        }
    }

    return notes;
}

// Whether wave archive `to` of `after` decodes to the waves of wave archive `from` of `before`.
bool SameWaves(const sar::SoundArchive& before, uint32_t from, const sar::SoundArchive& after, uint32_t to)
{
    const std::size_t count = song::WaveCountOf(before, from);
    bool same = count > 0 && song::WaveCountOf(after, to) == count;
    for (uint32_t wave = 0; same && wave < count; ++wave)
    {
        const sar::Pcm old_wave = song::DecodeItemWave(before, from, wave);
        const sar::Pcm new_wave = song::DecodeItemWave(after, to, wave);
        same = new_wave.sample_rate == old_wave.sample_rate && new_wave.loop == old_wave.loop &&
               new_wave.loop_start == old_wave.loop_start && new_wave.loop_end == old_wave.loop_end &&
               new_wave.channels == old_wave.channels;
    }

    return same;
}

// Whether `warnings` hold `warning`.
bool Holds(const std::vector<std::string>& warnings, const std::string& warning)
{
    return std::ranges::find(warnings, warning) != warnings.end();
}

// Whether `file` comes back from both forms unchanged.
bool ComesBack(const File& file)
{
    return format::Read(format::Write(file)) == file && format::ReadText(format::WriteText(file)) == file;
}

// The warnings of the song of the test file of `from` on its way to `to`, in their order: the waves, the sequence file
// and the effects.
std::vector<std::string> SongWarnings(sar::Flavor from, sar::Flavor to)
{
    std::vector<std::string> warnings;
    const bool ctr = from == sar::Flavor::kCtr || to == sar::Flavor::kCtr;
    if (to == sar::Flavor::kNx)
    {
        warnings.push_back(
            "1 wave of WARC_B goes in as 16-bit PCM, with the same samples. The Switch's renderer plays no 8-bit PCM.");
    }
    else
    {
        warnings.push_back("The waves of WARC_B go in as version " +
                           std::string(to == sar::Flavor::kCtr ? "2.1.0.0" : "1.1.0") + ", the oldest that the " +
                           song::ConsoleName(to) +
                           "'s games hold. That version holds no original start of a loop. Playback starts each loop "
                           "at its start either way.");
    }
    if (!ctr && to == sar::Flavor::kCafe)
    {
        warnings.push_back(
            "The sequence file goes in as version 1.0.0, the oldest that the Wii U's games hold. It held "
            "2.0.0. Its commands stay the same.");
    }
    warnings.push_back("The sequence file holds the commands of 1 other sound too. They go along unused.");

    // Bus A holds the Switch's reverb before the 3DS's, and bus B the Wii U's chorus before the Wii's.
    const std::string console = song::ConsoleName(to);
    if (to == sar::Flavor::kCtr)
    {
        warnings.push_back("The I3DL2 Reverb on bus A stays behind. The 3DS runs no such effect.");
        warnings.push_back("The nw::snd Chorus and AXFX Chorus on bus B stay behind. The 3DS runs no such effects.");
    }
    else if (from == sar::Flavor::kCtr)
    {
        warnings.push_back("The nw::snd Reverb (3DS) on bus A stays behind. The " + console + " runs no such effect.");
        warnings.push_back("The AXFX Chorus on bus B stays behind. The " + console + " runs no such effect.");
    }
    else if (to == sar::Flavor::kNx)
    {
        warnings.push_back("The nw::snd Reverb (3DS) on bus A stays behind. The Switch runs no such effect.");
        warnings.push_back("The nw::snd Chorus and AXFX Chorus on bus B stay behind. The Switch runs no such effects.");
    }
    else
    {
        warnings.push_back(
            "The I3DL2 Reverb and nw::snd Reverb (3DS) on bus A stay behind. The Wii U runs no such effects.");
        warnings.push_back("The AXFX Chorus on bus B stays behind. The Wii U runs no such effect.");
    }

    return warnings;
}

// The song goes from the console of `from` to the console of `to`. Its values, banks, waves and notes stay. So do the
// parts of the file that go with it.
void TestSong(sar::Flavor from, sar::Flavor to)
{
    const File source = TestFile(from);
    const hokuto::edit::OtherConsoleSong result = hokuto::edit::ForOtherConsole(source, kSong, to);
    const File& file = result.file;
    XSFLIB_CHECK(file.form == format::ArchiveForm::kModel && file.native->flavor == to &&
                 file.engine == format::EngineOf(to));

    // The tables: the song with its values, and the banks and wave archives that it plays.
    const format::NativeTables& tables = *file.native->tables;
    const format::NativeSound& before = source.native->tables->sounds[kSong];
    XSFLIB_CHECK(tables.sounds.size() == 1);
    const format::NativeSound& after = tables.sounds.at(0);
    XSFLIB_CHECK(after.name == "SEQ_SONG" && after.volume == 90 && after.pan_mode == 1 && after.pan_curve == 2 &&
                 after.player_priority == 48 && after.actor_player == 2 && after.tracks == 3 &&
                 after.channel_priority == 40 && after.release_priority_fix);
    XSFLIB_CHECK(after.volume == before.volume && after.tracks == before.tracks);
    XSFLIB_CHECK(
        (after.banks == std::vector<uint32_t>{sar::ItemId(sar::kBankItem, 0), sar::ItemId(sar::kBankItem, 1)}));
    XSFLIB_CHECK((tables.banks == std::vector<std::string>{"BANK_A", "BANK_B"}));
    XSFLIB_CHECK((tables.wave_archives == std::vector<std::string>{"WARC_A", "WARC_B"}));

    // The settings that the model holds.
    const sar::SoundModel old_model = sar::ReadArchiveModel(format::ExportArchive(source), from).sounds[kSong];
    const sar::SoundModel new_model = sar::ReadArchiveModel(format::ExportArchive(file), to).sounds.at(0);
    XSFLIB_CHECK(new_model.settings_3d == old_model.settings_3d && new_model.single_play == old_model.single_play &&
                 new_model.user_options == old_model.user_options &&
                 new_model.remote_filter == old_model.remote_filter);

    // The notes, the instruments and the waves.
    const std::shared_ptr<const sar::SoundArchive> old_archive = ArchiveOf(source);
    const std::shared_ptr<const sar::SoundArchive> new_archive = ArchiveOf(file);
    const std::vector<std::vector<song::Note>> notes = NotesOf(*old_archive, kSong);
    XSFLIB_CHECK(notes.size() == 1 && notes[0].size() == 2);
    XSFLIB_CHECK(NotesOf(*new_archive, 0) == notes);
    XSFLIB_CHECK(song::ProgramsOf(*new_archive, 0) == song::ProgramsOf(*old_archive, 0) &&
                 song::ProgramsOf(*new_archive, 1) == song::ProgramsOf(*old_archive, 1));
    XSFLIB_CHECK(SameWaves(*old_archive, kWavesA, *new_archive, 0) &&
                 SameWaves(*old_archive, kWavesB, *new_archive, 1));
    XSFLIB_CHECK(hokuto::edit::PresentWave(*new_archive, 1, 1).encoding ==
                 (to == sar::Flavor::kNx ? sar::WaveEncoding::kPcm16 : sar::WaveEncoding::kPcm8));

    // The parts of the file that go with the song. The song keeps its mixing on every console.
    XSFLIB_CHECK((file.tags == format::Tags{{"title", "Test songs"}}));
    XSFLIB_CHECK((file.sound_tags == std::map<uint32_t, format::Tags>{{0, {{"title", "The song"}}}}));
    XSFLIB_CHECK(file.scenes.size() == 1 && file.scenes[0].sound == 0 && file.scenes[0].name == "Loud" &&
                 file.scenes[0].values == source.scenes[0].values &&
                 file.scenes[0].changes == source.scenes[0].changes);
    XSFLIB_CHECK((file.track_colours == std::map<format::TrackPlace, format::Colour>{{{0, 0}, {200, 100, 50}}}));
    const bool enhanced = from != sar::Flavor::kCtr;
    XSFLIB_CHECK(file.mixing == (enhanced ? format::Mixing::kEnhanced : format::Mixing::kStrict) &&
                 file.sound_mixing.empty());
    XSFLIB_CHECK(file.aux_volumes == source.aux_volumes);

    // Each console keeps the effects that it runs. The Switch keeps the I3DL2 reverb of a file of the Wii U, and the
    // Wii U the chorus of a file of the Switch. The 3DS keeps its reverb. The AXFX chorus stays behind.
    const bool later = from != sar::Flavor::kCtr;
    const std::size_t on_a = to == sar::Flavor::kCtr || (later && to == sar::Flavor::kNx) ? 1 : 0;
    const std::size_t on_b = later && to == sar::Flavor::kCafe ? 1 : 0;
    XSFLIB_CHECK(file.aux_effects[0].size() == on_a && file.aux_effects[1].size() == on_b);
    XSFLIB_CHECK(file.aux_effects[0] == KeptEffects(source.aux_effects[0], to));
    XSFLIB_CHECK(file.aux_effects[1] == KeptEffects(source.aux_effects[1], to));
    XSFLIB_CHECK(file.aux_effects[2].empty());

    // The warnings.
    XSFLIB_CHECK(result.warnings == SongWarnings(from, to));
    XSFLIB_CHECK(ComesBack(file));

    // Back to the first console: the same notes and waves again.
    const hokuto::edit::OtherConsoleSong back = hokuto::edit::ForOtherConsole(file, 0, from);
    XSFLIB_CHECK(back.file.native->flavor == from);
    const std::shared_ptr<const sar::SoundArchive> back_archive = ArchiveOf(back.file);
    XSFLIB_CHECK(NotesOf(*back_archive, 0) == notes);
    XSFLIB_CHECK(SameWaves(*old_archive, kWavesA, *back_archive, 0) &&
                 SameWaves(*old_archive, kWavesB, *back_archive, 1));
    XSFLIB_CHECK(back.file.native->tables->sounds.at(0) == file.native->tables->sounds.at(0));
    XSFLIB_CHECK(ComesBack(back.file));
}

// A bank whose wave archive the archive lacks stays behind, and its slot stays empty.
void TestBankLeftBehind(sar::Flavor from, sar::Flavor to)
{
    const hokuto::edit::OtherConsoleSong result = hokuto::edit::ForOtherConsole(TestFile(from), kOther, to);
    const format::NativeTables& tables = *result.file.native->tables;
    XSFLIB_CHECK((tables.sounds.at(0).banks == std::vector<uint32_t>{sar::kNoItem}));
    XSFLIB_CHECK(tables.banks.empty() && tables.wave_archives.empty());
    XSFLIB_CHECK(Holds(result.warnings,
                       "Bank BANK_C stays behind, and its slot stays empty. The archive has no wave archive 7."));
    XSFLIB_CHECK(ComesBack(result.file));
}

// An archive that the tools didn't lay out: the song goes to the other console without the settings that only the model
// reads.
void TestSettingsUnread(sar::Flavor from, sar::Flavor to)
{
    sar_files::SoundSpec sound;
    sound.name = "SEQ_SONG";
    sound.banks = {0};
    sar_files::ArchiveSpec spec;
    spec.sounds = {sound};
    spec.banks = {{"BANK_A", 1}};
    spec.wave_archives = {{"WARC_A", 2}};
    spec.players = {{"PLAYER_TEST", 1}};
    spec.files = {
        {.bytes = sar_files::Sequence(from, kBytecode, {{"Song", 0}})},
        {.bytes = Bank(from, {{kWavesAId, 0}, {kWavesAId, 1}})},
        {.bytes = sar_files::WaveArchive(from, {PcmWave(from, 0x30, OldWaveVersion(from)), AdpcmWave(from)})}};
    const File source = format::ImportArchive(sar_files::Archive(from, spec), from);
    const hokuto::edit::OtherConsoleSong result = hokuto::edit::ForOtherConsole(source, 0, to);
    const std::string kUnread =
        "The sound's 3D settings, single play, user parameters and remote filter stay behind. "
        "Hokuto can't read them from this archive.";
    XSFLIB_CHECK(
        std::ranges::any_of(result.warnings, [&](const std::string& warning) { return warning.starts_with(kUnread); }));
    const std::shared_ptr<const sar::SoundArchive> before = ArchiveOf(source);
    const std::shared_ptr<const sar::SoundArchive> after = ArchiveOf(result.file);
    XSFLIB_CHECK(NotesOf(*after, 0) == NotesOf(*before, 0) && SameWaves(*before, 0, *after, 0));
    XSFLIB_CHECK(ComesBack(result.file));
}

// The test sequence with a byte that no command reaches at its end, after `extra` at its start.
std::vector<uint8_t> FixedBytecode(const std::vector<uint8_t>& extra = {})
{
    std::vector<uint8_t> bytecode = extra;
    bytecode.insert(bytecode.end(), kBytecode.begin(), kBytecode.end());
    bytecode.push_back(0x55);

    return bytecode;
}

// A volume2 of 90 over 48 ticks, a main volume of 80 over 24 ticks and a pitch bend of 16 over 12 ticks. The 3DS and
// the Wii U write them alike. The 3DS sets volume2 and the main volume at once, and moves the pitch bend over its time.
// The Wii U and the Switch move all three over their times.
const std::vector<uint8_t> kTimedSettings = {
    0xA3, 0xD5, 0x5A, 0x00, 0x30, // 0: volume2 90 over 48 ticks
    0xA3, 0xC2, 0x50, 0x00, 0x18, // 5: main volume 80 over 24 ticks
    0xA3, 0xC4, 0x10, 0x00, 0x0C, // 10: pitch bend 16 over 12 ticks
};

// A song of the 3DS goes to the console of `to` with its volume2 and its main volume set at once, with a warning. The
// 3DS sets them at once too. The pitch bend keeps its time. A sequence whose commands keep their places keeps the
// times, with a warning. A song of the Wii U keeps its times on the way to the 3DS, without a warning.
void TestCtrTimes(sar::Flavor to)
{
    using ntdlib::commands::Command;
    using ntdlib::commands::Kind;
    std::vector<uint8_t> bytecode = kTimedSettings;
    bytecode.insert(bytecode.end(), kBytecode.begin(), kBytecode.end());
    const auto other_start = static_cast<uint32_t>(kTimedSettings.size() + 13);
    const File source = TestFile(sar::Flavor::kCtr, bytecode, other_start);
    const File fixed = TestFile(sar::Flavor::kCtr, FixedBytecode(kTimedSettings), other_start);
    const File later = TestFile(sar::Flavor::kCafe, bytecode, other_start);

    const auto timed = [](const ntdlib::commands::Sequence& sequence, Kind kind)
    {
        const auto found = std::ranges::find(sequence.items, kind, &Command::kind);
        return found != sequence.items.end() && found->time.has_value();
    };

    const auto times_warned = [](const std::vector<std::string>& warnings)
    {
        const auto timed_warning = [](const std::string& warning)
        {
            return warning.starts_with("The 3DS sets volume2 and the main volume at once.");
        };
        return std::ranges::any_of(warnings, timed_warning);
    };

    const hokuto::edit::OtherConsoleSong result = hokuto::edit::ForOtherConsole(source, kSong, to);
    const hokuto::edit::OtherConsoleSong kept = hokuto::edit::ForOtherConsole(fixed, kSong, to);
    const hokuto::edit::OtherConsoleSong to_ctr = hokuto::edit::ForOtherConsole(later, kSong, sar::Flavor::kCtr);

    const ntdlib::commands::Sequence& sequence = *result.file.sequences.at(0);
    XSFLIB_CHECK(!timed(sequence, Kind::kVolume2) && !timed(sequence, Kind::kMainVolume) &&
                 timed(sequence, Kind::kPitchBend));
    XSFLIB_CHECK(Holds(result.warnings,
                       "The 3DS sets volume2 and the main volume at once. The times of 2 such "
                       "settings stay behind. The " +
                           song::ConsoleName(to) + " sets those settings at once too."));
    XSFLIB_CHECK(WithoutOffsets(NotesOf(*ArchiveOf(result.file), 0)) ==
                 WithoutOffsets(NotesOf(*ArchiveOf(source), kSong)));
    XSFLIB_CHECK(Holds(kept.warnings,
                       "The 3DS sets volume2 and the main volume at once. 2 such settings keep their "
                       "times: the file's commands keep their places. The " +
                           song::ConsoleName(to) + " moves those settings over the times."));
    XSFLIB_CHECK(NotesOf(*ArchiveOf(kept.file), 0) == NotesOf(*ArchiveOf(fixed), kSong));
    const ntdlib::commands::Sequence& kept_sequence = *kept.file.sequences.at(0);
    XSFLIB_CHECK(timed(kept_sequence, Kind::kVolume2) && timed(kept_sequence, Kind::kMainVolume));
    const ntdlib::commands::Sequence& kept_times = *to_ctr.file.sequences.at(0);
    XSFLIB_CHECK(timed(kept_times, Kind::kVolume2) && timed(kept_times, Kind::kMainVolume) &&
                 timed(kept_times, Kind::kPitchBend) && !times_warned(to_ctr.warnings));
    XSFLIB_CHECK(ComesBack(result.file));
}

// The test sequence after a biquad filter of `type` with value 70. The other sound starts at offset 17.
std::vector<uint8_t> BiquadBytecode(uint8_t type)
{
    std::vector<uint8_t> bytecode = {0xB4, type, 0xB5, 70};
    bytecode.insert(bytecode.end(), kBytecode.begin(), kBytecode.end());

    return bytecode;
}

// The warning about the biquad filters between the Switch and `other`, after the sentences that name the rates.
std::string BiquadText(const std::string& other, const std::string& changes)
{
    return "The Switch plays the biquad filters of types 1 to 5 at 48 kHz, and the " + other + " at " +
           (other == "3DS" ? "32,728 Hz" : "32 kHz") +
           ". Their cutoffs lie about 1.5 times higher on the Switch. The Switch's types 6 to 10 are the same filters "
           "made for 48 kHz. " +
           changes;
}

// The Switch plays the biquad filters of types 1 to 5 at 48 kHz, and the other consoles at 32 kHz or about 32,728 Hz. A
// high-pass of the Wii U or the 3DS goes to the Switch as type 7, the same filter made for 48 kHz. A high-pass of the
// Switch goes to the Wii U, the 3DS and the Wii with the value whose passband lies nearest there
// (song::NearestBiquadValue). Type 7 goes to the Wii U and the 3DS as type 2 with its value. Each gets a warning. The
// high-pass plays its notes as before on the Switch, the Wii U and the 3DS.
void TestBiquads()
{
    using ntdlib::commands::Command;
    using ntdlib::commands::Kind;
    using ntdlib::commands::kOne;

    const auto setting = [](const ntdlib::commands::Sequence& sequence, Kind kind)
    {
        const auto found = std::ranges::find(sequence.items, kind, &Command::kind);
        return found != sequence.items.end() ? found->value.value / kOne : -1;
    };

    const auto plays_as = [](const File& before, const hokuto::edit::OtherConsoleSong& after)
    {
        return WithoutOffsets(NotesOf(*ArchiveOf(after.file), 0)) == WithoutOffsets(NotesOf(*ArchiveOf(before), kSong));
    };

    for (const sar::Flavor from : {sar::Flavor::kCafe, sar::Flavor::kCtr})
    {
        const File source = TestFile(from, BiquadBytecode(2), 17);
        const hokuto::edit::OtherConsoleSong result = hokuto::edit::ForOtherConsole(source, kSong, sar::Flavor::kNx);
        const ntdlib::commands::Sequence& sequence = *result.file.sequences.at(0);
        XSFLIB_CHECK(setting(sequence, Kind::kBiquadType) == 7 && setting(sequence, Kind::kBiquadValue) == 70);
        XSFLIB_CHECK(Holds(result.warnings,
                           BiquadText(song::ConsoleName(from), "1 setting of types 1 to 5 took types 6 to 10.")));
        XSFLIB_CHECK(plays_as(source, result));
    }

    const File high_pass = TestFile(sar::Flavor::kNx, BiquadBytecode(2), 17);
    const File later = TestFile(sar::Flavor::kNx, BiquadBytecode(7), 17);
    for (const sar::Flavor to : {sar::Flavor::kCafe, sar::Flavor::kCtr})
    {
        const std::string name = song::ConsoleName(to);
        const hokuto::edit::OtherConsoleSong result = hokuto::edit::ForOtherConsole(high_pass, kSong, to);
        const ntdlib::commands::Sequence& sequence = *result.file.sequences.at(0);
        const int nearest = song::NearestBiquadValue(2, 70, {.flavor = to});
        XSFLIB_CHECK(nearest != 70 && setting(sequence, Kind::kBiquadType) == 2 &&
                     setting(sequence, Kind::kBiquadValue) == nearest);
        XSFLIB_CHECK(Holds(result.warnings, BiquadText(name,
                                                       "1 setting of the value of a filter of types 1 to 5 "
                                                       "took the value whose passband on the " +
                                                           name + " lies nearest.")));
        XSFLIB_CHECK(plays_as(high_pass, result));

        const hokuto::edit::OtherConsoleSong exact = hokuto::edit::ForOtherConsole(later, kSong, to);
        const ntdlib::commands::Sequence& kept = *exact.file.sequences.at(0);
        XSFLIB_CHECK(setting(kept, Kind::kBiquadType) == 2 && setting(kept, Kind::kBiquadValue) == 70);
        XSFLIB_CHECK(Holds(exact.warnings, BiquadText(name, "1 setting of types 6 to 10 took types 1 to 5.")));
    }

    // The Wii's build of June 2008 plays the biquad filters at 32 kHz.
    const hokuto::edit::OtherConsoleSong wii = hokuto::edit::ForWii(high_pass, kSong);
    const ntdlib::commands::Sequence& sequence = *wii.file.sequences.at(0);
    const int nearest = song::NearestBiquadValue(2, 70, {.wii = format::BuildOfWiiArchive(*wii.file.native)});
    XSFLIB_CHECK(setting(sequence, Kind::kBiquadType) == 2 && setting(sequence, Kind::kBiquadValue) == nearest);
    XSFLIB_CHECK(Holds(wii.warnings, BiquadText("Wii",
                                                "1 setting of the value of a filter of types 1 to 5 took "
                                                "the value whose passband on the Wii lies nearest.")));
}

// A sequence file with a byte that no command reaches: its commands have to keep their places. The file goes in as the
// other console's decoder reads the converted file, and the song plays the same notes.
void TestFixedCommands(sar::Flavor from, sar::Flavor to)
{
    const File source = TestFile(from, FixedBytecode());
    const hokuto::edit::OtherConsoleSong result = hokuto::edit::ForOtherConsole(source, kSong, to);
    XSFLIB_CHECK(Holds(result.warnings,
                       "The sequence file's commands have to keep their places. The file goes in "
                       "converted with each command in its place."));
    const std::shared_ptr<const sar::SoundArchive> before = ArchiveOf(source);
    const std::shared_ptr<const sar::SoundArchive> after = ArchiveOf(result.file);
    XSFLIB_CHECK(NotesOf(*after, 0) == NotesOf(*before, kSong));
    XSFLIB_CHECK(after->Sounds().at(0).start_offset == before->Sounds().at(kSong).start_offset);
    XSFLIB_CHECK(ComesBack(result.file));
}

// A sequence file with a send to bus C and the settings of two LFOs. The song sends to bus C, calls a subroutine at
// offset 20 and plays key 62. The subroutine sets LFO 0's curve, plays key 60 and sets LFO 1's depth. The 3DS's player
// skips the three settings. The 3DS reads LFO 1's depth without its byte. The other sound starts at offset 14. The
// Switch gives the call's offset in little-endian order, and the Wii U and the 3DS in big-endian order.
std::vector<uint8_t> SkippedBytecode(sar::Flavor flavor)
{
    const bool little = flavor == sar::Flavor::kNx;
    const uint8_t low = little ? 20 : 0;
    const uint8_t high = little ? 0 : 20;
    std::vector<uint8_t> bytecode = {
        0xB0, 48,             // 0: timebase 48
        0xDE, 20,             // 2: send C 20
        0x8A, low,  0,  high, // 4: call 20
        0x81, 1,              // 8: program 1
        62,   90,   12,       // 10: key 62 for 12 ticks
        0xFF,                 // 13: the end
        0x81, 0,              // 14: program 0
        64,   80,   24,       // 16: key 64 for 24 ticks
        0xFF,                 // 19: the end
        0xBE, 1,              // 20: LFO 0's curve 1
        0x81, 0,              // 22: program 0
        60,   100,  24,       // 24: key 60 for 24 ticks
        0xF0, 0xA2,           // 27: LFO 1's depth
    };
    if (flavor != sar::Flavor::kCtr)
    {
        bytecode.push_back(5);
    }
    bytecode.push_back(0xFD); // the return

    return bytecode;
}

// The commands that the 3DS's player skips stay out of a song that goes to the 3DS or comes from it. A call of a
// command that stays out calls the next command that stays. The song plays the same notes. Between the Switch and the
// Wii U, every command goes along.
void TestSkipped(sar::Flavor from, sar::Flavor to)
{
    using ntdlib::commands::Command;
    using ntdlib::commands::Kind;
    const File source = TestFile(from, SkippedBytecode(from), 14);
    const hokuto::edit::OtherConsoleSong result = hokuto::edit::ForOtherConsole(source, kSong, to);
    const ntdlib::commands::Sequence& kept = *result.file.sequences.at(0);
    const auto skipped = [](const Command& command)
    {
        return command.kind == Kind::kSend || command.kind == Kind::kLfoCurve || command.kind == Kind::kLfoDepth ||
               command.kind == Kind::kOther;
    };
    const auto call = std::ranges::find(kept.items, Kind::kCall, &Command::kind);
    XSFLIB_CHECK(call != kept.items.end());
    if (from != sar::Flavor::kCtr && to != sar::Flavor::kCtr)
    {
        XSFLIB_CHECK(kept.items.size() == 14 && std::ranges::count_if(kept.items, skipped) == 3);
        const auto left_out_warning = [](const std::string& warning)
        {
            return warning.starts_with("The new file leaves out");
        };
        XSFLIB_CHECK(std::ranges::none_of(result.warnings, left_out_warning));
        XSFLIB_CHECK(call != kept.items.end() && kept.items.at(call->target).kind == Kind::kLfoCurve);
    }
    else
    {
        const std::string why = to == sar::Flavor::kCtr ? "The 3DS's player skips them."
                                                        : "The 3DS's player skipped them. The " +
                                                              song::ConsoleName(to) + "'s player would run them.";
        XSFLIB_CHECK(Holds(result.warnings,
                           "The new file leaves out 3 of the sequence's commands: 1 of LFO 0's curve, "
                           "1 of LFOs 1 to 3 and 1 of send C. " +
                               why));
        XSFLIB_CHECK(kept.items.size() == 11 && std::ranges::none_of(kept.items, skipped));
        XSFLIB_CHECK(call != kept.items.end() && kept.items.at(call->target).kind == Kind::kProgram);
    }

    // The notes, and the other sound's start.
    const std::shared_ptr<const sar::SoundArchive> before = ArchiveOf(source);
    const std::shared_ptr<const sar::SoundArchive> after = ArchiveOf(result.file);
    const std::vector<std::vector<song::Note>> notes = NotesOf(*before, kSong);
    XSFLIB_CHECK(notes.size() == 1 && notes[0].size() == 2);
    XSFLIB_CHECK(WithoutOffsets(NotesOf(*after, 0)) == WithoutOffsets(notes));
    const auto other_program = [&](const ntdlib::commands::Label& label)
    {
        return label.name == "Other" && kept.items.at(label.item).kind == Kind::kProgram;
    };
    XSFLIB_CHECK(std::ranges::any_of(kept.labels, other_program));
    XSFLIB_CHECK(ComesBack(result.file));
}

// A sequence whose commands have to keep their places goes to the 3DS or comes from it with the commands that the 3DS's
// player skips in their places, with a warning. A setting of LFO 1 can't keep its place: the 3DS reads it without its
// byte.
void TestFixedSkipped()
{
    // A send to bus C keeps its place.
    for (const auto [from, to] :
         {std::pair{sar::Flavor::kNx, sar::Flavor::kCtr}, std::pair{sar::Flavor::kCtr, sar::Flavor::kCafe}})
    {
        const File source = TestFile(from, FixedBytecode({0xDE, 20}), 15);
        const hokuto::edit::OtherConsoleSong result = hokuto::edit::ForOtherConsole(source, kSong, to);
        const std::string why = to == sar::Flavor::kCtr ? "The 3DS's player skips it."
                                                        : "The 3DS's player skipped it. The Wii U's player runs it.";
        XSFLIB_CHECK(Holds(result.warnings, "1 of the sequence's commands stays in its place: 1 of send C. " + why));
        const std::shared_ptr<const sar::SoundArchive> before = ArchiveOf(source);
        const std::shared_ptr<const sar::SoundArchive> after = ArchiveOf(result.file);
        XSFLIB_CHECK(NotesOf(*after, 0) == NotesOf(*before, kSong));
        XSFLIB_CHECK(after->Sounds().at(0).start_offset == before->Sounds().at(kSong).start_offset);
        XSFLIB_CHECK(ComesBack(result.file));
    }

    // A setting of LFO 1 can't.
    for (const auto [from, to] :
         {std::pair{sar::Flavor::kCafe, sar::Flavor::kCtr}, std::pair{sar::Flavor::kCtr, sar::Flavor::kNx}})
    {
        const std::vector<uint8_t> depth =
            from == sar::Flavor::kCtr ? std::vector<uint8_t>{0xF0, 0xA2} : std::vector<uint8_t>{0xF0, 0xA2, 5};
        const File source = TestFile(from, FixedBytecode(depth), static_cast<uint32_t>(13 + depth.size()));
        const std::string refusal = RefusalOf([&] { hokuto::edit::ForOtherConsole(source, kSong, to); });
        XSFLIB_CHECK(refusal == "The sequence file's commands have to keep their places, and the " +
                                    song::ConsoleName(to) + " reads some of them with other lengths.");
    }
}

// The sequence of the trim's test: the song picks program 2, or a program from variable 3 with `variable`, and plays a
// note.
std::vector<uint8_t> TrimBytecode(bool variable)
{
    return variable ? std::vector<uint8_t>{0xB0, 48, 0xA1, 0x81, 3, 60, 100, 24, 0xFF}
                    : std::vector<uint8_t>{0xB0, 48, 0x81, 2, 60, 100, 24, 0xFF};
}

// A looped wave of two channels of 16-bit PCM samples. The first channel holds PcmWave's samples of `value`. The second
// holds the same, or silence with `silent`.
std::vector<uint8_t> StereoWave(sar::Flavor flavor, uint8_t value, bool silent, uint32_t version)
{
    sar_files::WaveSpec spec;
    spec.encoding = 1;
    spec.loop = true;
    spec.loop_end = 4;
    const std::vector<uint8_t> samples = {value, value, 0, value, value, 0, 0, value};
    spec.channels = {samples, silent ? std::vector<uint8_t>(samples.size()) : samples};
    std::vector<uint8_t> wave = sar_files::Wave(flavor, spec);
    (flavor == sar::Flavor::kCafe ? xsflib::StoreBe32 : xsflib::StoreLe32)(&wave[0x08], version);

    return wave;
}

// The trim's test file of `flavor`, with `bytecode` in its sequence file. Bank A in the song's first slot holds three
// instruments, for programs 0 to 2, and they play the three waves of wave archive A. Bank B in the second slot holds
// two, and they play the two waves of wave archive B. NintendoWare's tools lay the banks out. Each wave is mono, or
// with `stereo`, the first and third waves of wave archive A hold two channels. The first wave's second channel is
// silent. The third wave's channels are the same, and they come within 0.04 dB of full scale.
File TrimFile(sar::Flavor flavor, const std::vector<uint8_t>& bytecode, bool stereo = false)
{
    sar::ArchiveModel model;
    model.flavor = flavor;
    model.version = 0x00020200;
    model.players = {{.name = "PLAYER_TEST", .playable_sound_max = 1}};
    sar::SoundModel trimmed;
    trimmed.name = "SEQ_TRIM";
    trimmed.banks = {sar::ItemId(sar::kBankItem, 0), sar::ItemId(sar::kBankItem, 1)};
    trimmed.allocate_track_flags = 1;
    model.sounds = {trimmed};
    model.banks = {{.name = "BANK_A", .file_id = 1}, {.name = "BANK_B", .file_id = 2}};
    model.wave_archives = {{.name = "WARC_A", .file_id = 3}, {.name = "WARC_B", .file_id = 4}};
    const uint32_t version = OldWaveVersion(flavor);
    model.files = {
        {.bytes = sar_files::Sequence(flavor, bytecode, {{"Trim", 0}})},
        {.bytes = ToolsBank(flavor, {{kWavesAId, 0}, {kWavesAId, 1}, {kWavesAId, 2}})},
        {.bytes = ToolsBank(flavor, {{kWavesBId, 0}, {kWavesBId, 1}})},
        {.bytes = sar_files::WaveArchive(
             flavor, {stereo ? StereoWave(flavor, 0x10, true, version) : PcmWave(flavor, 0x10, version),
                      PcmWave(flavor, 0x20, version),
                      stereo ? StereoWave(flavor, 0x7F, false, version) : PcmWave(flavor, 0x30, version)})},
        {.bytes = sar_files::WaveArchive(flavor, {PcmWave(flavor, 0x40, version), PcmWave(flavor, 0x50, version)})},
    };

    return format::ImportArchive(sar::BuildArchive(model), flavor);
}

// The same with the sequence of TrimBytecode.
File TrimFile(sar::Flavor flavor, bool variable)
{
    return TrimFile(flavor, TrimBytecode(variable));
}

// The bytes that the wave archives of `archive` take in the memory of the 3DS's model, each at a multiple of 32 bytes.
std::size_t WaveMemoryOf(const sar::SoundArchive& archive)
{
    std::size_t bytes = 0;
    for (uint32_t i = 0; i < archive.WaveArchives().size(); ++i)
    {
        bytes += (song::ItemFile(archive, {.kind = song::ItemKind::kWaveArchive, .index = i}).size() + 31) / 32 * 32;
    }

    return bytes;
}

// `bytes` in the warnings' words for sizes below 1 MB, such as "800 bytes" or "1.2 KB".
std::string KbText(std::size_t bytes)
{
    std::array<char, 32> text{};
    if (bytes < 1024)
    {
        std::snprintf(text.data(), text.size(), "%zu bytes", bytes);
    }
    else
    {
        std::snprintf(text.data(), text.size(), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    }

    return text.data();
}

// The message of the std::invalid_argument that ForOtherConsole throws for `file`'s first sound on its way to the 3DS
// with `memory`, or nothing.
std::string RefusalForCtr(const File& file, std::size_t memory)
{
    return RefusalOf(
        [&] { hokuto::edit::ForOtherConsole(file, 0, sar::Flavor::kCtr, hokuto::edit::MonoWaves::kAuto, memory); });
}

// A song of the console of `from` goes to the 3DS. When the wave archives fit in the memory of the 3DS's model as files
// of the 3DS, they go whole. When they don't, bank A keeps the instruments of programs 0 and 2, and bank B that of
// program 0. Wave archive A keeps its first and third waves, and wave archive B its first. The song plays the same
// note. A memory too small for the waves that stay refuses the song. So does a program from a variable, when the wave
// archives don't fit whole.
void TestTrim(sar::Flavor from)
{
    const File file = TrimFile(from, false);
    const std::shared_ptr<const sar::SoundArchive> old_archive = ArchiveOf(file);
    const hokuto::edit::OtherConsoleSong fits = hokuto::edit::ForOtherConsole(file, 0, sar::Flavor::kCtr);
    const std::shared_ptr<const sar::SoundArchive> fitting = ArchiveOf(fits.file);
    const std::size_t whole = WaveMemoryOf(*fitting);
    const hokuto::edit::OtherConsoleSong song =
        hokuto::edit::ForOtherConsole(file, 0, sar::Flavor::kCtr, hokuto::edit::MonoWaves::kAuto, whole - 1);
    const std::shared_ptr<const sar::SoundArchive> new_archive = ArchiveOf(song.file);
    const std::size_t kept = WaveMemoryOf(*new_archive);

    // Whether wave `new_wave` of the new archive's wave archive `new_archive_index` holds the samples and the loop of
    // wave `old_wave` of the old archive's wave archive `old_archive_index`.
    const auto same_wave =
        [&](uint32_t old_archive_index, uint32_t old_wave, uint32_t new_archive_index, uint32_t new_wave)
    {
        const sar::Pcm before = song::DecodeItemWave(*old_archive, old_archive_index, old_wave);
        const sar::Pcm after = song::DecodeItemWave(*new_archive, new_archive_index, new_wave);
        return before.channels == after.channels && before.loop_start == after.loop_start &&
               before.loop_end == after.loop_end;
    };

    XSFLIB_CHECK(song::ProgramsOf(*fitting, 0) == (std::vector<uint32_t>{0, 1, 2}));
    XSFLIB_CHECK(SameWaves(*old_archive, kWavesA, *fitting, 0) && SameWaves(*old_archive, kWavesB, *fitting, 1));
    XSFLIB_CHECK(std::ranges::none_of(
        fits.warnings, [](const std::string& warning) { return warning.starts_with("The banks go in with only"); }));
    XSFLIB_CHECK(song::ProgramsOf(*new_archive, 0) == (std::vector<uint32_t>{0, 2}));
    XSFLIB_CHECK(song::ProgramsOf(*new_archive, 1) == (std::vector<uint32_t>{0}));
    SEQLIB_CHECK_EQ(song::WaveCountOf(*new_archive, 0), std::size_t{2});
    SEQLIB_CHECK_EQ(song::WaveCountOf(*new_archive, 1), std::size_t{1});
    XSFLIB_CHECK(same_wave(kWavesA, 0, 0, 0) && same_wave(kWavesA, 2, 0, 1) && same_wave(kWavesB, 0, 1, 0));
    XSFLIB_CHECK(WithoutOffsets(NotesOf(*new_archive, 0)) == WithoutOffsets(NotesOf(*old_archive, 0)));
    XSFLIB_CHECK(!NotesOf(*new_archive, 0).empty() && NotesOf(*new_archive, 0)[0].size() == 1);
    XSFLIB_CHECK(Holds(song.warnings,
                       "The banks go in with only the instruments of program 0 and of the programs that "
                       "the sound selects. Their wave archives hold only those instruments' waves: " +
                           KbText(kept) + ". Whole, the wave archives would take " + KbText(whole) +
                           ". Hokuto's model of the 3DS holds " + KbText(whole - 1) + " of wave archives."));
    XSFLIB_CHECK(ComesBack(song.file));

    XSFLIB_CHECK(RefusalForCtr(file, kept - 1) ==
                 "The wave archives of the sound's banks take " + KbText(kept) +
                     ", even with only the instruments that the sound selects. Hokuto's model of the 3DS holds " +
                     KbText(kept - 1) + " of wave archives.");
    XSFLIB_CHECK(RefusalForCtr(TrimFile(from, true), whole - 1) ==
                 "The wave archives of the sound's banks take " + KbText(whole) + ". Hokuto's model of the 3DS holds " +
                     KbText(whole - 1) +
                     " of wave archives. A variable or a random number decides some of the sound's programs. Hokuto "
                     "can't tell which instruments the sound leaves unplayed.");
    XSFLIB_CHECK(RefusalForCtr(TrimFile(from, true), whole).empty());
}

// A sequence that plays `count` notes at once without the note wait: keys 40 and up, each for 96 ticks.
std::vector<uint8_t> HeldBytecode(int count)
{
    std::vector<uint8_t> bytecode = {0xB0, 48, 0xC7, 0x00};
    for (int i = 0; i < count; ++i)
    {
        bytecode.insert(bytecode.end(), {static_cast<uint8_t>(40 + i), 100, 96});
    }
    bytecode.insert(bytecode.end(), {0x80, 96, 0xFF});

    return bytecode;
}

// A song of the console of `from` goes to the 3DS. A song with 24 notes at once, of mono waves, gets a warning that the
// 3DS plays 23. A song with 23 doesn't. A song that goes to another console doesn't either.
void TestHeld(sar::Flavor from)
{
    const auto voice_warnings = [](const hokuto::edit::OtherConsoleSong& song)
    {
        std::vector<std::string> warnings;
        std::ranges::copy_if(song.warnings, std::back_inserter(warnings),
                             [](const std::string& warning) { return warning.starts_with("The 3DS plays "); });

        return warnings;
    };

    const File many = TrimFile(from, HeldBytecode(24));
    SEQLIB_CHECK_EQ(NotesOf(*ArchiveOf(many), 0).at(0).size(), std::size_t{24});
    XSFLIB_CHECK(voice_warnings(hokuto::edit::ForOtherConsole(many, 0, sar::Flavor::kCtr)) ==
                 std::vector<std::string>{
                     "The 3DS plays 23 notes and 24 channels of their waves at once. Up to 24 of the sound's notes "
                     "sound at once before their releases, with 24 channels. The 3DS drops the notes of the lowest "
                     "priority for the others."});
    XSFLIB_CHECK(
        voice_warnings(hokuto::edit::ForOtherConsole(TrimFile(from, HeldBytecode(23)), 0, sar::Flavor::kCtr)).empty());
    const sar::Flavor other = from == sar::Flavor::kNx ? sar::Flavor::kCafe : sar::Flavor::kNx;
    XSFLIB_CHECK(voice_warnings(hokuto::edit::ForOtherConsole(many, 0, other)).empty());
}

// The volume of the region of program `program` of bank `bank` of `archive`.
uint8_t VolumeOf(const sar::SoundArchive& archive, uint32_t bank, uint32_t program)
{
    const sar::BankFile read =
        sar::BankFile::Read(song::ItemFile(archive, {.kind = song::ItemKind::kBank, .index = bank}));
    return read.instruments.at(program)->key_regions.at(0).regions.at(0).values.volume;
}

// The warnings of `song` that start with `start`.
std::vector<std::string> WarningsFrom(const hokuto::edit::OtherConsoleSong& song, const std::string& start)
{
    std::vector<std::string> warnings;
    std::ranges::copy_if(song.warnings, std::back_inserter(warnings),
                         [&](const std::string& warning) { return warning.starts_with(start); });

    return warnings;
}

// A song of the console of `from` with stereo waves goes to the 3DS. With 13 notes at once of the first wave, it holds
// 26 channels at once, and its stereo waves go in as mono. The notes play in the centre. Each mono wave holds the mean
// of its two channels, times the gain of the 3DS's pan law from the side to the centre. The first wave's gain is also
// the square root of 2 for its silent channel. The first wave's samples take its whole gain, and its regions keep their
// volumes. The third wave's samples take as much of the gain as their peak allows, and its regions the rest. The song
// plays the same notes, and the voices' warning goes. With 12 notes, 24 channels, the waves stay stereo. The option
// turns the mono waves on and off either way. The trim counts the waves' size in mono, and only the waves that stay go
// in as mono. A song of mono waves has no stereo waves.
void TestMono(sar::Flavor from)
{
    using hokuto::edit::MonoWaves;

    const File many = TrimFile(from, HeldBytecode(13), true);
    const File fewer = TrimFile(from, HeldBytecode(12), true);
    const std::shared_ptr<const sar::SoundArchive> old_archive = ArchiveOf(many);
    const hokuto::edit::OtherConsoleSong mono = hokuto::edit::ForOtherConsole(many, 0, sar::Flavor::kCtr);
    const std::shared_ptr<const sar::SoundArchive> new_archive = ArchiveOf(mono.file);
    XSFLIB_CHECK(mono.stereo_waves && mono.mono_waves);
    const ntdlib::nwsnd::PanInfo pan = ntdlib::nwsnd::PanInfoOf(0);
    const double gain =
        ntdlib::nwsnd::CalcPanRatio<ntdlib::nwsnd::CtrPolicy>(-1.0f, pan, ntdlib::nwsnd::OutputMode::kStereo) /
        ntdlib::nwsnd::CalcPanRatio<ntdlib::nwsnd::CtrPolicy>(0.0f, pan, ntdlib::nwsnd::OutputMode::kStereo);
    std::map<uint32_t, double> region_gains;
    for (const auto& [wave, full_gain] : {std::pair{0U, gain * std::sqrt(2.0)}, std::pair{2U, gain}})
    {
        const sar::Pcm before = song::DecodeItemWave(*old_archive, kWavesA, wave);
        const sar::Pcm after = song::DecodeItemWave(*new_archive, 0, wave);
        SEQLIB_REQUIRE(before.channels.size() == 2 && after.channels.size() == 1);
        XSFLIB_CHECK(after.loop == before.loop && after.loop_start == before.loop_start &&
                     after.loop_end == before.loop_end && after.sample_rate == before.sample_rate);
        long peak = 0;
        for (std::size_t i = 0; i < before.channels[0].size(); ++i)
        {
            peak = std::max(peak, std::abs(std::lround((before.channels[0][i] + before.channels[1][i]) * 0.5)));
        }
        const double samples = std::clamp(32767.0 / static_cast<double>(peak), 1.0, full_gain);
        region_gains[wave] = full_gain / samples;
        bool means = after.channels[0].size() == before.channels[0].size();
        for (std::size_t i = 0; means && i < after.channels[0].size(); ++i)
        {
            const double mean = (before.channels[0][i] + before.channels[1][i]) * 0.5 * samples;
            means = after.channels[0][i] == std::clamp(std::lround(mean), -32768L, 32767L);
        }
        XSFLIB_CHECK(means);
    }
    XSFLIB_CHECK(region_gains[0] == 1.0 && region_gains[2] > 1.4 && region_gains[2] < gain);
    XSFLIB_CHECK(song::DecodeItemWave(*new_archive, 0, 1).channels.size() == 1);
    SEQLIB_CHECK_EQ(VolumeOf(*new_archive, 0, 0), uint8_t{127});
    SEQLIB_CHECK_EQ(VolumeOf(*new_archive, 0, 1), uint8_t{127});
    SEQLIB_CHECK_EQ(VolumeOf(*new_archive, 0, 2), static_cast<uint8_t>(std::lround(127.0 * region_gains[2])));
    SEQLIB_CHECK_EQ(VolumeOf(*new_archive, 1, 0), uint8_t{127});
    std::array<char, 64> range{};
    std::snprintf(range.data(), range.size(), "%.1f to %.1f dB", 20.0 * std::log10(gain),
                  20.0 * std::log10(gain * std::sqrt(2.0)));
    const std::string rise = " The mean rises by " + std::string(range.data()) +
                             " to keep the power of the two channels at the pans of the notes. The samples take as "
                             "much of the rise as their peak allows, up to the rise in the centre. The regions that "
                             "play the waves take the rest.";
    XSFLIB_CHECK(WarningsFrom(mono, "2 stereo waves") ==
                 std::vector<std::string>{"2 stereo waves go in as mono. Each holds the mean of its two channels. In "
                                          "stereo, up to 26 channels of the sound's waves sound at once before their "
                                          "releases. The 3DS plays 24." +
                                          rise + " The waves lose their stereo width."});
    XSFLIB_CHECK(WarningsFrom(mono, "The 3DS plays ").empty());
    XSFLIB_CHECK(WithoutOffsets(NotesOf(*new_archive, 0)) == WithoutOffsets(NotesOf(*old_archive, 0)));
    XSFLIB_CHECK(ComesBack(mono.file));

    const hokuto::edit::OtherConsoleSong stereo = hokuto::edit::ForOtherConsole(fewer, 0, sar::Flavor::kCtr);
    XSFLIB_CHECK(stereo.stereo_waves && !stereo.mono_waves);
    XSFLIB_CHECK(song::DecodeItemWave(*ArchiveOf(stereo.file), 0, 0).channels.size() == 2);
    XSFLIB_CHECK(WarningsFrom(stereo, "2 stereo waves").empty() && WarningsFrom(stereo, "The 3DS plays ").empty());
    const hokuto::edit::OtherConsoleSong on =
        hokuto::edit::ForOtherConsole(fewer, 0, sar::Flavor::kCtr, MonoWaves::kOn);
    XSFLIB_CHECK(on.mono_waves && song::DecodeItemWave(*ArchiveOf(on.file), 0, 0).channels.size() == 1);
    XSFLIB_CHECK(WarningsFrom(on, "2 stereo waves") ==
                 std::vector<std::string>{"2 stereo waves go in as mono. Each holds the mean of its two channels." +
                                          rise + " The waves lose their stereo width."});
    const hokuto::edit::OtherConsoleSong off =
        hokuto::edit::ForOtherConsole(many, 0, sar::Flavor::kCtr, MonoWaves::kOff);
    XSFLIB_CHECK(off.stereo_waves && !off.mono_waves);
    XSFLIB_CHECK(WarningsFrom(off, "The 3DS plays ") ==
                 std::vector<std::string>{
                     "The 3DS plays 23 notes and 24 channels of their waves at once. Up to 13 of the sound's notes "
                     "sound at once before their releases, with 26 channels. The 3DS drops the notes of the lowest "
                     "priority for the others."});

    // The trim: the mono waves fit whole in a memory too small for the stereo ones. Below that, only the first wave
    // stays, in mono.
    const std::size_t mono_memory = WaveMemoryOf(*new_archive);

    const auto trimmed = [](const hokuto::edit::OtherConsoleSong& song)
    {
        return !WarningsFrom(song, "The banks go in with only").empty();
    };

    const hokuto::edit::OtherConsoleSong fits =
        hokuto::edit::ForOtherConsole(many, 0, sar::Flavor::kCtr, MonoWaves::kAuto, mono_memory);
    XSFLIB_CHECK(!trimmed(fits) && fits.mono_waves);
    XSFLIB_CHECK(trimmed(hokuto::edit::ForOtherConsole(many, 0, sar::Flavor::kCtr, MonoWaves::kOff, mono_memory)));
    const hokuto::edit::OtherConsoleSong kept =
        hokuto::edit::ForOtherConsole(many, 0, sar::Flavor::kCtr, MonoWaves::kAuto, mono_memory - 1);
    const std::shared_ptr<const sar::SoundArchive> kept_archive = ArchiveOf(kept.file);
    XSFLIB_CHECK(trimmed(kept) && kept.mono_waves);
    SEQLIB_CHECK_EQ(song::WaveCountOf(*kept_archive, 0), std::size_t{1});
    XSFLIB_CHECK(song::DecodeItemWave(*kept_archive, 0, 0).channels.size() == 1);
    std::array<char, 16> first{};
    std::snprintf(first.data(), first.size(), "%.1f dB", 20.0 * std::log10(gain * std::sqrt(2.0)));
    XSFLIB_CHECK(WarningsFrom(kept, "1 stereo wave") ==
                 std::vector<std::string>{
                     "1 stereo wave goes in as mono. It holds the mean of its two channels. In stereo, up to 26 "
                     "channels of the sound's waves sound at once before their releases. The 3DS plays 24. The mean "
                     "rises by " +
                     std::string(first.data()) +
                     " to keep the power of the two channels at the pans of the notes. The samples take as much of the "
                     "rise as their peak allows, up to the rise in the centre. The regions that play the wave take the "
                     "rest. The wave loses its stereo width."});
    SEQLIB_CHECK_EQ(VolumeOf(*kept_archive, 0, 0), uint8_t{127});

    const hokuto::edit::OtherConsoleSong plain =
        hokuto::edit::ForOtherConsole(TrimFile(from, HeldBytecode(13)), 0, sar::Flavor::kCtr);
    XSFLIB_CHECK(!plain.stereo_waves && !plain.mono_waves);
}

// A sequence that plays `count` notes of program 2 at once, at the track's pan `pan` from 0 to 127.
std::vector<uint8_t> PannedBytecode(int count, uint8_t pan)
{
    std::vector<uint8_t> bytecode = {0xB0, 48, 0x81, 2, 0xC0, pan, 0xC7, 0x00};
    for (int i = 0; i < count; ++i)
    {
        bytecode.insert(bytecode.end(), {static_cast<uint8_t>(40 + i), 100, 96});
    }
    bytecode.insert(bytecode.end(), {0x80, 96, 0xFF});

    return bytecode;
}

// A song whose 13 notes play the third wave goes to the 3DS with its stereo waves as mono, in the centre and off it.
// The wave's two channels are the same. In the dual mode, the 3DS plays a stereo wave's first channel at the note's pan
// less 1 and its second at the pan plus 1, and a mono wave at the pan (Voice::CalcMixParam). With its region's new
// volume, the mono wave's power at the pan comes within 0.05 dB of the stereo wave's. Off the centre, the stereo wave's
// channels overlap on one side, and the region takes a higher volume than in the centre.
void TestMonoPans(sar::Flavor from)
{
    const ntdlib::nwsnd::PanInfo info = ntdlib::nwsnd::PanInfoOf(0);
    const auto ratio = [&](float at)
    {
        return static_cast<double>(
            ntdlib::nwsnd::CalcPanRatio<ntdlib::nwsnd::CtrPolicy>(at, info, ntdlib::nwsnd::OutputMode::kStereo));
    };

    std::map<uint8_t, uint8_t> volumes;
    for (const uint8_t value : {uint8_t{64}, uint8_t{96}, uint8_t{20}})
    {
        const File file = TrimFile(from, PannedBytecode(13, value), true);
        const hokuto::edit::OtherConsoleSong song = hokuto::edit::ForOtherConsole(file, 0, sar::Flavor::kCtr);
        XSFLIB_CHECK(song.mono_waves);
        const std::shared_ptr<const sar::SoundArchive> new_archive = ArchiveOf(song.file);
        const sar::Pcm before = song::DecodeItemWave(*ArchiveOf(file), kWavesA, 2);
        const sar::Pcm after = song::DecodeItemWave(*new_archive, 0, 2);
        SEQLIB_REQUIRE(before.channels.size() == 2 && after.channels.size() == 1 &&
                       after.channels[0].size() == before.channels[0].size());
        volumes[value] = VolumeOf(*new_archive, 0, 2);
        const double volume = volumes[value] / 127.0;
        const float pan = static_cast<float>(value - 64) / 63.0f;
        double stereo = 0.0;
        double mono = 0.0;
        for (std::size_t i = 0; i < before.channels[0].size(); ++i)
        {
            const double first = before.channels[0][i];
            const double second = before.channels[1][i];
            const double left = ratio(pan - 1.0f) * first + ratio(pan + 1.0f) * second;
            const double right = ratio(1.0f - pan) * first + ratio(-1.0f - pan) * second;
            stereo += left * left + right * right;
            const double mean = after.channels[0][i] * volume;
            mono += mean * mean * (ratio(pan) * ratio(pan) + ratio(-pan) * ratio(-pan));
        }
        XSFLIB_CHECK(stereo > 0.0 && std::abs(10.0 * std::log10(mono / stereo)) < 0.05);
    }
    XSFLIB_CHECK(volumes[96] > volumes[64] && volumes[20] > volumes[64]);
}

// The sequence file of the Wii's test archive. The archive's version, 1.4, gives NW4R SND's build of June 2008. The
// song picks program 0, selects bank 1 and sets a low-pass of byte 34. The Wii's player skips the bank select. The song
// then picks program 1 if global variable 20 holds 0, and plays key 60. NW4R SND starts the global variables at 0.
// Volume2 then goes to 90 over 48 ticks. NW4R SND sets it at once. The song transposes by variable 3 behind a random
// prefix, and plays key 62. The prefix gives nothing on the Wii. An end after a comparison ends the track on the Wii
// whatever the comparison gave: key 64 never plays.
const std::vector<uint8_t> kWiiBytecode = {
    0xB0, 0x30,                   // 0: timebase 48
    0x81, 0x00,                   // 2: program 0
    0xB6, 0x01,                   // 4: bank select 1
    0xC1, 0x64,                   // 6: volume 100
    0xD8, 0x22,                   // 8: low-pass 34
    0xF0, 0x90, 0x14, 0x00, 0x00, // 10: whether variable 20 holds 0
    0xA2, 0x81, 0x01,             // 15: if so, program 1
    0x3C, 0x64, 0x18,             // 18: key 60 for 24 ticks
    0xA3, 0xD5, 0x5A, 0x00, 0x30, // 21: volume2 90 over 48 ticks
    0xA0, 0xA1, 0xC3, 0x03,       // 26: transpose by variable 3, behind a random prefix
    0x3E, 0x5A, 0x0C,             // 30: key 62 for 12 ticks
    0xF0, 0x90, 0x03, 0x00, 0x05, // 33: whether variable 3 holds 5
    0xA2, 0xFF,                   // 38: if so, the end
    0x40, 0x50, 0x0C,             // 40: key 64 for 12 ticks
    0xFF,                         // 43: the end
};

// A file of the Wii whose one sound plays `bytecode` by the rules of `build`, or else of the build that the archive's
// version gives. The sound's bank plays two waves of an RWAR.
File WiiFile(const std::vector<uint8_t>& bytecode = kWiiBytecode, const std::optional<ntdlib::SndBuild>& build = {})
{
    ntdlib::rsar::SequenceFile sequence;
    sequence.data = bytecode;
    sequence.labels = {{"SEQ_TEST", 0}};

    return format::ImportWiiArchive(ntdlib::testing::rsar_files::MakeArchive(sequence.Write(), 0, 0b1), build);
}

// The Wii archive that `file` exports, opened.
std::shared_ptr<const ntdlib::rsar::SoundArchive> WiiArchiveOf(const File& file)
{
    return song::LoadWiiArchive(format::ExportArchive(file), "test.brsar", ntdlib::rvl::snd::kRsarOptions);
}

// The notes that ntdlib's performer plays of sound `sound` of the Wii's `archive`, track by track.
std::vector<std::vector<song::Note>> WiiNotesOf(const ntdlib::rsar::SoundArchive& archive, uint32_t sound)
{
    std::vector<std::vector<song::Note>> notes;
    for (const song::Track& track : song::ReadSequence(archive, sound).tracks)
    {
        notes.push_back(track.notes);
    }

    return notes;
}

// Whether `a` and `b` hold the same samples, at the same rate, with the same loop.
bool SamePcm(const ntdlib::wave::Pcm& a, const ntdlib::wave::Pcm& b)
{
    return a.sample_rate == b.sample_rate && a.loop == b.loop && a.loop_start == b.loop_start &&
           a.loop_end == b.loop_end && a.channels == b.channels;
}

// The warnings of a song of WiiFile on its way to each later console.
const std::vector<std::string> kWiiWarnings = {
    "The new file leaves out 1 of the sequence's commands: 1 of bank select. The Wii's player skipped it.",
    "The sound reads global variable 20 before it sets it. NW4R SND starts the global variables at 0, and the later "
    "consoles start them at -1. Commands at the sound's start set it to 0 while it holds -1.",
};

// A song of the Wii goes to the Wii U, and on from there to the Switch and the 3DS. The commands that the Wii's player
// skips stay out, and so do bytes that no command reaches. A file with such bytes stays in the archive's bytes at the
// import, and goes along all the same. The other commands go in as the later consoles read the Wii's. Commands at the
// sound's start give global variable 20 the Wii's starting value. On each console, the song plays the Wii's notes. Its
// values, its bank and its waves go along. Byte 34 of a low-pass gives a cutoff of about 1,241 Hz on the Wii's build of
// June 2008. On the later consoles, it gives 1,000 Hz, and byte 35 gives 1,280 Hz, the nearest cutoff of their table.
void TestWii()
{
    using ntdlib::commands::Command;
    using ntdlib::commands::Kind;
    using ntdlib::commands::kOne;
    using ntdlib::commands::Source;
    for (const bool dead : {false, true})
    {
        std::vector<uint8_t> bytecode = kWiiBytecode;
        if (dead)
        {
            bytecode.insert(bytecode.end(), {0x00, 0x11, 0x22, 0x33});
        }
        const File wii = WiiFile(bytecode);
        XSFLIB_CHECK(wii.sequences.size() == (dead ? 0U : 1U));
        const hokuto::edit::OtherConsoleSong result = hokuto::edit::ForOtherConsole(wii, 0, sar::Flavor::kCafe);
        const File& file = result.file;
        XSFLIB_CHECK(file.form == format::ArchiveForm::kModel && file.native->flavor == sar::Flavor::kCafe &&
                     !file.native->wii && file.engine == format::Engine::kWiiU);
        XSFLIB_CHECK(result.warnings == kWiiWarnings);
        XSFLIB_CHECK(ComesBack(file));

        // The values, the bank and its wave archive.
        const format::NativeTables& tables = *file.native->tables;
        XSFLIB_CHECK(tables.sounds.size() == 1);
        const format::NativeSound& sound = tables.sounds.at(0);
        XSFLIB_CHECK(sound.name == "SEQ_TEST" && sound.volume == 90 && sound.pan_mode == 0 && sound.pan_curve == 3 &&
                     sound.channel_priority == 70 && sound.release_priority_fix && sound.tracks == 1);
        XSFLIB_CHECK((sound.banks == std::vector<uint32_t>{sar::ItemId(sar::kBankItem, 0)}));
        XSFLIB_CHECK((tables.banks == std::vector<std::string>{"BANK_TEST"}));
        XSFLIB_CHECK((tables.wave_archives == std::vector<std::string>{"WARC_TEST"}));

        // The commands.
        const ntdlib::commands::Sequence& sequence = *file.sequences.at(0);
        XSFLIB_CHECK(sequence.items.size() == 17);
        const Command& compare = sequence.items.at(0);
        const Command& set = sequence.items.at(1);
        const Command& reset = sequence.items.at(2);
        XSFLIB_CHECK(compare.kind == Kind::kCompareEqual && compare.index == 20 &&
                     compare.value.source == Source::kValue && compare.value.value == -kOne);
        XSFLIB_CHECK(set.kind == Kind::kSetVariable && set.conditional && set.index == 20 &&
                     set.value.source == Source::kValue && set.value.value == 0);
        XSFLIB_CHECK(reset.kind == Kind::kCompareEqual && reset.index == 20 &&
                     reset.value.source == Source::kVariable && reset.value.value == 20);
        const auto bank_or_data = [](const Command& command)
        {
            return command.kind == Kind::kBankSelect || command.kind == Kind::kData;
        };
        XSFLIB_CHECK(std::ranges::none_of(sequence.items, bank_or_data));
        const auto low_pass = std::ranges::find(sequence.items, Kind::kLowPass, &Command::kind);
        XSFLIB_CHECK(low_pass != sequence.items.end() && low_pass->value.value == (35 - 64) * kOne / 64);
        const auto volume2 = std::ranges::find(sequence.items, Kind::kVolume2, &Command::kind);
        XSFLIB_CHECK(volume2 != sequence.items.end() && !volume2->time);
        const auto transpose = std::ranges::find(sequence.items, Kind::kTranspose, &Command::kind);
        XSFLIB_CHECK(transpose != sequence.items.end() && transpose->value.source == Source::kVariable &&
                     transpose->value.value == 3 && !transpose->encoding.unused_random);
        XSFLIB_CHECK(std::ranges::none_of(
            sequence.items, [](const Command& command) { return command.kind == Kind::kEnd && command.conditional; }));

        // The notes and the waves.
        const std::shared_ptr<const ntdlib::rsar::SoundArchive> wii_archive = WiiArchiveOf(wii);
        const std::vector<std::vector<song::Note>> notes = WiiNotesOf(*wii_archive, 0);
        XSFLIB_CHECK(notes.size() == 1 && notes[0].size() == 2 && notes[0][0].key == 60 && notes[0][0].program == 1 &&
                     notes[0][1].key == 61 && notes[0][1].program == 1);
        const std::shared_ptr<const sar::SoundArchive> new_archive = ArchiveOf(file);
        XSFLIB_CHECK(WithoutOffsets(NotesOf(*new_archive, 0)) == WithoutOffsets(notes));
        XSFLIB_CHECK(song::ProgramsOf(*new_archive, 0) == song::ProgramsOf(*wii_archive, 0));
        const uint32_t bank_file = wii_archive->Banks().at(0).file_id;
        SEQLIB_CHECK_EQ(song::WaveCountOf(*new_archive, 0), song::WaveCountOf(*wii_archive, bank_file));
        for (uint32_t wave = 0; wave < song::WaveCountOf(*wii_archive, bank_file); ++wave)
        {
            XSFLIB_CHECK(SamePcm(song::DecodeItemWave(*new_archive, 0, wave),
                                 song::DecodeItemWave(*wii_archive, bank_file, wave)));
        }

        // On to the Switch and the 3DS.
        for (const sar::Flavor to : {sar::Flavor::kNx, sar::Flavor::kCtr})
        {
            const hokuto::edit::OtherConsoleSong next = hokuto::edit::ForOtherConsole(wii, 0, to);
            XSFLIB_CHECK(next.file.native->flavor == to && next.file.engine == format::EngineOf(to));
            XSFLIB_CHECK(next.warnings == kWiiWarnings);
            XSFLIB_CHECK(WithoutOffsets(NotesOf(*ArchiveOf(next.file), 0)) == WithoutOffsets(notes));
            XSFLIB_CHECK(ComesBack(next.file));
        }

        // The engines of the later consoles take the sound, and the Wii U's plays the same conversion.
        for (const format::Engine engine : {format::Engine::k3ds, format::Engine::kWiiU, format::Engine::kSwitch})
        {
            XSFLIB_CHECK(!hokuto::edit::EngineRefused(wii, engine));
        }
        const hokuto::edit::OtherConsoleSong converted = hokuto::edit::ForEngine(wii, 0, format::Engine::kWiiU);
        XSFLIB_CHECK(format::Write(converted.file) == format::Write(file) && converted.warnings == result.warnings);
    }
}

// A random low-pass of the Wii goes to the later consoles with both bounds mapped like a plain low-pass. On NW4R SND's
// build of June 2008, a low-pass of byte b gives 32000 * 2^(10 (b - 64) / 64) Hz: about 805 Hz for byte 30 and 1,241 Hz
// for byte 34. The later consoles take kLpfFreq's entry at the step below b / 64, in steps a thirtieth apart from
// 0.13561438. There, byte 31 gives 800 Hz and byte 35 gives 1,280 Hz: the nearest cutoffs of the table. Another Console
// and the browser's Export… of the sequence file both map the bounds.
void TestWiiRandomLowPass()
{
    using ntdlib::commands::Command;
    using ntdlib::commands::Kind;
    using ntdlib::commands::kOne;
    using ntdlib::commands::Source;
    const std::vector<uint8_t> kLowPassBytecode = {
        0xB0, 0x30,                         // 0: timebase 48
        0x81, 0x00,                         // 2: program 0
        0xA0, 0xD8, 0x00, 0x1E, 0x00, 0x22, // 4: a random low-pass from byte 30 to byte 34
        0x3C, 0x64, 0x18,                   // 10: key 60 for 24 ticks
        0xFF,                               // 13: the end
    };
    const File wii = WiiFile(kLowPassBytecode);
    const std::shared_ptr<const ntdlib::rsar::SoundArchive> wii_archive = WiiArchiveOf(wii);
    const ntdlib::SndBuild build = format::BuildOfWiiArchive(*wii.native);
    const auto mapped = [](const ntdlib::commands::Sequence& sequence)
    {
        const auto low_pass = std::ranges::find(sequence.items, Kind::kLowPass, &Command::kind);
        return low_pass != sequence.items.end() && low_pass->value.source == Source::kRandom &&
               low_pass->value.value == (31 - 64) * kOne / 64 && low_pass->value.high == (35 - 64) * kOne / 64;
    };

    const hokuto::edit::OtherConsoleSong result = hokuto::edit::ForOtherConsole(wii, 0, sar::Flavor::kCafe);
    const hokuto::edit::ConvertedItem file =
        hokuto::edit::LaterSequenceFileOf(*wii_archive, 0, build, sar::Flavor::kNx);

    XSFLIB_CHECK(mapped(*result.file.sequences.at(0)));
    XSFLIB_CHECK(mapped(hokuto::edit::ReadIncoming(file.file).sequence));
}

// A sequence of two sounds in the byte order of the console of `flavor`: little-endian on the Switch, and big-endian on
// the Wii U and the 3DS. The first sound sets a random low-pass from byte 31 to byte 35 before key 60. The other sound
// starts at offset 14.
std::vector<uint8_t> RandomLowPassBytecode(sar::Flavor flavor)
{
    // 0: timebase 48. 2: program 0. 4: a random low-pass. 6 and 8: its bounds, in the console's byte order.
    std::vector<uint8_t> bytes = {0xB0, 0x30, 0x81, 0x00, 0xA0, 0xD8};
    for (const uint8_t bound : {uint8_t{31}, uint8_t{35}})
    {
        bytes.insert(bytes.end(), flavor == sar::Flavor::kNx ? std::initializer_list<uint8_t>{bound, 0}
                                                             : std::initializer_list<uint8_t>{0, bound});
    }
    bytes.insert(bytes.end(), {0x3C, 0x64, 0x18, 0xFF});             // 10: key 60 for 24 ticks, and the end
    bytes.insert(bytes.end(), {0x81, 0x00, 0x40, 0x50, 0x18, 0xFF}); // 14: program 0, key 64 for 24 ticks, the end

    return bytes;
}

// A random low-pass of each later console goes to the Wii with both bounds mapped like a plain low-pass.
// TestWiiRandomLowPass checks the other way. The later consoles give byte 31 a cutoff of 800 Hz and byte 35 one of
// 1,280 Hz. A new file of the Wii takes NW4R SND's build of June 2008. There, byte 30 gives about 805 Hz and byte 34
// about 1,241 Hz: the nearest cutoffs. Another Console and the browser's Export… of the sequence file for the Wii both
// map the bounds.
void TestRandomLowPassToWii()
{
    using ntdlib::commands::Command;
    using ntdlib::commands::Kind;
    using ntdlib::commands::kOne;
    using ntdlib::commands::Source;
    const auto holds = [](const ntdlib::commands::Sequence& sequence, int64_t low, int64_t high)
    {
        const auto low_pass = std::ranges::find(sequence.items, Kind::kLowPass, &Command::kind);
        return low_pass != sequence.items.end() && low_pass->value.source == Source::kRandom &&
               low_pass->value.value == (low - 64) * kOne / 64 && low_pass->value.high == (high - 64) * kOne / 64;
    };

    for (const sar::Flavor from : {sar::Flavor::kNx, sar::Flavor::kCafe, sar::Flavor::kCtr})
    {
        const File source = TestFile(from, RandomLowPassBytecode(from), 14);
        const hokuto::edit::OtherConsoleSong result = hokuto::edit::ForWii(source, kSong);
        const hokuto::edit::ConvertedItem file = hokuto::edit::WiiSequenceFileOf(*ArchiveOf(source), kSong);
        const ntdlib::SndBuild build = format::BuildOfWiiArchive(*result.file.native);

        XSFLIB_CHECK(holds(*source.sequences.at(0), 31, 35));
        XSFLIB_CHECK(build.SinceJune2008() && !build.SinceNovember2008());
        XSFLIB_CHECK(holds(*result.file.sequences.at(0), 30, 34));
        XSFLIB_CHECK(holds(hokuto::edit::ReadIncoming(file.file, song::Console{.wii = build}).sequence, 30, 34));
    }
}

// The browser's Export… sends a sequence file, a bank, a wave archive and a wave of the Wii to each later console, and
// the same items of a later console to the Wii. A sequence file leaves out the commands that its new console's player
// skips, and keeps its label. A bank's waves go along in a file of their own beside it. Each wave keeps its samples,
// its rate and its loop.
void TestItemFiles()
{
    using ntdlib::commands::Command;
    using ntdlib::commands::Kind;
    const File wii = WiiFile();
    const std::shared_ptr<const ntdlib::rsar::SoundArchive> wii_archive = WiiArchiveOf(wii);
    const ntdlib::SndBuild build = format::BuildOfWiiArchive(*wii.native);
    const uint32_t bank_file = wii_archive->Banks().at(0).file_id;
    const std::size_t waves = song::WaveCountOf(*wii_archive, bank_file);
    XSFLIB_CHECK(waves == 2);
    const auto plays_on_later = [](const Command& command)
    {
        return command.kind != Kind::kBankSelect && command.kind != Kind::kData &&
               (command.kind != Kind::kEnd || !command.conditional);
    };
    for (const sar::Flavor to : {sar::Flavor::kCafe, sar::Flavor::kNx, sar::Flavor::kCtr})
    {
        // The sequence file.
        const hokuto::edit::ConvertedItem sequence = hokuto::edit::LaterSequenceFileOf(*wii_archive, 0, build, to);
        const hokuto::edit::Incoming read = hokuto::edit::ReadIncoming(sequence.file);
        XSFLIB_CHECK(read.flavor == to && sequence.waves.empty());
        XSFLIB_CHECK(read.sequence.labels.size() == 1 && read.sequence.labels[0].name == "SEQ_TEST" &&
                     read.sequence.labels[0].item == 0);
        XSFLIB_CHECK(std::ranges::all_of(read.sequence.items, plays_on_later));
        XSFLIB_CHECK((sequence.warnings == std::vector<std::string>{kWiiWarnings[0]}));

        // The bank, its wave archive and each wave.
        const sar::Options options{.flavor = to};
        const hokuto::edit::ConvertedItem bank = hokuto::edit::LaterBankFilesOf(*wii_archive, 0, build, to);
        SEQLIB_CHECK_EQ(sar::Bank::Parse(bank.file, options).waves.size(), waves);
        const sar::WaveArchiveFile wave_archive = sar::WaveArchiveFile::Read(bank.waves);
        SEQLIB_CHECK_EQ(wave_archive.waves.size(), waves);
        XSFLIB_CHECK(song::LaterWaveFileOf(*wii_archive, {song::ItemKind::kWaveArchive, bank_file, 0}, to) ==
                     bank.waves);
        for (uint32_t wave = 0; wave < waves; ++wave)
        {
            const ntdlib::wave::Pcm source = song::DecodeItemWave(*wii_archive, bank_file, wave);
            XSFLIB_CHECK(SamePcm(sar::DecodeWave(wave_archive.waves[wave], options), source));
            const std::vector<uint8_t> file =
                song::LaterWaveFileOf(*wii_archive, {song::ItemKind::kWave, bank_file, wave}, to);
            XSFLIB_CHECK(file == wave_archive.waves[wave]);
        }
    }

    // The same items of the Wii U go to the Wii. The sequence file reads as the Wii's, without a conversion.
    const hokuto::edit::OtherConsoleSong later = hokuto::edit::ForOtherConsole(wii, 0, sar::Flavor::kCafe);
    const std::shared_ptr<const sar::SoundArchive> archive = ArchiveOf(later.file);
    const hokuto::edit::ConvertedItem sequence = hokuto::edit::WiiSequenceFileOf(*archive, 0);
    const hokuto::edit::Incoming read = hokuto::edit::ReadIncoming(sequence.file, song::Console{.wii = build});
    XSFLIB_CHECK(read.wii && !read.converted && sequence.waves.empty());
    XSFLIB_CHECK(std::ranges::any_of(read.sequence.labels,
                                     [](const ntdlib::commands::Label& label) { return label.name == "SEQ_TEST"; }));
    XSFLIB_CHECK(std::ranges::all_of(read.sequence.items, plays_on_later));

    const hokuto::edit::ConvertedItem bank = hokuto::edit::WiiBankFilesOf(*archive, 0);
    SEQLIB_CHECK_EQ(ntdlib::rsar::BankFile::Read(bank.file, bank.waves).waves.size(), waves);
    XSFLIB_CHECK(bank.warnings.empty());
    const ntdlib::rsar::WaveArchiveFile rwar = ntdlib::rsar::WaveArchiveFile::Read(bank.waves);
    const ntdlib::rsar::WaveArchiveFile whole =
        ntdlib::rsar::WaveArchiveFile::Read(song::WiiWaveFileOf(*archive, {song::ItemKind::kWaveArchive, 0, 0}));
    SEQLIB_CHECK_EQ(rwar.waves.size(), waves);
    SEQLIB_CHECK_EQ(whole.waves.size(), waves);
    for (uint32_t wave = 0; wave < waves; ++wave)
    {
        const ntdlib::wave::Pcm source = song::DecodeItemWave(*wii_archive, bank_file, wave);
        XSFLIB_CHECK(SamePcm(ntdlib::rsar::DecodeWave(ntdlib::rsar::Wave::ParseFile(rwar.waves[wave])), source));
        XSFLIB_CHECK(SamePcm(ntdlib::rsar::DecodeWave(ntdlib::rsar::Wave::ParseFile(whole.waves[wave])), source));
        const std::vector<uint8_t> file = song::WiiWaveFileOf(*archive, {song::ItemKind::kWave, 0, wave});
        XSFLIB_CHECK(SamePcm(ntdlib::rsar::DecodeWave(ntdlib::rsar::Wave::ParseFile(file)), source));
    }
}

// A sequence file of the Wii with a high-pass of type 2 at value 70 before key 60.
const std::vector<uint8_t> kWiiBiquadBytecode = {
    0xB0, 0x30,       // 0: timebase 48
    0xB4, 0x02,       // 2: biquad type 2
    0xB5, 0x46,       // 4: biquad value 70
    0x3C, 0x64, 0x18, // 6: key 60 for 24 ticks
    0xFF,             // 9: the end
};

// A sound of the Wii goes to the Switch through the Wii U. Its high-pass of type 2 takes type 7 there. The warning
// about the biquad filters names the Wii, the console that the sound came from.
void TestWiiBiquads()
{
    const File wii = WiiFile(kWiiBiquadBytecode);
    const hokuto::edit::OtherConsoleSong result = hokuto::edit::ForOtherConsole(wii, 0, sar::Flavor::kNx);
    XSFLIB_CHECK(Holds(result.warnings, BiquadText("Wii", "1 setting of types 1 to 5 took types 6 to 10.")));
    const auto rate_warning = [](const std::string& warning)
    {
        return warning.find("Wii U at 32 kHz") != std::string::npos;
    };
    XSFLIB_CHECK(std::ranges::none_of(result.warnings, rate_warning));
}

// NW4R SND plays a sound's volume above 127 at 127, and the sound goes to the Wii U at 127. The build of October 2006
// pans by the dual mode and the square-root curve alone, and a sound of that build takes them. The Wii's effects stay
// behind. A sound past the archive's sounds doesn't go.
void TestWiiValues()
{
    const File loud = hokuto::edit::ChangeSound(WiiFile(), 0, {.value = hokuto::edit::SoundValue::kVolume, .to = 200});
    const hokuto::edit::OtherConsoleSong louder = hokuto::edit::ForOtherConsole(loud, 0, sar::Flavor::kCafe);
    XSFLIB_CHECK(louder.file.native->tables->sounds.at(0).volume == 127);
    XSFLIB_CHECK(Holds(louder.warnings,
                       "The sound's volume of 200 goes in as 127. NW4R SND plays a sound's volume above 127 at 127."));

    const std::vector<uint8_t> kPlain = {0xB0, 0x30, 0x81, 0x00, 0x3C, 0x64, 0x18, 0xFF};
    const File old = WiiFile(kPlain, ntdlib::SndBuild{.october_2006 = true});
    const hokuto::edit::OtherConsoleSong panned = hokuto::edit::ForOtherConsole(old, 0, sar::Flavor::kCafe);
    const format::NativeSound& sound = panned.file.native->tables->sounds.at(0);
    XSFLIB_CHECK(sound.pan_mode == 0 && sound.pan_curve == 0);
    XSFLIB_CHECK(panned.warnings ==
                 std::vector<std::string>{"The sound takes the dual pan mode and the square-root pan "
                                          "curve. The build of October 2006 of NW4R SND pans by "
                                          "them alone."});
    XSFLIB_CHECK(WithoutOffsets(NotesOf(*ArchiveOf(panned.file), 0)) ==
                 WithoutOffsets(WiiNotesOf(*WiiArchiveOf(old), 0)));

    File effects = WiiFile(kPlain);
    effects.aux_effects[1] = {{EffectKind::kWiiChorus, {10.0f, 0.5f, 1.0f}}};
    const hokuto::edit::OtherConsoleSong dry = hokuto::edit::ForOtherConsole(effects, 0, sar::Flavor::kCafe);
    XSFLIB_CHECK(dry.warnings ==
                 std::vector<std::string>{"The Wii's AXFX Chorus on bus B stays behind. Hokuto plays the "
                                          "Wii's effects only on the Wii."});
    XSFLIB_CHECK(std::ranges::all_of(dry.file.aux_effects, &std::vector<format::Effect>::empty));

    const std::string refusal = RefusalOf([&] { hokuto::edit::ForOtherConsole(effects, 1, sar::Flavor::kCafe); });
    XSFLIB_CHECK(refusal == "Only a sequence sound goes to another console.");
}

// The song for the Wii, in the bytes of the console of `flavor`. Its values of 16 bits take the console's byte order:
// little-endian on the Switch, and big-endian on the Wii U and the 3DS. The song sets a low-pass of byte 35 and picks
// program 1 if global variable 20 holds -1. The later consoles start the global variables at -1. Key 60 plays, then
// volume2 goes to 90 over 48 ticks. NW4R SND sets it at once. Key 62 plays. An end follows after a comparison that
// fails, and key 64 plays. The Wii's end would end the track whatever the comparison gave. A bank select comes last.
// The Wii's player skips it. The other sound starts at offset 38 and plays key 64 for 24 ticks.
std::vector<uint8_t> ToWiiBytecode(sar::Flavor flavor)
{
    std::vector<uint8_t> bytes;
    const auto add = [&bytes](std::initializer_list<uint8_t> more)
    {
        bytes.insert(bytes.end(), more);
    };

    const auto value = [&bytes, flavor](int16_t number)
    {
        const auto bits = static_cast<uint16_t>(number);
        const auto high = static_cast<uint8_t>(bits >> 8);
        const auto low = static_cast<uint8_t>(bits & 0xFF);
        bytes.insert(bytes.end(), flavor == sar::Flavor::kNx ? std::initializer_list<uint8_t>{low, high}
                                                             : std::initializer_list<uint8_t>{high, low});
    };
    add({0xB0, 0x30, 0x81, 0x00, 0xD8, 0x23}); // 0: timebase 48, program 0, low-pass 35
    add({0xF0, 0x90, 0x14});                   // 6: whether variable 20 holds -1
    value(-1);
    add({0xA2, 0x81, 0x01, 0x3C, 0x64, 0x18}); // 11: if so, program 1. 14: key 60 for 24 ticks
    add({0xA3, 0xD5, 0x5A});                   // 17: volume2 90 over 48 ticks
    value(48);
    add({0x3E, 0x5A, 0x0C, 0xF0, 0x90, 0x14}); // 22: key 62 for 12 ticks. 25: whether variable 20 holds 5
    value(5);
    add({0xA2, 0xFF, 0x40, 0x50, 0x0C});       // 30: if so, the end. 32: key 64 for 12 ticks
    add({0xB6, 0x01, 0xFF});                   // 35: bank select 1, and the end
    add({0x81, 0x00, 0x40, 0x50, 0x18, 0xFF}); // 38: program 0, key 64 for 24 ticks, the end

    return bytes;
}

// What a listener hears of `notes`: when each note starts, how long it lasts, its key, its velocity and its program.
std::vector<std::vector<std::array<uint32_t, 5>>> Heard(const std::vector<std::vector<song::Note>>& notes)
{
    std::vector<std::vector<std::array<uint32_t, 5>>> heard;
    for (const std::vector<song::Note>& track : notes)
    {
        std::vector<std::array<uint32_t, 5>>& out = heard.emplace_back();
        for (const song::Note& note : track)
        {
            out.push_back({note.start, note.length, note.key, note.velocity, note.program});
        }
    }

    return heard;
}

// The warnings of the song of the console of `from` on its way to the Wii, in their order.
std::vector<std::string> ToWiiWarnings(sar::Flavor from)
{
    std::vector<std::string> warnings = {
        "The new file leaves out 1 of the sequence's commands: 1 of bank select. The Wii's player skips it.",
        "NW4R SND sets volume2, the main volume and the pitch bend at once. 1 of the sequence's settings of them "
        "loses its time, and changes at once.",
        "The sound reads global variable 20 before it sets it. NW4R SND starts the global variables at 0, and the "
        "later consoles start them at -1. Commands at the sound's start set it to -1 while it holds 0.",
        "The bank in the sound's other slot stays behind: BANK_B. None of the sound's notes plays it.",
        "The sound's 3D settings, single play and user parameters stay behind. NW4R SND reads other fields.",
        "The sound's Actor Player stays behind. Hokuto reads no actor player in an archive of the Wii.",
        "The sequence file holds the commands of 1 other sound too. They go along unused.",
    };
    if (from == sar::Flavor::kCtr)
    {
        warnings.push_back(
            "The nw::snd Reverb (3DS) on bus A stays behind. The Wii runs the effects of its SDK's AXFX "
            "library instead.");
    }
    else
    {
        warnings.push_back(
            "The sound plays in strict mixing. AX's microcode mixes the Wii's sounds. Hokuto has no "
            "enhanced mixing for the Wii yet.");
        warnings.push_back(
            "The I3DL2 Reverb and nw::snd Reverb (3DS) on bus A stay behind. The Wii runs the effects of its SDK's "
            "AXFX library instead.");
        warnings.push_back(
            "The nw::snd Chorus on bus B stays behind. The Wii runs the effects of its SDK's AXFX library "
            "instead.");
    }

    return warnings;
}

// The song of each later console goes to the Wii. NW4R SND's build of June 2008 plays the new file. The bank select
// stays out, and the Wii plays the song's notes. The end after a comparison becomes a jump to an end. Volume2 loses its
// time, and global variable 20 starts at -1. Byte 35 of a low-pass gives 1,280 Hz on the later consoles. On the Wii's
// build of June 2008, byte 34 gives about 1,241 Hz, the nearest cutoff. The bank of the first slot plays the same
// waves, and the values that NW4R SND reads go along. The AXFX chorus on bus B stays there, and the other effects stay
// behind. The other sound's bank names a wave archive that the archive lacks, and its region plays nothing on either
// console.
void TestToWii(sar::Flavor from)
{
    using ntdlib::commands::Command;
    using ntdlib::commands::Kind;
    using ntdlib::commands::kOne;
    const File source = TestFile(from, ToWiiBytecode(from), 38);
    const hokuto::edit::OtherConsoleSong result = hokuto::edit::ForWii(source, kSong);
    const File& file = result.file;
    XSFLIB_CHECK(file.form == format::ArchiveForm::kModel && file.native->wii && file.engine == format::Engine::kWii);
    XSFLIB_CHECK(result.warnings == ToWiiWarnings(from));
    XSFLIB_CHECK(ComesBack(file));
    const std::vector<format::Effect> kChorus = {{EffectKind::kWiiChorus, {10.0f, 0.5f, 1.0f}}};
    XSFLIB_CHECK(file.aux_effects[0].empty() && file.aux_effects[1] == kChorus && file.aux_effects[2].empty());

    // The values that the Wii's archive holds.
    const format::NativeTables& tables = *file.native->tables;
    XSFLIB_CHECK(tables.sounds.size() == 1);
    const format::NativeSound& sound = tables.sounds.at(0);
    XSFLIB_CHECK(sound.name == "SEQ_SONG" && sound.volume == 90 && sound.pan_mode == 1 && sound.pan_curve == 2 &&
                 sound.player_priority == 48 && sound.actor_player == 0 && sound.tracks == 3 &&
                 sound.channel_priority == 40 && sound.release_priority_fix);
    XSFLIB_CHECK((sound.banks == std::vector<uint32_t>{sar::ItemId(sar::kBankItem, 0)}));
    XSFLIB_CHECK((tables.banks == std::vector<std::string>{"BANK_A"}));

    // The commands.
    const ntdlib::commands::Sequence& sequence = *file.sequences.at(0);
    const auto wii_skips = [](const Command& command)
    {
        return command.kind == Kind::kBankSelect || (command.conditional && command.kind == Kind::kEnd);
    };
    XSFLIB_CHECK(std::ranges::none_of(sequence.items, wii_skips));
    const auto low_pass = std::ranges::find(sequence.items, Kind::kLowPass, &Command::kind);
    XSFLIB_CHECK(low_pass != sequence.items.end() && low_pass->value.value == (34 - 64) * kOne / 64);
    const auto volume2 = std::ranges::find(sequence.items, Kind::kVolume2, &Command::kind);
    XSFLIB_CHECK(volume2 != sequence.items.end() && !volume2->time);
    XSFLIB_CHECK(sequence.items.at(0).kind == Kind::kCompareEqual && sequence.items.at(0).index == 20 &&
                 sequence.items.at(0).value.value == 0 && sequence.items.at(1).kind == Kind::kSetVariable &&
                 sequence.items.at(1).conditional && sequence.items.at(1).value.value == -kOne);
    const auto jump = std::ranges::find_if(
        sequence.items, [](const Command& command) { return command.kind == Kind::kJump && command.conditional; });
    XSFLIB_CHECK(jump != sequence.items.end() && jump->target < sequence.items.size() &&
                 sequence.items[jump->target].kind == Kind::kEnd && !sequence.items[jump->target].conditional);

    // The notes and the waves.
    const std::shared_ptr<const sar::SoundArchive> old_archive = ArchiveOf(source);
    const std::shared_ptr<const ntdlib::rsar::SoundArchive> wii_archive = WiiArchiveOf(file);
    const std::vector<std::vector<song::Note>> notes = NotesOf(*old_archive, kSong);
    XSFLIB_CHECK(notes.size() == 1 && notes[0].size() == 3 && notes[0][0].program == 1 && notes[0][2].key == 64);
    XSFLIB_CHECK(Heard(WiiNotesOf(*wii_archive, 0)) == Heard(notes));
    XSFLIB_CHECK(song::ProgramsOf(*wii_archive, 0) == song::ProgramsOf(*old_archive, 0));
    const uint32_t bank_file = wii_archive->Banks().at(0).file_id;
    XSFLIB_CHECK(song::WaveCountOf(*wii_archive, bank_file) == song::WaveCountOf(*old_archive, kWavesA));
    for (uint32_t wave = 0; wave < song::WaveCountOf(*old_archive, kWavesA); ++wave)
    {
        XSFLIB_CHECK(SamePcm(song::DecodeItemWave(*wii_archive, bank_file, wave),
                             song::DecodeItemWave(*old_archive, kWavesA, wave)));
    }

    // The other sound's bank goes along without the waves that the archive lacks.
    const hokuto::edit::OtherConsoleSong other = hokuto::edit::ForWii(source, kOther);
    const std::shared_ptr<const ntdlib::rsar::SoundArchive> other_archive = WiiArchiveOf(other.file);
    XSFLIB_CHECK(song::WaveCountOf(*other_archive, other_archive->Banks().at(0).file_id) == 0);
    XSFLIB_CHECK(Heard(WiiNotesOf(*other_archive, 0)) == Heard(NotesOf(*old_archive, kOther)));
    XSFLIB_CHECK(ComesBack(other.file));

    // A sound above volume 127 goes in at 127.
    const File loud = hokuto::edit::ChangeSound(source, kSong, {.value = hokuto::edit::SoundValue::kVolume, .to = 200});
    const hokuto::edit::OtherConsoleSong louder = hokuto::edit::ForWii(loud, kSong);
    XSFLIB_CHECK(louder.file.native->tables->sounds.at(0).volume == 127);
    XSFLIB_CHECK(Holds(louder.warnings,
                       "The sound's volume of 200 goes in as 127, and the sound plays 3.9 dB quieter. "
                       "NW4R SND plays a sound's volume above 127 at 127."));

    // The engine of the Wii plays the same conversion.
    XSFLIB_CHECK(!hokuto::edit::EngineRefused(source, format::Engine::kWii));
    const hokuto::edit::OtherConsoleSong converted = hokuto::edit::ForEngine(source, kSong, format::Engine::kWii);
    XSFLIB_CHECK(format::Write(converted.file) == format::Write(file) && converted.warnings == result.warnings);
}

// A song that plays both of its banks for the Wii. Key 60 plays program 1 of slot 0. A bank select of slot 1 follows,
// and key 62 plays program 1 there. Key 64 plays program 0 of slot 1, and key 65 program 0 of slot 0 after a bank
// select of slot 0. The other sound starts at offset 23 and plays key 64 for 24 ticks.
const std::vector<uint8_t> kTwoBanksBytecode = {
    0xB0, 48,      // 0: timebase 48
    0x81, 1,       // 2: program 1
    60,   100, 24, // 4: key 60 for 24 ticks
    0xB6, 1,       // 7: bank select 1
    62,   90,  12, // 9: key 62 for 12 ticks
    0x81, 0,       // 12: program 0
    64,   80,  12, // 14: key 64 for 12 ticks
    0xB6, 0,       // 17: bank select 0
    65,   80,  12, // 19: key 65 for 12 ticks
    0xFF,          // 22: the end
    0x81, 0,       // 23: program 0
    64,   80,  24, // 25: key 64 for 24 ticks
    0xFF,          // 28: the end
};

// The song's two banks go to the Wii as one bank: bank A at programs 0 and 1, and bank B at programs 2 and 3. The bank
// selects become program changes into their slots' programs, and the program change of slot 1 takes slot 1's. The Wii
// plays each note with its instrument. Bank B's second wave is bank A's second wave, and the bank holds it once.
void TestWiiBanks(sar::Flavor from)
{
    const File source = TestFile(from, kTwoBanksBytecode, 23);
    const hokuto::edit::OtherConsoleSong result = hokuto::edit::ForWii(source, kSong);
    const File& file = result.file;
    XSFLIB_CHECK((result.slot_programs == std::map<int, uint32_t>{{0, 0}, {1, 2}}));
    XSFLIB_CHECK((file.native->tables->banks == std::vector<std::string>{"BANK_A_BANK_B"}));
    XSFLIB_CHECK(
        Holds(result.warnings,
              "A sound of the Wii plays one bank, and NW4R SND has no bank select. The notes play the banks of "
              "several slots. They go in as one bank, BANK_A_BANK_B: BANK_A at programs 0 to 1 and BANK_B at "
              "programs 2 to 3. 2 bank selects became program changes and 1 program change took the programs "
              "of its slot."));
    XSFLIB_CHECK(std::ranges::none_of(
        result.warnings, [](const std::string& warning) { return warning.find("leaves out") != std::string::npos; }));
    const auto bank_warning = [](const std::string& warning)
    {
        return warning.find("stays behind: BANK") != std::string::npos;
    };
    XSFLIB_CHECK(std::ranges::none_of(result.warnings, bank_warning));
    XSFLIB_CHECK(ComesBack(file));

    // Each note plays its slot's program in the Wii's bank.
    const std::shared_ptr<const sar::SoundArchive> old_archive = ArchiveOf(source);
    const std::shared_ptr<const ntdlib::rsar::SoundArchive> wii_archive = WiiArchiveOf(file);
    std::vector<std::vector<song::Note>> notes = NotesOf(*old_archive, kSong);
    SEQLIB_REQUIRE(notes.size() == 1 && notes[0].size() == 4);
    XSFLIB_CHECK(notes[0][1].bank_slot == 1 && notes[0][1].program == 1 && notes[0][2].bank_slot == 1 &&
                 notes[0][2].program == 0 && notes[0][3].bank_slot == 0);
    for (std::vector<song::Note>& track : notes)
    {
        for (song::Note& note : track)
        {
            note.program += result.slot_programs.at(note.bank_slot);
        }
    }
    const std::vector<std::vector<song::Note>> wii_notes = WiiNotesOf(*wii_archive, 0);
    XSFLIB_CHECK(Heard(wii_notes) == Heard(notes));
    XSFLIB_CHECK(wii_notes.size() == 1 && wii_notes[0].size() == 4 && wii_notes[0][0].program == 1 &&
                 wii_notes[0][1].program == 3 && wii_notes[0][2].program == 2 && wii_notes[0][3].program == 0);

    // The bank's waves: bank A's two, then bank B's first.
    const uint32_t bank_file = wii_archive->Banks().at(0).file_id;
    XSFLIB_CHECK(song::WaveCountOf(*wii_archive, bank_file) == 3);
    XSFLIB_CHECK(
        SamePcm(song::DecodeItemWave(*wii_archive, bank_file, 0), song::DecodeItemWave(*old_archive, kWavesA, 0)));
    XSFLIB_CHECK(
        SamePcm(song::DecodeItemWave(*wii_archive, bank_file, 1), song::DecodeItemWave(*old_archive, kWavesA, 1)));
    XSFLIB_CHECK(
        SamePcm(song::DecodeItemWave(*wii_archive, bank_file, 2), song::DecodeItemWave(*old_archive, kWavesB, 0)));

    // The engine of the Wii plays the same conversion.
    const hokuto::edit::OtherConsoleSong converted = hokuto::edit::ForEngine(source, kSong, format::Engine::kWii);
    XSFLIB_CHECK(format::Write(converted.file) == format::Write(file) &&
                 converted.slot_programs == result.slot_programs);
}

// A song for the Wii that plays program 1 of slot 0, then program 0 of slot 1 after a bank select. The other sound
// starts at offset 15.
const std::vector<uint8_t> kTwoProgramsBytecode = {
    0xB0, 48,      // 0: timebase 48
    0x81, 1,       // 2: program 1
    60,   100, 24, // 4: key 60 for 24 ticks
    0xB6, 1,       // 7: bank select 1
    0x81, 0,       // 9: program 0
    62,   90,  12, // 11: key 62 for 12 ticks
    0xFF,          // 14: the end
    0x81, 0,       // 15: program 0
    64,   80,  24, // 17: key 64 for 24 ticks
    0xFF,          // 20: the end
};

// A song for the Wii that plays program 1 of slot 0 alone. The other sound starts at offset 8.
const std::vector<uint8_t> kOneProgramBytecode = {
    0xB0, 48,      // 0: timebase 48
    0x81, 1,       // 2: program 1
    60,   100, 24, // 4: key 60 for 24 ticks
    0xFF,          // 7: the end
    0x81, 0,       // 8: program 0
    64,   80,  24, // 10: key 64 for 24 ticks
    0xFF,          // 13: the end
};

// The bank of a song for the Wii whose waves don't fit in the memory of the Wii's model takes only the instruments that
// the notes select, with their waves. Each note plays as before. Without room for those waves, the song doesn't go. The
// merged bank of the song of both slots keeps program 1 of bank A and program 0 of bank B: wave A's second wave and
// wave B's first. The bank of the song of slot 0 keeps program 1 of bank A.
void TestWiiTrim(sar::Flavor from)
{
    const auto wave_bytes = [](const ntdlib::rsar::SoundArchive& archive)
    {
        return archive.File(archive.Banks().at(0).file_id)->wave_data.size();
    };

    const auto refusal = [](const File& file, std::size_t memory)
    {
        return RefusalOf([&] { hokuto::edit::ForWii(file, kSong, std::nullopt, memory); });
    };

    struct Case
    {
        const std::vector<uint8_t>& bytecode;
        uint32_t other_start;
        std::size_t waves;
        std::vector<std::pair<uint32_t, uint32_t>> kept; // the source's wave archive and wave of each wave kept
    };
    const std::array kCases = {Case{kTwoProgramsBytecode, 15, 3, {{kWavesA, 1}, {kWavesB, 0}}},
                               Case{kOneProgramBytecode, 8, 2, {{kWavesA, 1}}}};
    for (const Case& test : kCases)
    {
        const File source = TestFile(from, test.bytecode, test.other_start);
        const std::shared_ptr<const sar::SoundArchive> old_archive = ArchiveOf(source);
        const hokuto::edit::OtherConsoleSong whole = hokuto::edit::ForWii(source, kSong);
        const std::shared_ptr<const ntdlib::rsar::SoundArchive> whole_archive = WiiArchiveOf(whole.file);
        const std::size_t all = wave_bytes(*whole_archive);
        XSFLIB_CHECK(song::WaveCountOf(*whole_archive, whole_archive->Banks().at(0).file_id) == test.waves);

        const hokuto::edit::OtherConsoleSong result = hokuto::edit::ForWii(source, kSong, std::nullopt, all - 1);
        XSFLIB_CHECK(result.slot_programs == whole.slot_programs && ComesBack(result.file));
        const std::shared_ptr<const ntdlib::rsar::SoundArchive> wii_archive = WiiArchiveOf(result.file);
        const uint32_t bank_file = wii_archive->Banks().at(0).file_id;
        const std::size_t kept = wave_bytes(*wii_archive);
        XSFLIB_CHECK(kept < all && all < 1024 && song::WaveCountOf(*wii_archive, bank_file) == test.kept.size());
        for (std::size_t wave = 0; wave < test.kept.size(); ++wave)
        {
            const auto [wave_archive, index] = test.kept[wave];
            XSFLIB_CHECK(SamePcm(song::DecodeItemWave(*wii_archive, bank_file, static_cast<uint32_t>(wave)),
                                 song::DecodeItemWave(*old_archive, wave_archive, index)));
        }
        XSFLIB_CHECK(Holds(result.warnings,
                           "The bank goes in with only the instruments that the sound selects. Its "
                           "waves take " +
                               std::to_string(kept) +
                               " bytes. With every instrument, they would "
                               "take " +
                               std::to_string(all) + " bytes. Hokuto's model of the Wii holds " +
                               std::to_string(all - 1) + " bytes of waves."));

        // Each note plays its instrument as before.
        std::vector<std::vector<song::Note>> notes = NotesOf(*old_archive, kSong);
        for (std::vector<song::Note>& track : notes)
        {
            for (song::Note& note : track)
            {
                const auto range = result.slot_programs.find(note.bank_slot);
                note.program += range != result.slot_programs.end() ? range->second : 0;
            }
        }
        XSFLIB_CHECK(Heard(WiiNotesOf(*wii_archive, 0)) == Heard(notes) &&
                     Heard(WiiNotesOf(*whole_archive, 0)) == Heard(notes));

        XSFLIB_CHECK(refusal(source, kept - 1) ==
                     "The waves of the sound's bank take " + std::to_string(kept) +
                         " bytes, even with only the instruments that the sound selects. Hokuto's model of the Wii "
                         "holds " +
                         std::to_string(kept - 1) + " bytes of waves.");
    }
}

// A sound past the archive's sounds doesn't go to another console, and a sound goes only to another console.
void TestRefusals(sar::Flavor flavor)
{
    const File file = TestFile(flavor);
    const auto refusal = [&](uint32_t sound, sar::Flavor to)
    {
        return RefusalOf([&] { hokuto::edit::ForOtherConsole(file, sound, to); });
    };
    XSFLIB_CHECK(refusal(2, song::OtherConsole(flavor)) == "Only a sequence sound goes to another console.");
    XSFLIB_CHECK(refusal(kSong, flavor) == "The sound plays on the " + song::ConsoleName(flavor) + " already.");
}

// A sound plays on its file's engine as it is, and on another console's engine through its conversion. ForEngine
// converts as ForOtherConsole does.
void TestEngines(sar::Flavor flavor)
{
    const File file = TestFile(flavor);
    const sar::Flavor other = song::OtherConsole(flavor);
    XSFLIB_CHECK(!hokuto::edit::EngineRefused(file, file.engine));
    XSFLIB_CHECK(!hokuto::edit::EngineRefused(file, format::EngineOf(other)));
    XSFLIB_CHECK(!hokuto::edit::EngineRefused(file, format::Engine::kWii));

    const hokuto::edit::OtherConsoleSong converted = hokuto::edit::ForEngine(file, kSong, format::EngineOf(other));
    const hokuto::edit::OtherConsoleSong expected = hokuto::edit::ForOtherConsole(file, kSong, other);
    XSFLIB_CHECK(format::Write(converted.file) == format::Write(expected.file));
    XSFLIB_CHECK(converted.warnings == expected.warnings);

    const auto refusal = [&](format::Engine engine)
    {
        return RefusalOf([&] { hokuto::edit::ForEngine(file, kSong, engine); });
    };
    XSFLIB_CHECK(refusal(file.engine) == "The sound plays on the file's engine already.");
}

// Each kind of effect has a name that no other kind has: the name of its library and its class. The mixer and the
// warnings name an effect by it, and a file can hold the kinds of several consoles. The 3DS's and the Wii U's nw::snd
// both have a delay. Each delay's name gives its console.
void TestEffectNames()
{
    std::set<std::string> names;
    for (int kind = 0; kind <= static_cast<int>(EffectKind::kCtrNnReverb2011); kind++)
    {
        names.insert(hokuto::edit::EffectName(static_cast<EffectKind>(kind)));
    }

    SEQLIB_CHECK_EQ(names.size(), static_cast<std::size_t>(EffectKind::kCtrNnReverb2011) + 1);
    XSFLIB_CHECK(!names.contains("Effect"));
    XSFLIB_CHECK(std::string(hokuto::edit::EffectName(EffectKind::kDelay)) == "nw::snd Delay (Wii U)" &&
                 std::string(hokuto::edit::EffectName(EffectKind::kCtrDelay)) == "nw::snd Delay (3DS)" &&
                 std::string(hokuto::edit::EffectName(EffectKind::kWiiDelay)) == "AXFX Delay");
}

} // namespace

int main()
{
    constexpr std::array kConsoles = {sar::Flavor::kNx, sar::Flavor::kCafe, sar::Flavor::kCtr};
    for (const sar::Flavor from : kConsoles)
    {
        for (const sar::Flavor to : kConsoles)
        {
            if (to != from)
            {
                TestSong(from, to);
                TestBankLeftBehind(from, to);
                TestSettingsUnread(from, to);
                TestFixedCommands(from, to);
                TestSkipped(from, to);
            }
        }
        TestRefusals(from);
        TestEngines(from);
        TestToWii(from);
        TestWiiBanks(from);
        TestWiiTrim(from);
    }
    TestFixedSkipped();
    TestCtrTimes(sar::Flavor::kCafe);
    TestCtrTimes(sar::Flavor::kNx);
    TestTrim(sar::Flavor::kNx);
    TestTrim(sar::Flavor::kCafe);
    TestHeld(sar::Flavor::kNx);
    TestHeld(sar::Flavor::kCafe);
    TestMono(sar::Flavor::kNx);
    TestMono(sar::Flavor::kCafe);
    TestMonoPans(sar::Flavor::kNx);
    TestMonoPans(sar::Flavor::kCafe);
    TestBiquads();
    TestWii();
    TestWiiRandomLowPass();
    TestRandomLowPassToWii();
    TestWiiBiquads();
    TestWiiValues();
    TestItemFiles();
    TestEffectNames();

    return xsflib::testing::Summary("other_console");
}
