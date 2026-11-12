// SPDX-License-Identifier: MIT

// Tests the item views of the DS (src/app/item_views_ds.cpp): banks, regions and waves. A DS bank contains a wave, a
// square wave, noise, a drum set, a key split and a wave by address. The Bank Editor shows it, and its edits change the
// regions' bytes in place (src/edit/ds_banks.h).

#include "item_views_fixtures.h"

namespace
{

// The DS archive's bank file. Banks 0 and 1 play it, and bank 2 plays the file after it.
constexpr uint32_t kDsBankFile = 3;

// A DS archive for the Bank Editor. Sounds 0, 1 and 2 each play a note, with banks 0, 1 and 2. Slot 0 of banks 0 and 1
// names a wave archive of two waves, and bank 2 names none. Bank 0's program 0 plays wave 0, and program 7 plays the
// same bytes. Program 1 is a square wave at duty 3, and program 2 noise. Program 3 is a drum set from key 36: wave 1 at
// its root key, nothing, and a square wave at duty 5. Program 4 is a key split: keys up to 59 play a square wave at
// duty 1, and the others wave 0. Program 5 is empty, and program 6 plays a wave by its address. Bank 2's program 0
// plays wave 0 with a pan of 200, past the driver's range.
std::vector<uint8_t> DsBankArchive()
{
    namespace files = ntdlib::testing::sdat_files;
    namespace sdat = ntdlib::sdat;
    constexpr uint16_t kNone = 0xFFFF;
    constexpr std::size_t kTable = 0x3C;

    const files::Wave square{.loop = 1, .loop_length = 2, .data = {0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0}};
    const files::Wave ramp{.data = {0x00, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70}};
    files::Bytes bank = files::Bank(
        {{.type = 1, .region = {.wave = 0}},
         {.type = 2, .region = {.wave = 3}},
         {.type = 3, .region = {.wave = 0}},
         {.type = 16, .min_key = 36, .regions = {{.wave = 1, .root_key = 36}, {.type = 0}, {.type = 2, .wave = 5}}},
         {.type = 17, .split_keys = {59, 127}, .regions = {{.type = 2, .wave = 1}, {.wave = 0}}},
         {},
         {.type = 4, .region = {.wave = 0x1234}},
         {}});
    std::copy_n(bank.begin() + kTable, 4, bank.begin() + kTable + 7 * 4);
    const files::Bytes note = {0x3C, 0x7F, 0x18, 0xFF};

    sdat::ArchiveModel model;
    model.sequences = {sdat::SequenceInfo{0, 0, 127, 64, 64, 0, 0, ""}, sdat::SequenceInfo{1, 1, 127, 64, 64, 0, 0, ""},
                       sdat::SequenceInfo{2, 2, 127, 64, 64, 0, 0, ""}};
    model.banks = {sdat::BankInfo{kDsBankFile, {0, kNone, kNone, kNone}, ""},
                   sdat::BankInfo{kDsBankFile, {0, kNone, kNone, kNone}, ""},
                   sdat::BankInfo{kDsBankFile + 1, {kNone, kNone, kNone, kNone}, ""}};
    model.wave_archives = {sdat::WaveArchiveInfo{kDsBankFile + 2, ""}};
    model.players = {sdat::PlayerInfo{1, 0, 0, 0, ""}};
    model.files = {files::Sequence(note),
                   files::Sequence(note),
                   files::Sequence(note),
                   bank,
                   files::Bank({{.type = 1, .region = {.wave = 0, .pan = 200}}}),
                   files::WaveArchive({square, ramp})};

    return sdat::BuildArchive(model);
}

// The region at `place` of the archive that `file` builds.
ntdlib::sdat::Region DsRegionAt(const File& file, const hokuto::edit::DsRegionPlace& place)
{
    namespace sdat = ntdlib::sdat;
    const std::shared_ptr<const hokuto::song::DsArchive> archive =
        hokuto::song::LoadDsArchive(hokuto::format::ExportArchive(file));
    const sdat::Archive& tables = archive->archive;
    const sdat::Bank bank = sdat::Bank::Parse(tables.File(tables.banks.at(place.bank)->file_id));
    const sdat::Instrument& instrument = bank.instruments.at(place.program);
    const bool typed =
        instrument.type == sdat::InstrumentType::kDrumSet || instrument.type == sdat::InstrumentType::kKeySplit;

    return typed ? instrument.regions.at(place.region) : instrument.region;
}

// The Bank Editor shows each program that has an instrument, the drum set's keys with regions and the key split's runs
// of keys. Programs 0 and 7 play the same bytes. The menu of a region's wave lists the waves of slot 0's wave archive.
// Bank 1 plays the same file, and the sequences of banks 0 and 1 play it. Bank 2's slots name no wave archive.
void TestDsBankView()
{
    using hokuto::app::DsProgram;
    using hokuto::app::DsRegionRow;
    using hokuto::edit::DsRegionBytes;
    using hokuto::edit::DsRegionBytesOf;

    const std::shared_ptr<const hokuto::song::DsArchive> archive = hokuto::song::LoadDsArchive(DsBankArchive());
    const hokuto::app::DsBankView view = hokuto::app::MakeDsBankView(*archive, 0);
    XSFLIB_CHECK(view.bank == 0 && view.name == "BANK_0");
    std::vector<uint32_t> programs;
    std::vector<std::string> kinds;
    for (const DsProgram& program : view.programs)
    {
        programs.push_back(program.program);
        kinds.push_back(program.kind);
    }
    XSFLIB_CHECK((programs == std::vector<uint32_t>{0, 1, 2, 3, 4, 6, 7}));
    XSFLIB_CHECK((kinds == std::vector<std::string>{"Wave", "Square", "Noise", "Drum set", "Key split",
                                                    "Wave by address", "Wave"}));

    // Each row's keys, its region and its kind.
    const auto rows = [](const DsProgram& program)
    {
        std::vector<std::string> shown;
        for (const DsRegionRow& row : program.rows)
        {
            shown.push_back(std::to_string(row.first_key) + "-" + std::to_string(row.last_key) + " " +
                            std::to_string(row.region) + " " + row.kind);
        }
        return shown;
    };
    XSFLIB_CHECK((rows(view.programs[0]) == std::vector<std::string>{"0-127 0 Wave"}));
    XSFLIB_CHECK((rows(view.programs[3]) == std::vector<std::string>{"36-36 0 Wave", "38-38 2 Square"}));
    XSFLIB_CHECK((rows(view.programs[4]) == std::vector<std::string>{"0-59 0 Square", "60-127 1 Wave"}));
    XSFLIB_CHECK(view.programs[3].rows[0].values.root_key == 36 && view.programs[3].rows[1].values.wave == 5);
    XSFLIB_CHECK(view.programs[0].rows[0].shared == std::vector<std::string>{"program 7"});
    XSFLIB_CHECK(view.programs[6].rows[0].shared == std::vector<std::string>{"program 0"});
    XSFLIB_CHECK(view.programs[1].rows[0].shared.empty() && view.programs[3].rows[1].shared.empty());

    XSFLIB_CHECK((view.waves == std::vector<hokuto::app::DsWaveChoice>{{0, 0}, {0, 1}}));
    XSFLIB_CHECK((view.wave_names == std::vector<std::string>{"WAVEARC_0, wave 0", "WAVEARC_0, wave 1"}));
    XSFLIB_CHECK(view.sharing_banks == std::vector<std::string>{"BANK_1"});
    XSFLIB_CHECK((view.sounds == std::vector<std::string>{"SEQ_0", "SEQ_1"}));
    const hokuto::app::DsBankView other = hokuto::app::MakeDsBankView(*archive, 2);
    XSFLIB_CHECK(other.waves.empty() && other.sharing_banks.empty() &&
                 other.sounds == std::vector<std::string>{"SEQ_2"});
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::app::MakeDsBankView(*archive, 3); }) ==
                 "The archive has no bank 3.");

    // A region of one instrument starts at the instrument's offset, with its kind in the table. A drum set's regions
    // follow its two keys, and a key split's its 8 keys, each after its kind and a byte of padding.
    const ntdlib::sdat::Bank bank = ntdlib::sdat::Bank::Parse(archive->archive.File(kDsBankFile));
    const uint32_t single = bank.instruments[0].offset;
    XSFLIB_CHECK((DsRegionBytesOf(bank, 0, 0) == DsRegionBytes{single, 0x3C}));
    XSFLIB_CHECK((DsRegionBytesOf(bank, 7, 0) == DsRegionBytes{single, 0x3C + 7 * 4}));
    const uint32_t drums = bank.instruments[3].offset + 2 + 2 * 12;
    XSFLIB_CHECK((DsRegionBytesOf(bank, 3, 2) == DsRegionBytes{drums + 2, drums}));
    const uint32_t split = bank.instruments[4].offset + 8 + 12;
    XSFLIB_CHECK((DsRegionBytesOf(bank, 4, 1) == DsRegionBytes{split + 2, split}));
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { DsRegionBytesOf(bank, 3, 3); }) == "Program 3 has no region 3.");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { DsRegionBytesOf(bank, 0, 1); }) == "Program 0 has no region 1.");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { DsRegionBytesOf(bank, 5, 0); }) == "Program 5 has no region 0.");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { DsRegionBytesOf(bank, 8, 0); }) == "The bank has no program 8.");
}

