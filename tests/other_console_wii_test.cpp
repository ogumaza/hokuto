// SPDX-License-Identifier: MIT

// Tests the conversions to and from the Wii (src/edit/to_wii.cpp and from_wii.cpp). other_console_fixtures.h contains a
// small archive of the Switch, the Wii U and the 3DS. Its song goes to the Wii from each of the three. The Wii's player
// skips a bank select, and an end after a comparison becomes a jump to an end. ntdlib's performer has to play the
// song's notes on the Wii as before, and ntdlib has to decode the bank's waves as before. The file has to come back
// from both forms. A song that plays both of its banks goes to the Wii with them as one bank, and the Wii has to play
// each note with its instrument there. A bank whose waves don't fit in the memory of the Wii's model takes only the
// instruments that the notes select.

#include "other_console_fixtures.h"

namespace
{

// A song of the Wii goes to the Wii U, and on from there to the Switch and the 3DS. The fixture contains the song
// of the Wii, with bytes that no command reaches when `dead` is true, and its conversion to the Wii U.
struct WiiSongFixture
{
    File wii;
    hokuto::edit::OtherConsoleSong result;
};

// The fixture of the tests of a song of the Wii.
WiiSongFixture MakeWiiSongFixture(bool dead)
{
    std::vector<uint8_t> bytecode = kWiiBytecode;
    if (dead)
    {
        bytecode.insert(bytecode.end(), {0x00, 0x11, 0x22, 0x33});
    }
    const File wii = WiiFile(bytecode);
    const hokuto::edit::OtherConsoleSong result = hokuto::edit::ForOtherConsole(wii, 0, sar::Flavor::kCafe);

    return {wii, result};
}

// With `dead`, the Wii's file contains bytes that no command reaches. Such a file stays in the archive's bytes at the
// import, and goes along all the same. Its values, its bank and its waves go along.
void TestWiiToWiiU(bool dead)
{
    const WiiSongFixture fixture = MakeWiiSongFixture(dead);
    const File& wii = fixture.wii;
    const hokuto::edit::OtherConsoleSong& result = fixture.result;
    const File& file = result.file;

    XSFLIB_CHECK(wii.sequences.size() == (dead ? 0U : 1U));
    XSFLIB_CHECK(file.form == format::ArchiveForm::kModel && FlavorOf(*file.native) == sar::Flavor::kCafe &&
                 file.native->engine != format::Engine::kWii && file.engine == format::Engine::kWiiU);
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
}

// The commands that the Wii's player skips stay out, and so do bytes that no command reaches. The other commands go in
// as the later consoles read the Wii's. Commands at the sound's start give global variable 20 the Wii's starting value.
// Byte 34 of a low-pass gives a cutoff of about 1,241 Hz on the Wii's build of June 2008. On the later consoles, it
// gives 1,000 Hz, and byte 35 gives 1,280 Hz, the nearest cutoff of their table.
void TestWiiCommands(bool dead)
{
    using ntdlib::commands::Command;
    using ntdlib::commands::Kind;
    using ntdlib::commands::kOne;
    using ntdlib::commands::Source;
    const WiiSongFixture fixture = MakeWiiSongFixture(dead);
    const hokuto::edit::OtherConsoleSong& result = fixture.result;
    const File& file = result.file;

    // The commands.
    const ntdlib::commands::Sequence& sequence = *file.sequences.at(0);
    XSFLIB_CHECK(sequence.items.size() == 17);
    const Command& compare = sequence.items.at(0);
    const Command& set = sequence.items.at(1);
    const Command& reset = sequence.items.at(2);
    XSFLIB_CHECK(compare.kind == Kind::kCompareEqual && compare.index == 20 && compare.value.source == Source::kValue &&
                 compare.value.value == -kOne);
    XSFLIB_CHECK(set.kind == Kind::kSetVariable && set.conditional && set.index == 20 &&
                 set.value.source == Source::kValue && set.value.value == 0);
    XSFLIB_CHECK(reset.kind == Kind::kCompareEqual && reset.index == 20 && reset.value.source == Source::kVariable &&
                 reset.value.value == 20);
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
}

// On each console, the song plays the Wii's notes.
void TestWiiNotes(bool dead)
{
    const WiiSongFixture fixture = MakeWiiSongFixture(dead);
    const File& wii = fixture.wii;
    const hokuto::edit::OtherConsoleSong& result = fixture.result;
    const File& file = result.file;

    // The notes and the waves.
    const std::shared_ptr<const ntdlib::rsar::SoundArchive> wii_archive = WiiArchiveOf(wii);
    const std::vector<std::vector<song::Note>> notes = NotesOf(*wii_archive, 0);
    XSFLIB_CHECK(notes.size() == 1 && notes[0].size() == 2 && notes[0][0].key == 60 && notes[0][0].program == 1 &&
                 notes[0][1].key == 61 && notes[0][1].program == 1);
    const std::shared_ptr<const sar::SoundArchive> new_archive = ArchiveOf(file);
    XSFLIB_CHECK(WithoutOffsets(NotesOf(*new_archive, 0)) == WithoutOffsets(notes));
    XSFLIB_CHECK(song::ProgramsOf(*new_archive, 0) == song::ProgramsOf(*wii_archive, 0));
    const uint32_t bank_file = wii_archive->Banks().at(0).file_id;
    SEQLIB_CHECK_EQ(song::WaveCountOf(*new_archive, 0), song::WaveCountOf(*wii_archive, bank_file));
    for (uint32_t wave = 0; wave < song::WaveCountOf(*wii_archive, bank_file); ++wave)
    {
        XSFLIB_CHECK(song::DecodeItemWave(*new_archive, 0, wave) ==
                     song::DecodeItemWave(*wii_archive, bank_file, wave));
    }

    // On to the Switch and the 3DS.
    for (const sar::Flavor to : {sar::Flavor::kNx, sar::Flavor::kCtr})
    {
        const hokuto::edit::OtherConsoleSong next = hokuto::edit::ForOtherConsole(wii, 0, to);
        XSFLIB_CHECK(FlavorOf(*next.file.native) == to && next.file.engine == format::EngineOf(to));
        XSFLIB_CHECK(next.warnings == kWiiWarnings);
        XSFLIB_CHECK(WithoutOffsets(NotesOf(*ArchiveOf(next.file), 0)) == WithoutOffsets(notes));
        XSFLIB_CHECK(ComesBack(next.file));
    }
}

// The later consoles accept the song of the Wii in Another Console.
void TestWiiEngines(bool dead)
{
    const WiiSongFixture fixture = MakeWiiSongFixture(dead);
    const File& wii = fixture.wii;
    const hokuto::edit::OtherConsoleSong& result = fixture.result;
    const File& file = result.file;

    // The engines of the later consoles take the sound, and the Wii U's plays the same conversion.
    for (const format::Engine engine : {format::Engine::k3ds, format::Engine::kWiiU, format::Engine::kSwitch})
    {
        XSFLIB_CHECK(!hokuto::edit::EngineRefused(wii, engine));
    }
    const hokuto::edit::OtherConsoleSong converted = hokuto::edit::ForEngine(wii, 0, format::Engine::kWiiU);
    XSFLIB_CHECK(format::Write(converted.file) == format::Write(file) && converted.warnings == result.warnings);
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
        XSFLIB_CHECK(holds(
            hokuto::edit::ReadIncoming(file.file, song::Console{.engine = hokuto::format::Engine::kWii, .wii = build})
                .sequence,
            30, 34));
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
    XSFLIB_CHECK(Holds(result.warnings, BiquadText("Wii", "1 setting changed from types 1–5 to types 6–10.")));
    const auto rate_warning = [](const std::string& warning)
    {
        return warning.find("the Wii U runs them at") != std::string::npos;
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
                       "The sound's volume is lowered from 200 to 127. The Wii plays volumes above 127 at 127."));

