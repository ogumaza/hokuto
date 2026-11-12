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
// biquad filters of a song take the types and values that come nearest on the other console.

#include "other_console_fixtures.h"

namespace
{

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

// The warnings of the song of the test file of `from` on its way to `to`, in their order: the waves, the sequence file
// and the effects.
std::vector<std::string> SongWarnings(sar::Flavor from, sar::Flavor to)
{
    std::vector<std::string> warnings;
    const bool ctr = from == sar::Flavor::kCtr || to == sar::Flavor::kCtr;
    if (to == sar::Flavor::kNx)
    {
        warnings.push_back(
            "1 wave of WARC_B is converted to 16-bit PCM without loss. The Switch audio renderer doesn't support 8-bit "
            "PCM.");
    }
    else
    {
        warnings.push_back("The waves of WARC_B are written as version " +
                           std::string(to == sar::Flavor::kCtr ? "2.1.0.0" : "1.1.0") + ", the oldest that " +
                           song::ConsoleName(to) +
                           " games read. That version has no original loop start. Playback is unchanged.");
    }
    if (!ctr && to == sar::Flavor::kCafe)
    {
        warnings.push_back(
            "The sequence file is written as version 1.0.0, the oldest that Wii U games read. It was version "
            "2.0.0. Its commands are unchanged.");
    }
    warnings.push_back("The sequence file also contains the commands of 1 other sound. They are copied but unused.");

    // Bus A holds the Switch's reverb before the 3DS's, and bus B the Wii U's chorus before the Wii's.
    const std::string console = song::ConsoleName(to);
    if (to == sar::Flavor::kCtr)
    {
        warnings.push_back("The I3DL2 Reverb on bus A is left out. The 3DS doesn't support it.");
        warnings.push_back("The nw::snd Chorus and AXFX Chorus on bus B are left out. The 3DS doesn't support them.");
    }
    else if (from == sar::Flavor::kCtr)
    {
        warnings.push_back("The nw::snd Reverb (3DS) on bus A is left out. The " + console + " doesn't support it.");
        warnings.push_back("The AXFX Chorus on bus B is left out. The " + console + " doesn't support it.");
    }
    else if (to == sar::Flavor::kNx)
    {
        warnings.push_back("The nw::snd Reverb (3DS) on bus A is left out. The Switch doesn't support it.");
        warnings.push_back(
            "The nw::snd Chorus and AXFX Chorus on bus B are left out. The Switch doesn't support them.");
    }
    else
    {
        warnings.push_back(
            "The I3DL2 Reverb and nw::snd Reverb (3DS) on bus A are left out. The Wii U doesn't support them.");
        warnings.push_back("The AXFX Chorus on bus B is left out. The Wii U doesn't support it.");
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
    XSFLIB_CHECK(file.form == format::ArchiveForm::kModel && FlavorOf(*file.native) == to &&
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
    XSFLIB_CHECK(FlavorOf(*back.file.native) == from);
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
    XSFLIB_CHECK(
        Holds(result.warnings, "Bank BANK_C is left out. Its slot is empty. The archive has no wave archive 7."));
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
        "The sound's 3D settings, single play, user parameters and remote filter are left out. "
        "Can't read them from this archive.";
    XSFLIB_CHECK(
        std::ranges::any_of(result.warnings, [&](const std::string& warning) { return warning.starts_with(kUnread); }));
    const std::shared_ptr<const sar::SoundArchive> before = ArchiveOf(source);
    const std::shared_ptr<const sar::SoundArchive> after = ArchiveOf(result.file);
    XSFLIB_CHECK(NotesOf(*after, 0) == NotesOf(*before, 0) && SameWaves(*before, 0, *after, 0));
    XSFLIB_CHECK(ComesBack(result.file));
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
            return warning.starts_with("The 3DS applies volume2 and main volume changes at once, without a ramp.");
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
                       "The 3DS applies volume2 and main volume changes at once, without a ramp. The ramp times of 2 "
                       "such changes are dropped. The " +
                           song::ConsoleName(to) + " applies them at once too."));
    XSFLIB_CHECK(WithoutOffsets(NotesOf(*ArchiveOf(result.file), 0)) ==
                 WithoutOffsets(NotesOf(*ArchiveOf(source), kSong)));
    XSFLIB_CHECK(Holds(kept.warnings,
                       "The 3DS applies volume2 and main volume changes at once, without a ramp. 2 such changes keep "
                       "their ramp times because the commands in the file must stay at their offsets. The " +
                           song::ConsoleName(to) + " ramps them over those times."));
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
                           BiquadText(song::ConsoleName(from), "1 setting changed from types 1–5 to types 6–10.")));
        XSFLIB_CHECK(plays_as(source, result));
    }

    const File high_pass = TestFile(sar::Flavor::kNx, BiquadBytecode(2), 17);
    const File later = TestFile(sar::Flavor::kNx, BiquadBytecode(7), 17);
    for (const sar::Flavor to : {sar::Flavor::kCafe, sar::Flavor::kCtr})
    {
        const std::string name = song::ConsoleName(to);
        const hokuto::edit::OtherConsoleSong result = hokuto::edit::ForOtherConsole(high_pass, kSong, to);
        const ntdlib::commands::Sequence& sequence = *result.file.sequences.at(0);
        const int nearest = song::NearestBiquadValue(2, 70, {.engine = hokuto::format::EngineOf(to)});
        XSFLIB_CHECK(nearest != 70 && setting(sequence, Kind::kBiquadType) == 2 &&
                     setting(sequence, Kind::kBiquadValue) == nearest);
        XSFLIB_CHECK(Holds(result.warnings, BiquadText(name,
                                                       "1 value of types 1–5 changed to the value with the "
                                                       "nearest passband on the " +
                                                           name + ".")));
        XSFLIB_CHECK(plays_as(high_pass, result));

        const hokuto::edit::OtherConsoleSong exact = hokuto::edit::ForOtherConsole(later, kSong, to);
        const ntdlib::commands::Sequence& kept = *exact.file.sequences.at(0);
        XSFLIB_CHECK(setting(kept, Kind::kBiquadType) == 2 && setting(kept, Kind::kBiquadValue) == 70);
        XSFLIB_CHECK(Holds(exact.warnings, BiquadText(name, "1 setting changed from types 6–10 to types 1–5.")));
    }

    // The Wii's build of June 2008 plays the biquad filters at 32 kHz.
    const hokuto::edit::OtherConsoleSong wii = hokuto::edit::ForWii(high_pass, kSong);
    const ntdlib::commands::Sequence& sequence = *wii.file.sequences.at(0);
    const int nearest = song::NearestBiquadValue(2, 70, {.wii = format::BuildOfWiiArchive(*wii.file.native)});
    XSFLIB_CHECK(setting(sequence, Kind::kBiquadType) == 2 && setting(sequence, Kind::kBiquadValue) == nearest);
    XSFLIB_CHECK(Holds(wii.warnings, BiquadText("Wii",
                                                "1 value of types 1–5 changed to the value with the nearest "
                                                "passband on the Wii.")));
}

