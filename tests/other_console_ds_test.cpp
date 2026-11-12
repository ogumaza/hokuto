// SPDX-License-Identifier: MIT

// Tests the conversions to and from the DS (src/edit/to_ds.cpp). The song of other_console_fixtures.h converts to the
// DS from the Switch, the Wii U and the 3DS, from the Wii and from the GBA. The DS's performer has to play its notes as
// before, with both banks as one bank, and a bank whose files don't fit in the memory of the DS's model takes only the
// instruments that the notes select.

#include "other_console_fixtures.h"

namespace
{

// The archive that a file of the DS exports, opened.
std::shared_ptr<const song::DsArchive> DsArchiveOf(const File& file)
{
    return song::LoadDsArchive(format::ExportArchive(file));
}

// The song converts to the DS with its volume, its priorities, its notes, the instruments of its first bank and the
// waves that they play, and the parts of the file that go with it. The effects and the values of the sound that the DS
// has no place for stay behind.
void TestToDs(sar::Flavor from)
{
    const File source = TestFile(from);

    const hokuto::edit::OtherConsoleSong result = hokuto::edit::ForDs(source, kSong);

    const File& file = result.file;
    XSFLIB_CHECK(file.engine == format::Engine::kDs && file.native->engine == format::Engine::kDs);
    XSFLIB_CHECK(ComesBack(file));
    XSFLIB_CHECK(file.tags == source.tags && file.sound_tags.at(0) == source.sound_tags.at(kSong));
    XSFLIB_CHECK(file.scenes.size() == 1 && file.scenes[0].sound == 0 && file.scenes[0].name == "Loud");
    XSFLIB_CHECK(file.track_colours.size() == 1 && file.track_colours.contains({0, 0}));
    XSFLIB_CHECK(std::ranges::all_of(file.aux_effects, [](const auto& bus) { return bus.empty(); }));

    const ntdlib::sdat::ArchiveModel model = ntdlib::sdat::ReadArchiveModel(format::ExportArchive(file));
    XSFLIB_CHECK(model.sequences.size() == 1 && model.banks.size() == 1 && model.wave_archives.size() == 1);
    SEQLIB_REQUIRE(model.symbols.has_value());
    XSFLIB_CHECK(model.symbols->names[0].at(0) == "SEQ_SONG" && model.symbols->names[2].at(0) == "BANK_A" &&
                 model.symbols->names[3].at(0) == "WARC_A");
    const std::shared_ptr<const song::DsArchive> archive = DsArchiveOf(file);
    const hokuto::edit::DsSoundValues values = hokuto::edit::DsSoundValuesOf(*archive, 0);
    XSFLIB_CHECK(values.bank == 0 && values.volume == 90 && values.channel_priority == 40 &&
                 values.player_priority == 48 && values.player == 0);

    // The song plays key 60 of program 0 and key 62 of program 1, at the source's ticks.
    const std::shared_ptr<const sar::SoundArchive> old_archive = ArchiveOf(source);
    XSFLIB_CHECK(Heard(NotesOf(*archive, 0)) == Heard(NotesOf(*old_archive, kSong)));

    // The sequence contains the song's commands alone. Bank A contains two instruments, on waves 0 and 1.
    const ntdlib::sdat::Bank bank = ntdlib::sdat::Bank::Parse(model.files.at(model.banks[0]->file_id));
    SEQLIB_REQUIRE(bank.instruments.size() == 2);
    XSFLIB_CHECK(bank.instruments[0].region.wave == 0 && bank.instruments[1].region.wave == 1);
    const ntdlib::sdat::WaveArchive waves =
        ntdlib::sdat::WaveArchive::Parse(model.files.at(model.wave_archives[0]->file_id));
    SEQLIB_REQUIRE(waves.waves.size() == 2);
    XSFLIB_CHECK(waves.waves[0].encoding == ntdlib::sdat::WaveEncoding::kImaAdpcm && waves.waves[0].loop == 1);

    XSFLIB_CHECK(
        Holds(result.warnings, "The bank in the sound's other slot is left out: BANK_B. None of the notes use it."));
    XSFLIB_CHECK(Holds(result.warnings,
                       "The sound's 3D settings, single play, user parameters and remote filter are left "
                       "out. DS sounds have no such settings."));
    XSFLIB_CHECK(
        Holds(result.warnings, "The sound's pan mode and pan curve are left out. The DS has only one way to pan."));
    const auto no_effect = [](const std::string& warning)
    {
        return warning.ends_with("The DS doesn't support it.") || warning.ends_with("The DS doesn't support them.");
    };
    XSFLIB_CHECK(std::ranges::count_if(result.warnings, no_effect) == 2);
    const auto left_out = [](const std::string& warning)
    {
        return warning.starts_with("The new file leaves out");
    };
    XSFLIB_CHECK(std::ranges::none_of(result.warnings, left_out));
}

// A sound whose only setting that the DS has no place for is its remote filter, or the value after its 3D settings,
// converts to the DS with the warning that names the settings left out.
void TestDsSettingsLeft(sar::Flavor from)
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

