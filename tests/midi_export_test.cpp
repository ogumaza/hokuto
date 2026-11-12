// SPDX-License-Identifier: MIT

// Tests the exports to MIDI and SoundFont files and the dry render (src/song/midi_export.cpp and dry_render.cpp).

#include "song_runs.h"

namespace
{

// Each file under `folder` by its path there, with its bytes.
std::map<std::string, std::vector<uint8_t>> FilesUnder(const std::filesystem::path& folder)
{
    std::map<std::string, std::vector<uint8_t>> files;
    for (const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(folder))
    {
        if (entry.is_regular_file())
        {
            std::vector<uint8_t> bytes;
            xsflib::ReadFile(entry.path(), bytes);
            const std::u8string path = std::filesystem::relative(entry.path(), folder).generic_u8string();
            files.emplace(std::string(path.begin(), path.end()), bytes);
        }
    }

    return files;
}

// An export to MIDI with SoundFont 2 and SFZ writes the files that ntdlib's converter frame writes for the same sound,
// in the same places: the MIDI file and the SoundFont side by side, the SFZ files in a folder of the sound's name, and
// the samples in a folder that the sounds share.
void TestExportLayout()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const TemporaryFolder folder;
    const std::filesystem::path ours = folder.Path() / "ours";
    const std::filesystem::path theirs = folder.Path() / "theirs";
    std::filesystem::create_directory(ours);
    std::filesystem::create_directory(theirs);
    const auto convert = [&](std::size_t sound)
    {
        return export_::ConvertSequence(*archive, static_cast<uint32_t>(sound), export_::PerformOptions(),
                                        export_::SwitchSettings(), "Hokuto");
    };

    const export_::Conversion conversion = ConvertToMidi(*archive, kNoteSound, {}, {}, "Hokuto");
    WriteMidiAndSoundFont(conversion, ours / "SEQ_NOTE.mid");
    WriteMidiAndSfz(conversion, ours / "SEQ_NOTE.mid");
    export_::ConvertAll<export_::Conversion>({{kNoteSound, "SEQ_NOTE"}}, theirs, true, export_::kSfzSamplesFolder,
                                             convert);

    const std::map<std::string, std::vector<uint8_t>> files = FilesUnder(ours);
    SEQLIB_REQUIRE(!conversion.sfz.files.empty() && !conversion.sfz.samples.empty());
    SEQLIB_CHECK(files.contains("SEQ_NOTE.mid") && files.contains("SEQ_NOTE.sf2"));
    SEQLIB_CHECK(files.contains("sfz/SEQ_NOTE/" + conversion.sfz.files[0].name));
    SEQLIB_CHECK(files.contains("sfz/samples/" + conversion.sfz.samples[0].name));
    SEQLIB_CHECK(files == FilesUnder(theirs));
}

// A change at a set time reaches the MIDI file. The sequence's loop plays key 60 for 48 ticks, half a second, until
// variable 0 is 1, and then key 72. The converter repeats a loop shorter than 10 s until the file lasts 10 s: 20 notes.
// The change at 1,012 ms comes before tick 97, the start of the third pass. The loop's markers go round the first pass
// after the change. A MIDI quarter note holds 160 times the sequence's 48 ticks.
void TestExportChanges()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenChangesArchive();

    const auto keys = [](const export_::Conversion& conversion)
    {
        std::vector<int> played;
        for (const seqlib::midi::Note& note : seqlib::midi::Notes(seqlib::midi::Read(conversion.midi)))
        {
            played.push_back(note.key);
        }

        return played;
    };

    const auto loop = [](uint64_t start, uint64_t end)
    {
        return std::vector<seqlib::midi::Text>({{start * 160, "loopStart"}, {end * 160, "loopEnd"}});
    };

    std::vector<int> switched(20, 72);
    switched[0] = 60;
    switched[1] = 60;

    const export_::Conversion unchanged = ConvertToMidi(*archive, kChangesSound, {}, {}, "Hokuto");
    const export_::Conversion changed = ConvertToMidi(*archive, kChangesSound, {}, {{1012, 0, 1}}, "Hokuto");
    const export_::Conversion started = ConvertToMidi(*archive, kChangesSound, {{0, 1}}, {}, "Hokuto");

    SEQLIB_CHECK(keys(unchanged) == std::vector<int>(20, 60));
    SEQLIB_CHECK(keys(changed) == switched);
    SEQLIB_CHECK(keys(started) == std::vector<int>(20, 72));
    SEQLIB_CHECK(seqlib::midi::Markers(seqlib::midi::Read(unchanged.midi)) == loop(1, 49));
    SEQLIB_CHECK(seqlib::midi::Markers(seqlib::midi::Read(changed.midi)) == loop(97, 145));
}

