// SPDX-License-Identifier: MIT

// Tests the conversions of GBA songs and DSE songs to the other consoles through timelines. A song of the GBA converts
// to the Wii U as a timeline, and on to the Switch, the 3DS and the Wii. Each has to play the GBA's notes at their
// times, and loop where the GBA loops.

#include <atomic>

#include "ntdlib/testing/sequence_export_check.h"

#include "driver_test_support.h"
#include "edit/ds_banks.h"
#include "other_console_fixtures.h"
#include "song/audibility.h"
#include "song/dry_render.h"

namespace
{

using hokuto::testing::TakeRead;

// A song of the GBA converts to the Wii U, and on to the Switch, the 3DS and the Wii. The new file contains the song as
// a timeline of NintendoWare's MIDI converter. ntdlib's performer has to play each note with its key and level within
// 10 ms of the time at which the GBA plays it, and loop where the GBA loops. MP2K plays its tempo of 150 slower than it
// reads, and the warnings say that the tempo moves between whole tempos. The file has to come back from both forms. A
// song without notes is refused.
void TestGba()
{
    const std::vector<uint8_t> bytes = GbaCart();
    const File file = format::ImportGbaRom(bytes);
    const std::shared_ptr<const song::GbaRom> rom = song::LoadGbaRom(bytes);
    const song::Sequence gba = song::ReadSequence(*rom, 0);
    std::string name = song::GbaSongName(*rom, 0);
    std::ranges::replace(name, ' ', '_');

    const hokuto::edit::OtherConsoleSong wii_u = hokuto::edit::ForOtherConsole(file, 0, sar::Flavor::kCafe);
    XSFLIB_CHECK(wii_u.file.form == format::ArchiveForm::kModel && FlavorOf(*wii_u.file.native) == sar::Flavor::kCafe &&
                 wii_u.file.engine == format::Engine::kWiiU);
    XSFLIB_CHECK(wii_u.file.timelines && wii_u.file.timelines->contains(0));
    XSFLIB_CHECK(wii_u.file.native->tables->sounds.size() == 1 &&
                 wii_u.file.native->tables->sounds[0].name == "SEQ_" + name &&
                 wii_u.file.native->tables->sounds[0].pan_curve == 3);
    const auto alternates = [](const std::string& warning)
    {
        return warning.starts_with("NintendoWare tempos must be whole BPM values.") &&
               warning.find("alternates between the whole tempos below and above") != std::string::npos;
    };
    XSFLIB_CHECK(std::ranges::any_of(wii_u.warnings, alternates));
    XSFLIB_CHECK(PlaysAsGba(gba, song::ReadSequence(*ArchiveOf(wii_u.file), 0)));
    XSFLIB_CHECK(ComesBack(wii_u.file));

    for (const sar::Flavor to : {sar::Flavor::kNx, sar::Flavor::kCtr})
    {
        const hokuto::edit::OtherConsoleSong next = hokuto::edit::ForOtherConsole(file, 0, to);
        XSFLIB_CHECK(FlavorOf(*next.file.native) == to && next.file.engine == format::EngineOf(to));
        XSFLIB_CHECK(PlaysAsGba(gba, song::ReadSequence(*ArchiveOf(next.file), 0)));
        XSFLIB_CHECK(ComesBack(next.file));
    }

    const hokuto::edit::OtherConsoleSong wii = hokuto::edit::ForWii(file, 0);
    XSFLIB_CHECK(wii.file.engine == format::Engine::kWii);
    XSFLIB_CHECK(PlaysAsGba(gba, song::ReadSequence(*WiiArchiveOf(wii.file), 0)));
    XSFLIB_CHECK(ComesBack(wii.file));

    const std::string silent = song::GbaSongName(*rom, 1) +
                               " makes no sound. It has no notes, or all its notes play at a level of 0. The MIDI file "
                               "would be empty.";
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::ForOtherConsole(file, 1, sar::Flavor::kCafe); }) ==
                 silent);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::ForWii(file, 1); }) == silent);

    // The conversion of a song without notes throws EmptyConversion. The command line counts such songs apart from
    // failures.
    bool empty = false;
    try
    {
        song::ConvertToMidi(*rom, 1, "Hokuto");
    }
    catch (const song::EmptyConversion&)
    {
        empty = true;
    }
    XSFLIB_CHECK(empty);
}

// A sound of DSE in a DS image, with two notes on one track.
struct DseFixture
{
    File file;
};

// The fixture of the tests of a sound of DSE.
DseFixture MakeDseFixture()
{
    namespace dse_files = ntdlib::testing::dse_song_files;
    namespace nds_files = ntdlib::testing::nds_files;
    using dse_files::Join;
    const dse_files::Bytes conductor = {0, 0, 0, 0, 0xA4, 0x78, 0x98};
    const dse_files::Bytes track = Join({{1, 1, 0, 0},
                                         {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x7F},
                                         dse_files::Note(0x60, 0, 0, 0x18, 1),
                                         {0x92, 0x18},
                                         dse_files::Note(0x50, 7, 0, 0x18, 1),
                                         {0x92, 0x18, 0x98}});
    nds_files::Image image;
    image.files = {{"SOUND/BGM/bgm.swd", dse_files::MainBankFile()},
                   {"SOUND/BGM/bgm0001.smd", dse_files::SongFile({conductor, track})},
                   {"SOUND/BGM/bgm0001.swd", dse_files::BankFile()}};
    const File file = format::ImportDseImage(nds_files::BuildImage(image).first);

    return {file};
}