// After an edit of a program's structure, the Bank Editor selects the row of the new model that plays a key (DsRowOf).
// Program 4 is a key split of keys 0 to 59 and 60 to 127. A split of the second run at key 72 returns the row from 72.
// The editor selects that row. The keys of a deleted run move to the next run, or for the last run to the run before
// it: the row that plays the first key of the deleted row gets them. Program 3 is a drum set of keys 36 to 38, and key
// 37 plays nothing. Keys 37 to 40 keep key 38, and key 39 gets its values. A drum set made at key 60 has the row of key
// 60, and a key split or a single region made of a program has a row from key 0. A key that no row plays, and a program
// without an instrument, return none.
void TestDsRowsAfterEdits()
{
    namespace sdat = ntdlib::sdat;
    using hokuto::app::DsRowOf;

    const std::shared_ptr<const hokuto::song::DsArchive> archive = hokuto::song::LoadDsArchive(DsBankArchive());
    const sdat::Bank bank = sdat::Bank::Parse(archive->archive.File(kDsBankFile));

    const sdat::Bank split = hokuto::edit::WithDsSplit(bank, 4, 1, 72);
    const sdat::Bank next_takes = hokuto::edit::WithoutDsRun(split, 4, 1);
    const sdat::Bank before_takes = hokuto::edit::WithoutDsRun(split, 4, 2);
    const sdat::Bank keys = hokuto::edit::WithDsDrumKeys(bank, 3, 37, 40);
    const sdat::Bank drums = hokuto::edit::WithDsDrumSet(bank, 0, 60);
    const sdat::Bank key_split = hokuto::edit::WithDsKeySplit(bank, 0);
    const sdat::Bank one = hokuto::edit::WithDsOneRegion(bank, 4, 1);

    XSFLIB_CHECK(DsRowOf(bank, 4, 30) == 0 && DsRowOf(bank, 4, 60) == 60 && DsRowOf(bank, 3, 38) == 38);
    XSFLIB_CHECK(DsRowOf(split, 4, 72) == 72 && DsRowOf(split, 4, 100) == 72 && DsRowOf(split, 4, 71) == 60);
    XSFLIB_CHECK(DsRowOf(next_takes, 4, 60) == 60 && DsRowOf(next_takes, 4, 100) == 60);
    XSFLIB_CHECK(DsRowOf(before_takes, 4, 72) == 60);
    XSFLIB_CHECK(!DsRowOf(keys, 3, 37) && DsRowOf(keys, 3, 38) == 38 && DsRowOf(keys, 3, 39) == 39);
    XSFLIB_CHECK(DsRowOf(drums, 0, 60) == 60 && DsRowOf(key_split, 0, 0) == 0 && DsRowOf(one, 4, 0) == 0);
    XSFLIB_CHECK(!DsRowOf(bank, 3, 37) && !DsRowOf(bank, 5, 60) && !DsRowOf(bank, 99, 0));
}