        const hokuto::edit::OtherConsoleSong result = hokuto::edit::ForDs(source, kSong);

        XSFLIB_CHECK(Holds(result.warnings,
                           "The sound's 3D settings, single play, user parameters and remote filter are left "
                           "out. DS sounds have no such settings."));
    }
}

// A song that plays both of its banks goes to the DS with them as one bank, the same way as to the Wii. Each note plays
// the program of its slot there.
void TestDsBanks(sar::Flavor from)
{
    const File source = TestFile(from, kTwoBanksBytecode, 23);

    const hokuto::edit::OtherConsoleSong result = hokuto::edit::ForDs(source, kSong);

    XSFLIB_CHECK((result.slot_programs == std::map<int, uint32_t>{{0, 0}, {1, 2}}));
    XSFLIB_CHECK(Holds(result.warnings,
                       "A DS sound can use only one bank because NITRO-Composer has no bank select. The notes use the "
                       "banks in several slots. They are merged into one bank, BANK_A_BANK_B: BANK_A at programs 0 to "
                       "1 and BANK_B at programs 2 to 3. 2 bank selects are now program changes and 1 program change "
                       "is renumbered for its slot."));
    XSFLIB_CHECK(ComesBack(result.file));

    const std::shared_ptr<const sar::SoundArchive> old_archive = ArchiveOf(source);
    std::vector<std::vector<song::Note>> notes = NotesOf(*old_archive, kSong);
    for (std::vector<song::Note>& track : notes)
    {
        for (song::Note& note : track)
        {
            note.program += result.slot_programs.at(note.bank_slot);
        }
    }
    XSFLIB_CHECK(Heard(NotesOf(*DsArchiveOf(result.file), 0)) == Heard(notes));

    // Bank A's two waves, then bank B's first.
    const ntdlib::sdat::ArchiveModel model = ntdlib::sdat::ReadArchiveModel(format::ExportArchive(result.file));
    XSFLIB_CHECK(ntdlib::sdat::WaveArchive::Parse(model.files.at(model.wave_archives[0]->file_id)).waves.size() == 3);
    const ntdlib::sdat::Bank bank = ntdlib::sdat::Bank::Parse(model.files.at(model.banks[0]->file_id));
    SEQLIB_REQUIRE(bank.instruments.size() == 4);
    XSFLIB_CHECK(bank.instruments[0].region.wave == 0 && bank.instruments[1].region.wave == 1 &&
                 bank.instruments[2].region.wave == 2 && bank.instruments[3].region.wave == 1);
}

// The most that a DS file's sequence, bank and wave archive take of the model's heap. The heap puts each file on a
// multiple of 32 bytes. A wave archive of over 2 MiB loads its waves one at a time, each on such a multiple, with a
// table of 8 bytes for each wave.
std::size_t DsHeapBytesOf(const File& file)
{
    const ntdlib::sdat::ArchiveModel model = ntdlib::sdat::ReadArchiveModel(format::ExportArchive(file));
    const std::vector<uint8_t>& waves = model.files.at(model.wave_archives[0]->file_id);

    return model.files.at(model.banks[0]->file_id).size() + waves.size() +
           model.files.at(model.sequences[0]->file_id).size() +
           40 * ntdlib::sdat::WaveArchive::Parse(waves).waves.size() + 96;
}

