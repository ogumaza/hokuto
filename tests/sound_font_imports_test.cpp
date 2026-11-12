// SPDX-License-Identifier: MIT

// Tests File > Import > Bank… on a SoundFont 2 file (src/edit/sound_font_imports.h). A small SoundFont contains presets
// in MIDI banks 0 and 128 and a filter. Each MIDI bank has to become a bank, on the Switch, the Wii U and the 3DS with
// a wave archive of the saw's points. On the Wii, the bank contains the same points as its wave data, and on the DS the
// bank's first slot names a new wave archive. The model of each console has to play a note of the new bank at the pitch
// that the period of the saw sets. The import has to list what the banks leave out, and refuse a file that isn't a
// SoundFont. The names follow the rule of the header. An archive that already uses a name has to refuse the import.
//
// A new sound of a bank without program 0 tests edit::EditOpeningInstrument and edit::EditNotesWithInstrument too. A
// note plays nothing there until the track's opening selects a program that the bank contains.

#include "edit/sound_font_imports.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/bank_file.h"
#include "ntdlib/sar/wave_file.h"
#include "seqlib/sf2/soundfont.h"
#include "seqlib/sf2/writer.h"
#include "xsflib/testing/check.h"

#include "edit/converter.h"
#include "edit/ds_new_items.h"
#include "edit/instruments.h"
#include "edit/new_items.h"
#include "edit/notes.h"
#include "edit/timelines.h"
#include "format/archive.h"
#include "format/file.h"
#include "format/model.h"
#include "song/archive.h"
#include "song/ds.h"
#include "song/preview.h"
#include "song/sequence.h"

