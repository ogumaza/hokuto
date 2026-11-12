// SPDX-License-Identifier: MIT

// Tests the effects, the aux buses, the scenes, the mixing modes and the engines of the sounds in both forms of
// Hokuto's files. A Wii U file holds the console's four effects. Both forms refuse a reverb's mode outside its choices.
// The aux buses' volumes come back through both forms, and both refuse a volume outside 0 to 2.

#include "format_testing.h"

namespace
{

// The effects' lines of the text form take their parameters in any order, with 0 for each one that a line leaves out. A
// value rounds to the nearest float. A reader refuses lines with mistakes, and an effect after the sequences. The
// binary form refuses an effect of a later version, a parameter short and a parameter that isn't a number. Neither form
// writes an effect with a parameter short or an infinite one.
void TestEffects()
{
    const std::string kHeader = "hok 1.1\nengine switch\n";
    const auto effects = [&](std::string_view lines)
    {
        return format::ReadText(kHeader + std::string(lines)).aux_effects;
    };

    // Parameters in any order, with 0 for each one that a line leaves out. A bus keeps the order of its lines.
    const auto read = effects(
        "effect C i3dl2_reverb dry_gain 0.5 room_gain -1000\neffect A i3dl2_reverb\n"
        "effect C i3dl2_reverb late_reverb_delay 1e-3\n");
    std::vector<float> first(12, 0.0f);
    first[0] = -1000.0f;
    first[11] = 0.5f;
    std::vector<float> second(12, 0.0f);
    second[7] = 0.001f;
    const std::vector<Effect> kBusA = {{EffectKind::kI3dl2Reverb, std::vector<float>(12, 0.0f)}};
    const std::vector<Effect> kBusC = {{EffectKind::kI3dl2Reverb, first}, {EffectKind::kI3dl2Reverb, second}};
    XSFLIB_CHECK(read[0] == kBusA && read[1].empty() && read[2] == kBusC);

    // A value rounds to the nearest float, up to the largest.
    const auto value = [&](std::string_view number)
    {
        return effects("effect A i3dl2_reverb dry_gain " + std::string(number) + "\n")[0][0].parameters[11];
    };
    XSFLIB_CHECK(value("0.1") == 0.1f && value("1E3") == 1000.0f && value(".5") == 0.5f && value("7.") == 7.0f);
    XSFLIB_CHECK(value("1e-46") == 0.0f && value("8e-46") == std::numeric_limits<float>::denorm_min());
    XSFLIB_CHECK(value("3.4028235677e38") == std::numeric_limits<float>::max());
    XSFLIB_CHECK(value("-0") == 0.0f && std::signbit(value("-0")));

    // Lines that a reader refuses.
    const auto error = [&](std::string_view line)
    {
        return TextError(kHeader + std::string(line) + "\n");
    };
    XSFLIB_CHECK(error("effect D i3dl2_reverb").starts_with("Line 3: an effect's line is effect, a bus from A to C"));
    XSFLIB_CHECK(error("effect A").starts_with("Line 3: an effect's line"));
    XSFLIB_CHECK(error("effect A echo").starts_with("Line 3: there's no effect named echo"));
    XSFLIB_CHECK(
        error("effect A i3dl2_reverb loudness 3").starts_with("Line 3: the effect has no parameter named loudness"));
    XSFLIB_CHECK(
        error("effect A i3dl2_reverb dry_gain 1 dry_gain 2").starts_with("Line 3: the effect sets dry_gain twice"));
    XSFLIB_CHECK(error("effect A i3dl2_reverb dry_gain").starts_with("Line 3: dry_gain has to be a number"));
    for (const std::string_view number :
         {"3.4028235678e38", "1e39", "+1", "0,5", "1x", "\"1\"", "inf", "nan", "0x10", "-"})
    {
        XSFLIB_CHECK(error("effect A i3dl2_reverb dry_gain " + std::string(number))
                         .starts_with("Line 3: a parameter's value is a decimal number within a float's range"));
    }
    XSFLIB_CHECK(TextError(kHeader + "sequence 0\n    end\neffect A i3dl2_reverb\n")
                     .starts_with("Line 5: the effects come before the sequences"));

    // Effects that the binary form refuses: a kind of a later version, a parameter short, and a parameter that isn't a
    // number. MIXR's byte 4 holds the first effect's kind and byte 8 the number of its parameters. A third byte of 0xFF
    // makes its second parameter, the largest float, a NaN.
    const std::vector<uint8_t> binary = format::Write(Unusual());
    XSFLIB_CHECK(Patched(binary, 1, 4, 17).starts_with("The file contains an effect (17) that this version"));
    XSFLIB_CHECK(Patched(binary, 1, 8, 11) ==
                 "The file is damaged: an effect on aux bus A has 11 parameters, and its kind has 12.");
    XSFLIB_CHECK(Patched(binary, 1, 18, 0xFF) ==
                 "The file is damaged: an effect on aux bus A has a parameter that isn't a finite number.");
    XSFLIB_CHECK(Patched(binary, 1, 4, 1).empty());

    // Effects that neither form can hold.
    File short_effect = Unusual();
    short_effect.aux_effects[1] = {{EffectKind::kI3dl2Reverb, {1.0f}}};
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { format::Write(short_effect); }) ==
                 "an effect on aux bus B has 1 parameter, and its kind has 12");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { format::WriteText(short_effect); }) ==
                 "an effect on aux bus B has 1 parameter, and its kind has 12");
    File infinite = Unusual();
    infinite.aux_effects[2][1].parameters[3] = std::numeric_limits<float>::infinity();
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { format::Write(infinite); }) ==
                 "an effect on aux bus C has a parameter that isn't a finite number");
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { format::WriteText(infinite); }));
}