// The bank of a song for the DS whose files don't fit in the memory of the DS's model takes only the instruments that
// the notes select, with their waves. Without room for those, the song doesn't convert.
void TestDsTrim(sar::Flavor from)
{
    const File source = TestFile(from, kOneProgramBytecode, 8);

    const hokuto::edit::OtherConsoleSong whole = hokuto::edit::ForDs(source, kSong);
    const std::size_t memory = DsHeapBytesOf(whole.file) - 1;

    const hokuto::edit::OtherConsoleSong trimmed = hokuto::edit::ForDs(source, kSong, memory);

    XSFLIB_CHECK(ComesBack(trimmed.file));
    const ntdlib::sdat::ArchiveModel model = ntdlib::sdat::ReadArchiveModel(format::ExportArchive(trimmed.file));
    const ntdlib::sdat::Bank kept = ntdlib::sdat::Bank::Parse(model.files.at(model.banks[0]->file_id));
    SEQLIB_REQUIRE(kept.instruments.size() == 2);
    XSFLIB_CHECK(kept.instruments[0].type == ntdlib::sdat::InstrumentType::kNone &&
                 kept.instruments[1].type == ntdlib::sdat::InstrumentType::kPcm);
    XSFLIB_CHECK(ntdlib::sdat::WaveArchive::Parse(model.files.at(model.wave_archives[0]->file_id)).waves.size() == 1);
    const auto trim_warning = [](const std::string& warning)
    {
        return warning.starts_with("The bank keeps only the instruments that the sound uses.");
    };
    XSFLIB_CHECK(std::ranges::any_of(trimmed.warnings, trim_warning));
    XSFLIB_CHECK(Heard(NotesOf(*DsArchiveOf(trimmed.file), 0)) == Heard(NotesOf(*ArchiveOf(source), kSong)));

    const ntdlib::sdat::ArchiveModel sent = ntdlib::sdat::ReadArchiveModel(format::ExportArchive(whole.file));
    const std::size_t sequence = sent.files.at(sent.sequences[0]->file_id).size();
    const std::string refusal =
        ErrorOf<std::invalid_argument>([&] { hokuto::edit::ForDs(source, kSong, sequence + 8); });
    XSFLIB_CHECK(refusal.starts_with("The sound's bank and its waves use ") &&
                 refusal.find("even without the unused instruments. Hokuto's DS model has room for") !=
                     std::string::npos);
}

// Waves that don't fit at their rate go in at a lower one, with a warning. Each note plays as before.
void TestDsLowerRates(sar::Flavor from)
{
    const File source = TestFile(from, kOneProgramBytecode, 8);
    const auto waves_of = [](const File& file)
    {
        const ntdlib::sdat::ArchiveModel model = ntdlib::sdat::ReadArchiveModel(format::ExportArchive(file));
        return ntdlib::sdat::WaveArchive::Parse(model.files.at(model.wave_archives[0]->file_id));
    };

    // The instruments that the sound selects, with no byte to spare.
    const std::size_t all = DsHeapBytesOf(hokuto::edit::ForDs(source, kSong).file);
    const hokuto::edit::OtherConsoleSong trimmed = hokuto::edit::ForDs(source, kSong, all - 1);
    const uint16_t rate = waves_of(trimmed.file).waves.at(0).sample_rate;
    SEQLIB_REQUIRE(rate > 12000);

    const hokuto::edit::OtherConsoleSong lower = hokuto::edit::ForDs(source, kSong, DsHeapBytesOf(trimmed.file) - 1);

    XSFLIB_CHECK(ComesBack(lower.file));
    const uint16_t lowered = waves_of(lower.file).waves.at(0).sample_rate;
    XSFLIB_CHECK(lowered < rate);
    const auto warned = [](const std::string& warning)
    {
        for (const int named : {24000, 16364, 12000, 8182})
        {
            if (warning.starts_with("The waves don't fit in Hokuto's DS model at their sample rates.") &&
                warning.find(" resampled to " + std::to_string(named) + " Hz. Frequencies above " +
                             std::to_string(named / 2) + " Hz are lost.") != std::string::npos)
            {
                return true;
            }
        }
        return false;
    };
    XSFLIB_CHECK(std::ranges::any_of(lower.warnings, warned));
    XSFLIB_CHECK(Heard(NotesOf(*DsArchiveOf(lower.file), 0)) == Heard(NotesOf(*ArchiveOf(source), kSong)));
}

// A sound converts to the DS only from another console, and only a sequence sound whose commands can move.
void TestDsRefusals(sar::Flavor flavor)
{
    const File source = TestFile(flavor);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::ForDs(source, 7); }) ==
                 "Only a sequence sound can be sent to another console.");
    const File fixed = TestFile(flavor, FixedBytecode());
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::ForDs(fixed, kSong); }) ==
                 "The commands in the sequence file must stay at their offsets. Hokuto can't send such a sequence to "
                 "the DS yet.");
    const hokuto::edit::OtherConsoleSong sent = hokuto::edit::ForDs(source, kSong);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::ForDs(sent.file, 0); }) ==
                 "The sound already plays on the DS.");
}