// Key 48 at 0 and key 55 at 24 ticks of the song's 48 a quarter, at 120 beats a minute: 250 ms.
void CheckDseNotes(const std::vector<std::tuple<double, uint8_t, uint8_t>>& notes)
{
    SEQLIB_REQUIRE(notes.size() == 2);
    XSFLIB_CHECK(std::get<1>(notes[0]) == 48 && std::get<2>(notes[0]) == 0x60);
    XSFLIB_CHECK(std::get<1>(notes[1]) == 55 && std::get<2>(notes[1]) == 0x50);
    XSFLIB_CHECK(std::abs(std::get<0>(notes[1]) - std::get<0>(notes[0]) - 250.0) < 2.0);
}

// A sound of DSE converts to the Wii U through ntdlib's converter, and on from there. Each console plays the sound's
// notes, with their keys and velocities, at their times.
void TestDseToConsoles()
{
    const DseFixture fixture = MakeDseFixture();
    const File& file = fixture.file;

    for (const sar::Flavor to : {sar::Flavor::kCafe, sar::Flavor::kNx, sar::Flavor::kCtr})
    {
        const hokuto::edit::OtherConsoleSong sent = hokuto::edit::ForOtherConsole(file, 0, to);
        XSFLIB_CHECK(FlavorOf(*sent.file.native) == to);
        XSFLIB_CHECK(sent.file.native->tables->sounds.at(0).name == "SEQ_bgm0001");
        CheckDseNotes(TimedNotes(song::ReadSequence(*ArchiveOf(sent.file), 0)));
    }
    const hokuto::edit::OtherConsoleSong wii = hokuto::edit::ForWii(file, 0);
    XSFLIB_CHECK(wii.file.native->engine == format::Engine::kWii);
    CheckDseNotes(TimedNotes(
        song::ReadSequence(*song::LoadWiiArchive(format::ExportArchive(hokuto::edit::FoldTimelines(wii.file)),
                                                 "SEQ_bgm0001", ntdlib::rvl::snd::kRsarOptions),
                           0)));
    const hokuto::edit::OtherConsoleSong ds = hokuto::edit::ForDs(file, 0);
    XSFLIB_CHECK(ds.file.native->engine == format::Engine::kDs);
    CheckDseNotes(TimedNotes(song::ReadSequence(*song::LoadDsArchive(format::ExportArchive(ds.file)), 0)));
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::ForDs(file, 99); }) ==
                 "The image has no DSE sound 99.");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::ForOtherConsole(file, 99, sar::Flavor::kCafe); }) ==
                 "The image has no DSE sound 99.");
}

// A split of DSE comes to the Wii U at the level at which DSE plays it. The fixture's song plays program 0, whose one
// split sits at the centre at a volume of 0x6E in a program at 0x7F. DSE plays the split at the square of the product's
// fraction of 127: (110 / 127)^2, -2.50 dB. ntdlib's SoundFont gives every zone a further cut of PanCut, 30.1 cB. A
// zone panned to a side then requires no gain. The new bank removes that cut, and the region's volume is 127 times the
// split's level: 95.
void TestDseLevels()
{
    const DseFixture fixture = MakeDseFixture();

    const hokuto::edit::OtherConsoleSong wii_u = hokuto::edit::ForOtherConsole(fixture.file, 0, sar::Flavor::kCafe);

    SEQLIB_CHECK_EQ(VolumeOf(*ArchiveOf(wii_u.file), 0, 0), uint8_t{95});
}

// The levels in dB of the left and the right side of `render` from `start` to `end` seconds.
std::pair<double, double> SideLevels(const song::DryRender& render, double start, double end)
{
    const auto first = static_cast<std::size_t>(start * render.rate);
    const auto last = std::min(static_cast<std::size_t>(end * render.rate), render.samples.size() / 2);
    double left = 0.0;
    double right = 0.0;
    for (std::size_t frame = first; frame < last; frame++)
    {
        left += render.samples[2 * frame] * render.samples[2 * frame];
        right += render.samples[2 * frame + 1] * render.samples[2 * frame + 1];
    }

    const double frames = static_cast<double>(std::max<std::size_t>(last, first + 1) - first);
    return {10.0 * std::log10(left / frames + 1e-20), 10.0 * std::log10(right / frames + 1e-20)};
}

// The test's bank with program 1's first split on wave 2, which loops, in place of wave 1, which doesn't. The split
// keeps its keys up to 63 and its pan of 0x20.
ntdlib::testing::dse_song_files::Bytes LoopingBankFile()
{
    ntdlib::testing::dse_song_files::Bytes bank = ntdlib::testing::dse_song_files::BankFile();
    const ntdlib::testing::dse_song_files::Bytes split = {0x00, 0x00, 0x02, 0x01, 0x00, 0x3F, 0x00, 0x3F, 0x00, 0x7F,
                                                          0x00, 0x7F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00};
    const auto found = std::ranges::search(bank, split);
    const bool once =
        !found.empty() && std::ranges::search(found.end(), bank.end(), split.begin(), split.end()).empty();
    XSFLIB_CHECK(once);
    if (once)
    {
        found[18] = 0x02;
    }

    return bank;
}