namespace
{

namespace sar = ntdlib::sar;
namespace sf2 = seqlib::sf2;

// The saw's period in points, and its rate.
constexpr std::size_t kPeriod = 64;
constexpr uint32_t kRate = 16000;

// A SoundFont of a looped saw with its root at key 60. Preset 3 of MIDI bank 0 plays it through a filter, and preset 0
// of MIDI bank 128 plays it as it is.
std::vector<uint8_t> TestFont()
{
    using G = sf2::Generator;
    sf2::SoundFont font;
    sf2::Sample sample;
    sample.name = "saw";
    sample.sample_rate = kRate;
    sample.original_key = 60;
    sample.loop_start = 16 * kPeriod;
    sample.loop_end = 32 * kPeriod;
    for (std::size_t i = 0; i < 32 * kPeriod; ++i)
    {
        sample.data.push_back(static_cast<int16_t>(static_cast<int>(i % kPeriod) * 800 - 25000));
    }
    font.samples.push_back(sample);

    const auto zone = [](std::vector<G> generators)
    {
        generators.push_back(G::Value(sf2::kSampleModes, 1));
        generators.push_back(G::Value(sf2::kSampleId, 0));
        return sf2::Zone{std::move(generators), {}};
    };
    font.instruments.push_back({"filtered", {zone({G::Value(sf2::kInitialFilterFc, 6000)})}});
    font.instruments.push_back({"plain", {zone({})}});
    font.presets.push_back({"filtered", 3, 0, {{{G::Value(sf2::kInstrument, 0)}, {}}}});
    font.presets.push_back({"plain", 0, 128, {{{G::Value(sf2::kInstrument, 1)}, {}}}});

    return sf2::Write(font);
}

// The left channel of `preview`, up to `most` samples.
std::vector<float> LeftOf(hokuto::audio::Renderer& preview, std::size_t most)
{
    std::vector<float> out;
    std::vector<float> block(2 * 1024);
    while (out.size() < most)
    {
        const std::size_t got = preview.Render(block.data(), 1024);
        if (got == 0)
        {
            break;
        }
        for (std::size_t i = 0; i < got; ++i)
        {
            out.push_back(block[2 * i]);
        }
    }

    return out;
}

// The period of `samples` in samples of the output, from the rises through zero between `from` and the end.
double PeriodOf(const std::vector<float>& samples, std::size_t from)
{
    std::optional<std::size_t> first;
    std::size_t last = 0;
    std::size_t rises = 0;
    for (std::size_t i = from + 1; i < samples.size(); ++i)
    {
        if (samples[i - 1] < 0 && samples[i] >= 0)
        {
            if (!first)
            {
                first = i;
            }
            last = i;
            ++rises;
        }
    }
    XSFLIB_CHECK(rises > 8);

    return static_cast<double>(last - *first) / static_cast<double>(rises - 1);
}

float PeakOf(const std::vector<float>& samples)
{
    float peak = 0;
    for (const float sample : samples)
    {
        peak = std::max(peak, std::abs(sample));
    }

    return peak;
}

// Checks that the note of `preview` sounds, with the saw's period at `output_rate`. The note's key 60 plays the saw at
// its rate.
void CheckNote(hokuto::audio::Renderer& preview, double output_rate)
{
    const std::vector<float> left = LeftOf(preview, 16384);
    XSFLIB_CHECK(left.size() >= 8192);
    XSFLIB_CHECK(PeakOf(left) > 0.01F);
    const double expected = output_rate * static_cast<double>(kPeriod) / kRate;
    XSFLIB_CHECK(std::abs(PeriodOf(left, 2048) - expected) < expected * 0.01);
}

// A SoundFont is told from other files by its bytes and by its name. Its import lists its MIDI banks, its presets and
// the bytes of its samples.
void TestRead()
{
    const std::vector<uint8_t> font = TestFont();
    XSFLIB_CHECK(hokuto::edit::IsSoundFont(font));
    XSFLIB_CHECK(!hokuto::edit::IsSoundFont(std::vector<uint8_t>{'R', 'I', 'F', 'F'}));
    std::vector<uint8_t> other = font;
    other[8] = 'W';
    XSFLIB_CHECK(!hokuto::edit::IsSoundFont(other));
    XSFLIB_CHECK(hokuto::edit::IsSoundFontName("a.sf2") && hokuto::edit::IsSoundFontName("A.SF2"));
    XSFLIB_CHECK(!hokuto::edit::IsSoundFontName("a.sfz") && !hokuto::edit::IsSoundFontName("sf2"));

    const hokuto::format::File file = hokuto::format::NewFile(hokuto::format::Engine::kSwitch);
    const hokuto::edit::SoundFontImport import = hokuto::edit::ReadSoundFontImport(file, font);
    XSFLIB_CHECK((import.midi_banks == std::vector<int>{0, 128}));
    XSFLIB_CHECK(import.presets == 2);
    XSFLIB_CHECK(import.bytes > 2 * 32 * kPeriod);
    XSFLIB_CHECK(std::ranges::any_of(
        import.warnings, [](const std::string& warning) { return warning.find("filter") != std::string::npos; }));

    const bool refused = !xsflib::testing::ErrorOf<std::invalid_argument>(
                              [&] { hokuto::edit::ReadSoundFontImport(file, std::vector<uint8_t>{1, 2, 3}); })
                              .empty();
    XSFLIB_CHECK(refused);
}

// The names of the banks that a SoundFont's MIDI banks become, and of their wave archives. One MIDI bank keeps the
// name, and more add their numbers. A wave archive gets the name of its bank with WARC in place of BANK, or
// before a name without it.
void TestNames()
{
    const std::vector<int> one{0};
    const std::vector<int> two{0, 128};
    XSFLIB_CHECK(hokuto::edit::SoundFontBankNames("BANK_A", one) == std::vector<std::string>{"BANK_A"});
    XSFLIB_CHECK(
        (hokuto::edit::SoundFontBankNames("BANK_A", two) == std::vector<std::string>{"BANK_A_0", "BANK_A_128"}));
    XSFLIB_CHECK(hokuto::edit::SoundFontWaveArchiveName("BANK_A_0") == "WARC_A_0");
    XSFLIB_CHECK(hokuto::edit::SoundFontWaveArchiveName("Piano") == "WARC_Piano");
}

// In a file of `engine`, a SoundFont becomes a bank and a wave archive for each MIDI bank. Its note plays at `rate`.
void TestLaterConsole(hokuto::format::Engine engine, sar::Flavor flavor, double rate)
{
    const std::vector<uint8_t> font = TestFont();
    hokuto::format::File file = hokuto::format::NewFile(engine);
    file = hokuto::edit::AddSoundFont(file, font, "BANK_S");
    const auto archive = hokuto::song::LoadArchive(hokuto::format::ExportArchive(file), "x");
    XSFLIB_CHECK(archive->Banks().size() == 2);
    XSFLIB_CHECK(archive->WaveArchives().size() == 2);
    XSFLIB_CHECK(archive->Banks()[0].name == "BANK_S_0");
    XSFLIB_CHECK(archive->Banks()[1].name == "BANK_S_128");
    XSFLIB_CHECK(archive->WaveArchives()[1].name == "WARC_S_128");

    // Each MIDI bank's presets keep their programs. The new wave archive contains the saw's points.
    const sar::BankFile bank = sar::BankFile::Read(archive->FileData(archive->Banks()[0].file_id));
    XSFLIB_CHECK(bank.instruments.size() == 4);
    XSFLIB_CHECK(bank.instruments[3].has_value());
    XSFLIB_CHECK(!bank.instruments[0].has_value());
    const sar::WaveArchiveFile waves =
        sar::WaveArchiveFile::Read(archive->FileData(archive->WaveArchives()[0].file_id));
    XSFLIB_CHECK(waves.flavor == flavor);
    XSFLIB_CHECK(waves.waves.size() == 1);
    XSFLIB_CHECK(sar::WaveFile::Read(waves.waves[0]).sample_rate == kRate);

    // The second bank names the second wave archive.
    const sar::BankFile drums = sar::BankFile::Read(archive->FileData(archive->Banks()[1].file_id));
    XSFLIB_CHECK(drums.waves.size() == 1);
    XSFLIB_CHECK(drums.waves[0].wave_archive_item == sar::ItemId(sar::kWaveArchiveItem, 1));

    hokuto::song::NotePreview note(archive, 1, 0);
    CheckNote(note, rate);

    // Saved in both forms, the file reads back as the same archive.
    const hokuto::format::File read = hokuto::format::Read(hokuto::format::Write(file));
    XSFLIB_CHECK(hokuto::format::ExportArchive(read) == hokuto::format::ExportArchive(file));
}

// A SoundFont of a stereo sample at preset 0: the saw on the left, and silence on the right.
std::vector<uint8_t> StereoFont()
{
    using G = sf2::Generator;
    sf2::SoundFont font;
    sf2::Sample left;
    left.name = "left";
    left.sample_rate = kRate;
    left.original_key = 60;
    left.loop_start = 16 * kPeriod;
    left.loop_end = 32 * kPeriod;
    left.type = sf2::kLeft;
    left.link = 1;
    for (std::size_t i = 0; i < 32 * kPeriod; ++i)
    {
        left.data.push_back(static_cast<int16_t>(static_cast<int>(i % kPeriod) * 800 - 25000));
    }
    sf2::Sample right = left;
    right.name = "right";
    right.type = sf2::kRight;
    right.link = 0;
    std::ranges::fill(right.data, int16_t{0});
    font.samples.push_back(left);
    font.samples.push_back(right);

    const auto zone = [](int pan, int sample)
    {
        return sf2::Zone{{G::Value(sf2::kPan, pan), G::Value(sf2::kSampleModes, 1), G::Value(sf2::kSampleId, sample)},
                         {}};
    };
    font.instruments.push_back({"stereo", {zone(-500, 0), zone(500, 1)}});
    font.presets.push_back({"stereo", 0, 0, {{{G::Value(sf2::kInstrument, 0)}, {}}}});

    return sf2::Write(font);
}

// The two voices of a stereo sample make one region of a stereo wave at the centre. As with the two voices, the wave's
// left channel plays on the left alone.
void TestStereo(hokuto::format::Engine engine)
{
    const std::vector<uint8_t> font = StereoFont();
    const hokuto::format::File empty = hokuto::format::NewFile(engine);
    XSFLIB_CHECK(hokuto::edit::ReadSoundFontImport(empty, font).warnings.empty());
    const hokuto::format::File file = hokuto::edit::AddSoundFont(empty, font, "BANK_S");
    const auto archive = hokuto::song::LoadArchive(hokuto::format::ExportArchive(file), "x");
    const sar::WaveArchiveFile waves =
        sar::WaveArchiveFile::Read(archive->FileData(archive->WaveArchives()[0].file_id));
    SEQLIB_REQUIRE(waves.waves.size() == 1);
    const sar::WaveFile wave = sar::WaveFile::Read(waves.waves[0]);
    SEQLIB_REQUIRE(wave.channels.size() == 2);
    const sar::BankFile bank = sar::BankFile::Read(archive->FileData(archive->Banks()[0].file_id));
    SEQLIB_REQUIRE(bank.instruments.size() == 1 && bank.instruments[0].has_value());

    hokuto::song::NotePreview note(archive, 0, 0);
    std::vector<float> samples(16384 * 2);
    const std::size_t frames = note.Render(samples.data(), 16384);
    XSFLIB_CHECK(frames > 8192);
    float left_peak = 0.0F;
    float right_peak = 0.0F;
    for (std::size_t i = 0; i < frames; ++i)
    {
        left_peak = std::max(left_peak, std::abs(samples[i * 2]));
        right_peak = std::max(right_peak, std::abs(samples[i * 2 + 1]));
    }
    XSFLIB_CHECK(left_peak > 0.01F);
    XSFLIB_CHECK(right_peak < left_peak * 0.01F);
}

// A SoundFont is imported into a file of the Wii as a bank for each MIDI bank. Its note plays at 32,000 Hz.
void TestWii()
{
    const std::vector<uint8_t> font = TestFont();
    hokuto::format::File file = hokuto::format::NewFile(hokuto::format::Engine::kWii);
    file = hokuto::edit::AddSoundFont(file, font, "BANK_S");
    const auto archive =
        hokuto::song::LoadWiiArchive(hokuto::format::ExportArchive(file), "x", ntdlib::rvl::snd::kRsarOptions);
    XSFLIB_CHECK(archive->Banks().size() == 2);
    XSFLIB_CHECK(archive->Banks()[1].name == "BANK_S_128");

    hokuto::song::NotePreview note(archive, 0, 3);
    CheckNote(note, 32000);
}

// A SoundFont is imported into a file of the DS as a bank and a wave archive. Its note plays at 32,728 Hz. A second
// import under a name that the archive already uses is refused.
void TestDs()
{
    const std::vector<uint8_t> font = TestFont();
    hokuto::format::File file = hokuto::format::NewFile(hokuto::format::Engine::kDs);
    file = hokuto::edit::AddSoundFont(file, font, "BANK_S");
    const auto archive = hokuto::song::LoadDsArchive(hokuto::format::ExportArchive(file));
    // A new file of the DS contains an empty bank.
    XSFLIB_CHECK(archive->archive.banks.size() == 3);
    XSFLIB_CHECK(archive->archive.wave_archives.size() == 2);
    XSFLIB_CHECK(archive->archive.banks[2]->wave_archives[0] == 1);
    XSFLIB_CHECK(archive->archive.banks[2]->wave_archives[1] == 0xFFFF);

    hokuto::song::DsNotePreview note(*archive, 2, 0);
    CheckNote(note, 32728);

    // A name that the archive already uses stays out.
    const bool refused =
        !xsflib::testing::ErrorOf<std::invalid_argument>([&] { hokuto::edit::AddSoundFont(file, font, "BANK_S"); })
             .empty();
    XSFLIB_CHECK(refused);
}

// A first note on `file`'s new sound, its sound 0, of a bank whose only program is 3. `read` returns the song of the
// sound, and `saved` the file as a save writes it.
template <typename Read, typename Saved>
void CheckFirstNote(const hokuto::format::File& file, const Read& read, const Saved& saved)
{
    using hokuto::edit::Program;
    const hokuto::song::Sequence song = read(file);
    XSFLIB_CHECK(song.tracks.size() == 1 && song.tracks[0].notes.empty());
    const hokuto::song::Track& track = song.tracks[0];
    XSFLIB_CHECK(!hokuto::edit::OpeningInstrument(file, 0, track));

    // Program 0 plays nothing, and the note doesn't show.
    for (const uint32_t tick : {uint32_t{0}, uint32_t{96}})
    {
        const hokuto::edit::NewNote note{.stretch = track.stretches.at(0), .tick = tick, .length = 48};
        const hokuto::format::File silent = hokuto::edit::EditNotes(file, 0, {}, std::vector{note}).file;
        XSFLIB_CHECK(read(saved(silent)).tracks.at(0).notes.empty());

        // With the bank's program in the opening, it plays and shows at its tick.
        const hokuto::edit::Edited played =
            hokuto::edit::EditNotesWithInstrument(file, 0, track, std::vector{note}, Program{0, 3});
        XSFLIB_CHECK(played.added.size() == 1);
        const hokuto::song::Sequence heard = read(saved(played.file));
        XSFLIB_CHECK(heard.tracks.at(0).notes.size() == 1);
        XSFLIB_CHECK(heard.tracks[0].notes[0].start == tick);
        XSFLIB_CHECK(heard.tracks[0].notes[0].program == 3);
    }

    // The inspector's menus give the track a program before it has notes, and change that program.
    const hokuto::format::File chosen = hokuto::edit::EditOpeningInstrument(file, 0, track, Program{0, 3}).file;
    const hokuto::format::File read_chosen = saved(chosen);
    const hokuto::song::Sequence opened = read(read_chosen);
    XSFLIB_CHECK(opened.tracks.at(0).notes.empty());
    const std::optional<Program> opening = hokuto::edit::OpeningInstrument(read_chosen, 0, opened.tracks[0]);
    XSFLIB_CHECK(opening && *opening == (Program{0, 3}));
    const hokuto::format::File again =
        hokuto::edit::EditOpeningInstrument(read_chosen, 0, opened.tracks[0], Program{0, 5}).file;
    const std::optional<Program> changed = hokuto::edit::OpeningInstrument(again, 0, read(saved(again)).tracks.at(0));
    XSFLIB_CHECK(changed && *changed == (Program{0, 5}));
    XSFLIB_CHECK(again.sequences.at(0)->items.size() == chosen.sequences.at(0)->items.size());

    // A note then plays the track's program.
    const hokuto::edit::NewNote note{.stretch = opened.tracks[0].stretches.at(0), .tick = 96, .length = 48};
    const hokuto::song::Sequence heard =
        read(saved(hokuto::edit::EditNotes(read_chosen, 0, {}, std::vector{note}).file));
    XSFLIB_CHECK(heard.tracks.at(0).notes.size() == 1 && heard.tracks[0].notes[0].program == 3);

    // A track with notes requires EditInstrument.
    const bool refused = !xsflib::testing::ErrorOf<std::invalid_argument>(
                              [&] { hokuto::edit::EditOpeningInstrument(file, 0, heard.tracks[0], Program{0, 3}); })
                              .empty();
    XSFLIB_CHECK(refused);
}

// On the Switch, the Wii U and the 3DS, a new song in a file with a SoundFont's bank plays its first note with the
// bank's program 3 (CheckFirstNote).
void TestFirstNote()
{
    using hokuto::format::Engine;
    using hokuto::format::File;
    const std::vector<uint8_t> font = TestFont();
    for (const Engine engine : {Engine::kSwitch, Engine::kWiiU, Engine::k3ds})
    {
        const File banked = hokuto::edit::AddSoundFont(hokuto::format::NewFile(engine), font, "BANK_S");
        const File added = hokuto::edit::AddSequenceSound(
            banked, std::nullopt, "SEQ_NEW", hokuto::edit::EmptySong("SEQ_NEW"), hokuto::edit::kEmptySongStart, {});
        const File opened = hokuto::edit::WithNewModes(added, std::vector<uint32_t>{0});
        CheckFirstNote(
            opened,
            [](const File& file)
            {
                return hokuto::song::ReadSequence(*hokuto::song::LoadArchive(hokuto::format::ExportArchive(file), "x",
                                                                             hokuto::format::ExportGroupFiles(file)),
                                                  0);
            },
            [&opened](const File& file) { return hokuto::edit::SettleTimelines(opened, file).file; });
    }

    const File wii = hokuto::edit::AddSoundFont(hokuto::format::NewFile(Engine::kWii), font, "BANK_S");
    const auto wii_read = [](const File& file)
    {
        return hokuto::song::ReadSequence(
            *hokuto::song::LoadWiiArchive(hokuto::format::ExportArchive(file), "x", ntdlib::rvl::snd::kRsarOptions), 0);
    };
    const File wii_added = hokuto::edit::WithNewModes(
        hokuto::edit::AddWiiSequenceSound(wii, std::nullopt, "SEQ_NEW", hokuto::edit::EmptySong("SEQ_NEW"),
                                          hokuto::edit::kEmptySongStart, {}),
        std::vector<uint32_t>{0});
    CheckFirstNote(wii_added, wii_read,
                   [&wii_added](const File& file) { return hokuto::edit::SettleTimelines(wii_added, file).file; });

    const File ds = hokuto::edit::AddSoundFont(hokuto::format::NewFile(Engine::kDs), font, "BANK_S");
    const File ds_added =
        hokuto::edit::AddDsSequenceSound(ds, "SEQ_NEW", hokuto::edit::EmptyDsSequenceFile(), {.bank = 1});
    CheckFirstNote(
        ds_added, [](const File& file)
        { return hokuto::song::ReadSequence(*hokuto::song::LoadDsArchive(hokuto::format::ExportArchive(file)), 0); },
        [](const File& file) { return file; });
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("sound_font_imports",
                                    []
                                    {
                                        TestRead();
                                        TestNames();
                                        TestLaterConsole(hokuto::format::Engine::kSwitch, sar::Flavor::kNx, 48000);
                                        TestLaterConsole(hokuto::format::Engine::kWiiU, sar::Flavor::kCafe, 48000);
                                        TestLaterConsole(hokuto::format::Engine::k3ds, sar::Flavor::kCtr, 32728);
                                        TestStereo(hokuto::format::Engine::kSwitch);
                                        TestStereo(hokuto::format::Engine::kWiiU);
                                        TestStereo(hokuto::format::Engine::k3ds);
                                        TestWii();
                                        TestDs();
                                        TestFirstNote();
                                    });
}
