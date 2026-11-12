// SPDX-License-Identifier: MIT

// Tests the edits that the Bank Editor makes to the splits and waves of DSE (edit/dse_banks.h) and their views
// (app/item_views.h) on a set that ntdlib's test files build.

#include "edit/dse_banks.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "ntdlib/dse/formats.h"
#include "ntdlib/testing/dse_song_files.h"
#include "ntdlib/testing/nds_files.h"
#include "seqlib/testing/check.h"
#include "xsflib/testing/check.h"

#include "app/item_views.h"
#include "format/archive.h"
#include "format/file.h"
#include "song/dse.h"

namespace
{

namespace dse_files = ntdlib::testing::dse_song_files;
namespace nds_files = ntdlib::testing::nds_files;
namespace edit = hokuto::edit;
using dse_files::Bytes;
using dse_files::Join;
using hokuto::format::File;
using hokuto::song::DseSet;
using xsflib::testing::Check;

// Three songs of one note on program 0 of their banks. The banks of bgm0001 and bgm0002 contain the same bytes. The
// bank of bgm0003 has a volume of 0x50 in program 0. Each uses the waves of the main bank.
File TestFile()
{
    const Bytes conductor = {0, 0, 0, 0, 0xA4, 0x78, 0x98};
    const Bytes track = Join({{1, 1, 0, 0},
                              {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x7F},
                              dse_files::Note(0x60, 0, 0, 0x18, 1),
                              {0x92, 0x18, 0x98}});
    ntdlib::dse::WaveBank other = ntdlib::dse::WaveBank::Parse(dse_files::BankFile());
    other.instruments->programs[0]->splits[0][edit::kDseVolume] = 0x50;
    nds_files::Image image;
    image.files = {{"SOUND/BGM/bgm.swd", dse_files::MainBankFile()},
                   {"SOUND/BGM/bgm0001.smd", dse_files::SongFile({conductor, track})},
                   {"SOUND/BGM/bgm0001.swd", dse_files::BankFile()},
                   {"SOUND/BGM/bgm0002.smd", dse_files::SongFile({conductor, track})},
                   {"SOUND/BGM/bgm0002.swd", dse_files::BankFile()},
                   {"SOUND/BGM/bgm0003.smd", dse_files::SongFile({conductor, track})},
                   {"SOUND/BGM/bgm0003.swd", other.Write()}};

    return hokuto::format::ImportDseImage(nds_files::BuildImage(image).first);
}

std::shared_ptr<const DseSet> SetOf(const File& file)
{
    return hokuto::song::LoadDseSet(hokuto::format::ExportArchive(file), ntdlib::dse::Build::kLater);
}

// The banks, in the order of the files.
constexpr uint32_t kMainBank = 0;
constexpr uint32_t kFirstBank = 1;
constexpr uint32_t kSecondBank = 2;
constexpr uint32_t kOtherBank = 3;

edit::DseSplit SplitOf(const DseSet& set, uint32_t bank, uint32_t program, std::size_t split)
{
    return set.banks.at(bank).bank->instruments->programs.at(program)->splits.at(split);
}

std::string Refusal(const File& file, const DseSet& set, const edit::DseSplitPlace& place, const edit::DseSplit& split)
{
    return xsflib::testing::ErrorOf<std::invalid_argument>([&] { (void)edit::WithDseSplit(file, set, place, split); });
}

std::string WaveRefusal(const File& file, const DseSet& set, uint32_t bank, uint32_t wave,
                        const edit::DseWaveValues& values)
{
    return xsflib::testing::ErrorOf<std::invalid_argument>([&]
                                                           { (void)edit::WithDseWave(file, set, bank, wave, values); });
}

// The view of a DSE bank names the bank and lists its programs with a row for each split. Each row names the wave and
// contains the bytes of its split.
void TestBankView()
{
    const File file = TestFile();
    const std::shared_ptr<const DseSet> set = SetOf(file);
    SEQLIB_REQUIRE(set->banks.size() == 4);
    const hokuto::app::DseBankView view = hokuto::app::MakeDseBankView(*set, kFirstBank);
    Check(view.bank == kFirstBank, "the view names its bank");
    SEQLIB_REQUIRE(view.programs.size() == 4);
    Check(view.programs[0].program == 0 && view.programs[0].rows.size() == 1, "program 0 has one split");
    Check(view.programs[1].program == 1 && view.programs[1].rows.size() == 2, "program 1 has two splits");
    SEQLIB_REQUIRE(view.programs[1].rows.size() >= 2);
    Check(view.programs[1].rows[1].split == 1 && view.programs[1].rows[1].wave == "Wave 2", "a row names its wave");
    Check(view.programs[1].rows[1].values == SplitOf(*set, kFirstBank, 1, 1), "a row holds its split's bytes");
    const std::vector<uint16_t> waves = {0, 1, 2, 3, 4, 5};
    Check(view.waves == waves && view.wave_names.size() == 6 && view.wave_names[2] == "Wave 2", "the bank's waves");
    const std::vector<std::string> sounds = {"bgm0001", "bgm0002"};
    Check(view.sounds == sounds, "the sounds that play the bank's bytes");
    Check(hokuto::app::MakeDseBankView(*set, kOtherBank).sounds == std::vector<std::string>{"bgm0003"},
          "a bank of other bytes has its sounds");
    const bool refused =
        !xsflib::testing::ErrorOf<std::invalid_argument>([&] { (void)hokuto::app::MakeDseBankView(*set, 4); }).empty();
    Check(refused, "the view of a bank missing from the set is refused");
}

// The root key, the tuning and the wave of a DSE split read and write the split's bytes: a coarse tuning as a signed
// offset from key 60, and the tuning and the wave in little-endian order.
void TestSplitValues()
{
    edit::DseSplit split{};
    split[edit::kDseCoarseTuning] = 0xF4;
    Check(edit::DseRootKey(split) == 72, "a coarse tuning of -12 is a root key of 72");
    edit::SetDseRootKey(split, 48);
    Check(split[edit::kDseCoarseTuning] == 12, "a root key of 48 is a coarse tuning of 12");
    edit::SetDseTuning(split, -1742);
    Check(split[edit::kDseTuning] == 0x32 && split[edit::kDseTuning + 1] == 0xF9, "the tuning is little-endian");
    Check(edit::DseTuning(split) == -1742, "the tuning is signed");
    edit::SetDseSplitWave(split, 0x123);
    Check(split[edit::kDseWave] == 0x23 && split[edit::kDseWave + 1] == 0x01, "the wave is little-endian");
    Check(edit::DseSplitWave(split) == 0x123, "the wave reads back");
}

// The file of TestFile, the set read from it, the archive exported from it, and the place and the bytes of split 1
// of program 1 in the first bank.
struct SplitEditFixture
{
    File file;
    std::shared_ptr<const DseSet> set;
    std::vector<uint8_t> before;
    edit::DseSplitPlace place;
    edit::DseSplit old;
};

// The fixture of the tests of edits of DSE splits.
SplitEditFixture MakeSplitEditFixture()
{
    const File file = TestFile();
    const std::shared_ptr<const DseSet> set = SetOf(file);
    const std::vector<uint8_t> before = hokuto::format::ExportArchive(file);
    const edit::DseSplitPlace place{kFirstBank, 1, 1};
    const edit::DseSplit old = SplitOf(*set, kFirstBank, 1, 1);

    return {file, set, before, place, old};
}

// An edit of a DSE split changes the bytes of both files of the bank's bytes and no others. A byte that the editor
// doesn't show keeps its value.
void TestSplitEditChanges()
{
    const SplitEditFixture fixture = MakeSplitEditFixture();
    const File& file = fixture.file;
    const std::shared_ptr<const DseSet>& set = fixture.set;
    const std::vector<uint8_t>& before = fixture.before;
    const edit::DseSplitPlace& place = fixture.place;
    const edit::DseSplit& old = fixture.old;

    edit::DseSplit changed = old;
    changed[edit::kDseLowKey] = 50;
    changed[edit::kDseHighKey] = 90;
    changed[edit::kDseLowVelocity] = 10;
    changed[edit::kDseHighVelocity] = 100;
    changed[edit::kDseVolume] = 0x33;
    changed[edit::kDsePan] = 0x11;
    changed[edit::kDseKeyGroup] = 2;
    changed[edit::kDseBendRange] = 7;
    changed[edit::kDseMultiplier] = 3;
    changed[edit::kDseAttackLevel] = 1;
    changed[edit::kDseAttack] = 2;
    changed[edit::kDseDecay] = 3;
    changed[edit::kDseSustain] = 4;
    changed[edit::kDseHold] = 5;
    changed[edit::kDseDecay2] = 6;
    changed[edit::kDseRelease] = 7;
    edit::SetDseTuning(changed, 300);
    edit::SetDseRootKey(changed, 70);
    edit::SetDseSplitWave(changed, 5);

    // A byte that the editor doesn't show keeps its value.
    changed[0x0C] = 0x77;
    const File edited = edit::WithDseSplit(file, *set, place, changed);
    const std::vector<uint8_t> after = hokuto::format::ExportArchive(edited);
    Check(after.size() == before.size(), "the set keeps its size");
    const std::shared_ptr<const DseSet> read = SetOf(edited);
    SEQLIB_REQUIRE(read->banks.size() == 4);
    const edit::DseSplit held = SplitOf(*read, kFirstBank, 1, 1);
    Check(held[edit::kDseLowKey] == 50 && held[edit::kDseHighKey] == 90, "the keys change");
    Check(held[edit::kDseLowKey + 2] == 50 && held[edit::kDseHighKey + 2] == 90, "the second copy of the keys changes");
    Check(held[edit::kDseLowVelocity] == 10 && held[edit::kDseHighVelocity] == 100, "the velocities change");
    Check(held[edit::kDseLowVelocity + 2] == 10 && held[edit::kDseHighVelocity + 2] == 100,
          "the second copy of the velocities changes");
    Check(held[edit::kDseVolume] == 0x33 && held[edit::kDsePan] == 0x11 && held[edit::kDseKeyGroup] == 2 &&
              held[edit::kDseBendRange] == 7,
          "the volume, the pan, the key group and the bend range change");
    Check(held[edit::kDseMultiplier] == 3 && held[edit::kDseAttackLevel] == 1 && held[edit::kDseAttack] == 2 &&
              held[edit::kDseDecay] == 3 && held[edit::kDseSustain] == 4 && held[edit::kDseHold] == 5 &&
              held[edit::kDseDecay2] == 6 && held[edit::kDseRelease] == 7,
          "the envelope changes");
    Check(edit::DseTuning(held) == 300 && edit::DseSplitWave(held) == 5, "the tuning and the wave change");
    Check(edit::DseRootKey(held) == 70, "the root key sets the coarse tuning");
    Check(held[edit::kDseRootKey] == old[edit::kDseRootKey] + edit::DseRootKey(held) - edit::DseRootKey(old),
          "the byte that the tools write for the root key moves with it");
    Check(held[0x0C] == old[0x0C], "a byte that the editor doesn't show stays");

    // The two files of one bank's bytes both change, and no other bytes do.
    Check(read->banks[kSecondBank].bank == read->banks[kFirstBank].bank, "the files of one bank's bytes stay one bank");
    Check(SplitOf(*read, kOtherBank, 1, 1) == old, "a bank of other bytes stays");
    std::size_t differing = 0;
    for (std::size_t at = 0; at < before.size(); ++at)
    {
        differing += before[at] != after[at] ? 1 : 0;
    }
    std::size_t in_split = 0;
    for (std::size_t at = 0; at < old.size(); ++at)
    {
        in_split += old[at] != held[at] ? 1 : 0;
    }
    Check(differing == 2 * in_split, "only the split's bytes differ, in both files");
}

// An edit that changes nothing returns the same file, and a new value past the limits is refused.
void TestSplitEditUnchangedAndRefused()
{
    const SplitEditFixture fixture = MakeSplitEditFixture();
    const File& file = fixture.file;
    const std::shared_ptr<const DseSet>& set = fixture.set;
    const std::vector<uint8_t>& before = fixture.before;
    const edit::DseSplitPlace& place = fixture.place;
    const edit::DseSplit& old = fixture.old;

    // An edit that changes nothing returns the same file.
    const File same = edit::WithDseSplit(file, *set, place, old);
    Check(hokuto::format::ExportArchive(same) == before, "an edit without a change keeps every byte");

    // A value past the limits in the bank stays, and a new one past them is refused.
    const auto refused = [&](std::size_t at, int value, const std::string& message)
    {
        edit::DseSplit wrong = old;
        wrong[at] = static_cast<uint8_t>(value);
        Check(Refusal(file, *set, place, wrong) == message, message);
    };
    refused(edit::kDseVolume, 128, "Volume must be from 0 to 127.");
    refused(edit::kDsePan, 200, "Pan must be from 0 to 127.");
    refused(edit::kDseRelease, 255, "Release must be from 0 to 127.");
    refused(edit::kDseLowKey, 128, "Low key must be from 0 to 127.");
    refused(edit::kDseLowVelocity, 128, "Velocities must be from 0 to 127.");
    edit::DseSplit crossed = old;
    crossed[edit::kDseLowKey] = 100;
    crossed[edit::kDseHighKey] = 90;
    Check(Refusal(file, *set, place, crossed) == "The low key of a split can't be above its high key.", "crossed keys");
    crossed = old;
    crossed[edit::kDseLowVelocity] = 100;
    crossed[edit::kDseHighVelocity] = 90;
    Check(Refusal(file, *set, place, crossed) == "The low velocity of a split can't be above its high velocity.",
          "crossed velocities");
    edit::DseSplit missing = old;
    edit::SetDseSplitWave(missing, 6);
    Check(Refusal(file, *set, place, missing).find("has no wave 6") != std::string::npos,
          "a wave missing from the bank");
    edit::DseSplit high_root = old;
    edit::SetDseRootKey(high_root, 128);
    Check(Refusal(file, *set, place, high_root) == "Root key must be from 0 to 127.", "a root key past 127");
    Check(Refusal(file, *set, {kFirstBank, 5, 0}, old).find("has no program 5") != std::string::npos, "a program");
    Check(Refusal(file, *set, {kFirstBank, 0, 1}, old) == "Program 0 has no split 1.",
          "a split missing from the program");
    Check(Refusal(file, *set, {4, 0, 0}, old) == "The image has no bank 4.", "a bank missing from the set");
}

// The file of TestFile, the set read from it, the archive exported from it and the values of waves 0, 2 and 1 of the
// main bank.
struct WaveEditFixture
{
    File file;
    std::shared_ptr<const DseSet> set;
    std::vector<uint8_t> before;
    edit::DseWaveValues zero;
    edit::DseWaveValues two;
    edit::DseWaveValues one;
};

// The fixture of the tests of edits of DSE waves.
WaveEditFixture MakeWaveEditFixture()
{
    const File file = TestFile();
    const std::shared_ptr<const DseSet> set = SetOf(file);
    const std::vector<uint8_t> before = hokuto::format::ExportArchive(file);

    // Wave 0 contains PCM16 at 32,000 Hz, with a loop from word 4 of 64. Wave 2 contains IMA-ADPCM at 22,050 Hz, with a
    // loop from word 9 of 32: sample 64 of 248. Wave 1 contains PCM8 and plays once.
    const edit::DseWaveValues zero = edit::DseWaveValuesOf(*set, kMainBank, 0);
    const edit::DseWaveValues two = edit::DseWaveValuesOf(*set, kMainBank, 2);
    const edit::DseWaveValues one = edit::DseWaveValuesOf(*set, kMainBank, 1);

    return {file, set, before, zero, two, one};
}

// An edit of a DSE wave's rate moves the tuning of each split that plays it, in each bank.
void TestWaveEditRate()
{
    const WaveEditFixture fixture = MakeWaveEditFixture();
    const File& file = fixture.file;
    const std::shared_ptr<const DseSet>& set = fixture.set;
    const std::vector<uint8_t>& before = fixture.before;
    const edit::DseWaveValues& zero = fixture.zero;
    const edit::DseWaveValues& two = fixture.two;
    const edit::DseWaveValues& one = fixture.one;

    Check(zero.rate == 32000 && zero.loop && zero.loop_start == 8, "wave 0's values");
    Check(two.rate == 22050 && two.loop && two.loop_start == 64, "wave 2's values");
    Check(one.rate == 16000 && !one.loop && one.loop_start == 0, "wave 1's values");

    // A doubled rate moves the tuning of each split that plays the wave by 12 semitones, in each bank.
    const edit::DseSplit first = SplitOf(*set, kFirstBank, 0, 0);
    SEQLIB_REQUIRE(edit::DseSplitWave(first) == 0);
    File edited = edit::WithDseWave(file, *set, kMainBank, 0, {64000, zero.loop, zero.loop_start});
    std::shared_ptr<const DseSet> read = SetOf(edited);
    for (const uint32_t bank : {kMainBank, kFirstBank, kOtherBank})
    {
        Check(read->banks[bank].bank->waves[0]->SampleRate() == 64000, "each table takes the new rate");
    }
    Check(edit::DseTuning(SplitOf(*read, kFirstBank, 0, 0)) == edit::DseTuning(first) + 3072,
          "a doubled rate moves the tuning by 12 semitones");
    Check(edit::DseTuning(SplitOf(*read, kOtherBank, 0, 0)) == edit::DseTuning(first) + 3072,
          "each bank's split moves");
    Check(SplitOf(*read, kFirstBank, 1, 1) == SplitOf(*set, kFirstBank, 1, 1), "a split of another wave stays");
    Check(read->banks[kFirstBank].bank->waves[1] == set->banks[kFirstBank].bank->waves[1], "another wave stays");
    Check(hokuto::format::ExportArchive(edited).size() == before.size(), "the set keeps its size");
}

// A loop's start moves by words and its end stays. A wave that plays once can get a loop.
void TestWaveEditLoop()
{
    const WaveEditFixture fixture = MakeWaveEditFixture();
    const File& file = fixture.file;
    const std::shared_ptr<const DseSet>& set = fixture.set;
    const edit::DseWaveValues& zero = fixture.zero;
    const edit::DseWaveValues& two = fixture.two;
    const edit::DseWaveValues& one = fixture.one;
    File edited;
    std::shared_ptr<const DseSet> read;

    // A loop's start moves by words, and its end stays.
    edited = edit::WithDseWave(file, *set, kMainBank, 2, {two.rate, true, 72});
    read = SetOf(edited);
    for (const uint32_t bank : {kMainBank, kFirstBank})
    {
        const ntdlib::dse::WaveInfo& held = *read->banks[bank].bank->waves[2];
        Check(held.Loops() && held.LoopStart() == 10 && held.LoopLength() == 22, "the loop's start moves");
    }
    Check(
        hokuto::song::DseWavePcm(*read, kMainBank, 2).channels == hokuto::song::DseWavePcm(*set, kMainBank, 2).channels,
        "the samples stay");

    // Without a loop, the loop start is the first word of samples.
    edited = edit::WithDseWave(file, *set, kMainBank, 2, {two.rate, false, 0});
    read = SetOf(edited);
    const ntdlib::dse::WaveInfo& off = *read->banks[kMainBank].bank->waves[2];
    Check(!off.Loops() && off.LoopStart() == 1 && off.LoopLength() == 31, "an IMA-ADPCM wave without a loop");
    edited = edit::WithDseWave(file, *set, kMainBank, 0, {zero.rate, false, 0});
    read = SetOf(edited);
    const ntdlib::dse::WaveInfo& pcm_off = *read->banks[kMainBank].bank->waves[0];
    Check(!pcm_off.Loops() && pcm_off.LoopStart() == 0 && pcm_off.LoopLength() == 64, "a PCM wave without a loop");

    // A wave that plays once gets a loop.
    edited = edit::WithDseWave(file, *set, kMainBank, 1, {one.rate, true, 8});
    read = SetOf(edited);
    const ntdlib::dse::WaveInfo& on = *read->banks[kMainBank].bank->waves[1];
    Check(on.Loops() && on.LoopStart() == 2 && on.LoopLength() == 30, "a loop in a wave that played once");
}

// The same values keep every byte. A value that the wave can't take is refused.
void TestWaveEditUnchangedAndRefused()
{
    const WaveEditFixture fixture = MakeWaveEditFixture();
    const File& file = fixture.file;
    const std::shared_ptr<const DseSet>& set = fixture.set;
    const std::vector<uint8_t>& before = fixture.before;
    const edit::DseWaveValues& zero = fixture.zero;

    // The same values keep every byte.
    Check(hokuto::format::ExportArchive(edit::WithDseWave(file, *set, kMainBank, 0, zero)) == before,
          "an edit without a change keeps every byte");

    // What the wave can't take.
    Check(WaveRefusal(file, *set, kMainBank, 0, {255, zero.loop, zero.loop_start}) ==
              "The sample rate of a DSE wave must be from 256 to 192000 Hz.",
          "a rate below the limit");
    Check(WaveRefusal(file, *set, kMainBank, 0, {192001, zero.loop, zero.loop_start}) ==
              "The sample rate of a DSE wave must be from 256 to 192000 Hz.",
          "a rate above the limit");
    Check(WaveRefusal(file, *set, kMainBank, 0, {zero.rate, true, 9}) ==
              "The loop start of this wave must be a multiple of 2 samples.",
          "a loop off a word");
    Check(WaveRefusal(file, *set, kMainBank, 0, {zero.rate, true, 128}) ==
              "The loop must start before the end of the wave.",
          "a loop at the end");
    Check(WaveRefusal(file, *set, kFirstBank, 3, {16000, true, 0}) ==
              "Square waves and noise have no sample rate or loop.",
          "a square wave");
    const auto refused = [&](uint32_t wave)
    {
        return !xsflib::testing::ErrorOf<std::runtime_error>([&]
                                                             { (void)edit::DseWaveValuesOf(*set, kMainBank, wave); })
                    .empty();
    };
    Check(refused(3), "a wave missing from the bank");
    Check(refused(100), "a wave past the bank's table");
}

// The view of a DSE wave in IMA-ADPCM: its rate and loop, its samples decoded, a loop that steps by a word and what an
// edit changes.
void TestWaveView()
{
    const File file = TestFile();
    const std::shared_ptr<const DseSet> set = SetOf(file);
    const hokuto::app::WaveView view = hokuto::app::MakeDseWaveView(*set, kMainBank, 2);
    Check(view.ds && view.dse, "the view is a DSE wave's");
    Check(view.values.rate == 22050 && view.values.loop && view.values.loop_start == 64, "the view's values");
    Check(view.held == 248 && view.values.loop_end == 248, "the view's samples");
    Check(view.values.encoding == ntdlib::sar::WaveEncoding::kImaAdpcm && view.encodings.size() == 1,
          "the view keeps the wave's encoding");
    Check(hokuto::app::LoopStepOf(view, view.values.encoding) == 8, "the loop steps by a word of IMA-ADPCM");
    Check(view.samples.size() == 1 && view.samples[0].size() == 248, "the view decodes the wave");
    Check(view.reach.find("Edits affect ") == 0, "the view says what an edit changes");
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("dse_banks",
                                    []
                                    {
                                        TestBankView();
                                        TestSplitValues();
                                        TestSplitEditChanges();
                                        TestSplitEditUnchangedAndRefused();
                                        TestWaveEditRate();
                                        TestWaveEditLoop();
                                        TestWaveEditUnchangedAndRefused();
                                        TestWaveView();
                                    });
}