// A sound of DSE sent to the DS keeps the balance of its pans and its level (H-766). DSE's driver gives a voice the sum
// of its split's pan and its channel's, less 64, held to 0 to 127 (ntdlib's dse::Driver, from the game's code).
// NITRO-Composer sums a track's pan and a region's in the same way, and the DS's sound unit pans linearly: a pan of p
// plays the left side at (128 - p) / 128 and the right at p / 128. The track plays four notes of 24 ticks, 48 ticks
// apart at 120 beats a minute: key 48 of program 0, whose split sits at the centre, key 64 of program 1, whose split
// there has a pan of 0x60, key 48 of program 0 with the channel's pan at 0x10, and key 40 of program 1, whose split
// there has a pan of 0x20, with the channel still at 0x10. The last sum is -16, and both drivers hold it to 0: the
// left side alone. Every split's wave loops (LoopingBankFile). The source plays on ntdlib's model of DSE, and the new
// file on ntdlib's model of the DS, both dry in strict mixing. Each note has to play each side within 1 dB of the
// source. The left side's level over the right's has to lie within 0.5 dB of the pan's: 0 dB at the centre, 32 : 96 at
// 0x60 and 112 : 16 at 0x10. At the held pan, the right side has to lie 60 dB or more under the left. The DS's regions
// keep the splits' pans.
//
// Program 0's notes play about 0.8 dB under DSE on the DS, evenly over the note, and program 1's at 0x60 within 0.15
// dB. Program 0's wave is 16-bit PCM of the test's made-up bytes, which are close to noise, and the conversion encodes
// it in IMA-ADPCM. Program 1's wave is IMA-ADPCM already. The difference probably comes from that encoding. Program
// 1's split at 0x20 falls during its sustain time from about 100 ms, and DS regions have no sustain time (the
// conversion's warning about holds): the last note's levels count up to 100 ms.
void TestDseBalanceOnTheDs()
{
    namespace dse_files = ntdlib::testing::dse_song_files;
    namespace nds_files = ntdlib::testing::nds_files;
    using dse_files::Join;
    const dse_files::Bytes conductor = {0, 0, 0, 0, 0xA4, 0x78, 0x98};
    const dse_files::Bytes track = Join({{1, 1, 0, 0},
                                         {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x7F, 0xE8, 0x40},
                                         dse_files::Note(0x7F, 0, 0, 0x18, 1),
                                         {0x92, 0x30, 0xAC, 0x01},
                                         dse_files::Note(0x7F, 4, 1, 0x18, 1),
                                         {0x92, 0x30, 0xAC, 0x00, 0xE8, 0x10},
                                         dse_files::Note(0x7F, 0, -1, 0x18, 1),
                                         {0x92, 0x30, 0xAC, 0x01},
                                         dse_files::Note(0x7F, 4, -1, 0x18, 1),
                                         {0x92, 0x30, 0x98}});
    nds_files::Image image;
    image.files = {{"SOUND/BGM/bgm.swd", dse_files::MainBankFile()},
                   {"SOUND/BGM/bgm0001.smd", dse_files::SongFile({conductor, track})},
                   {"SOUND/BGM/bgm0001.swd", LoopingBankFile()}};
    const File file = format::ImportDseImage(nds_files::BuildImage(image).first);
    const std::shared_ptr<const song::DseSet> set =
        song::LoadDseSet(format::ExportArchive(file), ntdlib::dse::Build::kLater, {});

    const hokuto::edit::OtherConsoleSong ds = hokuto::edit::ForDs(file, 0);
    const std::shared_ptr<const song::DsArchive> archive = song::LoadDsArchive(format::ExportArchive(ds.file));

    const std::atomic<bool> cancel = false;
    const auto source = std::make_shared<const song::Sequence>(song::ReadSequence(*set, 0));
    const song::DryRender dse = song::DryOf(set, source).render(2.1, cancel);
    const auto sent = std::make_shared<const song::Sequence>(song::ReadSequence(*archive, 0));
    const song::DryRender on_ds = song::DryOf(archive, sent).render(2.1, cancel);
    SEQLIB_REQUIRE(dse.rate == on_ds.rate);
    const std::array<double, 3> balances = {0.0, 20.0 * std::log10(32.0 / 96.0), 20.0 * std::log10(112.0 / 16.0)};
    for (std::size_t note = 0; note < 4; note++)
    {
        // From 50 ms into the note to 20 ms before its release. The last note's window ends at 100 ms.
        const double start = 0.5 * static_cast<double>(note);
        const double end = start + (note < balances.size() ? 0.23 : 0.10);
        const auto [dse_left, dse_right] = SideLevels(dse, start + 0.05, end);
        const auto [ds_left, ds_right] = SideLevels(on_ds, start + 0.05, end);
        XSFLIB_CHECK(std::abs(ds_left - dse_left) < 1.0);
        if (note < balances.size())
        {
            XSFLIB_CHECK(std::abs(ds_right - dse_right) < 1.0);
            XSFLIB_CHECK(std::abs((dse_left - dse_right) - balances[note]) < 0.5);
            XSFLIB_CHECK(std::abs((ds_left - ds_right) - balances[note]) < 0.5);
        }
        else
        {
            XSFLIB_CHECK(dse_left - dse_right >= 60.0 && ds_left - ds_right >= 60.0);
        }
    }

    std::set<uint8_t> pans;
    for (const ntdlib::sdat::Instrument& instrument : hokuto::edit::DsBankModelOf(*archive, 0).instruments)
    {
        if (instrument.regions.empty() && instrument.type != ntdlib::sdat::InstrumentType::kNone)
        {
            pans.insert(instrument.region.pan);
        }

        for (const ntdlib::sdat::Region& region : instrument.regions)
        {
            pans.insert(region.pan);
        }
    }
    XSFLIB_CHECK((pans == std::set<uint8_t>{0x20, 0x40, 0x60}));
}