// A sequence file with a byte that no command reaches: its commands have to keep their places. The file goes in as the
// other console's decoder reads the converted file, and the song plays the same notes.
void TestFixedCommands(sar::Flavor from, sar::Flavor to)
{
    const File source = TestFile(from, FixedBytecode());
    const hokuto::edit::OtherConsoleSong result = hokuto::edit::ForOtherConsole(source, kSong, to);
    XSFLIB_CHECK(Holds(result.warnings,
                       "The commands in the sequence file must stay at their offsets. The file is converted with "
                       "each command at its offset."));
    const std::shared_ptr<const sar::SoundArchive> before = ArchiveOf(source);
    const std::shared_ptr<const sar::SoundArchive> after = ArchiveOf(result.file);
    XSFLIB_CHECK(NotesOf(*after, 0) == NotesOf(*before, kSong));
    XSFLIB_CHECK(after->Sounds().at(0).start_offset == before->Sounds().at(kSong).start_offset);
    XSFLIB_CHECK(ComesBack(result.file));
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
            return warning.starts_with("The new file leaves out ");
        };
        XSFLIB_CHECK(std::ranges::none_of(result.warnings, left_out_warning));
        XSFLIB_CHECK(call != kept.items.end() && kept.items.at(call->target).kind == Kind::kLfoCurve);
    }
    else
    {
        const std::string why = to == sar::Flavor::kCtr
                                    ? "The 3DS skips them."
                                    : "The 3DS skipped them. The " + song::ConsoleName(to) + " would run them.";
        XSFLIB_CHECK(
            Holds(result.warnings,
                  "The new file leaves out 3 commands: LFO 0's curve (1), LFOs 1 to 3 (1) and send C (1). " + why));
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
        const std::string why =
            to == sar::Flavor::kCtr ? "The 3DS skips it." : "The 3DS skipped it. The Wii U runs it.";
        XSFLIB_CHECK(Holds(result.warnings, "1 command stays in the file: send C (1). " + why));
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
        const std::string refusal =
            ErrorOf<std::invalid_argument>([&] { hokuto::edit::ForOtherConsole(source, kSong, to); });
        XSFLIB_CHECK(refusal == "The commands in the sequence file must stay at their offsets, but the " +
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
    return ErrorOf<std::invalid_argument>(
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
        fits.warnings, [](const std::string& warning) { return warning.starts_with("The banks keep only"); }));
    XSFLIB_CHECK(song::ProgramsOf(*new_archive, 0) == (std::vector<uint32_t>{0, 2}));
    XSFLIB_CHECK(song::ProgramsOf(*new_archive, 1) == (std::vector<uint32_t>{0}));
    SEQLIB_CHECK_EQ(song::WaveCountOf(*new_archive, 0), std::size_t{2});
    SEQLIB_CHECK_EQ(song::WaveCountOf(*new_archive, 1), std::size_t{1});
    XSFLIB_CHECK(same_wave(kWavesA, 0, 0, 0) && same_wave(kWavesA, 2, 0, 1) && same_wave(kWavesB, 0, 1, 0));
    XSFLIB_CHECK(WithoutOffsets(NotesOf(*new_archive, 0)) == WithoutOffsets(NotesOf(*old_archive, 0)));
    XSFLIB_CHECK(!NotesOf(*new_archive, 0).empty() && NotesOf(*new_archive, 0)[0].size() == 1);
    XSFLIB_CHECK(Holds(song.warnings,
                       "The banks keep only the instruments of program 0 and of the programs that the sound uses. "
                       "Their wave archives shrink from " +
                           KbText(whole) + " to " + KbText(kept) + ". Hokuto's 3DS model has room for " +
                           KbText(whole - 1) + "."));
    XSFLIB_CHECK(ComesBack(song.file));

    XSFLIB_CHECK(RefusalForCtr(file, kept - 1) ==
                 "The sound's banks use " + KbText(kept) +
                     " of wave archives, even without the unused instruments. Hokuto's 3DS model has room for only " +
                     KbText(kept - 1) + ".");
    XSFLIB_CHECK(RefusalForCtr(TrimFile(from, true), whole - 1) ==
                 "The sound's banks use " + KbText(whole) + " of wave archives. Hokuto's 3DS model has room for only " +
                     KbText(whole - 1) +
                     ". Can't drop the unused instruments because a variable or a random number selects some of the "
                     "programs.");
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
                     "The 3DS plays at most 23 notes and 24 channels at once. The sound has up to 24 notes and 24 "
                     "channels at once, not counting releases. The 3DS drops the notes with the lowest priority."});
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