// The archive of the tests of region edits, its file, its bank and the offset of the bank file in the archive.
struct DsRegionFixture
{
    std::vector<uint8_t> bytes;
    File file;
    std::shared_ptr<const hokuto::song::DsArchive> archive;
    ntdlib::sdat::Bank bank;
    std::size_t start;
};

// The fixture of the tests of region edits.
DsRegionFixture MakeDsRegionFixture()
{
    namespace sdat = ntdlib::sdat;

    const std::vector<uint8_t> bytes = DsBankArchive();
    const File file = hokuto::format::ImportDsArchive(bytes);
    const std::shared_ptr<const hokuto::song::DsArchive> archive = hokuto::song::LoadDsArchive(bytes);
    const sdat::Bank bank = sdat::Bank::Parse(archive->archive.File(kDsBankFile));
    const std::size_t start = archive->archive.files[kDsBankFile].offset;

    return {bytes, file, archive, bank, start};
}

// The offsets of the bytes of `edited`, exported, that differ from `bytes`.
std::vector<std::size_t> Changed(const std::vector<uint8_t>& bytes, const File& edited)
{
    const std::vector<uint8_t> exported = hokuto::format::ExportArchive(edited);
    XSFLIB_CHECK(exported.size() == bytes.size());
    std::vector<std::size_t> offsets;
    for (std::size_t i = 0; i < exported.size() && i < bytes.size(); ++i)
    {
        if (exported[i] != bytes[i])
        {
            offsets.push_back(i);
        }
    }
    return offsets;
}