// The Wii U's four effects in both forms. MIXR gives each effect's kind and the number of its parameters before them.
// Neither form holds a reverb's mode that isn't a whole number among its choices. A Switch file holds the Wii U's
// effects too.
void TestWiiUEffects()
{
    File file;
    file.engine = format::Engine::kWiiU;
    file.aux_effects[0] = {
        {EffectKind::kReverbHi, {0.02f, 3.0f, 0.6f, 0.4f, 0.1f, 1.0f, 7.0f, 0.02f, 5.0f, 0.0f, 1.0f}},
        {EffectKind::kChorus, {10.0f, 0.5f, 1.0f, 0.0f, 1.0f}}};
    file.aux_effects[1] = {{EffectKind::kReverbStd, {0.05f, 1.5f, 0.5f, 0.5f, 0.8f, 0.0f, 0.05f, 2.0f, 0.25f, 0.75f}}};
    file.aux_effects[2] = {{EffectKind::kDelay, {160.0f, 0.4f, 1.0f, 500.0f, 1.0f}}};

    const std::vector<uint8_t> binary = format::Write(file);
    const std::string text = format::WriteText(file);

    // Both forms hold the four kinds. MIXR gives each effect's kind and the number of its parameters before them: the
    // high reverb's 11 at 4, the chorus's 5 at 56, the standard reverb's 10 at 88 and the delay's 5 at 140.
    XSFLIB_CHECK(format::Read(binary) == file && format::ReadText(text) == file);
    const std::span<const uint8_t> mixer = ChunkBytes(binary, 1);
    SEQLIB_REQUIRE(mixer.size() == 168);
    XSFLIB_CHECK(xsflib::LoadLe32(&mixer[0]) == 2);
    XSFLIB_CHECK(mixer[4] == 2 && xsflib::LoadLe32(&mixer[8]) == 11 && mixer[56] == 4 &&
                 xsflib::LoadLe32(&mixer[60]) == 5);
    XSFLIB_CHECK(xsflib::LoadLe32(&mixer[84]) == 1 && mixer[88] == 3 && xsflib::LoadLe32(&mixer[92]) == 10);
    XSFLIB_CHECK(xsflib::LoadLe32(&mixer[136]) == 1 && mixer[140] == 5 && xsflib::LoadLe32(&mixer[144]) == 5);
    XSFLIB_CHECK(text.find("\neffect A reverb_hi pre_delay_time 0.02 fused_time 3 coloration 0.6 damping 0.4 "
                           "crosstalk 0.1 out_gain 1 early_mode 7 pre_delay_time_max 0.02 fused_mode 5 early_gain 0 "
                           "fused_gain 1\neffect A chorus delay_time 10 depth 0.5 rate 1 feedback 0 out_gain 1\n"
                           "effect B reverb_std pre_delay_time 0.05 fused_time 1.5 coloration 0.5 damping 0.5 "
                           "out_gain 0.8 early_mode 0 pre_delay_time_max 0.05 fused_mode 2 early_gain 0.25 "
                           "fused_gain 0.75\neffect C delay delay 160 feedback 0.4 out_gain 1 max_delay 500 lpf 1\n") !=
                 std::string::npos);

    // Modes that neither form holds: a fraction, one past the last choice, and a negative one. The binary form's early
    // mode is the high reverb's parameter 6, at 36 in MIXR. A third byte of 0xF0 makes its 7 a 7.5.
    File fraction = file;
    fraction.aux_effects[0][0].parameters[6] = 2.5f;
    File past = file;
    past.aux_effects[1][0].parameters[7] = 6.0f;
    File negative = file;
    negative.aux_effects[0][0].parameters[8] = -1.0f;
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { format::Write(fraction); }) ==
                 "an effect on aux bus A has a value of early_mode that isn't a whole number from 0 to 7");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { format::WriteText(past); }) ==
                 "an effect on aux bus B has a value of fused_mode that isn't a whole number from 0 to 5");
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { format::Write(negative); }));
    XSFLIB_CHECK(Patched(binary, 1, 38, 0xF0) ==
                 "The file is damaged: an effect on aux bus A has a value of early_mode that isn't a whole number "
                 "from 0 to 7.");
    const std::string kHeader = "hok 1.0\nengine wii_u\n";
    XSFLIB_CHECK(TextError(kHeader + "effect A reverb_std fused_mode 6\n") ==
                 "Line 3: the effect has a value of fused_mode that isn't a whole number from 0 to 5.");
    XSFLIB_CHECK(
        TextError(kHeader + "effect A reverb_hi early_mode 0.5\n").starts_with("Line 3: the effect has a value"));
    XSFLIB_CHECK(TextError(kHeader + "effect A reverb_hi early_mode -0\n").empty());

    // A file holds an effect of the other console too.
    File switch_file;
    switch_file.aux_effects = file.aux_effects;
    XSFLIB_CHECK(format::Read(format::Write(switch_file)) == switch_file);
}

// The Wii's effects in both forms: each kind's code, then its parameters. A reverb of 2008 holds whole modes, and the
// other kinds hold none.
void TestWiiEffects()
{
    File file;
    file.aux_effects[0] = {{EffectKind::kWiiReverbHi, {0.02f, 3.0f, 0.6f, 0.4f, 0.1f, 1.0f}},
                           {EffectKind::kWiiChorus, {10.0f, 0.5f, 1.0f}}};
    file.aux_effects[1] = {
        {EffectKind::kWiiReverbStd2008, {0.05f, 1.5f, 0.5f, 0.5f, 0.8f, 0.0f, 0.05f, 2.0f, 0.25f, 0.75f}},
        {EffectKind::kWiiReverbStd, {0.02f, 3.0f, 0.6f, 0.4f, 1.0f}}};
    file.aux_effects[2] = {
        {EffectKind::kWiiDelay, {160.0f, 0.4f, 1.0f}},
        {EffectKind::kWiiReverbHi2008, {0.02f, 3.0f, 0.6f, 0.4f, 0.1f, 1.0f, 7.0f, 0.02f, 5.0f, 0.0f, 1.0f}}};

    const std::vector<uint8_t> binary = format::Write(file);
    const std::string text = format::WriteText(file);

    // MIXR gives bus A's count, then the high reverb's code 6 and its 6 parameters at 4, and the chorus's code 8 and
    // its 3 parameters at 36.
    XSFLIB_CHECK(format::Read(binary) == file && format::ReadText(text) == file);
    const std::span<const uint8_t> mixer = ChunkBytes(binary, 1);
    SEQLIB_REQUIRE(mixer.size() >= 44);
    XSFLIB_CHECK(xsflib::LoadLe32(&mixer[0]) == 2 && mixer[4] == 6 && xsflib::LoadLe32(&mixer[8]) == 6);
    XSFLIB_CHECK(mixer[36] == 8 && xsflib::LoadLe32(&mixer[40]) == 3);
    XSFLIB_CHECK(text.find("\neffect A wii_reverb_hi pre_delay_time 0.02 fused_time 3 coloration 0.6 damping 0.4 "
                           "crosstalk 0.1 out_gain 1\neffect A wii_chorus delay_time 10 depth 0.5 rate 1\n") !=
                 std::string::npos);
    XSFLIB_CHECK(text.find("\neffect C wii_delay delay 160 feedback 0.4 out_gain 1\neffect C wii_reverb_hi_2008 ") !=
                 std::string::npos);

    // A mode of a reverb of 2008 is a whole number from 0, as on the Wii U.
    File fraction = file;
    fraction.aux_effects[2][1].parameters[8] = 1.5f;
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { format::Write(fraction); }) ==
                 "an effect on aux bus C has a value of fused_mode that isn't a whole number from 0 to 5");
    XSFLIB_CHECK(TextError("hok 1.0\nengine wii\neffect A wii_chorus delay_time 10 depth 0.5 rate 1 feedback 0\n")
                     .starts_with("Line 3: "));
}