// A sample's level under which a render counts as silent: about -120 dBFS.
constexpr float kSilence = 1e-6F;

// The time in seconds from which every sample of `render` lies below `floor` (full scale at 1.0).
double SilentFrom(const song::DryRender& render, float floor)
{
    std::size_t frames = render.samples.size() / 2;
    while (frames > 0 && std::abs(render.samples[2 * frames - 1]) < floor &&
           std::abs(render.samples[2 * frames - 2]) < floor)
    {
        frames--;
    }

    return static_cast<double>(frames) / render.rate;
}

// The louder side of `render` from `start` to `end` seconds, in dB (SideLevels).
double LouderSide(const song::DryRender& render, double start, double end)
{
    const auto [left, right] = SideLevels(render, start, end);

    return std::max(left, right);
}

// A release that DSE plays silent stays silent on the Wii U and on the DS (H-769 and H-771). DSE's driver squares the
// product of a voice's volumes, and a quotient under 23 gives the sound unit a volume of 0 (ntdlib's
// dse::VoiceView::volume, from the game's code). The track plays key 48 of program 0 for 24 ticks at a channel volume
// of 0x40, then waits 400 ticks. Program 0's split plays at 0x6E, and its wave loops. The voice falls silent about 70
// ms into its release, while its envelope still runs. ntdlib's converter stops the MIDI part there with control change
// 120. The new file of each console stops the track's channels there with a mute command. The source plays on ntdlib's
// model of DSE, dry in strict mixing: the anchor. The note has to sound on each console within 3 dB of the source up to
// its release. From 30 ms after the source falls silent, each console has to stay 60 dB or more under the note's level.
// Without the mute, the release of the new bank's region rang on there, 41 dB under the note on the Wii U and 28 dB
// under it on the DS.
void TestDseSilentRelease()
{
    namespace dse_files = ntdlib::testing::dse_song_files;
    namespace nds_files = ntdlib::testing::nds_files;
    using dse_files::Join;
    const dse_files::Bytes conductor = {0, 0, 0, 0, 0xA4, 0x78, 0x98};
    const dse_files::Bytes track = Join({{1, 1, 0, 0},
                                         {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x40},
                                         dse_files::Note(0x7F, 0, 0, 0x18, 1),
                                         {0x92, 0x18, 0x93, 0x90, 0x01, 0x98}});
    nds_files::Image image;
    image.files = {{"SOUND/BGM/bgm.swd", dse_files::MainBankFile()},
                   {"SOUND/BGM/bgm0001.smd", dse_files::SongFile({conductor, track})},
                   {"SOUND/BGM/bgm0001.swd", dse_files::BankFile()}};
    const File file = format::ImportDseImage(nds_files::BuildImage(image).first);
    const std::shared_ptr<const song::DseSet> set =
        song::LoadDseSet(format::ExportArchive(file), ntdlib::dse::Build::kLater, {});
    const std::atomic<bool> cancel = false;
    const auto source = std::make_shared<const song::Sequence>(song::ReadSequence(*set, 0));
    const song::DryRender dse = song::DryOf(set, source).render(2.0, cancel);

    const hokuto::edit::OtherConsoleSong wii_u = hokuto::edit::ForOtherConsole(file, 0, sar::Flavor::kCafe);
    const std::shared_ptr<const sar::SoundArchive> archive = ArchiveOf(wii_u.file);
    const auto on_wii_u = std::make_shared<const song::Sequence>(song::ReadSequence(*archive, 0));
    const song::DryRender wii_u_render = song::DryOf(archive, on_wii_u).render(2.0, cancel);
    const hokuto::edit::OtherConsoleSong ds = hokuto::edit::ForDs(file, 0);
    const std::shared_ptr<const song::DsArchive> ds_archive = song::LoadDsArchive(format::ExportArchive(ds.file));
    const auto on_ds = std::make_shared<const song::Sequence>(song::ReadSequence(*ds_archive, 0));
    const song::DryRender ds_render = song::DryOf(ds_archive, on_ds).render(2.0, cancel);

    const double silent_from = SilentFrom(dse, kSilence);
    const double note = LouderSide(dse, 0.05, 0.23);
    XSFLIB_CHECK(silent_from > 0.26 && silent_from < 1.0);
    for (const song::DryRender* render : {&wii_u_render, &ds_render})
    {
        XSFLIB_CHECK(std::abs(LouderSide(*render, 0.05, 0.23) - note) < 3.0);
        XSFLIB_CHECK(LouderSide(*render, silent_from + 0.03, 1.5) < note - 60.0);
    }
}