// A song of the console of `from` with stereo waves goes to the 3DS. The fixture contains the song, the archives
// before and after, the gain of the pan law and the raise that the warnings name.
struct MonoFixture
{
    File many;
    File fewer;
    std::shared_ptr<const sar::SoundArchive> old_archive;
    hokuto::edit::OtherConsoleSong mono;
    std::shared_ptr<const sar::SoundArchive> new_archive;
    double gain;
    std::string rise;
};

// The fixture of the tests of mono waves, for a song of the console of `from`.
MonoFixture MakeMonoFixture(sar::Flavor from)
{
    const File many = TrimFile(from, HeldBytecode(13), true);
    const File fewer = TrimFile(from, HeldBytecode(12), true);
    const std::shared_ptr<const sar::SoundArchive> old_archive = ArchiveOf(many);
    const hokuto::edit::OtherConsoleSong mono = hokuto::edit::ForOtherConsole(many, 0, sar::Flavor::kCtr);
    const std::shared_ptr<const sar::SoundArchive> new_archive = ArchiveOf(mono.file);
    const ntdlib::nwsnd::PanInfo pan = ntdlib::nwsnd::PanInfoOf(0);
    const double gain =
        ntdlib::nwsnd::CalcPanRatio<ntdlib::nwsnd::CtrPolicy>(-1.0f, pan, ntdlib::nwsnd::OutputMode::kStereo) /
        ntdlib::nwsnd::CalcPanRatio<ntdlib::nwsnd::CtrPolicy>(0.0f, pan, ntdlib::nwsnd::OutputMode::kStereo);
    std::array<char, 64> range{};
    std::snprintf(range.data(), range.size(), "%.1f to %.1f dB", 20.0 * std::log10(gain),
                  20.0 * std::log10(gain * std::sqrt(2.0)));
    const std::string rise = " The mean is raised by " + std::string(range.data()) +
                             " to keep the power of the two channels at the pans of the notes. The samples are raised "
                             "as far as their peaks allow, up to the raise at the centre. The volumes of the regions "
                             "that use the waves make up the rest.";

    return {many, fewer, old_archive, mono, new_archive, gain, rise};
}