// The 3DS's effects are 12 to 16, with the fields of their classes' Param, and a reverb's filter sizes in place of the
// pointer to them.
void TestCtrEffectKinds()
{
    for (const auto& [kind, code, name, parameters] :
         {EffectKindCode{EffectKind::kCtrReverb, 12, "3ds_reverb", 12},
          EffectKindCode{EffectKind::kCtrDelay, 13, "3ds_delay", 4},
          EffectKindCode{EffectKind::kCtrNnReverb, 14, "3ds_nn_reverb", 11},
          EffectKindCode{EffectKind::kCtrNnDelay, 15, "3ds_nn_delay", 4},
          EffectKindCode{EffectKind::kCtrNnReverb2011, 16, "3ds_nn_reverb_2011", 10}})
    {
        XSFLIB_CHECK(format::CodeOf(kind) == code && format::EffectKindOfCode(code) == kind);
        XSFLIB_CHECK(format::NameOf(kind) == name && format::EffectKindNamed(name) == kind);
        XSFLIB_CHECK(format::ParameterNamesOf(kind).size() == parameters && format::IsCtrKind(kind));
    }
    XSFLIB_CHECK(!format::IsCtrKind(EffectKind::kI3dl2Reverb) && !format::IsCtrKind(EffectKind::kDelay) &&
                 !format::IsCtrKind(EffectKind::kWiiDelay));
    const std::span<const std::string_view> reverb = format::ParameterNamesOf(EffectKind::kCtrReverb);
    XSFLIB_CHECK(reverb[0] == "early_reflection_time" && reverb[1] == "fused_time" && reverb[5] == "comb_size_0" &&
                 reverb[7] == "all_pass_size" && reverb[10] == "surround" && reverb[11] == "high_pass_damping");
    XSFLIB_CHECK(format::ParameterNamesOf(EffectKind::kCtrNnReverb)[10] == "high_pass_damping");
    XSFLIB_CHECK(format::ParameterNamesOf(EffectKind::kCtrNnReverb2011)[9] == "fused_gain");
    const std::span<const std::string_view> delay = format::ParameterNamesOf(EffectKind::kCtrNnDelay);
    XSFLIB_CHECK(delay[0] == "delay_time" && delay[1] == "feedback" && delay[2] == "damping" && delay[3] == "surround");
}

// Their times, sizes and gains have limits, and their switches two choices.
void TestCtrEffectLimits()
{
    // The switches have two choices. The times are whole milliseconds and the sizes whole frames of 160 samples.
    XSFLIB_CHECK(format::ChoicesOf(EffectKind::kCtrReverb, 10) == 2 &&
                 format::ChoicesOf(EffectKind::kCtrReverb, 11) == 2 &&
                 format::ChoicesOf(EffectKind::kCtrNnReverb, 10) == 2 &&
                 format::ChoicesOf(EffectKind::kCtrDelay, 3) == 2 && format::ChoicesOf(EffectKind::kCtrReverb, 9) == 0);

    const auto limits = [](EffectKind kind, std::size_t parameter)
    {
        const std::optional<format::ParameterLimits> found = format::LimitsOf(kind, parameter);
        return found ? std::array{found->low, found->high, found->step} : std::array{-1.0f, -1.0f, -1.0f};
    };
    XSFLIB_CHECK((limits(EffectKind::kCtrReverb, 0) == std::array{0.0f, 1000.0f, 1.0f}));
    XSFLIB_CHECK((limits(EffectKind::kCtrReverb, 1) == std::array{0.0f, 20000.0f, 1.0f}));
    XSFLIB_CHECK((limits(EffectKind::kCtrNnReverb2011, 7) == std::array{160.0f, 32000.0f, 160.0f}));
    XSFLIB_CHECK((limits(EffectKind::kCtrNnReverb, 9) == std::array{0.0f, 1.0f, 0.0f}));
    XSFLIB_CHECK((limits(EffectKind::kCtrDelay, 0) == std::array{1.0f, 5000.0f, 1.0f}));
    XSFLIB_CHECK(!format::LimitsOf(EffectKind::kCtrReverb, 10) && !format::LimitsOf(EffectKind::kCtrReverb, 12) &&
                 !format::LimitsOf(EffectKind::kReverbHi, 0));
}

// A file of the 3DS with each kind of effect on buses A and B, in both forms.
struct CtrEffectsFixture
{
    File file;
    std::vector<uint8_t> binary;
    std::string text;
};