// The image of a sound of DSE that holds key 48 of program 0 for 192 ticks at 120 beats a minute: 2 s. Program 0's
// split plays at 0x6E in a program at 0x7F. With `lfos`, program 0 takes those LFOs.
File VolumeLfoImage(std::initializer_list<ntdlib::testing::dse_song_files::Lfo> lfos)
{
    namespace dse_files = ntdlib::testing::dse_song_files;
    namespace nds_files = ntdlib::testing::nds_files;
    using dse_files::Join;
    const dse_files::Envelope slow = {
        .attack_begin = 0x20, .attack = 0x10, .decay = 0x20, .sustain = 0x60, .release = 0x28};
    const dse_files::Split split = {.wave = 0, .tuning = -1742, .coarse = -12, .volume = 0x6E, .envelope = slow};
    const dse_files::Bytes plain = dse_files::Program(0, 0x7F, {}, {split});
    const dse_files::Bytes with_lfos = dse_files::Program(0, 0x7F, lfos, {split});
    dse_files::Bytes bank = dse_files::BankFile();
    const auto found = std::ranges::search(bank, plain);
    const bool placed = !found.empty() && with_lfos.size() == plain.size();
    XSFLIB_CHECK(placed);
    if (placed)
    {
        std::ranges::copy(with_lfos, found.begin());
    }

    const dse_files::Bytes conductor = {0, 0, 0, 0, 0xA4, 0x78, 0x98};
    const dse_files::Bytes track = Join({{1, 1, 0, 0},
                                         {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x7F, 0xE8, 0x40},
                                         dse_files::Note(0x7F, 0, 0, 0xC0, 1),
                                         {0x92, 0xC0, 0x98}});
    nds_files::Image image;
    image.files = {{"SOUND/BGM/bgm.swd", dse_files::MainBankFile()},
                   {"SOUND/BGM/bgm0001.smd", dse_files::SongFile({conductor, track})},
                   {"SOUND/BGM/bgm0001.swd", bank}};

    return format::ImportDseImage(nds_files::BuildImage(image).first);
}

// The changes of level in dB of `render` against `reference` in the measure's windows of 50 ms from `start` to `end`
// seconds (song::WindowLevels). Both renders play at one rate.
std::vector<double> LevelChanges(const song::DryRender& render, const song::DryRender& reference, double start,
                                 double end)
{
    const auto first = 2 * static_cast<std::size_t>(start * render.rate);
    const auto count = 2 * static_cast<std::size_t>((end - start) * render.rate);
    const std::vector<double> levels = song::WindowLevels(std::span(render.samples).subspan(first, count), render.rate);
    const std::vector<double> reference_levels =
        song::WindowLevels(std::span(reference.samples).subspan(first, count), reference.rate);
    std::vector<double> changes;
    for (std::size_t i = 0; i < std::min(levels.size(), reference_levels.size()); i++)
    {
        changes.push_back(levels[i] - reference_levels[i]);
    }

    return changes;
}

// The dry renders of a sound of DSE: the source on ntdlib's model of DSE, and the new files of the Wii U and the DS
// from Another Console on their models, all for 2 s in strict mixing.
struct VolumeLfoRenders
{
    song::DryRender dse;
    song::DryRender wii_u;
    song::DryRender ds;
};

// The renders of VolumeLfoRenders for sound 0 of `file`, sent to the Wii U and the DS through Another Console.
VolumeLfoRenders RenderVolumeLfo(const File& file)
{
    const std::atomic<bool> cancel = false;
    const std::shared_ptr<const song::DseSet> set =
        song::LoadDseSet(format::ExportArchive(file), ntdlib::dse::Build::kLater, {});
    const auto source = std::make_shared<const song::Sequence>(song::ReadSequence(*set, 0));
    const hokuto::edit::OtherConsoleSong wii_u = hokuto::edit::ForOtherConsole(file, 0, sar::Flavor::kCafe);
    const std::shared_ptr<const sar::SoundArchive> archive = ArchiveOf(wii_u.file);
    const auto on_wii_u = std::make_shared<const song::Sequence>(song::ReadSequence(*archive, 0));
    const hokuto::edit::OtherConsoleSong ds = hokuto::edit::ForDs(file, 0);
    const std::shared_ptr<const song::DsArchive> ds_archive = song::LoadDsArchive(format::ExportArchive(ds.file));
    const auto on_ds = std::make_shared<const song::Sequence>(song::ReadSequence(*ds_archive, 0));

    return {song::DryOf(set, source).render(2.0, cancel), song::DryOf(archive, on_wii_u).render(2.0, cancel),
            song::DryOf(ds_archive, on_ds).render(2.0, cancel)};
}

