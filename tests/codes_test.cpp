// SPDX-License-Identifier: MIT

// Tests Hokuto's files (src/format/): the numbers of the kinds, engines, effects, archives and sounds, the CRC-32, the
// values of commands, and the kinds of files. A read of the text refuses a value that the console's commands can't
// hold. The messages name the kinds of the files that Hokuto doesn't take.

#include "format_testing.h"

namespace
{

// The CRC-32 of "123456789" is the standard check value, 0xCBF43926. The CRC-32 of no bytes is 0.
void TestCrc()
{
    const std::string_view kCheck = "123456789";

    XSFLIB_CHECK(format::Crc32(std::span(reinterpret_cast<const uint8_t*>(kCheck.data()), kCheck.size())) ==
                 0xCBF43926u);
    XSFLIB_CHECK(format::Crc32({}) == 0);
}

// The numbers and names that never change: those of the commands' kinds, the engines, the mixing modes, the consoles,
// the kinds of sound and the effects. Each code reads back as its kind. A code past the last reads as nothing. So does
// 0 where it names no kind: mixing mode 0 is strict, and sound kind 0 is unknown.
void TestKindCodes()
{
    std::vector<uint16_t> codes;
    for (int k = 0; k < ntdlib::commands::kKindCount; ++k)
    {
        const auto kind = static_cast<Kind>(k);
        const uint16_t code = format::CodeOf(kind);
        XSFLIB_CHECK(code != 0 && format::KindOfCode(code) == kind);
        XSFLIB_CHECK(std::find(codes.begin(), codes.end(), code) == codes.end());
        codes.push_back(code);
    }
    XSFLIB_CHECK(format::CodeOf(Kind::kNote) == 1 && format::CodeOf(Kind::kData) == 76);
    XSFLIB_CHECK(format::CodeOf(Kind::kNoteOff) == 77 && format::CodeOf(Kind::kSampleOffset) == 93);
    XSFLIB_CHECK(!format::KindOfCode(0) && !format::KindOfCode(94));
}

// The codes and names of the engines and the mixing modes, and the engine of each console's archives.
void TestEngineCodes()
{
    // The engines and the mixing modes.
    XSFLIB_CHECK(format::CodeOf(Engine::kSwitch) == 1 && format::NameOf(Engine::kSwitch) == "switch");
    XSFLIB_CHECK(format::EngineNamed("switch") == Engine::kSwitch && !format::EngineOfCode(0));
    XSFLIB_CHECK(format::CodeOf(Engine::kWiiU) == 2 && format::EngineNamed("wii_u") == Engine::kWiiU);
    XSFLIB_CHECK(format::CodeOf(Engine::k3ds) == 3 && format::NameOf(Engine::k3ds) == "3ds");
    XSFLIB_CHECK(format::EngineOfCode(3) == Engine::k3ds && !format::EngineOfCode(200));
    XSFLIB_CHECK(format::CodeOf(Engine::kGba) == 5 && format::EngineNamed("gba") == Engine::kGba);

    // Every engine has the code that docs/format.md lists in ENGN, 1 to 14 in the order of the enum, and the name that
    // its text form uses. Each code and each name reads back as its engine.
    constexpr std::array<std::string_view, format::kEngineCount> kEngineNames = {
        "switch", "wii_u", "3ds",     "wii",      "gba",  "ds",      "dse",
        "konami", "rd2",   "brownie", "ubimilan", "rare", "quintet", "krawall"};
    for (std::size_t e = 0; e < format::kEngineCount; ++e)
    {
        const auto engine = static_cast<Engine>(e);
        const uint8_t code = format::CodeOf(engine);
        XSFLIB_CHECK(code == e + 1);
        XSFLIB_CHECK(format::EngineOfCode(code) == engine);
        XSFLIB_CHECK(format::NameOf(engine) == kEngineNames[e]);
        XSFLIB_CHECK(format::EngineNamed(kEngineNames[e]) == engine);
    }

    XSFLIB_CHECK(!format::EngineOfCode(format::kEngineCount + 1));
    XSFLIB_CHECK(format::EngineOf(Flavor::kNx) == Engine::kSwitch && format::EngineOf(Flavor::kCafe) == Engine::kWiiU &&
                 format::EngineOf(Flavor::kCtr) == Engine::k3ds);
    XSFLIB_CHECK(format::MixingNamed("enhanced") == Mixing::kEnhanced && format::MixingOfCode(0) == Mixing::kStrict);
    XSFLIB_CHECK(format::MixingOfCode(1) == Mixing::kEnhanced && !format::MixingOfCode(2));
}

// Code 4 of the archives is the Wii's. It reads as no flavor: ntdlib gives the Wii's archives none.
void TestFlavorCodes()
{
    // The consoles and the kinds of sound.
    for (const Flavor flavor : {Flavor::kNx, Flavor::kCafe, Flavor::kCtr})
    {
        XSFLIB_CHECK(format::FlavorOfCode(format::CodeOf(flavor)) == flavor);
        XSFLIB_CHECK(format::FlavorNamed(format::NameOf(flavor)) == flavor);
    }

    XSFLIB_CHECK(!format::FlavorOfCode(0) && !format::FlavorOfCode(format::kWiiArchiveCode));

    for (const SoundType type : {SoundType::kUnknown, SoundType::kSequence, SoundType::kWaveSound, SoundType::kStream})
    {
        XSFLIB_CHECK(format::SoundTypeOfCode(format::CodeOf(type)) == type);
        XSFLIB_CHECK(format::SoundTypeNamed(format::NameOf(type)) == type);
    }

    XSFLIB_CHECK(format::CodeOf(SoundType::kStream) == 3 && !format::SoundTypeOfCode(4));
}

// The effects take their parameters' names, and the reverbs their modes.
void TestEffectCodes()
{
    // An I3DL2 reverb is effect 1, with nn::audio's 12 parameters.
    XSFLIB_CHECK(format::CodeOf(EffectKind::kI3dl2Reverb) == 1 &&
                 format::EffectKindOfCode(1) == EffectKind::kI3dl2Reverb);
    XSFLIB_CHECK(format::EffectKindNamed("i3dl2_reverb") == EffectKind::kI3dl2Reverb &&
                 format::NameOf(EffectKind::kI3dl2Reverb) == "i3dl2_reverb");
    XSFLIB_CHECK(!format::EffectKindOfCode(0) && !format::EffectKindOfCode(17));
    const std::span<const std::string_view> names = format::ParameterNamesOf(EffectKind::kI3dl2Reverb);
    XSFLIB_CHECK(names.size() == 12 && names[0] == "room_gain" && names[11] == "dry_gain");

    // The Wii U's effects are 2 to 5, with the fields of their nw::snd parameter structures. Only the reverbs have
    // modes: 8 early modes and 6 fused modes.
    for (const auto& [kind, code, name, parameters] :
         {EffectKindCode{EffectKind::kReverbHi, 2, "reverb_hi", 11},
          EffectKindCode{EffectKind::kReverbStd, 3, "reverb_std", 10},
          EffectKindCode{EffectKind::kChorus, 4, "chorus", 5}, EffectKindCode{EffectKind::kDelay, 5, "delay", 5}})
    {
        XSFLIB_CHECK(format::CodeOf(kind) == code && format::EffectKindOfCode(code) == kind);
        XSFLIB_CHECK(format::NameOf(kind) == name && format::EffectKindNamed(name) == kind);
        XSFLIB_CHECK(format::ParameterNamesOf(kind).size() == parameters);
    }

    // The Wii's effects are 6 to 11. The reverbs of 2008 have the Wii U's parameters and modes. The older reverbs, the
    // chorus and the delay have the parameters of NW4R SND's classes, and no modes.
    for (const auto& [kind, code, name, parameters] :
         {EffectKindCode{EffectKind::kWiiReverbHi, 6, "wii_reverb_hi", 6},
          EffectKindCode{EffectKind::kWiiReverbHi2008, 7, "wii_reverb_hi_2008", 11},
          EffectKindCode{EffectKind::kWiiChorus, 8, "wii_chorus", 3},
          EffectKindCode{EffectKind::kWiiDelay, 9, "wii_delay", 3},
          EffectKindCode{EffectKind::kWiiReverbStd, 10, "wii_reverb_std", 5},
          EffectKindCode{EffectKind::kWiiReverbStd2008, 11, "wii_reverb_std_2008", 10}})
    {
        XSFLIB_CHECK(format::CodeOf(kind) == code && format::EffectKindOfCode(code) == kind);
        XSFLIB_CHECK(format::NameOf(kind) == name && format::EffectKindNamed(name) == kind);
        XSFLIB_CHECK(format::ParameterNamesOf(kind).size() == parameters);
    }

    XSFLIB_CHECK(format::ParameterNamesOf(EffectKind::kWiiReverbHi)[4] == "crosstalk" &&
                 format::ParameterNamesOf(EffectKind::kWiiReverbStd)[4] == "out_gain");
    XSFLIB_CHECK(format::ParameterNamesOf(EffectKind::kWiiChorus)[2] == "rate" &&
                 format::ParameterNamesOf(EffectKind::kWiiDelay)[2] == "out_gain");
    XSFLIB_CHECK(format::ChoicesOf(EffectKind::kWiiReverbHi2008, 6) == 8 &&
                 format::ChoicesOf(EffectKind::kWiiReverbStd2008, 7) == 6);
    for (std::size_t p = 0; p < 6; ++p)
    {
        XSFLIB_CHECK(format::ChoicesOf(EffectKind::kWiiReverbHi, p) == 0);
    }

    // The names of the Wii U's parameters, and their modes.
    const std::span<const std::string_view> reverb = format::ParameterNamesOf(EffectKind::kReverbHi);
    XSFLIB_CHECK(reverb[0] == "pre_delay_time" && reverb[4] == "crosstalk" && reverb[10] == "fused_gain");
    XSFLIB_CHECK(format::ParameterNamesOf(EffectKind::kReverbStd)[4] == "out_gain");
    XSFLIB_CHECK(format::ParameterNamesOf(EffectKind::kChorus)[0] == "delay_time");
    XSFLIB_CHECK(format::ParameterNamesOf(EffectKind::kDelay)[4] == "lpf");
    XSFLIB_CHECK(format::ChoicesOf(EffectKind::kReverbHi, 6) == 8 && format::ChoicesOf(EffectKind::kReverbHi, 8) == 6);
    XSFLIB_CHECK(format::ChoicesOf(EffectKind::kReverbStd, 5) == 8 &&
                 format::ChoicesOf(EffectKind::kReverbStd, 7) == 6);
    XSFLIB_CHECK(format::ChoicesOf(EffectKind::kReverbHi, 7) == 0 && format::ChoicesOf(EffectKind::kReverbHi, 11) == 0);

    // The I3DL2 reverb, the chorus and the delay have no modes.
    for (std::size_t p = 0; p < 12; ++p)
    {
        XSFLIB_CHECK(format::ChoicesOf(EffectKind::kI3dl2Reverb, p) == 0 &&
                     format::ChoicesOf(EffectKind::kChorus, p) == 0 && format::ChoicesOf(EffectKind::kDelay, p) == 0);
    }
}

// The values of commands that a read of the text refuses: those that the sequences of the archive's console can't hold,
// with the line and the values that they hold there. The ranges come from the Switch's commands: a wait in up to 4
// bytes of 7 bits and a byte for the rest, a volume in a byte, a transpose in a signed byte of semitones, a low-pass in
// 64ths around 64, and a time in two bytes. A note's key takes 7 bits. The depth of an LFO past the first takes no
// random value. A file without an archive names no console, and its values aren't checked.
void TestCommandValues()
{
    const auto error = [](std::string_view command)
    {
        return TextError("hok 1.0\nengine switch\nsequence 0\n    " + std::string(command) +
                         "\nnative switch\n    bytes 3\n        AQID\n");
    };
    XSFLIB_CHECK(error("wait -5") == "Line 4: wait has to be a whole number from 0 to 4294967295, not -5.");
    XSFLIB_CHECK(error("wait 99999999999") ==
                 "Line 4: wait has to be a whole number from 0 to 4294967295, not 99999999999.");
    XSFLIB_CHECK(error("wait 1.5") == "Line 4: wait has to be a whole number from 0 to 4294967295, not 1.5.");
    XSFLIB_CHECK(error("volume 300") == "Line 4: volume has to be a whole number from 0 to 255, not 300.");
    XSFLIB_CHECK(error("volume random(0,0.5)") ==
                 "Line 4: volume has to be a whole number from -32768 to 32767, not random(0,0.5).");
    XSFLIB_CHECK(error("transpose 150") ==
                 "Line 4: transpose has to be a number from -12800 to 12700 in steps of 100, not 150.");
    XSFLIB_CHECK(error("low_pass 0.01") ==
                 "Line 4: low_pass has to be a number from -1 to 2.984375 in steps of 0.015625, not 0.01.");
    XSFLIB_CHECK(error("pan 3 over 1.5") == "Line 4: pan's time has to be a whole number from 0 to 65535, not 1.5.");
    XSFLIB_CHECK(error("pan var(300)") == "Line 4: pan has to read a variable from 0 to 255, not var(300).");
    XSFLIB_CHECK(error("lfo_depth 2 random(0,1)") == "Line 4: lfo_depth takes no random value, not random(0,1).");
    XSFLIB_CHECK(error("note 60 1.5 48") ==
                 "Line 4: a note's velocity has to be a whole number from 0 to 255, not 1.5.");
    XSFLIB_CHECK(error("note 128 100 48") == "Line 4: a note's key has to be a whole number from 0 to 127, not 128.");
    XSFLIB_CHECK(error("note 60 100 -1") ==
                 "Line 4: a note's length has to be a whole number from 0 to 4294967295, not -1.");
    for (const std::string_view held :
         {"wait 268435455", "volume 255", "transpose 12700", "low_pass 0.015625", "pan 3 over 65535", "pan var(255)",
          "note 127 255 48", "end [index 3] [value var(2)]"})
    {
        XSFLIB_CHECK(error(held).empty());
    }
    XSFLIB_CHECK(TextError("hok 1.0\nengine switch\nsequence 0\n    wait 1.5\n").empty());

    // A file in the model form takes its console from the model. A new file of the Wii holds RSAR 1.4. Its sequences
    // come before its chunks.
    const std::string wii = format::WriteText(format::NewFile(format::Engine::kWii));
    const std::size_t chunks = wii.find("\nchunk ") + 1;
    const auto line = std::ranges::count(wii.begin(), wii.begin() + static_cast<std::ptrdiff_t>(chunks), '\n') + 2;
    XSFLIB_CHECK(TextError(wii.substr(0, chunks) + "sequence 0\n    wait 1.5\n" + wii.substr(chunks)) ==
                 "Line " + std::to_string(line) + ": wait has to be a whole number from 0 to 4294967295, not 1.5.");

    // An empty file names no other kind. A file of another kind gives its kind.
    const auto either = [](std::vector<uint8_t> bytes)
    {
        return ErrorOf<xsflib::FormatError>([&] { format::ReadEitherForm(bytes); });
    };
    XSFLIB_CHECK(either({}) == "It's empty.");
    XSFLIB_CHECK(either({'x', 'y', 'z'}) == "It isn't a Hokuto file.");
}

// The kinds that the messages name. The first bytes give a file's kind: the console's letter, three letters of the kind
// and a byte order mark for the consoles' files. The extension gives the kind that a file's name indicates. File > Open
// and File > Import > Sound Archive… name the kind of a file that they don't take, or the kind that its name indicates,
// and the command that takes the file's kind (src/app/menus.cpp): an item of File > Import, or Replace… for an AAC
// file. A MIDI file opens by its bytes and never gets such a message, but the text still names its kind.
void TestKinds()
{
    const auto bytes = [](std::string_view text)
    {
        return std::vector<uint8_t>(text.begin(), text.end());
    };

    const std::vector<uint8_t> kNxWave = {'F', 'W', 'A', 'V', 0xFF, 0xFE, 0x40, 0};
    const std::vector<uint8_t> kCafeBank = {'F', 'B', 'N', 'K', 0xFE, 0xFF, 0, 0x40};
    const std::vector<uint8_t> kWiiArchive = {'R', 'S', 'A', 'R', 0xFE, 0xFF, 1, 4};
    const std::vector<uint8_t> kCtrSequence = {'C', 'S', 'E', 'Q', 0xFF, 0xFE, 0x40, 0};
    const std::vector<uint8_t> kUnmarked = {'F', 'S', 'E', 'Q', 0, 0, 0, 0};
    const std::vector<uint8_t> kWav = {'R', 'I', 'F', 'F', 0x10, 0, 0, 0, 'W', 'A', 'V', 'E'};
    const std::vector<uint8_t> kAac = {0xFF, 0xF1, 0x50, 0x80};
    const std::vector<uint8_t> hok = format::Write(format::ImportArchive(Archive(), Flavor::kNx));
    const std::vector<uint8_t> notes = bytes("Notes for the next session.\n");
    const std::vector<uint8_t> kMidi = {'M', 'T', 'h', 'd', 0, 0, 0, 6};
    const std::vector<uint8_t> kDsArchive = {'S', 'D', 'A', 'T', 0xFF, 0xFE, 0, 1};

    XSFLIB_CHECK(format::KindOfBytes({}) == "an empty file" && format::KindOfBytes(hok) == "a Hokuto file" &&
                 format::KindOfBytes(bytes("hok 1.0\n")) == "a Hokuto file");
    XSFLIB_CHECK(format::KindOfBytes(kWav) == "a WAV file" && format::KindOfBytes(kAac) == "an AAC file" &&
                 format::KindOfBytes(bytes("SARC\xFF\xFE")) == "a SARC archive");
    XSFLIB_CHECK(format::KindOfBytes(kMidi) == "a MIDI file" &&
                 format::KindOfBytes(kDsArchive) == "a sound archive of the DS");
    XSFLIB_CHECK(format::KindOfBytes(kNxWave) == "a wave file of the Switch" &&
                 format::KindOfBytes(kCafeBank) == "a bank file of the Wii U" &&
                 format::KindOfBytes(kWiiArchive) == "a sound archive of the Wii" &&
                 format::KindOfBytes(kCtrSequence) == "a sequence file of the 3DS");
    XSFLIB_CHECK(format::KindOfBytes(kUnmarked).empty() && format::KindOfBytes(notes).empty());
    XSFLIB_CHECK(format::KindOfName("Song.hok") == "a Hokuto file" && format::KindOfName("Take.WAV") == "a WAV file" &&
                 format::KindOfName("BGM.bfseq") == "a sequence file of the Wii U or the Switch" &&
                 format::KindOfName("Drums.bcbnk") == "a bank file of the 3DS" &&
                 format::KindOfName("Voice.brwsd") == "a wave sound file of the Wii");
    XSFLIB_CHECK(format::KindOfName("Notes.txt").empty() && format::KindOfName("Archive.bxsar").empty());
    XSFLIB_CHECK(format::NotKindText(kNxWave, "a sequence file") ==
                 "It's a wave file of the Switch, not a sequence file.");
    XSFLIB_CHECK(format::NotKindText({}, "a sequence file") == "It's empty." &&
                 format::NotKindText(notes, "a sequence file") == "It isn't a sequence file." &&
                 format::NotKindText(kCtrSequence, "a sequence file") == "It isn't a sequence file.");
    XSFLIB_CHECK(format::NotKindText(kCafeBank, "a bank file of the Wii") ==
                 "It's a bank file of the Wii U, not a bank file of the Wii.");

    // File > Open, and File > Import > Sound Archive….
    const std::string kTakes = "Hokuto opens Hokuto files, sound archives, GBA ROMs, DS images and MIDI files.";
    const std::string kImports =
        "File > Import > Sound Archive… imports sound archives, GBA ROMs, DS images and MIDI files.";
    XSFLIB_CHECK(format::NotOpenedText("Empty.hok", {}, false) == "Can't open Empty.hok. It's empty. " + kTakes);
    XSFLIB_CHECK(format::NotOpenedText("Notes.wav", notes, false) ==
                 "Can't open Notes.wav. Its name ends in .wav, but it isn't a WAV file. " + kTakes);
    XSFLIB_CHECK(format::NotOpenedText("Note.bfwav", kNxWave, false) ==
                 "Can't open Note.bfwav. It's a wave file of the Switch. " + kTakes +
                     " Use File > Import > Wave… instead.");
    XSFLIB_CHECK(format::NotOpenedText("Notes.txt", notes, false) == "Can't open Notes.txt. " + kTakes);
    XSFLIB_CHECK(format::NotOpenedText("Take.wav", kWav, true) ==
                 "Can't import Take.wav. It's a WAV file. " + kImports +
                     " Use File > Import > Audio File… or Stream… instead.");
    XSFLIB_CHECK(format::NotOpenedText("Song.aac", kAac, true) ==
                 "Can't import Song.aac. It's an AAC file. " + kImports +
                     " Use Replace… on a 3DS stream sound that plays an AAC stream instead.");
    XSFLIB_CHECK(format::NotOpenedText("Song.mid", kMidi, false) == "Can't open Song.mid. It's a MIDI file. " + kTakes);
    XSFLIB_CHECK(format::NotOpenedText("sound_data.sdat", kDsArchive, true) ==
                 "Can't import sound_data.sdat. It's a sound archive of the DS. " + kImports);
    XSFLIB_CHECK(format::AsSentence("too short to be an FSAR file") == "Too short to be an FSAR file." &&
                 format::AsSentence("It's done.") == "It's done." &&
                 format::AsSentence("sound_main can't be read.") == "sound_main can't be read." &&
                 format::AsSentence("") == "");
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("codes",
                                    []
                                    {
                                        TestCrc();
                                        TestKindCodes();
                                        TestEngineCodes();
                                        TestFlavorCodes();
                                        TestEffectCodes();
                                        TestCommandValues();
                                        TestKinds();
                                    });
}