// The fixture of the tests of the 3DS's effects in a file.
CtrEffectsFixture MakeCtrEffectsFixture()
{
    File file;
    file.engine = format::Engine::k3ds;
    file.aux_effects[0] = {{EffectKind::kCtrReverb,
                            {0.0f, 4600.0f, 1.0f, 0.5f, 0.91f, 1120.0f, 1440.0f, 4960.0f, 0.3f, 0.73f, 0.0f, 1.0f}},
                           {EffectKind::kCtrDelay, {250.0f, 0.4f, 0.5f, 1.0f}}};
    file.aux_effects[1] = {
        {EffectKind::kCtrNnReverb, {60.0f, 4000.0f, 100.0f, 0.5f, 0.4f, 3040.0f, 3680.0f, 2080.0f, 0.6f, 0.4f, 1.0f}},
        {EffectKind::kCtrNnDelay, {1.0f, 1.0f, -0.0f, 0.0f}},
        {EffectKind::kCtrNnReverb2011,
         {1000.0f, 20000.0f, 1000.0f, 1.0f, 1.0f, 160.0f, 32000.0f, 32000.0f, 1.0f, 1.0f}}};
    const std::vector<uint8_t> binary = format::Write(file);
    const std::string text = format::WriteText(file);

    return {file, binary, text};
}

// Both forms of a file with each kind of the 3DS's effects.
void TestCtrEffectForms()
{
    const CtrEffectsFixture fixture = MakeCtrEffectsFixture();
    const File& file = fixture.file;
    const std::vector<uint8_t>& binary = fixture.binary;
    const std::string& text = fixture.text;

    // Both forms hold the five kinds. MIXR gives each effect's kind and the number of its parameters before them: the
    // reverb's 12 at 4, the delay's 4 at 60, then on bus B the reverbs' 11 at 88 and 10 at 164, around the delay's 4 at
    // 140.
    XSFLIB_CHECK(format::Read(binary) == file && format::ReadText(text) == file);
    const std::span<const uint8_t> mixer = ChunkBytes(binary, 1);
    SEQLIB_REQUIRE(mixer.size() == 216);
    XSFLIB_CHECK(xsflib::LoadLe32(&mixer[0]) == 2);
    XSFLIB_CHECK(mixer[4] == 12 && xsflib::LoadLe32(&mixer[8]) == 12 && mixer[60] == 13 &&
                 xsflib::LoadLe32(&mixer[64]) == 4);
    XSFLIB_CHECK(xsflib::LoadLe32(&mixer[84]) == 3 && mixer[88] == 14 && xsflib::LoadLe32(&mixer[92]) == 11);
    XSFLIB_CHECK(mixer[140] == 15 && xsflib::LoadLe32(&mixer[144]) == 4 && mixer[164] == 16 &&
                 xsflib::LoadLe32(&mixer[168]) == 10 && xsflib::LoadLe32(&mixer[212]) == 0);
    XSFLIB_CHECK(text.find("\neffect A 3ds_reverb early_reflection_time 0 fused_time 4600 pre_delay_time 1 "
                           "coloration 0.5 damping 0.91 comb_size_0 1120 comb_size_1 1440 all_pass_size 4960 "
                           "early_gain 0.3 fused_gain 0.73 surround 0 high_pass_damping 1\neffect A 3ds_delay "
                           "delay_time 250 feedback 0.4 damping 0.5 surround 1\neffect B 3ds_nn_reverb "
                           "early_reflection_time 60 fused_time 4000 pre_delay_time 100 coloration 0.5 damping 0.4 "
                           "comb_size_0 3040 comb_size_1 3680 all_pass_size 2080 early_gain 0.6 fused_gain 0.4 "
                           "high_pass_damping 1\neffect B 3ds_nn_delay delay_time 1 feedback 1 damping -0 surround 0\n"
                           "effect B 3ds_nn_reverb_2011 early_reflection_time 1000 fused_time 20000 pre_delay_time "
                           "1000 coloration 1 damping 1 comb_size_0 160 comb_size_1 32000 all_pass_size 32000 "
                           "early_gain 1 fused_gain 1\n") != std::string::npos);
}

// Neither form writes a value of the 3DS's effects outside the limits. The readers refuse one too.
void TestCtrEffectValuesOutside()
{
    const CtrEffectsFixture fixture = MakeCtrEffectsFixture();
    const File& file = fixture.file;
    const std::vector<uint8_t>& binary = fixture.binary;

    // Values outside the limits. Neither form holds them.
    struct Outside
    {
        std::size_t bus;
        std::size_t effect;
        std::size_t parameter;
        float value;
        std::string_view problem;
    };
    for (const Outside& outside : {
             Outside{0, 0, 0, 1000.5f, "a value of early_reflection_time that isn't a whole number from 0 to 1000"},
             Outside{0, 0, 1, 20001.0f, "a value of fused_time that isn't a whole number from 0 to 20000"},
             Outside{0, 0, 2, -1.0f, "a value of pre_delay_time that isn't a whole number from 0 to 1000"},
             Outside{0, 0, 3, 1.01f, "a value of coloration that is outside 0 to 1"},
             Outside{0, 0, 5, 0.0f, "a value of comb_size_0 that isn't a multiple of 160 from 160 to 32000"},
             Outside{0, 0, 6, 1441.0f, "a value of comb_size_1 that isn't a multiple of 160 from 160 to 32000"},
             Outside{0, 0, 7, 32160.0f, "a value of all_pass_size that isn't a multiple of 160 from 160 to 32000"},
             Outside{0, 0, 8, -0.01f, "a value of early_gain that is outside 0 to 1"},
             Outside{0, 0, 10, 2.0f, "a value of surround that isn't a whole number from 0 to 1"},
             Outside{0, 1, 0, 0.0f, "a value of delay_time that isn't a whole number from 1 to 5000"},
             Outside{0, 1, 0, 5001.0f, "a value of delay_time that isn't a whole number from 1 to 5000"},
             Outside{1, 1, 1, 1.5f, "a value of feedback that is outside 0 to 1"},
             Outside{1, 2, 4, 2.0f, "a value of damping that is outside 0 to 1"},
             Outside{1, 0, 10, 0.5f, "a value of high_pass_damping that isn't a whole number from 0 to 1"},
         })
    {
        File outside_file = file;
        outside_file.aux_effects[outside.bus][outside.effect].parameters[outside.parameter] = outside.value;
        const std::string problem = "an effect on aux bus " + std::string(1, static_cast<char>('A' + outside.bus)) +
                                    " has " + std::string(outside.problem);
        XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { format::Write(outside_file); }) == problem);
        XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { format::WriteText(outside_file); }) == problem);
    }

    // A reader refuses them too. The first effect's comb_size_0 lies at 32 in MIXR. A first byte of 1 makes its 1120 a
    // little more. A line that leaves a filter's size out gives it 0.
    XSFLIB_CHECK(Patched(binary, 1, 32, 1) ==
                 "The file is damaged: an effect on aux bus A has a value of comb_size_0 that isn't a multiple of 160 "
                 "from 160 to 32000.");
    const std::string kHeader = "hok 1.0\nengine 3ds\n";
    XSFLIB_CHECK(TextError(kHeader + "effect A 3ds_delay delay_time 250\n").empty());
    XSFLIB_CHECK(TextError(kHeader + "effect A 3ds_nn_reverb_2011 fused_time 4000\n") ==
                 "Line 3: the effect has a value of comb_size_0 that isn't a multiple of 160 from 160 to 32000.");
    XSFLIB_CHECK(TextError(kHeader + "effect A 3ds_delay delay_time 250.5\n") ==
                 "Line 3: the effect has a value of delay_time that isn't a whole number from 1 to 5000.");
}