// Checks that wave `wave` of `new_archive` is the mean of the two channels of the same wave of `old_archive`, raised
// by the part of `full_gain` that the peak of the samples allows. Sets the rest of the gain, for the regions, in
// `region_gains`.
void MonoWaveGain(const std::shared_ptr<const sar::SoundArchive>& old_archive,
                  const std::shared_ptr<const sar::SoundArchive>& new_archive, uint32_t wave, double full_gain,
                  std::map<uint32_t, double>& region_gains)
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

// With 13 notes at once of the first wave, it holds 26 channels at once, and its stereo waves go in as mono. The notes
// play in the centre. Each mono wave holds the mean of its two channels, times the gain of the 3DS's pan law from the
// side to the centre. The first wave's gain is also the square root of 2 for its silent channel. The first wave's
// samples take its whole gain, and its regions keep their volumes. The third wave's samples take as much of the gain as
// their peak allows, and its regions the rest.
void TestMonoWaves(sar::Flavor from)
{
    const MonoFixture fixture = MakeMonoFixture(from);
    const std::shared_ptr<const sar::SoundArchive>& old_archive = fixture.old_archive;
    const hokuto::edit::OtherConsoleSong& mono = fixture.mono;
    const std::shared_ptr<const sar::SoundArchive>& new_archive = fixture.new_archive;
    const double gain = fixture.gain;

    XSFLIB_CHECK(mono.stereo_waves && mono.mono_waves);
    std::map<uint32_t, double> region_gains;
    for (const auto& [wave, full_gain] : {std::pair{0U, gain * std::sqrt(2.0)}, std::pair{2U, gain}})
    {
        MonoWaveGain(old_archive, new_archive, wave, full_gain, region_gains);
    }
    XSFLIB_CHECK(region_gains[0] == 1.0 && region_gains[2] > 1.4 && region_gains[2] < gain);
    XSFLIB_CHECK(song::DecodeItemWave(*new_archive, 0, 1).channels.size() == 1);
    SEQLIB_CHECK_EQ(VolumeOf(*new_archive, 0, 0), uint8_t{127});
    SEQLIB_CHECK_EQ(VolumeOf(*new_archive, 0, 1), uint8_t{127});
    SEQLIB_CHECK_EQ(VolumeOf(*new_archive, 0, 2), static_cast<uint8_t>(std::lround(127.0 * region_gains[2])));
    SEQLIB_CHECK_EQ(VolumeOf(*new_archive, 1, 0), uint8_t{127});
}