// A sample that the folder of samples holds with other bytes stops the export before it writes anything. A sample with
// the same bytes stays.
void TestExportKeepsOtherSamples()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const export_::Conversion conversion = ConvertToMidi(*archive, kNoteSound, {}, {}, "Hokuto");
    SEQLIB_REQUIRE(!conversion.sfz.samples.empty());
    const TemporaryFolder folder;
    const std::filesystem::path midi = folder.Path() / "SEQ_NOTE.mid";
    const std::filesystem::path sample = folder.Path() / "sfz" / "samples" / conversion.sfz.samples[0].name;
    std::filesystem::create_directories(sample.parent_path());
    const std::vector<uint8_t> kOther = {1, 2, 3};
    xsflib::WriteFile(sample, kOther);

    const bool stopped = ThrowsRuntimeError([&] { WriteMidiAndSfz(conversion, midi); });
    const bool wrote = std::filesystem::exists(midi);
    std::vector<uint8_t> kept;
    xsflib::ReadFile(sample, kept);
    xsflib::WriteFile(sample, conversion.sfz.samples[0].wav);
    WriteMidiAndSfz(conversion, midi);

    SEQLIB_CHECK(stopped && !wrote && kept == kOther);
    SEQLIB_CHECK(FilesUnder(folder.Path()).size() == 1 + conversion.sfz.files.size() + conversion.sfz.samples.size());
}

// The tags go into the MIDI file as Standard MIDI Files 1.0 lays out a meta event: FF, the type, the text's length as a
// variable-length number, then the text. The copyright notice (type 2) comes first in the first track. The title takes
// the place of the sequence's name (type 3), the sound's name. A length of 200 takes two bytes: 0x81 0x48. The other
// events and tracks stay as they were. Without tags, the file stays as it is. A file whose first track doesn't start
// with the sequence's name can't take them, such as one whose first event is a text event (type 1). That event's type
// is byte 24: after the header's 14 bytes, the track's ID and size, the delta and FF.
void TestExportTags()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const export_::Conversion conversion = ConvertToMidi(*archive, kNoteSound, {}, {}, "Hokuto");
    const seqlib::midi::File before = seqlib::midi::Read(conversion.midi);
    const std::string kTitle = "ノート (take 2)";
    const std::string kCopyright(200, 'c');

    const auto meta = [](uint8_t type, std::vector<uint8_t> bytes, const std::string& text)
    {
        bytes.insert(bytes.begin(), {0xFF, type});
        bytes.insert(bytes.end(), text.begin(), text.end());

        return seqlib::midi::Event{0, bytes};
    };

    const auto rest = [](const seqlib::midi::Track& track, std::size_t from)
    {
        return std::vector(track.events.begin() + static_cast<std::ptrdiff_t>(from), track.events.end());
    };

    const std::vector<uint8_t> untagged = WithMidiTags(conversion.midi, {});
    const seqlib::midi::File titled = seqlib::midi::Read(WithMidiTags(conversion.midi, {kTitle, ""}));
    const seqlib::midi::File noticed = seqlib::midi::Read(WithMidiTags(conversion.midi, {"", kCopyright}));
    const std::vector<uint8_t> both = WithMidiTags(conversion.midi, {kTitle, kCopyright});
    const seqlib::midi::File tagged = seqlib::midi::Read(both);
    std::vector<uint8_t> text_first = conversion.midi;
    text_first.at(24) = 0x01;
    const bool refused = ThrowsRuntimeError([] { WithMidiTags(std::vector<uint8_t>{1, 2, 3}, {"x", ""}); }) &&
                         ThrowsRuntimeError([&text_first] { WithMidiTags(text_first, {"x", ""}); });

    SEQLIB_REQUIRE(before.tracks.size() > 1 && !before.tracks[0].events.empty());
    SEQLIB_CHECK(untagged == conversion.midi);
    SEQLIB_CHECK(before.tracks[0].events[0] == meta(0x03, {8}, "SEQ_NOTE"));
    SEQLIB_CHECK(titled.tracks[0].events[0] == meta(0x03, {18}, kTitle));
    SEQLIB_CHECK(rest(titled.tracks[0], 1) == rest(before.tracks[0], 1));
    SEQLIB_CHECK(noticed.tracks[0].events[0] == meta(0x02, {0x81, 0x48}, kCopyright));
    SEQLIB_CHECK(rest(noticed.tracks[0], 1) == before.tracks[0].events);
    SEQLIB_CHECK(tagged.tracks[0].events[0] == meta(0x02, {0x81, 0x48}, kCopyright));
    SEQLIB_CHECK(tagged.tracks[0].events[1] == meta(0x03, {18}, kTitle));
    SEQLIB_CHECK(rest(tagged.tracks[0], 2) == rest(before.tracks[0], 1));
    SEQLIB_CHECK(seqlib::midi::TrackNames(tagged)[0] == kTitle);
    for (std::size_t track = 1; track < before.tracks.size(); track++)
    {
        SEQLIB_CHECK(tagged.tracks.size() == before.tracks.size() &&
                     tagged.tracks[track].events == before.tracks[track].events);
    }
    SEQLIB_CHECK(seqlib::midi::Problems(both).empty());
    SEQLIB_CHECK(refused);
}