// An edit of a region changes its bytes in place, and nothing else of the archive. Bank 1 plays the edit too.
void TestDsSquareEdit()
{
    namespace sdat = ntdlib::sdat;
    using enum sdat::InstrumentType;
    using hokuto::edit::DsRegionPlace;
    using hokuto::edit::WithDsRegion;

    const DsRegionFixture fixture = MakeDsRegionFixture();
    const std::vector<uint8_t>& bytes = fixture.bytes;
    const File& file = fixture.file;
    const std::shared_ptr<const hokuto::song::DsArchive>& archive = fixture.archive;
    const sdat::Bank& bank = fixture.bank;
    const std::size_t start = fixture.start;

    // The square wave gets another duty, a sustain and a pan.
    const DsRegionPlace square{0, 1, 0};
    sdat::Region region = DsRegionAt(file, square);
    XSFLIB_CHECK(region.type == kPsg && region.wave == 3);
    region.wave = 6;
    region.sustain = 100;
    region.pan = 20;
    const File edited = WithDsRegion(file, *archive, square, region);
    XSFLIB_CHECK(DsRegionAt(edited, square) == region);
    const std::size_t at = start + bank.instruments[1].offset;
    XSFLIB_CHECK((Changed(bytes, edited) == std::vector<std::size_t>{at, at + 7, at + 9}));
    const std::shared_ptr<const hokuto::song::DsArchive> after =
        hokuto::song::LoadDsArchive(hokuto::format::ExportArchive(edited));
    XSFLIB_CHECK(hokuto::app::MakeDsBankView(*after, 1).programs[1].rows[0].values == region);
}