    const std::vector<uint8_t> kPlain = {0xB0, 0x30, 0x81, 0x00, 0x3C, 0x64, 0x18, 0xFF};
    const File old = WiiFile(kPlain, ntdlib::SndBuild{.october_2006 = true});
    const hokuto::edit::OtherConsoleSong panned = hokuto::edit::ForOtherConsole(old, 0, sar::Flavor::kCafe);
    const format::NativeSound& sound = panned.file.native->tables->sounds.at(0);
    XSFLIB_CHECK(sound.pan_mode == 0 && sound.pan_curve == 0);
    XSFLIB_CHECK(panned.warnings ==
                 std::vector<std::string>{"The sound is set to the dual pan mode and the square-root pan "
                                          "curve. The October 2006 build of NW4R SND supports only "
                                          "those."});
    XSFLIB_CHECK(WithoutOffsets(NotesOf(*ArchiveOf(panned.file), 0)) == WithoutOffsets(NotesOf(*WiiArchiveOf(old), 0)));

    File effects = WiiFile(kPlain);
    effects.aux_effects[1] = {{EffectKind::kWiiChorus, {10.0f, 0.5f, 1.0f}}};
    const hokuto::edit::OtherConsoleSong dry = hokuto::edit::ForOtherConsole(effects, 0, sar::Flavor::kCafe);
    XSFLIB_CHECK(dry.warnings ==
                 std::vector<std::string>{"The AXFX Chorus on bus B is left out. Hokuto plays Wii effects only "
                                          "on the Wii."});
    XSFLIB_CHECK(std::ranges::all_of(dry.file.aux_effects, &std::vector<format::Effect>::empty));