// The tags' bytes at each length of a variable-length number, from one byte to four. In the first track's chunk, the
// notice's meta event and the title's take the place of the sequence's name: each is 00 FF, the type, the length and
// the text. The track's other events follow. The chunk's size follows "MTrk" as a big-endian 32-bit number, and the
// other tracks follow the chunk as they were. Standard MIDI Files 1.0 lays the bytes out so.
void TestExportTagBytes()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const std::vector<uint8_t> midi = ConvertToMidi(*archive, kNoteSound, {}, {}, "Hokuto").midi;

    // The first track's chunk starts at byte 14, and its events at byte 22, with the name "SEQ_NOTE".
    constexpr std::size_t kTrack = 14;
    constexpr std::size_t kEvents = 22;
    constexpr std::size_t kNamed = kEvents + 4 + 8;
    SEQLIB_REQUIRE(midi.size() > kNamed && midi[kEvents] == 0x00 && midi[kEvents + 1] == 0xFF &&
                   midi[kEvents + 2] == 0x03 && midi[kEvents + 3] == 8);
    const uint32_t size = uint32_t{midi[kTrack + 4]} << 24 | uint32_t{midi[kTrack + 5]} << 16 |
                          uint32_t{midi[kTrack + 6]} << 8 | uint32_t{midi[kTrack + 7]};
    SEQLIB_REQUIRE(kEvents + size <= midi.size());
    const auto end = midi.begin() + static_cast<std::ptrdiff_t>(kEvents + size);

    const std::pair<std::size_t, std::vector<uint8_t>> kLengths[] = {{1, {0x01}},
                                                                     {127, {0x7F}},
                                                                     {128, {0x81, 0x00}},
                                                                     {16383, {0xFF, 0x7F}},
                                                                     {16384, {0x81, 0x80, 0x00}},
                                                                     {2097151, {0xFF, 0xFF, 0x7F}},
                                                                     {2097152, {0x81, 0x80, 0x80, 0x00}}};
    for (const auto& [length, coded] : kLengths)
    {
        const std::string title(length, 't');
        const std::string notice(length, 'c');

        std::vector<uint8_t> track;
        for (const auto& [type, text] : {std::pair<uint8_t, const std::string&>{0x02, notice}, {0x03, title}})
        {
            track.insert(track.end(), {0x00, 0xFF, type});
            track.insert(track.end(), coded.begin(), coded.end());
            track.insert(track.end(), text.begin(), text.end());
        }
        track.insert(track.end(), midi.begin() + kNamed, end);
        std::vector<uint8_t> expected(midi.begin(), midi.begin() + kTrack);
        expected.insert(expected.end(), {'M', 'T', 'r', 'k'});
        for (const int shift : {24, 16, 8, 0})
        {
            expected.push_back(static_cast<uint8_t>(track.size() >> shift));
        }
        expected.insert(expected.end(), track.begin(), track.end());
        expected.insert(expected.end(), end, midi.end());

        SEQLIB_CHECK(WithMidiTags(midi, {title, notice}) == expected);
    }
}