// A volume LFO of DSE plays on the Wii U and the DS at its mean power (H-770). The banks of both consoles play no
// volume LFO: the import of the SoundFont leaves modLfoToVolume out. DSE's driver adds the LFO to the note's level and
// holds the sum from 0 to 127 before it squares it (ntdlib's dse::Driver, from the game's code). Program 0's LFO is a
// square of waveform 1 with an amplitude of 1,024 and phases of 100 ms: the level jumps between 110 + 64, held at 127,
// and 110 - 64 = 46. The source plays on ntdlib's model of DSE, dry in strict mixing: the anchor. Each render's level
// counts against the same sound without the LFO, in the measure's windows of 50 ms from 0.3 s, where the envelope
// sustains, to 1.9 s. The square holds each level for 100 ms, and half the windows hold one level alone.
//
// The new file of each console has to play within 0.5 dB of the source's mean power, about -0.4 dB. The straight line
// in dB of the LFO's two levels would sit at its middle, about -6.3 dB. The conversion for a bank holds no
// modLfoToVolume. The MIDI export keeps the LFO for SoundFont players: its zone sits at the middle of the source's two
// levels, within 0.5 dB, and its modLfoToVolume is half their difference.
void TestDseVolumeLfo()
{
    const File plain = VolumeLfoImage({});
    const File square = VolumeLfoImage({{.type = 1, .output = 2, .waveform = 1, .amplitude = 1024, .phase = 100}});

    const VolumeLfoRenders without = RenderVolumeLfo(plain);
    const VolumeLfoRenders with = RenderVolumeLfo(square);
    const std::shared_ptr<const song::DseSet> plain_set =
        song::LoadDseSet(format::ExportArchive(plain), ntdlib::dse::Build::kLater, {});
    const std::shared_ptr<const song::DseSet> set =
        song::LoadDseSet(format::ExportArchive(square), ntdlib::dse::Build::kLater, {});
    const std::pair<int, int> export_plain =
        ntdlib::testing::AttenuationAndVolumeLfo(song::ConvertToMidi(*plain_set, 0, "Hokuto").sf2);
    const std::pair<int, int> exported =
        ntdlib::testing::AttenuationAndVolumeLfo(song::ConvertToMidi(*set, 0, "Hokuto").sf2);
    const std::pair<int, int> for_bank =
        ntdlib::testing::AttenuationAndVolumeLfo(song::ConvertToMidi(*set, 0, "Hokuto", song::DseFilesFor::kBank).sf2);
    const std::pair<int, int> for_ds_bank = ntdlib::testing::AttenuationAndVolumeLfo(
        song::ConvertToMidi(*set, 0, "Hokuto", song::DseFilesFor::kDsBank).sf2);

    // dse::Player's two levels of the square, and its mean power. Half the windows hold a jump of the square.
    const std::vector<double> changes = LevelChanges(with.dse, without.dse, 0.3, 1.9);
    const auto [low, high] = ntdlib::testing::SquareLevels(changes, 0.5);
    const double played = ntdlib::testing::PowerMeanDb(changes);
    XSFLIB_CHECK(std::abs(high - 40.0 * std::log10(127.0 / 110.0)) < 0.5);
    XSFLIB_CHECK(std::abs(low - 40.0 * std::log10(46.0 / 110.0)) < 0.5);

    for (const auto& [render, plain_render] :
         {std::pair{&with.wii_u, &without.wii_u}, std::pair{&with.ds, &without.ds}})
    {
        const double on_console = ntdlib::testing::PowerMeanDb(LevelChanges(*render, *plain_render, 0.3, 1.9));
        XSFLIB_CHECK(std::abs(on_console - played) < 0.5);
    }
    XSFLIB_CHECK(for_bank.second == 0 && for_ds_bank.second == 0);

    // Players apply 0.4 of the attenuation generator (seqlib::sf2::ToZoneAttenuation).
    const double export_shift = 0.4 * (export_plain.first - exported.first) / 10.0;
    XSFLIB_CHECK(std::abs(export_shift - (low + high) / 2.0) < 0.5);
    XSFLIB_CHECK(std::abs(exported.second / 10.0 - (high - low) / 2.0) < 0.5);
}

// The engines that the mixer's switch offers to a sound of DSE.
void TestDseEngines()
{
    const DseFixture fixture = MakeDseFixture();
    const File& file = fixture.file;

    // The mixer's switch offers the engines of the later consoles and of the DS, and DSE only to a sound of DSE.
    XSFLIB_CHECK(!hokuto::edit::EngineRefused(file, format::Engine::kDse));
    for (const format::Engine engine : {format::Engine::kSwitch, format::Engine::kWiiU, format::Engine::k3ds,
                                        format::Engine::kWii, format::Engine::kDs})
    {
        XSFLIB_CHECK(!hokuto::edit::EngineRefused(file, engine));
    }
    XSFLIB_CHECK(hokuto::edit::EngineRefused(file, format::Engine::kGba) ==
                 "Can't play sounds of other consoles on the GBA engine.");
    XSFLIB_CHECK(hokuto::edit::EngineRefused(TestFile(sar::Flavor::kNx), format::Engine::kDse) ==
                 "Can't play sounds of other consoles on the DSE engine.");
}