// The buses' volumes, in a file without effects and in one with them. MIXR holds them after bus C's effects, and the
// text form a line for each bus whose volume isn't 1. A file whose buses are all at full volume keeps the chunk as it
// was, and one without effects has no MIXR chunk.
void TestBusVolumes()
{
    File file;
    file.aux_volumes = {0.5f, 1.0f, 0.0f};
    File with_effects = Unusual();
    with_effects.aux_volumes = {1.0f, 2.0f, 1.0f};

    const std::vector<uint8_t> binary = format::Write(file);
    const std::string text = format::WriteText(file);
    const std::vector<uint8_t> unusual = format::Write(with_effects);
    const std::string unusual_text = format::WriteText(with_effects);

    // Without effects: three counts, then 0.5, 1 and 0 as floats. The first and third counts are 0. With them:
    // Unusual's 180 bytes, then 1, 2 and 1.
    XSFLIB_CHECK(format::Read(binary) == file && format::ReadText(text) == file);
    XSFLIB_CHECK(ChunkIds(binary) == std::vector<std::string>({"ENGN", "MIXR", "SEQS"}));
    const std::span<const uint8_t> mixer = ChunkBytes(binary, 1);
    SEQLIB_REQUIRE(mixer.size() == 24);
    XSFLIB_CHECK(xsflib::LoadLe32(&mixer[0]) == 0 && xsflib::LoadLe32(&mixer[8]) == 0);
    XSFLIB_CHECK(xsflib::LoadLe32(&mixer[12]) == 0x3F000000u && xsflib::LoadLe32(&mixer[16]) == 0x3F800000u &&
                 xsflib::LoadLe32(&mixer[20]) == 0);
    XSFLIB_CHECK(text.find("\nmixing strict\nbus_volume A 0.5\nbus_volume C 0\n") != std::string::npos);
    XSFLIB_CHECK(format::Read(unusual) == with_effects && format::ReadText(unusual_text) == with_effects);
    const std::span<const uint8_t> unusual_mixer = ChunkBytes(unusual, 1);
    XSFLIB_CHECK(unusual_mixer.size() == 180 + 12 && xsflib::LoadLe32(&unusual_mixer[180]) == 0x3F800000u &&
                 xsflib::LoadLe32(&unusual_mixer[184]) == 0x40000000u);
    XSFLIB_CHECK(unusual_text.find("dry_gain 0.75\nbus_volume B 2\n\nsequence 0\n") != std::string::npos);

    // Full volume.
    XSFLIB_CHECK(ChunkBytes(format::Write(Unusual()), 1).size() == 180);
    XSFLIB_CHECK(ChunkIds(format::Write(File())) == std::vector<std::string>({"ENGN", "SEQS"}));
    const File unity = format::ReadText("hok 1.0\nengine switch\nbus_volume A 1\n");
    XSFLIB_CHECK(unity == File() && format::WriteText(unity).find("bus_volume") == std::string::npos);

    // Volumes that the binary form refuses: A's 0.5 becomes 8, and C's 0 becomes -0.5. -0 reads as a volume of 0.
    XSFLIB_CHECK(Patched(binary, 1, 15, 0x41) == "The file is damaged: aux bus A's volume is outside 0 to 2.");
    XSFLIB_CHECK(Patched(binary, 1, 23, 0xBF) == "The file is damaged: aux bus C's volume is outside 0 to 2.");
    XSFLIB_CHECK(Patched(binary, 1, 23, 0x80).empty());

    // Volumes that neither form can hold.
    File not_a_number;
    not_a_number.aux_volumes[1] = std::numeric_limits<float>::quiet_NaN();
    File loud;
    loud.aux_volumes[2] = 2.5f;
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { format::Write(not_a_number); }) ==
                 "aux bus B's volume isn't a finite number");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { format::WriteText(not_a_number); }) ==
                 "aux bus B's volume isn't a finite number");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { format::Write(loud); }) ==
                 "aux bus C's volume is outside 0 to 2");
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { format::WriteText(loud); }));

    // Lines that a reader refuses.
    const std::string kHeader = "hok 1.0\nengine switch\n";
    const auto error = [&](std::string_view lines)
    {
        return TextError(kHeader + std::string(lines));
    };
    XSFLIB_CHECK(error("bus_volume D 1\n") ==
                 "Line 3: a bus's volume's line is bus_volume, a bus from A to C and the volume.");
    XSFLIB_CHECK(error("bus_volume A\n").starts_with("Line 3: a bus's volume's line"));
    XSFLIB_CHECK(error("bus_volume A 1 2\n").starts_with("Line 3: a bus's volume's line"));
    XSFLIB_CHECK(error("bus_volume A 3\n") == "Line 3: the volume is outside 0 to 2.");
    XSFLIB_CHECK(error("bus_volume B -1\n") == "Line 3: the volume is outside 0 to 2.");
    XSFLIB_CHECK(error("bus_volume A nan\n").starts_with("Line 3: a parameter's value is a decimal number"));
    XSFLIB_CHECK(error("bus_volume A 1\nbus_volume A 0.5\n") == "Line 4: the file sets the volume of bus A twice.");
    XSFLIB_CHECK(error("sequence 0\n    end\nbus_volume A 0.5\n") ==
                 "Line 5: the buses' volumes come before the sequences.");
}
// The file from WithScenes, in both forms.
struct ScenesFixture
{
    File file;
    std::vector<uint8_t> binary;
    std::string text;
};