// A sound of the Wii and a song of the GBA go to the DS through the Wii U. The commands that the DS doesn't hold stay
// out with a warning.
void TestToDsSources()
{
    const File wii = WiiFile();
    const hokuto::edit::OtherConsoleSong from_wii = hokuto::edit::ForDs(wii, 0);
    XSFLIB_CHECK(from_wii.file.engine == format::Engine::kDs && ComesBack(from_wii.file));
    XSFLIB_CHECK(Heard(NotesOf(*DsArchiveOf(from_wii.file), 0)) == Heard(NotesOf(*WiiArchiveOf(wii), 0)));

    const std::vector<uint8_t> cart = GbaCart();
    const File gba = format::ImportGbaRom(cart);
    const hokuto::edit::OtherConsoleSong from_gba = hokuto::edit::ForDs(gba, 0);
    XSFLIB_CHECK(from_gba.file.engine == format::Engine::kDs && ComesBack(from_gba.file));
    XSFLIB_CHECK(
        PlaysAsGba(song::ReadSequence(*song::LoadGbaRom(cart), 0), song::ReadSequence(*DsArchiveOf(from_gba.file), 0)));

    const File skipped = TestFile(sar::Flavor::kNx, SkippedBytecode(sar::Flavor::kNx), 14);
    const hokuto::edit::OtherConsoleSong result = hokuto::edit::ForDs(skipped, kSong);
    const auto left_out = [](const std::string& warning)
    {
        return warning.starts_with("The new file leaves out ") && warning.ends_with("The DS skips them.");
    };
    XSFLIB_CHECK(std::ranges::any_of(result.warnings, left_out));
}

// A GBA song whose header sets the reverb sends to bus A on the Wii U, and the new file contains no effect there. The
// warnings say so, and name NintendoWare's whole tempos. The DS has no buses, and NITRO-Composer's tempo command stores
// whole beats a minute too. The song converts there without the advice about bus A. The warning about whole tempos
// names the DS.
void TestGbaReverbToDs()
{
    const File gba = format::ImportGbaRom(GbaCart(0x80 | 40));
    const auto bus_a = [](const std::string& warning)
    {
        return warning.starts_with("The song sends reverb to bus A");
    };
    const auto tempos_of = [](const std::string& who)
    {
        return [who](const std::string& warning)
        {
            return warning.starts_with(who + " tempos must be whole BPM values.");
        };
    };

    const hokuto::edit::OtherConsoleSong wii_u = hokuto::edit::ForOtherConsole(gba, 0, sar::Flavor::kCafe);
    const hokuto::edit::OtherConsoleSong ds = hokuto::edit::ForDs(gba, 0);

    XSFLIB_CHECK(std::ranges::any_of(wii_u.warnings, bus_a));
    XSFLIB_CHECK(std::ranges::any_of(wii_u.warnings, tempos_of("NintendoWare")));
    XSFLIB_CHECK(std::ranges::none_of(ds.warnings, bus_a));
    XSFLIB_CHECK(std::ranges::any_of(ds.warnings, tempos_of("DS")));
    XSFLIB_CHECK(std::ranges::none_of(ds.warnings, tempos_of("NintendoWare")));
}

// A sequence of the DS and a song of the GBA play on the engines of the later consoles too. A sound of another console
// plays on the DS's engine, and none plays on the GBA's. A DS sequence doesn't open as a timeline.
void TestDsEngines()
{
    namespace sdat_files = ntdlib::testing::sdat_files;
    sdat_files::Archive spec;
    spec.items[0] = {sdat_files::SequenceItem(0, 0, "SEQ_A")};
    spec.files = {sdat_files::Sequence({0xFE, 0x01, 0x00, 0x3C, 0x7F, 0x30, 0xFF})};
    const File ds = format::ImportDsArchive(sdat_files::SoundArchive(spec));
    const File gba = format::ImportGbaRom(GbaCart());
    const File other = TestFile(sar::Flavor::kNx);

    XSFLIB_CHECK(!hokuto::edit::EngineRefused(ds, format::Engine::kDs));
    for (const format::Engine engine :
         {format::Engine::kSwitch, format::Engine::kWiiU, format::Engine::k3ds, format::Engine::kWii})
    {
        XSFLIB_CHECK(!hokuto::edit::EngineRefused(ds, engine));
        XSFLIB_CHECK(!hokuto::edit::EngineRefused(gba, engine));
    }
    XSFLIB_CHECK(!hokuto::edit::EngineRefused(gba, format::Engine::kGba));
    XSFLIB_CHECK(!hokuto::edit::EngineRefused(other, format::Engine::kDs));
    XSFLIB_CHECK(!hokuto::edit::EngineRefused(gba, format::Engine::kDs));
    XSFLIB_CHECK(hokuto::edit::EngineRefused(ds, format::Engine::kGba) ==
                 "Can't play sounds of other consoles on the GBA engine.");

    // The DS's engine plays the conversion that Another Console writes.
    XSFLIB_CHECK(hokuto::edit::ForEngine(other, kSong, format::Engine::kDs).file.engine == format::Engine::kDs);
    XSFLIB_CHECK(hokuto::edit::ForEngine(gba, 0, format::Engine::kDs).file.engine == format::Engine::kDs);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::ForEngine(ds, 0, format::Engine::kDs); }) ==
                 "The sound already uses this engine.");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::AsTimeline(ds, 0); }) ==
                 "DS sequences can be edited only as a program. Hokuto can't lay them out the way the MIDI converter "
                 "of the DS SDK does.");
}