// An export of SFZ files leaves out the lines of ntdlib's report that name only the SoundFont, and an export of a
// SoundFont 2 file those that name only SFZ. A line that names both formats, or neither, stays in both. The lines are
// ntdlib's wording.
void TestReportFor()
{
    const std::vector<std::string> kReport = {
        "Approximated 4 times: low-pass filter (one-pole in the game, two-pole in the SoundFont)",
        "Approximated once: biquad type change (SFZ supports one type per track)",
        "Approximated twice: second to fourth LFOs (omitted)",
        "region volume exceeds 127 (SoundFont attenuated by 1.8 dB; SFZ keeps the original volume)"};

    SEQLIB_CHECK((ReportFor(kReport, true) == std::vector<std::string>{kReport[1], kReport[2], kReport[3]}));
    SEQLIB_CHECK((ReportFor(kReport, false) == std::vector<std::string>{kReport[0], kReport[2], kReport[3]}));
}

// A sequence of a Wii archive converts as revosf2 converts it without the game's main.dol: with the settings of the
// build of NW4R SND that the archive's version gives. Version 1.4 gives a build of June 2008, and earlier versions one
// of 2007. The sequence plays a note on the bank's first instrument.
void TestWiiExport()
{
    namespace rsar_files = ntdlib::testing::rsar_files;
    const std::vector<uint8_t> bytes =
        rsar_files::MakeArchive(rsar_files::MakeSequence({0x3C, 0x64, 0x30, 0xFF}), 0, 1);
    std::vector<uint8_t> older = bytes;
    xsflib::StoreBe16(&older[6], 0x0101);
    const std::shared_ptr<const ntdlib::rsar::SoundArchive> archive = LoadWiiArchive(bytes, "SOUND.brsar");
    const std::shared_ptr<const ntdlib::rsar::SoundArchive> older_archive = LoadWiiArchive(older, "SOUND.brsar");

    const export_::Conversion ours = ConvertToMidi(*archive, 0, {}, {}, "Hokuto");
    const export_::Conversion theirs =
        export_::ConvertSequence(*archive, 0, export_::PerformOptions(),
                                 export_::RevolutionSettings(export_::BuildOfRsarVersion(0x0104)), "Hokuto");

    SEQLIB_CHECK(ours.samples > 0 && ours.midi == theirs.midi && ours.sf2 == theirs.sf2);
    SEQLIB_REQUIRE(!ours.sfz.files.empty() && ours.sfz.files.size() == theirs.sfz.files.size());
    SEQLIB_CHECK(ours.sfz.files[0].text == theirs.sfz.files[0].text);
    SEQLIB_CHECK(hokuto::song::SettingsOf(*archive).wii_build.date == 20080620);
    SEQLIB_CHECK(hokuto::song::SettingsOf(*older_archive).wii_build.date == 20070101);

    // A sound past the archive's and bytes of another kind give messages.
    SEQLIB_CHECK(ThrowsRuntimeError([&] { ConvertToMidi(*archive, 1, {}, {}, "Hokuto"); }));
    SEQLIB_CHECK(ThrowsRuntimeError([] { LoadWiiArchive({1, 2, 3}, "SOUND.brsar"); }));
}