// The fixture of the tests of scenes in both forms.
ScenesFixture MakeScenesFixture()
{
    const File file = WithScenes();

    const std::vector<uint8_t> binary = format::Write(file);
    const std::string text = format::WriteText(file);

    return {file, binary, text};
}

// The scenes in both forms. SCEN follows GRPF, and the text form gives the scenes after the effects.
void TestScenesInBothForms()
{
    const ScenesFixture fixture = MakeScenesFixture();
    const File& file = fixture.file;
    const std::vector<uint8_t>& binary = fixture.binary;
    const std::string& text = fixture.text;

    XSFLIB_CHECK(format::Read(binary) == file);
    XSFLIB_CHECK(format::ReadText(text) == file);
    XSFLIB_CHECK(format::WriteText(format::ReadText(text)) == text);

    // The scenes in the SCEN chunk after GRPF: their number, then each scene's sound, name, starting values and
    // changes. The first scene's name takes 8 bytes after its length. Its first starting value follows the count at 20:
    // variable 0, a zero and -32768. Its first change follows the count at 40, and its second starts at 52.
    const std::vector<std::string> kIds = {"ENGN", "MIXR", "SEQS", "NATV", "TABL", "GRPF", "SCEN", "XTRA", "x y!"};
    const std::span<const uint8_t> scenes = ChunkBytes(binary, 6);
    XSFLIB_CHECK(ChunkIds(binary) == kIds);
    SEQLIB_REQUIRE(scenes.size() == 4 + 72 + 16 + 33);
    XSFLIB_CHECK(xsflib::LoadLe32(&scenes[0]) == 3 && xsflib::LoadLe32(&scenes[4]) == 0 &&
                 xsflib::LoadLe32(&scenes[8]) == 8 && xsflib::LoadLe32(&scenes[20]) == 4);
    XSFLIB_CHECK(scenes[24] == 0 && scenes[25] == 0 && xsflib::LoadLe16(&scenes[26]) == 0x8000);
    XSFLIB_CHECK(xsflib::LoadLe32(&scenes[40]) == 4 && xsflib::LoadLe32(&scenes[44]) == 0 && scenes[48] == 3 &&
                 scenes[49] == 0 && xsflib::LoadLe16(&scenes[50]) == 1 && xsflib::LoadLe32(&scenes[52]) == 90000);

    // The scenes after the effects, each after a blank line. A comment gives the sound's name from the tables. A time
    // is in seconds, with a fraction only where it has one.
    XSFLIB_CHECK(
        text.find("dry_gain 0.75\n\nscene 0 \"Last lap\" # Song\n    variable 0 -32768\n    variable 15 32767\n"
                  "    variable 16 -1\n    variable 31 0\n    at 0 variable 3 1\n    at 90 variable 3 2\n"
                  "    at 90 variable 31 -5\n    at 4294967.295 variable 0 7\n\nscene 0 \"\" # Song\n\n"
                  "scene 70000 scene\n    variable 1 1\n    at 1.5 variable 1 0\n\nsequence 0\n") != std::string::npos);
}

// A reader refuses times and values out of range, and lines out of place.
void TestScenesInText()
{
    // Times and values that a reader takes, in hex too for a value.
    const std::string kHeader = "hok 1.2\nengine switch\nscene 3 Lap\n";
    const auto scene = [&](std::string_view lines)
    {
        return format::ReadText(kHeader + std::string(lines)).scenes.at(0);
    };
    const format::Scene read =
        scene("    variable 7 0x7FFF\n    variable 2 -0\n    at 0.001 variable 2 5\n    at 12.34 variable 7 -1\n");
    XSFLIB_CHECK(read.sound == 3 && read.name == "Lap");
    XSFLIB_CHECK(read.values == (std::map<uint8_t, int16_t>{{2, 0}, {7, 32767}}));
    XSFLIB_CHECK(read.changes == (std::vector<format::VariableChange>{{1, 2, 5}, {12340, 7, -1}}));

    // Lines that a reader refuses.
    const auto error = [&](std::string_view lines)
    {
        return TextError(kHeader + std::string(lines));
    };
    for (const std::string_view time : {"4294967.296", "1.0005", "-1", "1.", ".5", "0x10", "1e3", "\"1\"", "1,5"})
    {
        XSFLIB_CHECK(error("    at " + std::string(time) + " variable 1 1\n")
                         .starts_with("Line 4: a time is a number of seconds from 0 to 4294967.295"));
    }
    for (const std::string_view value : {"32768", "-32769", "1.5", "\"1\"", "--1", "x"})
    {
        XSFLIB_CHECK(error("    variable 1 " + std::string(value) + "\n")
                         .starts_with("Line 4: a variable's value is a whole number from -32768 to 32767"));
    }
    XSFLIB_CHECK(error("    variable 32 1\n")
                     .starts_with("Line 4: a variable's number has to be a whole number from 0 to 31, not 32"));
    XSFLIB_CHECK(
        error("    variable 3 1\n    variable 3 2\n").starts_with("Line 5: the scene starts variable 3 twice"));
    XSFLIB_CHECK(error("    at 2 variable 3 1\n    at 1 variable 3 2\n")
                     .starts_with("Line 5: a scene's changes come in the order of their times"));
    XSFLIB_CHECK(error("    at 2 3 1\n").starts_with("Line 4: a scene's line is variable"));
    XSFLIB_CHECK(error("    variable 3\n").starts_with("Line 4: a scene's line is variable"));
    XSFLIB_CHECK(error("scene 4\n").starts_with("Line 4: a scene's line is scene, its sound's number and its name"));
    XSFLIB_CHECK(error("effect A i3dl2_reverb\n    variable 1 1\n")
                     .starts_with("Line 5: variable belongs to a scene, after the scene's line"));
    XSFLIB_CHECK(TextError("hok 1.2\nengine switch\n    at 1 variable 1 1\n")
                     .starts_with("Line 3: at belongs to a scene, after the scene's line"));
    XSFLIB_CHECK(
        error("sequence 0\n    end\nscene 0 Late\n").starts_with("Line 6: the scenes come before the sequences"));
    XSFLIB_CHECK(error("sequence 0\n    variable 1 1\n").starts_with("Line 5: no command"));
}