// The warning of the mono waves names the raise of their mean. The song plays the same notes, and the voices' warning
// goes.
void TestMonoWarnings(sar::Flavor from)
{
    const MonoFixture fixture = MakeMonoFixture(from);
    const std::shared_ptr<const sar::SoundArchive>& old_archive = fixture.old_archive;
    const hokuto::edit::OtherConsoleSong& mono = fixture.mono;
    const std::shared_ptr<const sar::SoundArchive>& new_archive = fixture.new_archive;
    const std::string& rise = fixture.rise;

    XSFLIB_CHECK(WarningsFrom(mono, "2 stereo waves") ==
                 std::vector<std::string>{"2 stereo waves are converted to mono. Each becomes the mean of its two "
                                          "channels. In stereo, the sound has up to 26 channels at once, not counting "
                                          "releases. The 3DS has 24." +
                                          rise + " The waves lose their stereo width."});
    XSFLIB_CHECK(WarningsFrom(mono, "The 3DS plays ").empty());
    XSFLIB_CHECK(WithoutOffsets(NotesOf(*new_archive, 0)) == WithoutOffsets(NotesOf(*old_archive, 0)));
    XSFLIB_CHECK(ComesBack(mono.file));
}

// With 12 notes, 24 channels, the waves stay stereo. The option turns the mono waves on and off either way.
void TestMonoOption(sar::Flavor from)
{
    using hokuto::edit::MonoWaves;

    const MonoFixture fixture = MakeMonoFixture(from);
    const File& many = fixture.many;
    const File& fewer = fixture.fewer;
    const std::string& rise = fixture.rise;

    const hokuto::edit::OtherConsoleSong stereo = hokuto::edit::ForOtherConsole(fewer, 0, sar::Flavor::kCtr);
    XSFLIB_CHECK(stereo.stereo_waves && !stereo.mono_waves);
    XSFLIB_CHECK(song::DecodeItemWave(*ArchiveOf(stereo.file), 0, 0).channels.size() == 2);
    XSFLIB_CHECK(WarningsFrom(stereo, "2 stereo waves").empty() && WarningsFrom(stereo, "The 3DS plays ").empty());
    const hokuto::edit::OtherConsoleSong on =
        hokuto::edit::ForOtherConsole(fewer, 0, sar::Flavor::kCtr, MonoWaves::kOn);
    XSFLIB_CHECK(on.mono_waves && song::DecodeItemWave(*ArchiveOf(on.file), 0, 0).channels.size() == 1);
    XSFLIB_CHECK(WarningsFrom(on, "2 stereo waves") ==
                 std::vector<std::string>{"2 stereo waves are converted to mono. Each becomes the mean of its two "
                                          "channels." +
                                          rise + " The waves lose their stereo width."});
    const hokuto::edit::OtherConsoleSong off =
        hokuto::edit::ForOtherConsole(many, 0, sar::Flavor::kCtr, MonoWaves::kOff);
    XSFLIB_CHECK(off.stereo_waves && !off.mono_waves);
    XSFLIB_CHECK(WarningsFrom(off, "The 3DS plays ") ==
                 std::vector<std::string>{
                     "The 3DS plays at most 23 notes and 24 channels at once. The sound has up to 13 notes and 26 "
                     "channels at once, not counting releases. The 3DS drops the notes with the lowest priority."});
}

