// SPDX-License-Identifier: MIT

// Tests File > Import > Bank… of SFZ files (src/edit/sfz_imports.h). A small set of files in memory names a saw and a
// stereo wave. The expected values come from the opcodes' definitions at sfzformat.com and from the SoundFont 2.04
// specification: SFZ's decay and release fall by a factor of e^-8 in their times (69.49 dB), and a SoundFont's fall
// 100 dB. Hokuto's reader of SoundFonts then returns the generators of each voice.

#include "edit/sfz_imports.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ntdlib/sar/bank_file.h"
#include "seqlib/pcm.h"
#include "seqlib/wav.h"
#include "xsflib/testing/check.h"

#include "edit/banks.h"
#include "edit/sound_font_banks.h"
#include "edit/sound_font_voices.h"
#include "synth/sound_font.h"
#include "test_math.h"

namespace
{

using hokuto::edit::GeneratorOf;
using hokuto::edit::SfzInstrument;
using hokuto::edit::SfzSoundFont;
using hokuto::edit::SoundFontOfSfz;
using hokuto::synth::PresetRecord;
using hokuto::synth::ReadSoundFont;
using hokuto::synth::SoundFontData;
using hokuto::synth::VoiceSetup;
using hokuto::synth::VoicesFor;
using hokuto::testing::Near;
namespace generator = hokuto::edit;

constexpr uint32_t kRate = 32000;
constexpr int kPoints = 4096;

using Files = std::map<std::string, std::vector<uint8_t>>;

std::vector<uint8_t> Bytes(std::string_view text)
{
    return {text.begin(), text.end()};
}

seqlib::Pcm SawPcm(std::size_t channels)
{
    seqlib::Pcm pcm;
    pcm.channels.assign(channels, std::vector<int16_t>(kPoints));
    for (std::size_t channel = 0; channel < channels; channel++)
    {
        for (int i = 0; i < kPoints; i++)
        {
            pcm.channels[channel][static_cast<std::size_t>(i)] = static_cast<int16_t>(((i % 64) - 32) * 800);
        }
    }

    return pcm;
}

// The samples of the tests: a saw, one with a loop in the file, one in stereo whose channels differ, and one in a
// folder.
Files Samples()
{
    Files files;
    files["saw.wav"] = seqlib::WavFile(SawPcm(1), kRate);
    files["sub/my saw.wav"] = seqlib::WavFile(SawPcm(1), kRate);

    seqlib::Pcm looped = SawPcm(1);
    looped.loop = true;
    looped.loop_start = 1024;
    looped.loop_end = 2048;
    files["looped.wav"] = seqlib::WavFile(looped, kRate);

    seqlib::Pcm stereo = SawPcm(2);
    std::ranges::fill(stereo.channels[1], static_cast<int16_t>(0));
    files["stereo.wav"] = seqlib::WavFile(stereo, kRate);

    return files;
}

SfzSoundFont Convert(Files files, const std::string& text)
{
    files["test.sfz"] = Bytes(text);
    const std::vector<SfzInstrument> instruments = {{"test.sfz", 0}};

    return SoundFontOfSfz(instruments,
                          [&files](const std::filesystem::path& path, std::vector<uint8_t>& bytes)
                          {
                              const auto found = files.find(path.generic_string());
                              if (found == files.end())
                              {
                                  return false;
                              }
                              bytes = found->second;

                              return true;
                          });
}

bool Refused(const Files& files, const std::string& text)
{
    return !xsflib::testing::ErrorOf<std::invalid_argument>([&] { Convert(files, text); }).empty();
}

const PresetRecord& PresetAt(const SoundFontData& font, int bank, int program)
{
    const auto found = std::ranges::find_if(
        font.presets, [&](const PresetRecord& p) { return p.bank == bank && p.program == program; });
    if (found == font.presets.end())
    {
        throw std::runtime_error("no such preset");
    }

    return *found;
}

// The one voice of `font` at `key` and `velocity`, or nothing.
std::optional<VoiceSetup> VoiceAt(const SoundFontData& font, int key, int velocity = 100)
{
    const std::vector<VoiceSetup> voices = VoicesFor(font, PresetAt(font, 0, 0), key, velocity);
    XSFLIB_CHECK(voices.size() <= 1);

    return voices.empty() ? std::nullopt : std::optional<VoiceSetup>(voices.front());
}

// One region with most of the opcodes that the import reads: its keys and velocities, its pitch, its volume and pan,
// its envelope in timecents, its exclusive group and its loop.
void TestRegion()
{
    const SfzSoundFont made =
        Convert(Samples(),
                "<region> sample=saw.wav lokey=36 hikey=c4 lovel=20 hivel=90 pitch_keycenter=c#4 "
                "transpose=2 tune=-30 pitch_keytrack=50 volume=-12 pan=-50 "
                "ampeg_delay=0.25 ampeg_attack=0.5 ampeg_hold=0.25 ampeg_decay=0.6949 "
                "ampeg_sustain=10 ampeg_release=1.3898 group=3 off_by=3 off_mode=time off_time=0.03 "
                "loop_mode=loop_continuous loop_start=1000 loop_end=1999");
    SEQLIB_CHECK_EQ(made.regions, std::size_t{1});
    SEQLIB_CHECK_EQ(made.warnings.size(), std::size_t{0});
    const SoundFontData font = ReadSoundFont(made.sf2);

    // lokey and hikey hold their keys, and lovel and hivel their velocities.
    XSFLIB_CHECK(!VoiceAt(font, 35).has_value());
    XSFLIB_CHECK(!VoiceAt(font, 61).has_value());
    XSFLIB_CHECK(!VoiceAt(font, 60, 19).has_value());
    XSFLIB_CHECK(!VoiceAt(font, 60, 91).has_value());
    const std::optional<VoiceSetup> voice = VoiceAt(font, 60, 90);
    SEQLIB_REQUIRE(voice.has_value());

    XSFLIB_CHECK(GeneratorOf(*voice, generator::kRootKey) == 61.0);
    XSFLIB_CHECK(GeneratorOf(*voice, generator::kCoarseTune) == 2.0);
    XSFLIB_CHECK(GeneratorOf(*voice, generator::kFineTune) == -30.0);
    XSFLIB_CHECK(GeneratorOf(*voice, generator::kScaleTuning) == 50.0);
    XSFLIB_CHECK(Near(hokuto::edit::SoundFontAttenuation(*voice), 120.0, 0.5));
    XSFLIB_CHECK(GeneratorOf(*voice, generator::kPan) == -250.0);

    // A time in timecents is 1200 log2 of its seconds. A fall of 0.6949 s in SFZ becomes 1 s in a SoundFont.
    XSFLIB_CHECK(GeneratorOf(*voice, generator::kDelayVolEnv) == -2400.0);
    XSFLIB_CHECK(GeneratorOf(*voice, generator::kAttackVolEnv) == -1200.0);
    XSFLIB_CHECK(GeneratorOf(*voice, generator::kHoldVolEnv) == -2400.0);
    XSFLIB_CHECK(Near(GeneratorOf(*voice, generator::kDecayVolEnv), 0.0, 1.0));
    XSFLIB_CHECK(Near(GeneratorOf(*voice, generator::kReleaseVolEnv), 1200.0, 1.0));

    // A sustain of 10 % of full amplitude is 20 dB below it.
    XSFLIB_CHECK(GeneratorOf(*voice, generator::kSustainVolEnv) == 200.0);
    XSFLIB_CHECK(GeneratorOf(*voice, generator::kExclusiveClass) == 3.0);
    XSFLIB_CHECK(GeneratorOf(*voice, generator::kSampleModes) == 1.0);

    // loop_end is the loop's last sample.
    std::map<std::string, std::size_t> left_out;
    const std::optional<hokuto::edit::SoundFontWave> wave = hokuto::edit::SoundFontWaveOf(font, *voice, left_out);
    XSFLIB_CHECK(wave.has_value());
    if (wave)
    {
        SEQLIB_CHECK_EQ(voice->sample->end - voice->sample->start, uint32_t{kPoints});
        SEQLIB_CHECK_EQ(wave->loop_start - wave->start, int64_t{1000});
        SEQLIB_CHECK_EQ(wave->loop_end - wave->loop_start, int64_t{1000});
    }
    SEQLIB_CHECK_EQ(voice->sample->rate, kRate);
}

void TestDefaults()
{
    // A region without opcodes plays every key at key 60's pitch, from the file's start to its end, without a loop.
    // sfzformat.com lists a default release of 1 ms.
    const SfzSoundFont made = Convert(Samples(), "<region> sample=saw.wav");
    const SoundFontData font = ReadSoundFont(made.sf2);
    const std::optional<VoiceSetup> low = VoiceAt(font, 0, 1);
    const std::optional<VoiceSetup> high = VoiceAt(font, 127, 127);
    SEQLIB_REQUIRE(low.has_value() && high.has_value());
    XSFLIB_CHECK(GeneratorOf(*low, generator::kRootKey) == 60.0);
    XSFLIB_CHECK(GeneratorOf(*low, generator::kSampleModes) == 0.0);
    XSFLIB_CHECK(GeneratorOf(*low, generator::kScaleTuning) == 100.0);
    XSFLIB_CHECK(GeneratorOf(*low, generator::kExclusiveClass) == 0.0);
    XSFLIB_CHECK(GeneratorOf(*low, generator::kAttackVolEnv) == -12000.0);
    XSFLIB_CHECK(GeneratorOf(*low, generator::kHoldVolEnv) == -12000.0);
    XSFLIB_CHECK(GeneratorOf(*low, generator::kDecayVolEnv) == -12000.0);
    XSFLIB_CHECK(GeneratorOf(*low, generator::kSustainVolEnv) == 0.0);
    XSFLIB_CHECK(Near(GeneratorOf(*low, generator::kReleaseVolEnv), 1200.0 * std::log2(0.001 * 100.0 / 69.49), 1.0));
    XSFLIB_CHECK(made.warnings.empty());

    // key sets the keys and the root.
    const SoundFontData single = ReadSoundFont(Convert(Samples(), "<region> sample=saw.wav key=e3").sf2);
    XSFLIB_CHECK(!VoiceAt(single, 51).has_value());
    XSFLIB_CHECK(!VoiceAt(single, 53).has_value());
    const std::optional<VoiceSetup> at_key = VoiceAt(single, 52);
    XSFLIB_CHECK(at_key.has_value());
    if (at_key)
    {
        XSFLIB_CHECK(GeneratorOf(*at_key, generator::kRootKey) == 52.0);
    }
}

// The loop of each region's wave: the file's loop and the loop modes, one_shot, offset and end, a loop outside the part
// that plays, and regions that share a sample or join into one zone.
void TestLoops()
{
    const auto wave_of = [](const std::string& text)
    {
        const SoundFontData font = ReadSoundFont(Convert(Samples(), text).sf2);
        const std::optional<VoiceSetup> voice = VoiceAt(font, 60);
        std::map<std::string, std::size_t> left_out;

        return voice ? hokuto::edit::SoundFontWaveOf(font, *voice, left_out) : std::nullopt;
    };

    // The file's loop plays unless no_loop or one_shot turns it off. loop_continuous on a file without one loops from
    // the start to the end.
    const auto from_file = wave_of("<region> sample=looped.wav");
    XSFLIB_CHECK(from_file && from_file->loop && from_file->loop_start - from_file->start == 1024 &&
                 from_file->loop_end - from_file->start == 2048);
    const auto turned_off = wave_of("<region> sample=looped.wav loop_mode=no_loop");
    XSFLIB_CHECK(turned_off && !turned_off->loop);
    const auto one_shot = wave_of("<region> sample=looped.wav loop_mode=one_shot");
    XSFLIB_CHECK(one_shot && !one_shot->loop);

    // sfzformat.com: a one_shot note plays its sample to the end, whatever the note-off. The new bank's region ignores
    // its note-off, and a region of the same file without it obeys.
    const SfzSoundFont shot = Convert(Samples(),
                                      "<region> sample=saw.wav key=60 loop_mode=one_shot\n"
                                      "<region> sample=saw.wav key=61\n");
    XSFLIB_CHECK(shot.warnings.empty());
    const SoundFontData shots = ReadSoundFont(shot.sf2);
    SEQLIB_REQUIRE(VoiceAt(shots, 60).has_value() && VoiceAt(shots, 61).has_value());
    XSFLIB_CHECK(GeneratorOf(*VoiceAt(shots, 60), hokuto::edit::kPlaysPastRelease) == 1.0);
    XSFLIB_CHECK(GeneratorOf(*VoiceAt(shots, 61), hokuto::edit::kPlaysPastRelease) == 0.0);
    const hokuto::edit::SoundFontBank bank = hokuto::edit::BankOfSoundFont(shot.sf2, ntdlib::sar::Flavor::kCafe);
    const ntdlib::sar::BankFile read = ntdlib::sar::BankFile::Read(bank.bank);
    SEQLIB_REQUIRE(!read.instruments.empty() && read.instruments[0].has_value());
    bool ignores = false;
    bool obeys = false;
    for (const hokuto::edit::Region& region : hokuto::edit::RegionsOf(*read.instruments[0]))
    {
        ignores = ignores || (region.first_key == 60 && region.values.ignore_note_off);
        obeys = obeys || (region.first_key == 61 && !region.values.ignore_note_off);
    }
    XSFLIB_CHECK(ignores && obeys);
    const auto whole = wave_of("<region> sample=saw.wav loop_mode=loop_continuous");
    XSFLIB_CHECK(whole && whole->loop && whole->loop_start == whole->start && whole->loop_end == whole->end);

    // offset and end cut the sample, and its loop counts from the file's start.
    const auto cut = wave_of("<region> sample=looped.wav offset=512 end=3071");
    XSFLIB_CHECK(cut && cut->loop && cut->loop_start - cut->start == 512 && cut->loop_end - cut->start == 1536);
    const auto cut_alone = wave_of("<region> sample=saw.wav offset=512 end=3071");
    XSFLIB_CHECK(cut_alone && !cut_alone->loop && cut_alone->end - cut_alone->start == 2560);

    // A loop outside the part that plays is lost, with a word for it.
    const SfzSoundFont outside = Convert(Samples(), "<region> sample=looped.wav end=1500");
    const SoundFontData outside_font = ReadSoundFont(outside.sf2);
    const std::optional<VoiceSetup> outside_voice = VoiceAt(outside_font, 60);
    XSFLIB_CHECK(outside_voice && GeneratorOf(*outside_voice, generator::kSampleModes) == 0.0);
    SEQLIB_CHECK_EQ(outside.warnings.size(), std::size_t{1});

    // Two regions of one part of a sample share it.
    const SoundFontData shared =
        ReadSoundFont(Convert(Samples(), "<region> sample=saw.wav key=60 <region> sample=saw.wav key=62").sf2);
    SEQLIB_CHECK_EQ(shared.samples.size(), std::size_t{1});

    // Regions that differ only in what stays out, at keys in a row, become one zone. They keep their keys, and the same
    // root. The two zones of the stereo sample join the zones next to them too.
    const SoundFontData joined =
        ReadSoundFont(Convert(Samples(),
                              "<region> sample=saw.wav lokey=60 hikey=61 pitch_keycenter=60 cutoff=100 "
                              "<region> sample=saw.wav lokey=62 hikey=63 pitch_keycenter=60 cutoff=200 "
                              "<region> sample=saw.wav lokey=65 hikey=65 pitch_keycenter=60 "
                              "<region> sample=stereo.wav lokey=70 hikey=70 pitch_keycenter=60 "
                              "<region> sample=stereo.wav lokey=71 hikey=71 pitch_keycenter=60")
                          .sf2);
    SEQLIB_REQUIRE(joined.instruments.size() == 1);
    SEQLIB_CHECK_EQ(joined.instruments.front().zones.size(), std::size_t{4});
    XSFLIB_CHECK(VoiceAt(joined, 63).has_value());
    XSFLIB_CHECK(!VoiceAt(joined, 64).has_value());
    SEQLIB_CHECK_EQ(VoicesFor(joined, PresetAt(joined, 0, 0), 71, 100).size(), std::size_t{2});
}

void TestSections()
{
    // A region inherits what the sections above it set, and a <group> or a <master> starts afresh.
    const std::string text =
        "// a comment\n"
        "/* a comment\nacross lines */\n"
        "#define $KEY 64\n"
        "<control> default_path=sub/ note_offset=12 octave_offset=-1\n"
        "<global> volume=-6\n"
        "<master> pan=20\n"
        "<group> tune=10 lovel=1\n"
        "<region> sample=my saw.wav key=$KEY // a sample's name can hold a space\n"
        "<region> sample=my saw.wav key=66 tune=20\n"
        "<group> transpose=1\n"
        "<region> sample=my saw.wav key=68\n"
        "<master>\n"
        "<region> sample=my saw.wav key=70\n"
        "<global>\n"
        "<region> sample=my saw.wav key=72\n";
    const SfzSoundFont made = Convert(Samples(), text);
    SEQLIB_CHECK_EQ(made.regions, std::size_t{5});
    const SoundFontData font = ReadSoundFont(made.sf2);

    // note_offset=12 and octave_offset=-1 cancel out.
    const std::optional<VoiceSetup> first = VoiceAt(font, 64);
    const std::optional<VoiceSetup> second = VoiceAt(font, 66);
    const std::optional<VoiceSetup> third = VoiceAt(font, 68);
    const std::optional<VoiceSetup> fourth = VoiceAt(font, 70);
    const std::optional<VoiceSetup> fifth = VoiceAt(font, 72);
    SEQLIB_REQUIRE(first && second && third && fourth && fifth);
    XSFLIB_CHECK(GeneratorOf(*first, generator::kRootKey) == 64.0);
    XSFLIB_CHECK(GeneratorOf(*first, generator::kFineTune) == 10.0);
    XSFLIB_CHECK(GeneratorOf(*first, generator::kPan) == 100.0);
    XSFLIB_CHECK(Near(hokuto::edit::SoundFontAttenuation(*first), 60.0, 0.5));
    XSFLIB_CHECK(GeneratorOf(*second, generator::kFineTune) == 20.0);
    XSFLIB_CHECK(GeneratorOf(*third, generator::kFineTune) == 0.0);
    XSFLIB_CHECK(GeneratorOf(*third, generator::kCoarseTune) == 1.0);
    XSFLIB_CHECK(GeneratorOf(*fourth, generator::kCoarseTune) == 0.0);
    XSFLIB_CHECK(GeneratorOf(*fourth, generator::kPan) == 0.0);
    XSFLIB_CHECK(Near(hokuto::edit::SoundFontAttenuation(*fourth), 60.0, 0.5));
    XSFLIB_CHECK(Near(hokuto::edit::SoundFontAttenuation(*fifth), 0.0, 0.5));

    // An included file's opcodes count where it stands.
    Files files = Samples();
    files["common.sfz"] = Bytes("<group> tune=33\n");
    const SoundFontData included =
        ReadSoundFont(Convert(files, "#include \"common.sfz\"\n<region> sample=saw.wav").sf2);
    const std::optional<VoiceSetup> included_voice = VoiceAt(included, 60);
    XSFLIB_CHECK(included_voice && GeneratorOf(*included_voice, generator::kFineTune) == 33.0);

    // A file that includes itself stops with a message.
    files["loop.sfz"] = Bytes("#include \"loop.sfz\"\n");
    XSFLIB_CHECK(Refused(files, "#include \"loop.sfz\"\n<region> sample=saw.wav"));
}

void TestNoteOffset()
{
    // sfzformat.com defines note_offset as an offset of the notes that come in: note 48 plus 12 plays the region at
    // key 60.
    const SoundFontData font = ReadSoundFont(
        Convert(Samples(), "<control> note_offset=12 <region> sample=saw.wav key=60 pitch_keycenter=62").sf2);
    XSFLIB_CHECK(!VoiceAt(font, 60).has_value());
    const std::optional<VoiceSetup> voice = VoiceAt(font, 48);
    XSFLIB_CHECK(voice.has_value());
    if (voice)
    {
        XSFLIB_CHECK(GeneratorOf(*voice, generator::kRootKey) == 50.0);
    }
}

void TestVolumes()
{
    // A SoundFont stores no level above full: the whole file is imported 6 dB lower.
    const SfzSoundFont made = Convert(Samples(),
                                      "<region> sample=saw.wav key=60 volume=6 <region> sample=saw.wav key=61 volume=0 "
                                      "<region> sample=saw.wav key=62 amplitude=50");
    const SoundFontData font = ReadSoundFont(made.sf2);
    const std::optional<VoiceSetup> loud = VoiceAt(font, 60);
    const std::optional<VoiceSetup> plain = VoiceAt(font, 61);
    const std::optional<VoiceSetup> half = VoiceAt(font, 62);
    XSFLIB_CHECK(loud && plain && half);
    if (loud && plain && half)
    {
        XSFLIB_CHECK(Near(hokuto::edit::SoundFontAttenuation(*loud), 0.0, 0.5));
        XSFLIB_CHECK(Near(hokuto::edit::SoundFontAttenuation(*plain), 60.0, 0.5));
        XSFLIB_CHECK(Near(hokuto::edit::SoundFontAttenuation(*half), 120.2, 0.5));
    }
    SEQLIB_CHECK_EQ(made.warnings.size(), std::size_t{1});
}

void TestLeftOut()
{
    // A release, a later turn of a round robin and a later random choice play no note on.
    const SfzSoundFont made = Convert(Samples(),
                                      "<region> sample=saw.wav key=60 trigger=release\n"
                                      "<region> sample=saw.wav key=61 seq_length=2 seq_position=1\n"
                                      "<region> sample=saw.wav key=61 seq_length=2 seq_position=2\n"
                                      "<region> sample=saw.wav key=62 lorand=0 hirand=0.5\n"
                                      "<region> sample=saw.wav key=62 lorand=0.5 hirand=1\n"
                                      "<region> sample=saw.wav key=63 cutoff=1000 resonance=3 ampeg_attackcc1=2\n"
                                      "<region> sample=saw.wav key=64 end=-1\n"
                                      "<region> sample=*sine key=65\n"
                                      "<region> sample=missing.wav key=66\n");
    SEQLIB_CHECK_EQ(made.regions, std::size_t{3});
    const SoundFontData font = ReadSoundFont(made.sf2);
    XSFLIB_CHECK(!VoiceAt(font, 60).has_value());
    XSFLIB_CHECK(VoiceAt(font, 61).has_value());
    XSFLIB_CHECK(VoiceAt(font, 62).has_value());
    XSFLIB_CHECK(VoiceAt(font, 63).has_value());
    XSFLIB_CHECK(!VoiceAt(font, 64).has_value());
    XSFLIB_CHECK(!VoiceAt(font, 65).has_value());
    XSFLIB_CHECK(!VoiceAt(font, 66).has_value());
    const auto says = [&made](std::string_view words)
    {
        return std::ranges::any_of(made.warnings,
                                   [words](const std::string& line) { return line.find(words) != std::string::npos; });
    };
    XSFLIB_CHECK(says("release triggers"));
    XSFLIB_CHECK(says("round robins"));
    XSFLIB_CHECK(says("random choices"));
    XSFLIB_CHECK(says("filters"));
    XSFLIB_CHECK(says("CC opcodes"));
    XSFLIB_CHECK(says("end=-1"));
    XSFLIB_CHECK(says("*sine"));
    XSFLIB_CHECK(says("missing.wav"));

    // A stereo sample becomes a voice for each channel, at its side. The right channel of this one is silent.
    const SfzSoundFont stereo = Convert(Samples(), "<region> sample=stereo.wav");
    const SoundFontData stereo_font = ReadSoundFont(stereo.sf2);
    SEQLIB_CHECK_EQ(stereo.regions, std::size_t{1});
    XSFLIB_CHECK(stereo.warnings.empty());
    const std::vector<VoiceSetup> sides = VoicesFor(stereo_font, PresetAt(stereo_font, 0, 0), 60, 100);
    SEQLIB_REQUIRE(sides.size() == 2);
    XSFLIB_CHECK(GeneratorOf(sides[0], generator::kPan) == -500.0);
    XSFLIB_CHECK(GeneratorOf(sides[1], generator::kPan) == 500.0);
    SEQLIB_CHECK_EQ(sides[0].sample->type, 4);
    SEQLIB_CHECK_EQ(sides[1].sample->type, 2);
    SEQLIB_CHECK_EQ(stereo_font.points.at(sides[0].sample->start), -32 * 800);
    SEQLIB_CHECK_EQ(stereo_font.points.at(sides[1].sample->start), 0);
    const std::optional<hokuto::edit::StereoVoices> pair = hokuto::edit::StereoVoicesOf(stereo_font, sides);
    XSFLIB_CHECK(pair.has_value());
    if (pair)
    {
        XSFLIB_CHECK(pair->left == &sides[0] && pair->right == &sides[1]);
    }

    // Files without a region that plays say why.
    XSFLIB_CHECK(Refused(Samples(), "<region> sample=missing.wav"));
    XSFLIB_CHECK(Refused(Samples(), "<group> tune=3"));
}

void TestTriggers()
{
    // trigger=first plays a note that starts while no other note plays, and stays in. trigger=legato plays only while
    // another note plays, and release_key at a note-off. Both stay out.
    const SfzSoundFont made = Convert(Samples(),
                                      "<region> sample=saw.wav key=60 trigger=first\n"
                                      "<region> sample=saw.wav key=60 trigger=legato\n"
                                      "<region> sample=saw.wav key=61 trigger=attack\n"
                                      "<region> sample=saw.wav key=62 trigger=release_key\n");

    SEQLIB_CHECK_EQ(made.regions, std::size_t{2});
    const SoundFontData font = ReadSoundFont(made.sf2);
    XSFLIB_CHECK(VoiceAt(font, 60).has_value());
    XSFLIB_CHECK(VoiceAt(font, 61).has_value());
    XSFLIB_CHECK(!VoiceAt(font, 62).has_value());
    const auto says = [&made](std::string_view words)
    {
        return std::ranges::any_of(made.warnings,
                                   [words](const std::string& line) { return line.find(words) != std::string::npos; });
    };
    XSFLIB_CHECK(says("1 region: legato triggers"));
    XSFLIB_CHECK(says("1 region: release triggers"));
}

void TestFilesBeside()
{
    // Export… names a bank's files after its programs. The others come in with the chosen one.
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() / ("hokuto_sfz_imports_test_" + std::to_string(std::random_device{}()));
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    for (const std::string name :
         {"BANK_GM_000.sfz", "BANK_GM_005.sfz", "BANK_GM_130.SFZ", "BANK_DR_001.sfz", "BANK_GM_x.sfz"})
    {
        std::ofstream(folder / name) << "\n";
    }
    const std::vector<SfzInstrument> beside = hokuto::edit::SfzInstrumentsBeside(folder / "BANK_GM_005.sfz");
    SEQLIB_CHECK_EQ(beside.size(), std::size_t{3});
    if (beside.size() == 3)
    {
        SEQLIB_CHECK_EQ(beside[0].program, uint32_t{0});
        SEQLIB_CHECK_EQ(beside[1].program, uint32_t{5});
        SEQLIB_CHECK_EQ(beside[2].program, uint32_t{130});
    }
    XSFLIB_CHECK(hokuto::edit::SfzImportName(beside, "BANK_GM_005.sfz") == std::string("BANK_GM.sfz"));

    const std::vector<SfzInstrument> alone = hokuto::edit::SfzInstrumentsBeside(folder / "BANK_GM_x.sfz");
    SEQLIB_CHECK_EQ(alone.size(), std::size_t{1});
    if (alone.size() == 1)
    {
        SEQLIB_CHECK_EQ(alone[0].program, uint32_t{0});
    }
    XSFLIB_CHECK(hokuto::edit::SfzImportName(alone, "BANK_GM_x.sfz") == std::string("BANK_GM_x.sfz"));
    std::filesystem::remove_all(folder);

    XSFLIB_CHECK(hokuto::edit::IsSfzName("a.sfz"));
    XSFLIB_CHECK(hokuto::edit::IsSfzName("a.SFZ"));
    XSFLIB_CHECK(!hokuto::edit::IsSfzName("a.sf2"));

    // Programs from 128 on are in the next MIDI banks of a SoundFont.
    Files files = Samples();
    files["a_130.sfz"] = Bytes("<region> sample=saw.wav");
    const std::vector<SfzInstrument> instruments = {{"a_130.sfz", 130}};
    const SfzSoundFont made = SoundFontOfSfz(instruments,
                                             [&files](const std::filesystem::path& path, std::vector<uint8_t>& bytes)
                                             {
                                                 bytes = files.at(path.generic_string());

                                                 return true;
                                             });
    const SoundFontData font = ReadSoundFont(made.sf2);
    SEQLIB_CHECK_EQ(font.presets.size(), std::size_t{1});
    XSFLIB_CHECK(PresetAt(font, 1, 2).name == "a_130");
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("sfz_imports",
                                    []
                                    {
                                        TestRegion();
                                        TestDefaults();
                                        TestLoops();
                                        TestSections();
                                        TestNoteOffset();
                                        TestVolumes();
                                        TestLeftOut();
                                        TestTriggers();
                                        TestFilesBeside();
                                    });
}