// The dry render of a sequence for the Export window matches ntdlib's model run directly with the aux buses at volume
// 0, from the first frame, in strict and in enhanced mixing. It lasts the seconds that it's given, or until the output
// ends. The send sequence's note reaches the output through aux bus A too, at full volume. Its dry render leaves that
// out, and ends once the note has faded. A render that's cancelled stops.
void TestDryRender()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenSendArchive();
    const auto loop = std::make_shared<const Sequence>(ReadSequence(*archive, kLoopSound));
    const auto send = std::make_shared<const Sequence>(ReadSequence(*archive, kSendSound));
    const std::atomic<bool> kGo{false};
    const std::atomic<bool> kStop{true};
    constexpr AuxVolumes kSilent = {0.0f, 0.0f, 0.0f};
    atk::EngineOptions options;
    options.enhanced = true;

    const DryRender looped = RenderDry(archive, loop, Mixing::kStrict, 3.0, kGo);
    const DryRender enhanced = RenderDry(archive, loop, Mixing::kEnhanced, 3.0, kGo);
    const DryRender dry = RenderDry(archive, send, Mixing::kStrict, 1.0, kGo);
    const ModelRun looped_model = RunModel(*archive, kLoopSound, 600, {}, {}, {}, kSilent);
    const ModelRun enhanced_model = RunModel(*archive, kLoopSound, 600, options, {}, {}, kSilent);
    const ModelRun dry_model = RunModel(*archive, kSendSound, 200, {}, {}, {}, kSilent);
    const ModelRun wet_model = RunModel(*archive, kSendSound, 200);

    const auto scaled = [](const ModelRun& run)
    {
        std::vector<float> floats(run.samples.size());
        std::transform(run.samples.begin(), run.samples.end(), floats.begin(),
                       [](int16_t sample) { return static_cast<float>(sample) / 32768.0f; });

        return floats;
    };

    SEQLIB_CHECK_EQ(looped.rate, 48000);
    SEQLIB_CHECK_EQ(looped.samples.size(), std::size_t{600 * kFrameSamples * 2});
    SEQLIB_CHECK(looped.samples == scaled(looped_model));
    SEQLIB_CHECK(enhanced.samples == enhanced_model.floats);
    const std::vector<float> dry_floats = scaled(dry_model);
    const std::vector<float> wet_floats = scaled(wet_model);
    SEQLIB_REQUIRE(dry.samples.size() < dry_floats.size());
    SEQLIB_CHECK(std::equal(dry.samples.begin(), dry.samples.end(), dry_floats.begin()));
    SEQLIB_CHECK(!HasSound(std::span<const float>(dry_floats).subspan(dry.samples.size())));
    SEQLIB_CHECK(HasSound(std::span<const float>(dry.samples)));
    SEQLIB_CHECK(!std::equal(dry.samples.begin(), dry.samples.end(), wet_floats.begin()));
    SEQLIB_CHECK(RenderDry(archive, loop, Mixing::kStrict, 3.0, kStop).samples.empty());
}

// An I3DL2 reverb's parameters go into nn::audio's set in the order of its fields, and come back out the same.
void TestI3dl2Order()
{
    std::array<float, hokuto::song::kI3dl2Parameters> parameters{};
    std::iota(parameters.begin(), parameters.end(), 1.0f);

    const ntdlib::nx::audio::I3dl2ReverbParameterSet reverb = I3dl2Reverb(parameters);

    SEQLIB_CHECK(reverb.room_gain == 1.0f && reverb.room_hf_gain == 2.0f && reverb.late_reverb_decay_time == 3.0f);
    SEQLIB_CHECK(reverb.late_reverb_hf_decay_ratio == 4.0f && reverb.reflections_gain == 5.0f &&
                 reverb.reflections_delay == 6.0f);
    SEQLIB_CHECK(reverb.reverb_gain == 7.0f && reverb.late_reverb_delay == 8.0f &&
                 reverb.late_reverb_diffusion == 9.0f);
    SEQLIB_CHECK(reverb.late_reverb_density == 10.0f && reverb.hf_reference == 11.0f && reverb.dry_gain == 12.0f);
    SEQLIB_CHECK(I3dl2Parameters(reverb) == parameters);
}