// The binary form refuses a kind of variable of a later version.
void TestScenesInBinary()
{
    const ScenesFixture fixture = MakeScenesFixture();
    const std::vector<uint8_t>& binary = fixture.binary;

    // Scenes that the binary form refuses: a zero of a later version, a variable past 31, a variable started twice and
    // changes out of order. A top byte of 0xFF puts the second change's time after the third's.
    XSFLIB_CHECK(Patched(binary, 6, 25, 1).starts_with("The file contains a variable type (1) that this version"));
    XSFLIB_CHECK(Patched(binary, 6, 49, 1).starts_with("The file contains a variable type (1) that this version"));
    XSFLIB_CHECK(Patched(binary, 6, 24, 40) ==
                 "The file is damaged: scene 0 sets variable 40, and a scene sets variables 0 to 31 only.");
    XSFLIB_CHECK(Patched(binary, 6, 28, 0) == "The file is damaged: scene 0 starts variable 0 twice.");
    XSFLIB_CHECK(Patched(binary, 6, 55, 0xFF) ==
                 "The file is damaged: scene 0 changes its variables out of the order of their times.");
    XSFLIB_CHECK(Patched(binary, 6, 48, 31).empty());
}

// Neither form holds a variable past 31 or changes out of the order of their times. A file without scenes has no SCEN
// chunk.
void TestScenesRefused()
{
    const std::vector<std::string> kIds = {"ENGN", "MIXR", "SEQS", "NATV", "TABL", "GRPF", "SCEN", "XTRA", "x y!"};

    // Scenes that neither form can hold.
    File past = WithScenes();
    past.scenes[1].changes.push_back({5, 32, 0});
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { format::Write(past); }) ==
                 "scene 1 sets variable 32, and a scene sets variables 0 to 31 only");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { format::WriteText(past); }) ==
                 "scene 1 sets variable 32, and a scene sets variables 0 to 31 only");
    File backwards = WithScenes();
    std::swap(backwards.scenes[0].changes[0], backwards.scenes[0].changes[1]);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { format::Write(backwards); }) ==
                 "scene 0 changes its variables out of the order of their times");
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { format::WriteText(backwards); }));

    // A file without scenes has no SCEN chunk.
    XSFLIB_CHECK(ChunkIds(format::Write(Unusual())).size() == kIds.size() - 1);
}

// The sounds in another mixing mode than the file's, in both forms. ENGN lists them after the file's mode. Hokuto plays
// the Wii's sounds only in strict mixing.
void TestMixing()
{
    File file = WithScenes();
    file.sound_mixing = {{2, Mixing::kStrict}, {3, Mixing::kStrict}};

    const std::vector<uint8_t> binary = format::Write(file);
    const std::string text = format::WriteText(file);

    XSFLIB_CHECK(format::Read(binary) == file);
    XSFLIB_CHECK(format::ReadText(text) == file);
    XSFLIB_CHECK(format::WriteText(format::ReadText(text)) == text);

    // The engine, the file's mode and two zeros, then the number of sounds, and each sound in 4 bytes, its mode and
    // three zeros. A file whose sounds all take its mode keeps the chunk at 4 bytes.
    const std::span<const uint8_t> chunk = ChunkBytes(binary, 0);
    const std::vector<uint8_t> kExpected = {1, 1, 0, 0, 2, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0};
    XSFLIB_CHECK(std::equal(chunk.begin(), chunk.end(), kExpected.begin(), kExpected.end()));
    XSFLIB_CHECK(ChunkBytes(format::Write(WithScenes()), 0).size() == 4);

    // A line for each sound after the mixing line, with the sound's name where the tables give it.
    XSFLIB_CHECK(
        text.find("\nmixing enhanced\nsound_mixing 2 strict # \"two words\"\nsound_mixing 3 strict # sound\n") !=
        std::string::npos);

    // Lines that a reader refuses.
    const auto error = [](std::string_view lines)
    {
        return TextError("hok 1.0\nengine switch\n" + std::string(lines));
    };
    XSFLIB_CHECK(error("sound_mixing 1 enhanced\nsound_mixing 2 strict\n").empty());
    XSFLIB_CHECK(error("sound_mixing 1\n").starts_with("Line 3: a sound's mixing mode is sound_mixing"));
    XSFLIB_CHECK(error("sound_mixing 1 loud\n").starts_with("Line 3: there's no mixing mode named loud"));
    XSFLIB_CHECK(error("sound_mixing 1 enhanced\nsound_mixing 1 strict\n")
                     .starts_with("Line 4: sound 1's mixing mode comes twice"));
    XSFLIB_CHECK(error("sequence 0\n    end\nsound_mixing 0 enhanced\n")
                     .starts_with("Line 5: a sound's mixing mode is sound_mixing"));

    // The binary form refuses a mode that this version doesn't define, and a sound twice.
    XSFLIB_CHECK(Patched(binary, 0, 12, 7) ==
                 "The file contains a mixing mode (7) that this version of Hokuto doesn't support. A later version of "
                 "Hokuto saved it.");
    XSFLIB_CHECK(Patched(binary, 0, 16, 2) == "The file is damaged: sound 2's mixing mode comes twice.");

    // A sound's mode is its mode in the list, or else the file's. A sound that takes the file's mode leaves the list,
    // and a mode for every sound empties it.
    XSFLIB_CHECK(format::MixingOf(file, 2) == Mixing::kStrict && format::MixingOf(file, 0) == Mixing::kEnhanced);
    XSFLIB_CHECK((format::WithMixing(file, 2, Mixing::kEnhanced).sound_mixing ==
                  std::map<uint32_t, Mixing>{{3, Mixing::kStrict}}));
    XSFLIB_CHECK(format::WithMixing(file, 70000, Mixing::kStrict).sound_mixing.size() == 3);
    const File strict = format::WithMixing(file, Mixing::kStrict);
    XSFLIB_CHECK(strict.mixing == Mixing::kStrict && strict.sound_mixing.empty());

    // MixingRefused refuses enhanced mixing on the Wii, and no mode on the Switch, the Wii U, the 3DS, the GBA and the
    // DS.
    XSFLIB_CHECK(MixingRefused(Engine::kWii, Mixing::kEnhanced) == "The Wii plays in strict mixing only.");
    XSFLIB_CHECK(!MixingRefused(Engine::kWii, Mixing::kStrict));
    for (const Engine engine : {Engine::kSwitch, Engine::kWiiU, Engine::k3ds, Engine::kGba, Engine::kDs})
    {
        XSFLIB_CHECK(!MixingRefused(engine, Mixing::kStrict) && !MixingRefused(engine, Mixing::kEnhanced));
    }
}