// The reader plays a sound of DSE on the engine of another console.
void TestDseReader()
{
    const DseFixture fixture = MakeDseFixture();
    const File& file = fixture.file;

    // The reader plays the sound's conversion on another console's engine, and the mixer's mutes follow its tracks.
    using hokuto::song::SequenceReader;
    const std::shared_ptr<const song::DseSet> set =
        song::LoadDseSet(format::ExportArchive(file), ntdlib::dse::Build::kLater, {});
    SequenceReader reader;
    reader.Request(set, 0,
                   [&file]
                   {
                       const hokuto::edit::OtherConsoleSong converted =
                           hokuto::edit::ForEngine(file, 0, format::Engine::kWiiU);
                       return SequenceReader::EngineSound{
                           .played = song::LoadArchive(format::ExportArchive(converted.file), "test")};
                   });
    std::optional<SequenceReader::Result> read;
    read = TakeRead(reader);
    SEQLIB_REQUIRE(read && read->sequence && read->dse == set && read->engine_sequence && read->engine);
    CheckDseNotes(TimedNotes(*read->engine_sequence));
    const std::array<uint32_t, 32> tracks = song::TracksOfConversion(*read->sequence, *read->engine_sequence);
    XSFLIB_CHECK(tracks[1] != 0);

    // The DS's engine plays the conversion in the same way.
    reader.Request(set, 0,
                   [&file]
                   {
                       const hokuto::edit::OtherConsoleSong converted =
                           hokuto::edit::ForEngine(file, 0, format::Engine::kDs);
                       XSFLIB_CHECK(converted.file.engine == format::Engine::kDs);
                       SequenceReader::EngineSound sound;
                       sound.played = song::LoadDsArchive(format::ExportArchive(converted.file));
                       return sound;
                   });
    read.reset();
    read = TakeRead(reader);
    SEQLIB_REQUIRE(read && read->sequence && read->dse == set && read->engine_sequence && read->engine &&
                   read->engine->played.Ds());
    CheckDseNotes(TimedNotes(*read->engine_sequence));
    XSFLIB_CHECK(song::TracksOfConversion(*read->sequence, *read->engine_sequence)[1] != 0);

    // A conversion that fails leaves the sound on DSE, with the conversion's message.
    reader.Request(set, 0, []() -> SequenceReader::EngineSound { throw std::invalid_argument("No conversion."); });
    read.reset();
    read = TakeRead(reader);
    SEQLIB_REQUIRE(read && read->sequence);
    XSFLIB_CHECK(read->engine_error == "No conversion." && !read->engine && !read->engine_sequence);
}

// A sequence of `timebase` ticks a quarter note, with notes at the ticks and keys of each numbered track.
song::Sequence SongOfNotes(int timebase,
                           std::initializer_list<std::pair<int, std::vector<std::pair<uint32_t, uint8_t>>>> tracks)
{
    song::Sequence sequence;
    sequence.timebase = timebase;
    for (const auto& [number, notes] : tracks)
    {
        song::Track track;
        track.number = number;
        for (const auto& [start, key] : notes)
        {
            track.notes.push_back(song::Note{.start = start, .key = key});
        }
        sequence.tracks.push_back(std::move(track));
    }

    return sequence;
}

// The tracks of a conversion match the tracks of the source by their notes, in quarter notes: a source's track without
// notes matches none, a track of two channels matches two, and tracks that play the same notes match one each. A
// conversion's track that shares no note matches none.
void TestTracksOfConversion()
{
    const song::Sequence shown = SongOfNotes(24, {{0, {{0, 60}, {24, 62}}},
                                                  {1, {}},
                                                  {2, {{0, 40}, {12, 41}}},
                                                  {3, {{0, 72}, {48, 74}}},
                                                  {4, {{0, 72}, {48, 74}}},
                                                  {5, {{96, 50}}}});
    const song::Sequence played = SongOfNotes(48, {{0, {{0, 60}, {48, 62}}},
                                                   {1, {{0, 40}}},
                                                   {2, {{24, 41}}},
                                                   {3, {{0, 72}, {96, 74}}},
                                                   {4, {{0, 72}, {96, 74}}},
                                                   {5, {{192, 50}}},
                                                   {6, {{0, 90}}},
                                                   {7, {}}});
    const std::array<uint32_t, 32> tracks = song::TracksOfConversion(shown, played);
    XSFLIB_CHECK(tracks[0] == 1 << 0);
    XSFLIB_CHECK(tracks[1] == 0);
    XSFLIB_CHECK(tracks[2] == (1 << 1 | 1 << 2));
    XSFLIB_CHECK(tracks[3] == 1 << 3);
    XSFLIB_CHECK(tracks[4] == 1 << 4);
    XSFLIB_CHECK(tracks[5] == 1 << 5);
    for (std::size_t i = 6; i < tracks.size(); i++)
    {
        XSFLIB_CHECK(tracks[i] == 0);
    }

    // A conversion that starts its notes up to a few ticks early. Track 7 plays 24 ticks after track 6, and track 8
    // echoes track 6 two ticks after it. Each track of the conversion is nearest the track it comes from. A note more
    // than an eighth of a quarter note off, or with another key, matches no track.
    const song::Sequence moved_from = SongOfNotes(48, {{6, {{96, 60}, {120, 62}, {144, 64}}},
                                                       {7, {{120, 60}, {144, 62}, {168, 64}}},
                                                       {8, {{98, 60}, {122, 62}, {146, 64}}}});
    const song::Sequence moved = SongOfNotes(48, {{0, {{95, 60}, {119, 62}, {142, 64}}},
                                                  {1, {{118, 60}, {143, 62}, {167, 64}}},
                                                  {2, {{98, 60}, {122, 62}, {145, 64}}},
                                                  {3, {{180, 64}}},
                                                  {4, {{96, 61}}}});
    const std::array<uint32_t, 32> moved_tracks = song::TracksOfConversion(moved_from, moved);
    XSFLIB_CHECK(moved_tracks[6] == 1 << 0);
    XSFLIB_CHECK(moved_tracks[7] == 1 << 1);
    XSFLIB_CHECK(moved_tracks[8] == 1 << 2);

    // A song of DSE can number its tracks past 15. Track 17 keeps a place apart from track 1.
    const song::Sequence many = SongOfNotes(48, {{1, {{0, 60}}}, {17, {{0, 70}}}});
    const song::Sequence many_played = SongOfNotes(48, {{0, {{0, 60}}}, {1, {{0, 70}}}});
    const std::array<uint32_t, 32> many_tracks = song::TracksOfConversion(many, many_played);
    XSFLIB_CHECK(many_tracks[1] == 1 << 0);
    XSFLIB_CHECK(many_tracks[17] == 1 << 1);
}