// Whether two conversions of a bank hold the same files, samples and warnings.
bool SameBankFiles(const export_::BankConversion& a, const export_::BankConversion& b)
{
    if (a.sound_fonts.size() != b.sound_fonts.size() || a.warnings != b.warnings ||
        a.sfz.files.size() != b.sfz.files.size() || a.sfz.samples.size() != b.sfz.samples.size())
    {
        return false;
    }

    for (std::size_t i = 0; i < a.sound_fonts.size(); i++)
    {
        if (a.sound_fonts[i].sf2 != b.sound_fonts[i].sf2 || a.sound_fonts[i].first != b.sound_fonts[i].first ||
            a.sound_fonts[i].last != b.sound_fonts[i].last)
        {
            return false;
        }
    }

    for (std::size_t i = 0; i < a.sfz.files.size(); i++)
    {
        if (a.sfz.files[i].name != b.sfz.files[i].name || a.sfz.files[i].text != b.sfz.files[i].text)
        {
            return false;
        }
    }

    for (std::size_t i = 0; i < a.sfz.samples.size(); i++)
    {
        if (a.sfz.samples[i].name != b.sfz.samples[i].name || a.sfz.samples[i].wav != b.sfz.samples[i].wav)
        {
            return false;
        }
    }

    return true;
}

// A bank's instruments go out as the files that ntdlib's converter makes of the bank with the settings of the archive's
// console: a SoundFont with a preset for each of programs 0, 1 and 3, and an SFZ file for each, named after the bank
// and the program. The Wii U's bank converts with the Wii U's settings.
void TestBankExport()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const std::shared_ptr<const sar::SoundArchive> wii_u = OpenWiiUArchive();

    const export_::BankConversion bank = ConvertBank(*archive, 0, "Hokuto");
    const export_::BankConversion cafe = ConvertBank(*wii_u, 0, "Hokuto");

    SEQLIB_CHECK(SameBankFiles(bank, export_::ConvertBank(*archive, 0, export_::SwitchSettings(), "Hokuto")));
    SEQLIB_CHECK(SameBankFiles(cafe, export_::ConvertBank(*wii_u, 0, export_::CafeSettings(), "Hokuto")));
    SEQLIB_REQUIRE(bank.sound_fonts.size() == 1 && bank.sfz.files.size() == 3);
    SEQLIB_CHECK(bank.warnings.empty() && seqlib::sf2::Read(bank.sound_fonts[0].sf2).Count("phdr", 38) == 4);
    SEQLIB_CHECK(bank.sfz.files[0].name == "BANK_TEST_0.sfz" && bank.sfz.files[1].name == "BANK_TEST_1.sfz" &&
                 bank.sfz.files[2].name == "BANK_TEST_3.sfz");
}

// A bank's SoundFont takes the export's name. SoundFonts of runs of programs take the name and their programs, with the
// digits of the last program.
void TestSoundFontNames()
{
    export_::BankConversion whole;
    whole.sound_fonts.resize(1);
    export_::BankConversion runs;
    runs.sound_fonts.resize(3);
    runs.sound_fonts[0].last = 48;
    runs.sound_fonts[1].first = 52;
    runs.sound_fonts[1].last = 99;
    runs.sound_fonts[2].first = 100;
    runs.sound_fonts[2].last = 130;

    const std::vector<std::string> one = SoundFontNames(whole, "BANK_GM");
    const std::vector<std::string> three = SoundFontNames(runs, "BANK_GM");

    SEQLIB_CHECK(one == std::vector<std::string>({"BANK_GM.sf2"}));
    SEQLIB_CHECK(three ==
                 std::vector<std::string>({"BANK_GM_000-048.sf2", "BANK_GM_052-099.sf2", "BANK_GM_100-130.sf2"}));
}