// The sounds on another engine than the file's, in both forms. ENGN lists them after the sounds in another mixing mode,
// and gives the number of those even when it's 0.
void TestSoundEngines()
{
    File file = WithScenes();
    file.sound_engines = {{2, Engine::kWiiU}, {3, Engine::k3ds}};

    const std::vector<uint8_t> binary = format::Write(file);
    const std::string text = format::WriteText(file);

    XSFLIB_CHECK(format::Read(binary) == file);
    XSFLIB_CHECK(format::ReadText(text) == file);
    XSFLIB_CHECK(format::WriteText(format::ReadText(text)) == text);

    // The engine, the file's mode and two zeros, no sounds in another mode, then the number of sounds on another
    // engine, and each sound in 4 bytes, its engine and three zeros.
    const std::span<const uint8_t> engine = ChunkBytes(binary, 0);
    const std::vector<uint8_t> kExpected = {1, 1, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 2, 0,
                                            0, 0, 2, 0, 0, 0, 3, 0, 0, 0, 3, 0, 0, 0};
    XSFLIB_CHECK(std::equal(engine.begin(), engine.end(), kExpected.begin(), kExpected.end()));

    // A line for each sound after the lines of the mixing modes, with the sound's name where the tables give it.
    File both = file;
    both.sound_mixing = {{1, Mixing::kStrict}};
    const std::string both_text = format::WriteText(both);
    XSFLIB_CHECK(text.find("\nmixing enhanced\nsound_engine 2 wii_u # \"two words\"\nsound_engine 3 3ds # sound\n") !=
                 std::string::npos);
    const std::size_t mixing_line = both_text.find("\nsound_mixing 1 strict");
    const std::size_t engine_line = both_text.find("\nsound_engine 2 wii_u");
    XSFLIB_CHECK(mixing_line != std::string::npos && engine_line != std::string::npos && mixing_line < engine_line);
    XSFLIB_CHECK(format::Read(format::Write(both)) == both);
    XSFLIB_CHECK(format::ReadText(both_text) == both);

    // Lines that a reader refuses.
    const auto error = [](std::string_view lines)
    {
        return TextError("hok 1.0\nengine switch\n" + std::string(lines));
    };
    XSFLIB_CHECK(error("sound_engine 1 wii_u\nsound_engine 2 wii\n").empty());
    XSFLIB_CHECK(error("sound_engine 1\n").starts_with("Line 3: a sound's engine is sound_engine"));
    XSFLIB_CHECK(error("sound_engine 1 gameboy\n").starts_with("Line 3: there's no engine named gameboy"));
    XSFLIB_CHECK(error("sound_engine 1 gba\n").starts_with("Line 3: a sound of another console can't play on the GBA"));
    XSFLIB_CHECK(error("sound_engine 1 dse\n").starts_with("Line 3: a sound of another console can't play on the DSE"));
    XSFLIB_CHECK(error("sound_engine 1 ds\n").empty());
    XSFLIB_CHECK(
        error("sound_engine 1 3ds\nsound_engine 1 wii_u\n").starts_with("Line 4: sound 1's engine comes twice"));
    XSFLIB_CHECK(
        error("sequence 0\n    end\nsound_engine 0 3ds\n").starts_with("Line 5: a sound's engine is sound_engine"));

    // The binary form refuses an engine that this version doesn't define, and a sound twice.
    XSFLIB_CHECK(Patched(binary, 0, 16, 200) ==
                 "The file contains an engine (200) that this version of Hokuto doesn't support. A later version of "
                 "Hokuto saved it.");
    XSFLIB_CHECK(Patched(binary, 0, 20, 2) == "The file is damaged: sound 2's engine comes twice.");

    // A sound can play on the DS's engine, and no sound of another console on the engine of the GBA or of DSE.
    XSFLIB_CHECK(Patched(binary, 0, 16, 6).empty());
    XSFLIB_CHECK(Patched(binary, 0, 16, 5) == "The file is damaged: sound 2 plays on the GBA's engine.");
    XSFLIB_CHECK(Patched(binary, 0, 16, 7) == "The file is damaged: sound 2 plays on the DSE's engine.");

    // A sound's engine is its engine in the list, or else the file's. A sound on the file's engine leaves the list.
    XSFLIB_CHECK(format::EngineOf(file, 2) == Engine::kWiiU && format::EngineOf(file, 0) == Engine::kSwitch);
    XSFLIB_CHECK(
        (format::WithEngine(file, 2, Engine::kSwitch).sound_engines == std::map<uint32_t, Engine>{{3, Engine::k3ds}}));
    XSFLIB_CHECK(format::WithEngine(file, 70000, Engine::kWii).sound_engines.size() == 3);
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("format_mix",
                                    []
                                    {
                                        TestEffects();
                                        TestWiiUEffects();
                                        TestWiiEffects();
                                        TestCtrEffectKinds();
                                        TestCtrEffectLimits();
                                        TestCtrEffectForms();
                                        TestCtrEffectValuesOutside();
                                        TestBusVolumes();
                                        TestScenesInBothForms();
                                        TestScenesInText();
                                        TestScenesInBinary();
                                        TestScenesRefused();
                                        TestMixing();
                                        TestSoundEngines();
                                    });
}