// The reader returns a song of the GBA and its conversion for the engine of each later console. Each track of the song
// has a track of the conversion, at the same quarter notes with the same keys. A conversion that fails leaves the song,
// with the conversion's message.
void TestGbaOnEngines()
{
    using hokuto::song::SequenceReader;

    const std::vector<uint8_t> bytes = GbaCart();
    const File file = format::ImportGbaRom(bytes);
    const std::shared_ptr<const song::GbaRom> rom = song::LoadGbaRom(bytes);
    const auto wait = [](SequenceReader& reader)
    {
        std::optional<SequenceReader::Result> read;
        read = TakeRead(reader);
        return read;
    };

    for (const format::Engine engine :
         {format::Engine::kSwitch, format::Engine::kWiiU, format::Engine::k3ds, format::Engine::kWii})
    {
        SequenceReader reader;
        reader.Request(rom, 0,
                       [&file, engine]
                       {
                           hokuto::edit::OtherConsoleSong converted = hokuto::edit::ForEngine(file, 0, engine);
                           const std::vector<uint8_t> built = format::ExportArchive(converted.file);
                           if (engine == format::Engine::kWii)
                           {
                               return SequenceReader::EngineSound{
                                   .played = song::LoadWiiArchive(built, "test", ntdlib::rvl::snd::kRsarOptions),
                                   .wii_build = format::BuildOfWiiArchive(*converted.file.native)};
                           }
                           return SequenceReader::EngineSound{.played = song::LoadArchive(built, "test")};
                       });
        const std::optional<SequenceReader::Result> read = wait(reader);
        SEQLIB_REQUIRE(read && read->sequence && read->gba == rom && read->engine_sequence && read->engine);
        XSFLIB_CHECK(read->engine_error.empty());

        const song::Sequence& shown = *read->sequence;
        const song::Sequence& played = *read->engine_sequence;
        const std::array<uint32_t, 32> tracks = song::TracksOfConversion(shown, played);
        XSFLIB_CHECK(tracks[0] != 0 && tracks[1] != 0 && (tracks[0] & tracks[1]) == 0);
        for (const song::Track& track : shown.tracks)
        {
            const auto first = std::ranges::find_if(played.tracks, [&](const song::Track& other)
                                                    { return (tracks[track.number] >> other.number & 1) != 0; });
            SEQLIB_REQUIRE(first != played.tracks.end());
            for (const song::Note& note : track.notes)
            {
                const uint32_t tick =
                    note.start * static_cast<uint32_t>(played.timebase) / static_cast<uint32_t>(shown.timebase);
                XSFLIB_CHECK(std::ranges::any_of(first->notes, [&](const song::Note& other)
                                                 { return other.start == tick && other.key == note.key; }));
            }
        }
    }

    SequenceReader reader;
    reader.Request(rom, 0, []() -> SequenceReader::EngineSound { throw std::invalid_argument("No conversion."); });
    const std::optional<SequenceReader::Result> read = wait(reader);
    SEQLIB_REQUIRE(read && read->sequence);
    XSFLIB_CHECK(read->engine_error == "No conversion." && !read->engine && !read->engine_sequence);
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("other_console_gba_dse",
                                    []
                                    {
                                        TestGba();
                                        TestDseToConsoles();
                                        TestDseLevels();
                                        TestDseBalanceOnTheDs();
                                        TestDseSilentRelease();
                                        TestDseVolumeLfo();
                                        TestDseEngines();
                                        TestDseReader();
                                        TestTracksOfConversion();
                                        TestGbaOnEngines();
                                    });
}