    const std::string refusal =
        ErrorOf<std::invalid_argument>([&] { hokuto::edit::ForOtherConsole(effects, 1, sar::Flavor::kCafe); });
    XSFLIB_CHECK(refusal == "Only a sequence sound can be sent to another console.");
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

// The warnings of the song of the console of `from` on its way to the Wii, in their order.
std::vector<std::string> ToWiiWarnings(sar::Flavor from)
{
    std::vector<std::string> warnings = {
        "The new file leaves out 1 command: bank select (1). The Wii skips it.",
        "NW4R SND applies volume2, main volume and pitch bend changes at once, without a ramp. The ramp time of 1 "
        "such change is dropped.",
        "The sound reads global variable 20 before setting it. NW4R SND starts global variables at 0. Later consoles "
        "start them at -1. New commands at the start of the sound set it to -1 if it is 0.",
        "The bank in the sound's other slot is left out: BANK_B. None of the notes use it.",
        "The sound's 3D settings, single play and user parameters are left out. NW4R SND reads other fields.",
        "The sound's Actor Player is left out. Hokuto doesn't read actor players in Wii archives.",
        "The sequence file also contains the commands of 1 other sound. They are copied but unused.",
    };
    if (from == sar::Flavor::kCtr)
    {
        warnings.push_back("The nw::snd Reverb (3DS) on bus A is left out. The Wii uses AXFX effects instead.");
    }
    else
    {
        warnings.push_back("The Wii plays in strict mixing only.");
        warnings.push_back(
            "The I3DL2 Reverb and nw::snd Reverb (3DS) on bus A are left out. The Wii uses AXFX effects instead.");
        warnings.push_back("The nw::snd Chorus on bus B is left out. The Wii uses AXFX effects instead.");
    }

    return warnings;
}

// A sound whose only setting that the Wii has no place for is the value after its 3D settings converts to the Wii with
// the warning that names the settings left out. The Wii keeps the remote filter, and no warning names it.
void TestWiiSettingsLeft(sar::Flavor from)
{
    for (const bool remote_filter : {true, false})
    {
        File source = TestFile(from);
        format::Model model = format::ModelOf(source);
        sar::SoundModel& song = model.archive.sounds.at(kSong);
        song.single_play.reset();
        song.settings_3d = {};
        song.user_options = {};
        song.option_17.reset();
        song.remote_filter = remote_filter ? 3 : 0;
        song.after_3d = remote_filter ? 0 : 1;
        source = format::WithModel(source, model);

        const hokuto::edit::OtherConsoleSong result = hokuto::edit::ForWii(source, kSong);

        XSFLIB_CHECK(Holds(result.warnings,
                           "The sound's 3D settings, single play and user parameters are left out. NW4R SND reads "
                           "other fields.") != remote_filter);
    }
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
    XSFLIB_CHECK(file.form == format::ArchiveForm::kModel && file.native->engine == format::Engine::kWii &&
                 file.engine == format::Engine::kWii);
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
    XSFLIB_CHECK(Heard(NotesOf(*wii_archive, 0)) == Heard(notes));
    XSFLIB_CHECK(song::ProgramsOf(*wii_archive, 0) == song::ProgramsOf(*old_archive, 0));
    const uint32_t bank_file = wii_archive->Banks().at(0).file_id;
    XSFLIB_CHECK(song::WaveCountOf(*wii_archive, bank_file) == song::WaveCountOf(*old_archive, kWavesA));
    for (uint32_t wave = 0; wave < song::WaveCountOf(*old_archive, kWavesA); ++wave)
    {
        XSFLIB_CHECK(song::DecodeItemWave(*wii_archive, bank_file, wave) ==
                     song::DecodeItemWave(*old_archive, kWavesA, wave));
    }

    // The other sound's bank goes along without the waves that the archive lacks.
    const hokuto::edit::OtherConsoleSong other = hokuto::edit::ForWii(source, kOther);
    const std::shared_ptr<const ntdlib::rsar::SoundArchive> other_archive = WiiArchiveOf(other.file);
    XSFLIB_CHECK(song::WaveCountOf(*other_archive, other_archive->Banks().at(0).file_id) == 0);
    XSFLIB_CHECK(Heard(NotesOf(*other_archive, 0)) == Heard(NotesOf(*old_archive, kOther)));
    XSFLIB_CHECK(ComesBack(other.file));

    // A sound above volume 127 goes in at 127.
    const File loud = hokuto::edit::ChangeSound(source, kSong, {.value = hokuto::edit::SoundValue::kVolume, .to = 200});
    const hokuto::edit::OtherConsoleSong louder = hokuto::edit::ForWii(loud, kSong);
    XSFLIB_CHECK(louder.file.native->tables->sounds.at(0).volume == 127);
    XSFLIB_CHECK(Holds(louder.warnings,
                       "The sound's volume is lowered from 200 to 127, the most that the Wii plays. The sound is "
                       "3.9 dB quieter."));

    // The engine of the Wii plays the same conversion.
    XSFLIB_CHECK(!hokuto::edit::EngineRefused(source, format::Engine::kWii));
    const hokuto::edit::OtherConsoleSong converted = hokuto::edit::ForEngine(source, kSong, format::Engine::kWii);
    XSFLIB_CHECK(format::Write(converted.file) == format::Write(file) && converted.warnings == result.warnings);
}

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
              "A Wii sound can use only one bank because NW4R SND has no bank select. The notes use the banks in "
              "several slots. They are merged into one bank, BANK_A_BANK_B: BANK_A at programs 0 to 1 and BANK_B at "
              "programs 2 to 3. 2 bank selects are now program changes and 1 program change is renumbered for its "
              "slot."));
    XSFLIB_CHECK(std::ranges::none_of(
        result.warnings, [](const std::string& warning) { return warning.find("leaves out") != std::string::npos; }));
    const auto bank_warning = [](const std::string& warning)
    {
        return warning.find("left out: BANK") != std::string::npos;
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
    const std::vector<std::vector<song::Note>> wii_notes = NotesOf(*wii_archive, 0);
    XSFLIB_CHECK(Heard(wii_notes) == Heard(notes));
    XSFLIB_CHECK(wii_notes.size() == 1 && wii_notes[0].size() == 4 && wii_notes[0][0].program == 1 &&
                 wii_notes[0][1].program == 3 && wii_notes[0][2].program == 2 && wii_notes[0][3].program == 0);

    // The bank's waves: bank A's two, then bank B's first.
    const uint32_t bank_file = wii_archive->Banks().at(0).file_id;
    XSFLIB_CHECK(song::WaveCountOf(*wii_archive, bank_file) == 3);
    XSFLIB_CHECK(song::DecodeItemWave(*wii_archive, bank_file, 0) == song::DecodeItemWave(*old_archive, kWavesA, 0));
    XSFLIB_CHECK(song::DecodeItemWave(*wii_archive, bank_file, 1) == song::DecodeItemWave(*old_archive, kWavesA, 1));
    XSFLIB_CHECK(song::DecodeItemWave(*wii_archive, bank_file, 2) == song::DecodeItemWave(*old_archive, kWavesB, 0));

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
        return ErrorOf<std::invalid_argument>([&] { hokuto::edit::ForWii(file, kSong, std::nullopt, memory); });
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
            XSFLIB_CHECK(song::DecodeItemWave(*wii_archive, bank_file, static_cast<uint32_t>(wave)) ==
                         song::DecodeItemWave(*old_archive, wave_archive, index));
        }
        XSFLIB_CHECK(Holds(result.warnings, "The bank keeps only the instruments that the sound uses. Its waves use " +
                                                std::to_string(kept) + " bytes instead of " + std::to_string(all) +
                                                " bytes. Hokuto's Wii model has room for " + std::to_string(all - 1) +
                                                " bytes of waves."));

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
        XSFLIB_CHECK(Heard(NotesOf(*wii_archive, 0)) == Heard(notes) &&
                     Heard(NotesOf(*whole_archive, 0)) == Heard(notes));

        XSFLIB_CHECK(refusal(source, kept - 1) ==
                     "The waves of the sound's bank use " + std::to_string(kept) +
                         " bytes, even without the unused instruments. Hokuto's Wii model has room for " +
                         std::to_string(kept - 1) + " bytes of waves.");
    }
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("other_console_wii",
                                    []
                                    {
                                        constexpr std::array kConsoles = {sar::Flavor::kNx, sar::Flavor::kCafe,
                                                                          sar::Flavor::kCtr};
                                        for (const sar::Flavor from : kConsoles)
                                        {
                                            TestToWii(from);
                                            TestWiiSettingsLeft(from);
                                            TestWiiBanks(from);
                                            TestWiiTrim(from);
                                        }
                                        for (const bool dead : {false, true})
                                        {
                                            TestWiiToWiiU(dead);
                                            TestWiiCommands(dead);
                                            TestWiiNotes(dead);
                                            TestWiiEngines(dead);
                                        }
                                        TestWiiRandomLowPass();
                                        TestRandomLowPassToWii();
                                        TestWiiBiquads();
                                        TestWiiValues();
                                    });
}