// A sequence of the DS converts to the Wii U through ntdlib's converter. Its name contains a character that a label
// can't contain. The labels of the new song have an underscore in its place, and the new sound starts at the song's
// start. It plays the DS's notes with their keys, each within 10 ms of its time.
void TestFromDs()
{
    namespace sdat = ntdlib::sdat;
    namespace sdat_files = ntdlib::testing::sdat_files;
    const sdat_files::Wave square{
        .loop = 1,
        .loop_length = 4,
        .data = {0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0}};
    constexpr uint16_t kNone = 0xFFFF;
    sdat::ArchiveModel model;
    model.sequences = {sdat::SequenceInfo{0, 0, 127, 64, 64, 0, 0, ""}};
    model.banks = {sdat::BankInfo{1, {0, kNone, kNone, kNone}, ""}};
    model.wave_archives = {sdat::WaveArchiveInfo{2, ""}};
    model.players = {sdat::PlayerInfo{1, 0, 0, 0, ""}};
    model.symbols.emplace();
    model.symbols->names[0] = {"SEQ_A-B"};
    model.symbols->names[2] = {"BANK_A"};
    model.symbols->names[3] = {"WARC_A"};
    model.files = {sdat_files::Sequence({0x3C, 100, 0x30, 0x40, 100, 0x30, 0xFF}),
                   sdat_files::Bank({{.type = 1, .region = {.wave = 0}}}), sdat_files::WaveArchive({square})};
    const File ds = format::ImportDsArchive(sdat::BuildArchive(model));

    const hokuto::edit::OtherConsoleSong wii_u = hokuto::edit::ForOtherConsole(ds, 0, sar::Flavor::kCafe);
    XSFLIB_CHECK(wii_u.file.native->tables->sounds.size() == 1 &&
                 wii_u.file.native->tables->sounds[0].name == "SEQ_A-B");
    const ntdlib::commands::Sequence& made = *wii_u.file.sequences.at(0);
    const auto start = std::ranges::find(made.labels, "SMF_A_B_Start", &ntdlib::commands::Label::name);
    SEQLIB_REQUIRE(start != made.labels.end());
    XSFLIB_CHECK(format::ModelOf(wii_u.file).starts.at(0) == start->item);
    const auto played = TimedNotes(song::ReadSequence(*DsArchiveOf(ds), 0));
    const auto sent = TimedNotes(song::ReadSequence(*ArchiveOf(wii_u.file), 0));
    SEQLIB_REQUIRE(played.size() == 2 && sent.size() == played.size());
    for (std::size_t i = 0; i < played.size(); ++i)
    {
        XSFLIB_CHECK(std::abs(std::get<0>(sent[i]) - std::get<0>(played[i])) <= 10.0);
        XSFLIB_CHECK(std::get<1>(sent[i]) == std::get<1>(played[i]));
    }
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("other_console_ds",
                                    []
                                    {
                                        constexpr std::array kConsoles = {sar::Flavor::kNx, sar::Flavor::kCafe,
                                                                          sar::Flavor::kCtr};
                                        for (const sar::Flavor from : kConsoles)
                                        {
                                            TestToDs(from);
                                            TestDsBanks(from);
                                            TestDsTrim(from);
                                            TestDsLowerRates(from);
                                            TestDsRefusals(from);
                                        }
                                        TestDsSettingsLeft(sar::Flavor::kNx);
                                        TestDsSettingsLeft(sar::Flavor::kCafe);
                                        TestToDsSources();
                                        TestGbaReverbToDs();
                                        TestDsEngines();
                                        TestFromDs();
                                    });
}