// A kind changes among a wave, a square wave and noise: in the table's record of an instrument of one region, or in the
// byte of kind that leads a drum set's region.
void TestDsKindEdits()
{
    namespace sdat = ntdlib::sdat;
    using enum sdat::InstrumentType;
    using hokuto::edit::DsRegionPlace;
    using hokuto::edit::WithDsRegion;

    const DsRegionFixture fixture = MakeDsRegionFixture();
    const std::vector<uint8_t>& bytes = fixture.bytes;
    const File& file = fixture.file;
    const std::shared_ptr<const hokuto::song::DsArchive>& archive = fixture.archive;
    const sdat::Bank& bank = fixture.bank;
    const std::size_t start = fixture.start;
    sdat::Region region;

    // Noise becomes a square wave: its record in the table stores the new kind.
    const DsRegionPlace noise{0, 2, 0};
    region = DsRegionAt(file, noise);
    region.type = kPsg;
    region.wave = 2;
    const File squared = WithDsRegion(file, *archive, noise, region);
    XSFLIB_CHECK(DsRegionAt(squared, noise) == region);
    XSFLIB_CHECK((Changed(bytes, squared) ==
                  std::vector<std::size_t>{start + 0x3C + 2 * 4, start + bank.instruments[2].offset}));

    // The drum set's key 38 becomes wave 1 at its key: the byte of kind that leads its values stores the new kind.
    const DsRegionPlace drum{0, 3, 2};
    region = DsRegionAt(file, drum);
    XSFLIB_CHECK(region.type == kPsg && region.wave == 5);
    region.type = kPcm;
    region.wave = 1;
    region.root_key = 38;
    const File drummed = WithDsRegion(file, *archive, drum, region);
    XSFLIB_CHECK(DsRegionAt(drummed, drum) == region);
    const std::size_t kind = start + bank.instruments[3].offset + 2 + 2 * 12;
    XSFLIB_CHECK((Changed(bytes, drummed) == std::vector<std::size_t>{kind, kind + 2, kind + 6}));
}