// The trim counts the waves' size in mono, and only the waves that stay go in as mono. A song of mono waves has no
// stereo waves.
void TestMonoTrim(sar::Flavor from)
{
    using hokuto::edit::MonoWaves;

    const MonoFixture fixture = MakeMonoFixture(from);
    const File& many = fixture.many;
    const std::shared_ptr<const sar::SoundArchive>& new_archive = fixture.new_archive;
    const double gain = fixture.gain;

    // The trim: the mono waves fit whole in a memory too small for the stereo ones. Below that, only the first wave
    // stays, in mono.
    const std::size_t mono_memory = WaveMemoryOf(*new_archive);

    const auto trimmed = [](const hokuto::edit::OtherConsoleSong& song)
    {
        return !WarningsFrom(song, "The banks keep only").empty();
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
                     "1 stereo wave is converted to mono. It becomes the mean of its two channels. In stereo, the "
                     "sound has up to 26 channels at once, not counting releases. The 3DS has 24. The mean is raised "
                     "by " +
                     std::string(first.data()) +
                     " to keep the power of the two channels at the pans of the notes. The samples are raised as far "
                     "as their peaks allow, up to the raise at the centre. The volumes of the regions that use the "
                     "wave make up the rest. The wave loses its stereo width."});
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
            XSFLIB_CHECK(sar::DecodeWave(wave_archive.waves[wave], options) == source);
            const std::vector<uint8_t> file =
                song::LaterWaveFileOf(*wii_archive, {song::ItemKind::kWave, bank_file, wave}, to);
            XSFLIB_CHECK(file == wave_archive.waves[wave]);
        }
    }

    // The same items of the Wii U go to the Wii. The sequence file reads as the Wii's, without a conversion.
    const hokuto::edit::OtherConsoleSong later = hokuto::edit::ForOtherConsole(wii, 0, sar::Flavor::kCafe);
    const std::shared_ptr<const sar::SoundArchive> archive = ArchiveOf(later.file);
    const hokuto::edit::ConvertedItem sequence = hokuto::edit::WiiSequenceFileOf(*archive, 0);
    const hokuto::edit::Incoming read =
        hokuto::edit::ReadIncoming(sequence.file, song::Console{.engine = hokuto::format::Engine::kWii, .wii = build});
    XSFLIB_CHECK(read.wii && !read.converted && sequence.waves.empty());
    XSFLIB_CHECK(std::ranges::any_of(read.sequence.labels,
                                     [](const ntdlib::commands::Label& label) { return label.name == "SEQ_TEST"; }));
    XSFLIB_CHECK(std::ranges::all_of(read.sequence.items, plays_on_later));

    const hokuto::edit::ConvertedItem bank = hokuto::edit::WiiBankFilesOf(*archive, 0);
    SEQLIB_CHECK_EQ(ntdlib::rsar::BankFile::Read(bank.file, bank.waves).waves.size(), waves);
    XSFLIB_CHECK(bank.warnings.empty());

    // The Wii's model keeps a bank's RWAR in MEM2, and can't load one that doesn't fit. The warning says so.
    const hokuto::edit::ConvertedItem cramped = hokuto::edit::WiiBankFilesOf(*archive, 0, bank.waves.size() - 1);
    XSFLIB_CHECK(cramped.file == bank.file && cramped.waves == bank.waves);
    XSFLIB_CHECK(cramped.warnings ==
                 std::vector<std::string>{*hokuto::edit::WiiWaveRoomWarning(bank.waves.size(), bank.waves.size() - 1)});
    XSFLIB_CHECK(cramped.warnings[0].starts_with("The waves use ") &&
                 cramped.warnings[0].ends_with(" of waves and can't play them."));
    XSFLIB_CHECK(!hokuto::edit::WiiWaveRoomWarning(bank.waves.size(), bank.waves.size()));
    XSFLIB_CHECK(*hokuto::edit::WiiWaveRoomWarning(67'619'296) ==
                 "The waves use 64.5 MB. Hokuto's Wii model has room for 64.0 MB of waves and can't play them.");
    const ntdlib::rsar::WaveArchiveFile rwar = ntdlib::rsar::WaveArchiveFile::Read(bank.waves);
    const ntdlib::rsar::WaveArchiveFile whole =
        ntdlib::rsar::WaveArchiveFile::Read(song::WiiWaveFileOf(*archive, {song::ItemKind::kWaveArchive, 0, 0}));
    SEQLIB_CHECK_EQ(rwar.waves.size(), waves);
    SEQLIB_CHECK_EQ(whole.waves.size(), waves);
    for (uint32_t wave = 0; wave < waves; ++wave)
    {
        const ntdlib::wave::Pcm source = song::DecodeItemWave(*wii_archive, bank_file, wave);
        XSFLIB_CHECK(ntdlib::rsar::DecodeWave(ntdlib::rsar::Wave::ParseFile(rwar.waves[wave])) == source);
        XSFLIB_CHECK(ntdlib::rsar::DecodeWave(ntdlib::rsar::Wave::ParseFile(whole.waves[wave])) == source);
        const std::vector<uint8_t> file = song::WiiWaveFileOf(*archive, {song::ItemKind::kWave, 0, wave});
        XSFLIB_CHECK(ntdlib::rsar::DecodeWave(ntdlib::rsar::Wave::ParseFile(file)) == source);
    }
}