// A bank that the archive lacks stops the export with a message, and so do a bank without its file and a bank without
// instruments.
void TestBankExportErrors()
{
    sar_files::ArchiveSpec spec = TestArchiveSpec();
    sar_files::BankSpec empty;
    empty.waves = {{0x05000000, 0}};
    empty.instruments = {std::nullopt};
    spec.banks.push_back({"BANK_EMPTY", 6});
    spec.banks.push_back({"BANK_OUTSIDE", 7});
    spec.files.push_back({sar_files::Bank(false, empty), "", false});
    spec.files.push_back({{}, "", true});
    const TemporaryFile file(sar_files::Archive(false, spec));
    const std::shared_ptr<const sar::SoundArchive> archive = OpenArchive(file.Path());
    const auto error = [&archive](uint32_t bank)
    {
        return ErrorOf<std::runtime_error>([&] { ConvertBank(*archive, bank, "Hokuto"); });
    };

    const std::string without_instruments = error(1);
    const std::string without_file = error(2);
    const std::string missing = error(3);

    SEQLIB_CHECK(without_instruments == "BANK_EMPTY has no instruments.");
    SEQLIB_CHECK(without_file == "The archive has no file for BANK_OUTSIDE.");
    SEQLIB_CHECK(missing == "The archive has no bank 3.");
}

// A bank's SFZ files go into a folder with the bank's name, and their samples into the folder samples beside it. The
// files that the bank's folder holds already wait for the user's choice. A sample there with other bytes stops the
// export before it writes anything, and a sample with the same bytes stays.
void TestBankSfzLayout()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const export_::BankConversion bank = ConvertBank(*archive, 0, "Hokuto");
    SEQLIB_REQUIRE(!bank.sfz.samples.empty());
    const TemporaryFolder folder;
    std::vector<std::string> asked;
    const auto keep = [&asked](const std::vector<std::string>& existing)
    {
        asked = existing;

        return false;
    };
    const auto replace = [](const std::vector<std::string>&)
    {
        return true;
    };
    const std::filesystem::path sample = folder.Path() / "samples" / bank.sfz.samples[0].name;
    const std::vector<uint8_t> kOther = {1, 2, 3};

    const bool first = WriteBankSfz(bank, folder.Path(), "BANK_TEST", keep);
    const std::map<std::string, std::vector<uint8_t>> written = FilesUnder(folder.Path());
    std::filesystem::remove(folder.Path() / "BANK_TEST" / bank.sfz.files[1].name);
    const bool second = WriteBankSfz(bank, folder.Path(), "BANK_TEST", keep);
    const bool third = WriteBankSfz(bank, folder.Path(), "BANK_TEST", replace);
    xsflib::WriteFile(sample, kOther);
    std::filesystem::remove_all(folder.Path() / "BANK_TEST");
    const bool stopped = ThrowsRuntimeError([&] { WriteBankSfz(bank, folder.Path(), "BANK_TEST", replace); });

    SEQLIB_CHECK(first && !second && third && stopped);
    SEQLIB_CHECK(written.size() == bank.sfz.files.size() + bank.sfz.samples.size());
    SEQLIB_CHECK(written.contains("BANK_TEST/" + bank.sfz.files[0].name));
    SEQLIB_CHECK(written.contains("samples/" + bank.sfz.samples[0].name));
    SEQLIB_CHECK(asked == std::vector<std::string>({bank.sfz.files[0].name, bank.sfz.files[2].name}));
    SEQLIB_CHECK(!std::filesystem::exists(folder.Path() / "BANK_TEST"));
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("midi_export",
                                    []
                                    {
                                        TestExportLayout();
                                        TestExportChanges();
                                        TestExportKeepsOtherSamples();
                                        TestExportTags();
                                        TestExportTagBytes();
                                        TestReportFor();
                                        TestWiiExport();
                                        TestDryRender();
                                        TestI3dl2Order();
                                        TestBankExport();
                                        TestSoundFontNames();
                                        TestBankExportErrors();
                                        TestBankSfzLayout();
                                    });
}