// An edit of program 7 reaches program 0. A value that the edit leaves as the archive stores it isn't checked: bank 2's
// pan stays at 200, and its wave stays though its slots name no wave archive.
void TestDsRegionRefusals()
{
    namespace sdat = ntdlib::sdat;
    using enum sdat::InstrumentType;
    using hokuto::edit::DsRegionPlace;
    using hokuto::edit::WithDsRegion;

    const DsRegionFixture fixture = MakeDsRegionFixture();
    const File& file = fixture.file;
    const std::shared_ptr<const hokuto::song::DsArchive>& archive = fixture.archive;
    const DsRegionPlace square{0, 1, 0};
    const DsRegionPlace noise{0, 2, 0};
    sdat::Region region;

    // The key split's upper keys play wave 1. Program 7's release reaches program 0. Bank 2 gets another attack.
    const DsRegionPlace upper{0, 4, 1};
    region = DsRegionAt(file, upper);
    region.wave = 1;
    region.root_key = 72;
    XSFLIB_CHECK(DsRegionAt(WithDsRegion(file, *archive, upper, region), upper) == region);
    const DsRegionPlace wave{0, 0, 0};
    const DsRegionPlace twin{0, 7, 0};
    region = DsRegionAt(file, twin);
    region.release = 50;
    XSFLIB_CHECK(DsRegionAt(WithDsRegion(file, *archive, twin, region), wave) == region);
    const DsRegionPlace loud{2, 0, 0};
    region = DsRegionAt(file, loud);
    XSFLIB_CHECK(region.pan == 200);
    region.attack = 100;
    XSFLIB_CHECK(DsRegionAt(WithDsRegion(file, *archive, loud, region), loud) == region);

    // The refusals, for each value that changes.
    const auto error = [&](const File& target, const DsRegionPlace& place, const sdat::Region& wrong)
    {
        return ErrorOf<std::invalid_argument>([&] { WithDsRegion(target, *archive, place, wrong); });
    };
    const sdat::Region plain = DsRegionAt(file, wave);
    sdat::Region wrong = plain;
    wrong.type = kPsg;
    XSFLIB_CHECK(error(file, wave, wrong) ==
                 "Program 7 shares this region but stores its type in another table entry. "
                 "Can't change the region type here.");
    wrong = plain;
    wrong.wave = 2;
    XSFLIB_CHECK(error(file, wave, wrong) == "WAVEARC_0 has no wave 2. It has 2 waves.");
    wrong = plain;
    wrong.wave_archive = 1;
    XSFLIB_CHECK(error(file, wave, wrong) == "Wave archive slot 1 of the bank is empty.");
    wrong.wave_archive = 4;
    XSFLIB_CHECK(error(file, wave, wrong) == "Wave archive slot 4 of the bank is empty.");
    wrong = plain;
    wrong.attack = 128;
    XSFLIB_CHECK(error(file, wave, wrong) == "Attack must be from 0 to 127.");
    wrong = plain;
    wrong.root_key = 200;
    XSFLIB_CHECK(error(file, wave, wrong) == "Root key must be from 0 to 127.");
    wrong = DsRegionAt(file, square);
    wrong.wave = 8;
    XSFLIB_CHECK(error(file, square, wrong) == "Square wave duty must be from 0 to 7.");
    const DsRegionPlace by_address{0, 6, 0};
    wrong = DsRegionAt(file, by_address);
    wrong.type = kPcm;
    XSFLIB_CHECK(error(file, by_address, wrong) ==
                 "A region can switch only between the wave, square wave and noise types.");
    wrong = DsRegionAt(file, noise);
    wrong.type = kDummy;
    XSFLIB_CHECK(error(file, noise, wrong) ==
                 "A region can switch only between the wave, square wave and noise types.");
    wrong = DsRegionAt(file, loud);
    wrong.wave = 1;
    XSFLIB_CHECK(error(file, loud, wrong) == "Wave archive slot 0 of the bank is empty.");
    XSFLIB_CHECK(error(file, {0, 5, 0}, plain) == "Program 5 has no region 0.");
    XSFLIB_CHECK(error(file, {0, 9, 0}, plain) == "The bank has no program 9.");
    XSFLIB_CHECK(error(file, {3, 0, 0}, plain) == "The archive has no bank 3.");
    XSFLIB_CHECK(error(hokuto::format::ImportGbaRom(MakeGbaCart().bytes), wave, plain) ==
                 "The file contains no DS sound archive.");
}