// A sound past the archive's sounds doesn't go to another console, and a sound goes only to another console.
void TestRefusals(sar::Flavor flavor)
{
    const File file = TestFile(flavor);
    const auto refusal = [&](uint32_t sound, sar::Flavor to)
    {
        return ErrorOf<std::invalid_argument>([&] { hokuto::edit::ForOtherConsole(file, sound, to); });
    };
    XSFLIB_CHECK(refusal(2, song::OtherConsole(flavor)) == "Only a sequence sound can be sent to another console.");
    XSFLIB_CHECK(refusal(kSong, flavor) == "The sound already plays on the " + song::ConsoleName(flavor) + ".");
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
        return ErrorOf<std::invalid_argument>([&] { hokuto::edit::ForEngine(file, kSong, engine); });
    };
    XSFLIB_CHECK(refusal(file.engine) == "The sound already uses this engine.");
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
    return xsflib::testing::RunTest("other_console",
                                    []
                                    {
                                        constexpr std::array kConsoles = {sar::Flavor::kNx, sar::Flavor::kCafe,
                                                                          sar::Flavor::kCtr};
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
                                        }
                                        TestFixedSkipped();
                                        TestCtrTimes(sar::Flavor::kCafe);
                                        TestCtrTimes(sar::Flavor::kNx);
                                        TestTrim(sar::Flavor::kNx);
                                        TestTrim(sar::Flavor::kCafe);
                                        TestHeld(sar::Flavor::kNx);
                                        TestHeld(sar::Flavor::kCafe);
                                        TestMonoWaves(sar::Flavor::kNx);
                                        TestMonoWarnings(sar::Flavor::kNx);
                                        TestMonoOption(sar::Flavor::kNx);
                                        TestMonoTrim(sar::Flavor::kNx);
                                        TestMonoWaves(sar::Flavor::kCafe);
                                        TestMonoWarnings(sar::Flavor::kCafe);
                                        TestMonoOption(sar::Flavor::kCafe);
                                        TestMonoTrim(sar::Flavor::kCafe);
                                        TestMonoPans(sar::Flavor::kNx);
                                        TestMonoPans(sar::Flavor::kCafe);
                                        TestBiquads();
                                        TestItemFiles();
                                        TestEffectNames();
                                    });
}