void TestDsWaveEdits()
{
    using hokuto::edit::DsWaveValues;
    using hokuto::edit::WithDsWave;

    const std::vector<uint8_t> bytes = DsBankArchive();
    const File file = hokuto::format::ImportDsArchive(bytes);
    const std::shared_ptr<const hokuto::song::DsArchive> archive = hokuto::song::LoadDsArchive(bytes);
    const hokuto::app::WaveView view = hokuto::app::MakeDsWaveView(*archive, 0, 0);
    XSFLIB_CHECK(view.ds && view.held == 8 && view.samples.size() == 1);
    XSFLIB_CHECK(
        (view.samples[0] == std::vector<int16_t>{0x4000, 0x4000, -0x4000, -0x4000, 0x4000, 0x4000, -0x4000, -0x4000}));
    XSFLIB_CHECK(view.values.rate == 32768 && view.values.loop && view.values.loop_start == 0 &&
                 view.values.loop_end == 8);
    XSFLIB_CHECK((view.encodings == std::vector<ntdlib::sar::WaveEncoding>{ntdlib::sar::WaveEncoding::kImaAdpcm,
                                                                           ntdlib::sar::WaveEncoding::kPcm16,
                                                                           ntdlib::sar::WaveEncoding::kPcm8}));
    XSFLIB_CHECK(view.values.encoding == ntdlib::sar::WaveEncoding::kPcm8);
    XSFLIB_CHECK(hokuto::app::LoopStepOf(view, view.values.encoding) == 4);
    XSFLIB_CHECK(view.reach == "Edits affect BANK_0 (programs 0, 4, 7) and BANK_1 (programs 0, 4, 7).");

    // The wave's parameters start at its offset in the wave archive file. The loop starts at word 1, and runs 1 word.
    const std::size_t at = archive->archive.files[kDsBankFile + 2].offset +
                           ntdlib::sdat::WaveArchive::Parse(archive->archive.File(kDsBankFile + 2)).offsets[0];
    const File edited = WithDsWave(file, *archive, 0, 0, DsWaveValues{16000, true, 4});
    const std::vector<uint8_t> exported = hokuto::format::ExportArchive(edited);
    XSFLIB_CHECK(exported.size() == bytes.size());
    std::vector<std::size_t> offsets;
    for (std::size_t i = 0; i < bytes.size(); ++i)
    {
        if (exported[i] != bytes[i])
        {
            offsets.push_back(i - at);
        }
    }
    XSFLIB_CHECK((offsets == std::vector<std::size_t>{2, 3, 4, 5, 6, 8}));
    XSFLIB_CHECK(exported[at + 2] == (16000 & 0xFF) && exported[at + 3] == 16000 >> 8);
    XSFLIB_CHECK(exported[at + 4] == (16756991 / 16000 & 0xFF) && exported[at + 5] == 16756991 / 16000 >> 8);
    XSFLIB_CHECK(exported[at + 6] == 1 && exported[at + 8] == 1);
    const std::shared_ptr<const hokuto::song::DsArchive> after = hokuto::song::LoadDsArchive(exported);
    XSFLIB_CHECK((hokuto::edit::DsWaveValuesOf(*after, 0, 0) == DsWaveValues{16000, true, 4}));
    XSFLIB_CHECK(hokuto::app::MakeDsWaveView(*after, 0, 0).values.loop_end == 8);

    // Without a loop, the loop start is the first word, and the same rate keeps the timer.
    const std::vector<uint8_t> off = hokuto::format::ExportArchive(WithDsWave(file, *archive, 0, 0, {32768, false, 0}));
    XSFLIB_CHECK(off[at + 1] == 0 && off[at + 6] == 0 && off[at + 8] == 2);
    XSFLIB_CHECK(off[at + 4] == bytes[at + 4] && off[at + 5] == bytes[at + 5]);

    // The refusals.
    const auto error = [&](const File& target, uint32_t wave_archive, uint32_t wave, const DsWaveValues& values)
    {
        return ErrorOf<std::invalid_argument>([&] { WithDsWave(target, *archive, wave_archive, wave, values); });
    };
    XSFLIB_CHECK(error(file, 0, 0, {255, true, 0}) == "The sample rate of a DS wave must be from 256 to 192000 Hz.");
    XSFLIB_CHECK(error(file, 0, 0, {192001, true, 0}) == "The sample rate of a DS wave must be from 256 to 192000 Hz.");
    XSFLIB_CHECK(error(file, 0, 0, {32768, true, 2}) == "The loop start of this wave must be a multiple of 4 samples.");
    XSFLIB_CHECK(error(file, 0, 0, {32768, true, 8}) == "The loop must start before the end of the wave.");
    XSFLIB_CHECK(error(file, 0, 2, {32768, true, 0}) == "WAVEARC_0 has no wave 2.");
    XSFLIB_CHECK(error(file, 1, 0, {32768, true, 0}) == "The archive has no WAVEARC_1.");
    XSFLIB_CHECK(error(hokuto::format::ImportGbaRom(MakeGbaCart().bytes), 0, 0, {32768, true, 0}) ==
                 "The file contains no DS sound archive.");
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("ds_item_views",
                                    []
                                    {
                                        TestDsBankView();
                                        TestDsRowsAfterEdits();
                                        TestDsSquareEdit();
                                        TestDsKindEdits();
                                        TestDsRegionRefusals();
                                        TestDsWaveEdits();
                                    });
}
