// SPDX-License-Identifier: MIT

// Tests the item files of the browser (src/song/items.cpp).

#include "song_runs.h"

namespace
{

// The test archive's bank holds programs 0, 1 and 3, and its wave archive two waves. Items that the archive lacks give
// nothing.
void TestPreviewItems()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();

    const std::vector<uint32_t> programs = ProgramsOf(*archive, 0);

    SEQLIB_CHECK(programs == std::vector<uint32_t>({0, 1, 3}));
    SEQLIB_CHECK(ProgramsOf(*archive, 1).empty());
    SEQLIB_CHECK_EQ(WaveCountOf(*archive, 0), 2);
    SEQLIB_CHECK_EQ(WaveCountOf(*archive, 1), 0);
}

// Each item's file holds the bytes that the test wrote into the archive: a sequence sound's sequence file, the bank,
// the wave archive, each of its waves, and a wave sound's file. An item that the archive lacks throws, and so does a
// sound of the other kind.
void TestItemFiles()
{
    const sar_files::ArchiveSpec spec = MadeUpWaveSoundSpec();
    const TemporaryFile file(sar_files::Archive(false, spec));
    const std::shared_ptr<const sar::SoundArchive> archive = OpenArchive(file.Path());
    const auto bytes = [&archive](const Item& item)
    {
        const std::span<const uint8_t> data = ItemFile(*archive, item);
        return std::vector<uint8_t>(data.begin(), data.end());
    };

    const std::vector<std::vector<uint8_t>> waves = TestWaveFiles(false);

    SEQLIB_CHECK(bytes({ItemKind::kSequence, kNoteSound, 0}) == spec.files[1].bytes);
    SEQLIB_CHECK(bytes({ItemKind::kBank, 0, 0}) == spec.files[2].bytes);
    SEQLIB_CHECK(bytes({ItemKind::kWaveArchive, 0, 0}) == spec.files[3].bytes);
    SEQLIB_CHECK(bytes({ItemKind::kWave, 0, 0}) == waves[0]);
    SEQLIB_CHECK(bytes({ItemKind::kWave, 0, 1}) == waves[1]);
    SEQLIB_CHECK(bytes({ItemKind::kWaveSound, kWaveSound, 0}) == spec.files[6].bytes);
    SEQLIB_CHECK(ThrowsRuntimeError([&] { ItemFile(*archive, {ItemKind::kSequence, kWaveSound, 0}); }));
    SEQLIB_CHECK(ThrowsRuntimeError([&] { ItemFile(*archive, {ItemKind::kWaveSound, kNoteSound, 0}); }));
    SEQLIB_CHECK(ThrowsRuntimeError([&] { ItemFile(*archive, {ItemKind::kSequence, 5, 0}); }));
    SEQLIB_CHECK(ThrowsRuntimeError([&] { ItemFile(*archive, {ItemKind::kBank, 1, 0}); }));
    SEQLIB_CHECK(ThrowsRuntimeError([&] { ItemFile(*archive, {ItemKind::kWaveArchive, 1, 0}); }));
    SEQLIB_CHECK(ThrowsRuntimeError([&] { ItemFile(*archive, {ItemKind::kWave, 0, 2}); }));
}

// A file that runs past the end of an archive that is cut short says so, on the Switch and on the Wii. The test
// archives end halfway through a bank's file. The Wii's notes of the bank's file and wave data say so too. The archive
// that isn't cut short keeps its notes as they are.
void TestCutShortItemFiles()
{
    const sar_files::ArchiveSpec spec = MadeUpWaveSoundSpec();
    const TemporaryFile file(CutInside(sar_files::Archive(false, spec), spec.files[2].bytes));
    const std::shared_ptr<const sar::SoundArchive> archive = OpenArchive(file.Path());

    // The Wii's archive contains a bank file (RBNK).
    namespace rsar_files = ntdlib::testing::rsar_files;
    const std::vector<uint8_t> whole = rsar_files::MakeArchive(rsar_files::MakeSequence({0xFF}), 0, 1);
    const std::vector<uint8_t> kBankMagic = {'R', 'B', 'N', 'K'};
    const std::shared_ptr<const ntdlib::rsar::SoundArchive> wii =
        LoadWiiArchive(CutInside(whole, kBankMagic), "SOUND.brsar", ntdlib::rvl::snd::kRsarOptions);
    const std::shared_ptr<const ntdlib::rsar::SoundArchive> wii_whole =
        LoadWiiArchive(whole, "SOUND.brsar", ntdlib::rvl::snd::kRsarOptions);
    const uint32_t bank_file = wii->Banks()[0].file_id;

    const std::string message = ErrorOf<std::runtime_error>([&] { ItemFile(*archive, {ItemKind::kBank, 0, 0}); });
    const std::string wii_message = ErrorOf<std::runtime_error>([&] { ItemFile(*wii, {ItemKind::kBank, 0, 0}); });
    const std::string note = WiiMissingNote(*wii, bank_file, "The bank file is missing.");
    const std::string whole_note = WiiMissingNote(*wii_whole, bank_file, "The bank file is missing.");

    SEQLIB_CHECK(message == "The archive is cut short. The file for BANK_TEST is missing.");
    SEQLIB_CHECK(wii_message == "The archive is cut short. The file for " + wii->Banks()[0].name + " is missing.");
    SEQLIB_CHECK(note == "The archive is cut short. The bank file is missing.");
    SEQLIB_CHECK(whole_note == "The bank file is missing.");
}

// An item's file takes the item's name and its format's extension. A wave takes its archive's name and its index, with
// zeros in front up to the digits of the archive's last index. A name loses the characters that a file name can't hold.
// The user sees a wave as its archive's name and its index.
void TestItemNames()
{
    sar_files::ArchiveSpec spec = TestArchiveSpec();
    spec.banks[0].first = "BANK:TEST";
    const TemporaryFile file(sar_files::Archive(false, spec));
    const std::shared_ptr<const sar::SoundArchive> archive = OpenArchive(file.Path());

    const std::string sequence = ItemFileName(*archive, {ItemKind::kSequence, kNoteSound, 0});
    const std::string bank = ItemFileName(*archive, {ItemKind::kBank, 0, 0});
    const std::string wave_archive = ItemFileName(*archive, {ItemKind::kWaveArchive, 0, 0});

    SEQLIB_CHECK(sequence == "SEQ_NOTE.bfseq");
    SEQLIB_CHECK(bank == "BANK_TEST.bfbnk");
    SEQLIB_CHECK(wave_archive == "WARC_TEST.bfwar");
    SEQLIB_CHECK(ItemFileName(*archive, {ItemKind::kWave, 0, 1}, 2) == "WARC_TEST_1.bfwav");
    SEQLIB_CHECK(ItemFileName(*archive, {ItemKind::kWave, 0, 3}, 11) == "WARC_TEST_03.bfwav");
    SEQLIB_CHECK(ItemFileName(*archive, {ItemKind::kWave, 0, 10}, 11) == "WARC_TEST_10.bfwav");
    SEQLIB_CHECK(ItemFileName(*archive, {ItemKind::kWave, 0, 7}, 300) == "WARC_TEST_007.bfwav");
    SEQLIB_CHECK(ItemName(*archive, {ItemKind::kBank, 0, 0}) == "BANK:TEST");
    SEQLIB_CHECK(ItemName(*archive, {ItemKind::kWave, 0, 1}) == "WARC_TEST, wave 1");
}

// The message about a folder that can't be made ends with one full stop. The Mac's reasons have none, such as "File
// exists". Those of Windows have one, such as "Access is denied."
void TestFolderFailure()
{
    const ReasonCategory kMac("File exists");
    const ReasonCategory kWindows("Access is denied.");

    const std::string without = FolderFailure("BANK", std::error_code(17, kMac));
    const std::string with = FolderFailure("BANK", std::error_code(5, kWindows));

    SEQLIB_CHECK(without == "Can't create the folder BANK. File exists.");
    SEQLIB_CHECK(with == "Can't create the folder BANK. Access is denied.");
}

// Three wave sound files hold five wave sounds. The first file's wave sounds come first, and one of them comes last. A
// sound group with a name holds the first file's first two wave sounds, and one without a name the second file's. The
// third file's wave sound lies in a group of another file only. Each file lists its wave sounds in order, under its
// group's name, WSDSET_ and the unnamed group's index, or its first wave sound's name. Each wave sound gives its file
// as a wave sound file, under the file's name. A sequence sound gives none.
void TestWaveSoundSets()
{
    sar_files::ArchiveSpec spec = TestArchiveSpec();
    const auto add = [&spec](const char* name, uint32_t file)
    {
        sar_files::SoundSpec sound;
        sound.name = name;
        sound.kind = sar_files::SoundSpec::kWaveSound;
        sound.file = file;
        spec.sounds.push_back(sound);
    };
    add("WSD_A1", 6);
    add("WSD_A2", 6);
    add("WSD_B", 7);
    add("WSD_C", 8);
    add("WSD_A3", 6);
    for (const uint8_t value : std::initializer_list<uint8_t>{1, 2, 3})
    {
        spec.files.push_back({{'F', 'W', 'S', 'D', value, value, value, value}, "", false});
    }
    spec.sound_groups = {{"WSDSET:A", 4, 5, {6}}, {"", 6, 6, {7}}, {"WSDSET_OTHER", 7, 7, {9}}};
    const TemporaryFile file(sar_files::Archive(false, spec));
    const std::shared_ptr<const sar::SoundArchive> archive = OpenArchive(file.Path());
    const auto bytes = [&archive](const Item& item)
    {
        const std::span<const uint8_t> data = ItemFile(*archive, item);
        return std::vector<uint8_t>(data.begin(), data.end());
    };

    const std::vector<WaveSoundSet> sets = WaveSoundSets(*archive);

    SEQLIB_CHECK_EQ(sets.size(), std::size_t{3});
    SEQLIB_CHECK(sets.size() == 3 && sets[0].file_id == 6 && sets[0].name == "WSDSET:A" &&
                 sets[0].sounds == std::vector<uint32_t>({4, 5, 8}));
    SEQLIB_CHECK(sets.size() == 3 && sets[1].file_id == 7 && sets[1].name == "WSDSET_1" &&
                 sets[1].sounds == std::vector<uint32_t>({6}));
    SEQLIB_CHECK(sets.size() == 3 && sets[2].file_id == 8 && sets[2].name == "WSD_C" &&
                 sets[2].sounds == std::vector<uint32_t>({7}));
    SEQLIB_CHECK(ItemName(*archive, {ItemKind::kWaveSoundFile, 8, 0}) == "WSDSET:A");
    SEQLIB_CHECK(ItemFileName(*archive, {ItemKind::kWaveSoundFile, 4, 0}) == "WSDSET_A.bfwsd");
    SEQLIB_CHECK(ItemFileName(*archive, {ItemKind::kWaveSoundFile, 6, 0}) == "WSDSET_1.bfwsd");
    SEQLIB_CHECK(bytes({ItemKind::kWaveSoundFile, 5, 0}) == spec.files[6].bytes);
    SEQLIB_CHECK(bytes({ItemKind::kWaveSoundFile, 7, 0}) == spec.files[8].bytes);
    SEQLIB_CHECK(hokuto::song::ItemFormat(ItemKind::kWaveSoundFile, sar::Flavor::kCafe) == "BFWSD");
    SEQLIB_CHECK(ThrowsRuntimeError([&] { ItemName(*archive, {ItemKind::kWaveSoundFile, kNoteSound, 0}); }));
    SEQLIB_CHECK(WaveSoundSets(*OpenTestArchive()).empty());
}

// A wave goes out as a WAV file of 16-bit PCM: the samples that the test wrote into it, at its rate, with its loop. A
// mono wave stays mono. The Wii U's waves give the same files. A wave that the archive lacks throws.
void TestWaveAsWav()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const std::shared_ptr<const sar::SoundArchive> wii_u = OpenWiiUArchive();
    seqlib::Pcm mono;
    mono.loop = true;
    mono.loop_end = 480;
    mono.channels.resize(1);
    seqlib::Pcm stereo = mono;
    stereo.channels.resize(2);
    for (int i = 0; i < 480; i++)
    {
        mono.channels[0].push_back(static_cast<int16_t>(SawSample(i, false)));
        stereo.channels[0].push_back(static_cast<int16_t>(SawSample(i, false)));
        stereo.channels[1].push_back(static_cast<int16_t>(SawSample(i, true)));
    }

    const std::vector<uint8_t> mono_file = WaveAsWav(*archive, 0, 0);
    const std::vector<uint8_t> stereo_file = WaveAsWav(*archive, 0, 1);

    SEQLIB_CHECK(mono_file == seqlib::WavFile(mono, 48000));
    SEQLIB_CHECK(stereo_file == seqlib::WavFile(stereo, 48000));
    SEQLIB_CHECK(WaveAsWav(*wii_u, 0, 1) == stereo_file);
    SEQLIB_CHECK_EQ(WaveChannels(*archive, 0, 0), 1);
    SEQLIB_CHECK_EQ(WaveChannels(*archive, 0, 1), 2);
    SEQLIB_CHECK(ThrowsRuntimeError([&] { WaveAsWav(*archive, 0, 2); }));
    SEQLIB_CHECK(ThrowsRuntimeError([&] { WaveAsWav(*archive, 1, 0); }));
}

// WaveSoundNoteOf gives a wave sound's note 0, and the sound's pitch times the note's. The consoles take the sound's
// pitch alone. The WAV file holds the wave's samples and loop at the rate that WaveSoundNoteOf's pitch gives, to the
// nearest hertz. The Wii U's wave sounds give the same files. A wave sound reports the note events of all its tracks, a
// pan away from the centre and an envelope that shapes the sound, from the sound or from its note. The wave sounds that
// WaveSoundArchiveSpec makes to fail throw with a message for the user, and so does a sequence sound.
void TestWaveSoundAsWav()
{
    const TemporaryFile switch_file(sar_files::Archive(false, WaveSoundArchiveSpec(false)));
    const TemporaryFile wii_u_file(sar_files::Archive(true, WaveSoundArchiveSpec(true)));
    const std::shared_ptr<const sar::SoundArchive> archive = OpenArchive(switch_file.Path());
    const std::shared_ptr<const sar::SoundArchive> wii_u = OpenArchive(wii_u_file.Path());
    seqlib::Pcm saw;
    saw.loop = true;
    saw.loop_end = 480;
    saw.channels.resize(1);
    seqlib::Pcm stereo = saw;
    stereo.channels.resize(2);
    for (int i = 0; i < 480; i++)
    {
        saw.channels[0].push_back(static_cast<int16_t>(SawSample(i, false)));
        stereo.channels[0].push_back(static_cast<int16_t>(SawSample(i, false)));
        stereo.channels[1].push_back(static_cast<int16_t>(SawSample(i, true)));
    }

    const WaveSoundNote high = WaveSoundNoteOf(*archive, kHighSound);
    const WaveSoundNote panned = WaveSoundNoteOf(*archive, kPannedSound);
    const WaveSoundNote plain = WaveSoundNoteOf(*archive, kPlainSound);
    const std::vector<uint8_t> high_file = WaveSoundAsWav(*archive, kHighSound);
    const std::vector<uint8_t> panned_file = WaveSoundAsWav(*archive, kPannedSound);
    const std::vector<uint8_t> plain_file = WaveSoundAsWav(*archive, kPlainSound);
    WavePreview preview(DecodeWaveSound(*archive, kHighSound));

    const auto error = [&archive](uint32_t sound)
    {
        return ErrorOf<std::runtime_error>([&] { WaveSoundAsWav(*archive, sound); });
    };

    SEQLIB_CHECK(high.wave_archive == 1 && high.wave == 0 && high.pitch == 1.5f);
    SEQLIB_CHECK(high.panned && high.shaped && high.events == 1);
    SEQLIB_CHECK(panned.wave_archive == 1 && panned.wave == 1 && panned.pitch == 0.5f);
    SEQLIB_CHECK(panned.panned && panned.shaped && panned.events == 3);
    SEQLIB_CHECK(!plain.panned && !plain.shaped && plain.events == 1);
    SEQLIB_CHECK(high_file == seqlib::WavFile(stereo, 72000));
    SEQLIB_CHECK(panned_file == seqlib::WavFile(saw, 24000));
    SEQLIB_CHECK(plain_file == seqlib::WavFile(saw, 48001));
    SEQLIB_CHECK(WaveSoundAsWav(*wii_u, kHighSound) == high_file);
    SEQLIB_CHECK_EQ(preview.SampleRate(), 72000);
    SEQLIB_CHECK(WaveSoundWavFileName(*archive, kHighSound) == "WSD_HIGH.wav");
    SEQLIB_CHECK(SoundsSharingFile(*archive, kHighSound) ==
                 std::vector<uint32_t>({kPannedSound, kSilentSound, kPastSound, kLostSound, kPlainSound, kNoNoteSound,
                                        kNoWaveSound, kBackwardsSound}));
    SEQLIB_CHECK(error(kSilentSound) == "WSD_SILENT has no notes.");
    SEQLIB_CHECK(error(kPastSound) == "The wave sound file of WSD_PAST has no wave sound 8.");
    SEQLIB_CHECK(error(kLostSound) == "The archive is missing the wave archive that WSD_LOST uses.");
    SEQLIB_CHECK(error(kNoNoteSound) == "WSD_NO_NOTE has no notes.");
    SEQLIB_CHECK(error(kNoWaveSound) == "The wave sound file of WSD_NO_WAVE is missing the wave that the sound uses.");
    SEQLIB_CHECK(error(kBackwardsSound) ==
                 "WSD_BACKWARDS plays its wave at a pitch of -1. No WAV sample rate matches that pitch.");
    SEQLIB_CHECK(error(kNoteSound) == "The archive has no wave sound 1.");
}

// The test archive with a wave sound file of two sounds, after the four sequence sounds. Each sound plays note 0,
// whatever its note events name: the consoles read none of the events (ntdlib's WaveSoundPlayer). Note 0 holds the saw
// and a pitch of 0.5, and note 1 the stereo wave and a pitch of 2. The consoles read neither pitch. The WAV export
// takes note 0's, and the rate of 24 kHz follows the export. WSD_LATER's first note event names note 1. WSD_UNPLAYED
// holds a track without note events.
sar_files::ArchiveSpec NoteZeroArchiveSpec(bool big_endian)
{
    sar_files::Region zero;
    zero.pitch = 0.5f;
    sar_files::Region other;
    other.wave = 1;
    other.pitch = 2.0f;
    sar_files::WaveSoundSpec later;
    later.tracks = {{{0.0f, 0.0f, 1}, {0.5f, 0.0f, 0}}};
    later.notes = {zero, other};
    sar_files::WaveSoundSpec unplayed;
    unplayed.tracks = {{}};
    unplayed.notes = {zero, other};
    sar_files::WaveSoundFileSpec file;
    file.waves = {{0x05000001, 1}, {0x05000001, 0}};
    file.sounds = {later, unplayed};

    sar_files::ArchiveSpec spec = TestArchiveSpec(big_endian);
    const std::array<const char*, 2> kSounds = {"WSD_LATER", "WSD_UNPLAYED"};
    for (std::size_t i = 0; i < kSounds.size(); i++)
    {
        sar_files::SoundSpec sound;
        sound.name = kSounds[i];
        sound.kind = sar_files::SoundSpec::kWaveSound;
        sound.file = 6;
        sound.wave_sound_index = static_cast<uint32_t>(i);
        spec.sounds.push_back(sound);
    }
    const std::vector<std::vector<uint8_t>> waves = TestWaveFiles(big_endian);
    spec.wave_archives.push_back({"WARC_SECOND", 7});
    spec.files.push_back({sar_files::WaveSoundFile(big_endian, file), "", false});
    spec.files.push_back({sar_files::WaveArchive(big_endian, {waves[1], waves[0]}), "", false});

    return spec;
}

// A wave sound's WAV file holds the wave of its note 0 at the sound's pitch times the note's (see TestWaveSoundAsWav).
// A note event that names another note, and a sound without note events, change nothing. The Wii U's wave sounds give
// the same files.
void TestWaveSoundNoteZero()
{
    const TemporaryFile switch_file(sar_files::Archive(false, NoteZeroArchiveSpec(false)));
    const TemporaryFile wii_u_file(sar_files::Archive(true, NoteZeroArchiveSpec(true)));
    const std::shared_ptr<const sar::SoundArchive> archive = OpenArchive(switch_file.Path());
    const std::shared_ptr<const sar::SoundArchive> wii_u = OpenArchive(wii_u_file.Path());
    seqlib::Pcm saw;
    saw.loop = true;
    saw.loop_end = 480;
    saw.channels.resize(1);
    for (int i = 0; i < 480; i++)
    {
        saw.channels[0].push_back(static_cast<int16_t>(SawSample(i, false)));
    }

    const WaveSoundNote later = WaveSoundNoteOf(*archive, 4);
    const WaveSoundNote unplayed = WaveSoundNoteOf(*archive, 5);
    const std::vector<uint8_t> later_file = WaveSoundAsWav(*archive, 4);
    const std::vector<uint8_t> unplayed_file = WaveSoundAsWav(*archive, 5);

    SEQLIB_CHECK(later.wave_archive == 1 && later.wave == 1 && later.pitch == 0.5f && later.events == 2);
    SEQLIB_CHECK(unplayed.wave_archive == 1 && unplayed.wave == 1 && unplayed.pitch == 0.5f && unplayed.events == 0);
    SEQLIB_CHECK(later_file == seqlib::WavFile(saw, 24000));
    SEQLIB_CHECK(unplayed_file == seqlib::WavFile(saw, 24000));
    SEQLIB_CHECK(WaveSoundAsWav(*wii_u, 4) == later_file);
    SEQLIB_CHECK(WaveSoundAsWav(*wii_u, 5) == unplayed_file);
}

// A sequence file can hold the sequences of several sounds. With SEQ_NOTE on SEQ_LOOP's file, each of the two shares
// the file with the other, and both go out as the same bytes. SEQ_PAN shares its file with no sound.
void TestSharedSequenceFiles()
{
    sar_files::ArchiveSpec spec = TestArchiveSpec();
    spec.sounds[kNoteSound].file = 0;
    const TemporaryFile file(sar_files::Archive(false, spec));
    const std::shared_ptr<const sar::SoundArchive> archive = OpenArchive(file.Path());

    const std::vector<uint32_t> loop = SoundsSharingFile(*archive, kLoopSound);
    const std::vector<uint32_t> note = SoundsSharingFile(*archive, kNoteSound);
    const std::span<const uint8_t> loop_file = ItemFile(*archive, {ItemKind::kSequence, kLoopSound, 0});
    const std::span<const uint8_t> note_file = ItemFile(*archive, {ItemKind::kSequence, kNoteSound, 0});

    SEQLIB_CHECK(loop == std::vector<uint32_t>({kNoteSound}));
    SEQLIB_CHECK(note == std::vector<uint32_t>({kLoopSound}));
    SEQLIB_CHECK(SoundsSharingFile(*archive, kPanSound).empty());
    SEQLIB_CHECK(loop_file.data() == note_file.data() && loop_file.size() == note_file.size());
}

// A sequence file goes to the other console with the same commands in that console's byte order. A file for the Wii U
// takes version 1.0.0. ntdlib reads it as a file of that console. Converted back, it gives the archive's bytes. Bytes
// that aren't a sequence file stop the conversion with a message.
void TestOtherConsoleSequenceFile()
{
    sar_files::ArchiveSpec spec = TestArchiveSpec();
    spec.sounds[kNoteSound].file = 0;
    const TemporaryFile file(sar_files::Archive(false, spec));
    const std::shared_ptr<const sar::SoundArchive> archive = OpenArchive(file.Path());
    const std::span<const uint8_t> bytes = ItemFile(*archive, {ItemKind::kSequence, kLoopSound, 0});
    const std::vector<uint32_t> starts = {archive->Sounds()[kLoopSound].start_offset,
                                          archive->Sounds()[kNoteSound].start_offset};
    sar::Options options;
    options.flavor = sar::Flavor::kCafe;

    const std::vector<uint8_t> wii_u = OtherConsoleSequenceFile(*archive, kLoopSound, sar::Flavor::kCafe).file;
    const sar::Sequence parsed = sar::Sequence::Parse(wii_u, options);
    const std::span<const uint8_t> commands = parsed.data.first(std::min(parsed.data.size(), kWiiULoopBytecode.size()));
    const std::vector<uint8_t> back = ConvertSequenceFile(wii_u, sar::Flavor::kCafe, sar::Flavor::kNx, starts).file;
    const bool refused = ThrowsRuntimeError(
        [] { ConvertSequenceFile(std::vector<uint8_t>(64), sar::Flavor::kNx, sar::Flavor::kCafe, {}); });

    SEQLIB_CHECK(OtherConsole(sar::Flavor::kNx) == sar::Flavor::kCafe);
    SEQLIB_CHECK(OtherConsole(sar::Flavor::kCafe) == sar::Flavor::kNx);
    SEQLIB_CHECK(wii_u.size() == bytes.size() && wii_u[4] == 0xFE && wii_u[5] == 0xFF && parsed.big_endian);
    SEQLIB_CHECK(wii_u[8] == 0x00 && wii_u[9] == 0x01 && wii_u[10] == 0x00 && wii_u[11] == 0x00);
    SEQLIB_CHECK(std::equal(commands.begin(), commands.end(), kWiiULoopBytecode.begin(), kWiiULoopBytecode.end()));
    SEQLIB_CHECK(std::equal(back.begin(), back.end(), bytes.begin(), bytes.end()));
    SEQLIB_CHECK(refused);
}

// A bank, a wave sound file, a wave archive and a wave go to the other console with the same structures in that
// console's byte order. The bank is the one that the test builder writes for the Wii U, and so is the wave sound file
// but for its version, 1.0.0. Converted back with the Switch's versions, they give the archive's bytes. The wave
// archive takes version 1.0.0 and its waves 1.1.0. Each wave decodes to the Wii U's samples, with the same rate and
// loop. Bytes of another kind or of another console stop the conversion with a message, and a sequence file isn't
// converted here.
void TestOtherConsoleFiles()
{
    const sar_files::ArchiveSpec wii_u_spec = WaveSoundArchiveSpec(true);
    const TemporaryFile file(sar_files::Archive(false, WaveSoundArchiveSpec(false)));
    const std::shared_ptr<const sar::SoundArchive> archive = OpenArchive(file.Path());
    const std::span<const uint8_t> bank = ItemFile(*archive, {ItemKind::kBank, 0, 0});
    const std::span<const uint8_t> sounds = ItemFile(*archive, {ItemKind::kWaveSound, kHighSound, 0});
    const std::vector<std::vector<uint8_t>> wii_u_waves = TestWaveFiles(true);
    std::vector<uint8_t> older_sounds = wii_u_spec.files[6].bytes;
    xsflib::StoreBe32(&older_sounds[0x08], 0x00010000);
    sar::Options options;
    options.flavor = sar::Flavor::kCafe;
    options.rules = sar::Rules::kStopAtFirstProblem;

    const auto version = [](std::span<const uint8_t> bytes)
    {
        return xsflib::Reader(bytes, true).U32(0x08);
    };

    const auto same_wave = [&options](std::span<const uint8_t> ours, std::span<const uint8_t> theirs)
    {
        const sar::Wave a = sar::Wave::Parse(ours, options);
        const sar::Wave b = sar::Wave::Parse(theirs, options);
        const sar::Pcm x = sar::DecodeWave(ours, options);
        const sar::Pcm y = sar::DecodeWave(theirs, options);
        return x.channels == y.channels && x.sample_rate == y.sample_rate && a.loop == b.loop &&
               a.loop_start == b.loop_start && a.loop_end == b.loop_end && a.encoding == b.encoding;
    };

    const sar::Flavor kWiiU = sar::Flavor::kCafe;
    const sar::Flavor kSwitch = sar::Flavor::kNx;
    const std::vector<uint8_t> wii_u_bank = OtherConsoleFile(*archive, {ItemKind::kBank, 0, 0}, kWiiU);
    const std::vector<uint8_t> wii_u_sounds = OtherConsoleFile(*archive, {ItemKind::kWaveSound, kHighSound, 0}, kWiiU);
    const std::vector<uint8_t> wii_u_archive = OtherConsoleFile(*archive, {ItemKind::kWaveArchive, 0, 0}, kWiiU);
    const std::vector<uint8_t> wii_u_wave = OtherConsoleFile(*archive, {ItemKind::kWave, 0, 1}, kWiiU);
    const std::vector<uint8_t> bank_back =
        ConvertItemFile(wii_u_bank, ItemKind::kBank, kWiiU, kSwitch, ExportVersions(ItemKind::kBank, kSwitch));
    const std::vector<uint8_t> sounds_back = ConvertItemFile(wii_u_sounds, ItemKind::kWaveSound, kWiiU, kSwitch,
                                                             ExportVersions(ItemKind::kWaveSound, kSwitch));
    const std::vector<std::span<const uint8_t>> waves = sar::WaveArchive::Parse(wii_u_archive, options).waves;
    const bool other_kind =
        ThrowsRuntimeError([&] { ConvertItemFile(bank, ItemKind::kWaveSound, kSwitch, kWiiU, FileVersions{}); });
    const bool other_console =
        ThrowsRuntimeError([&] { ConvertItemFile(bank, ItemKind::kBank, kWiiU, kSwitch, FileVersions{}); });
    const bool sequence = !ErrorOf<std::invalid_argument>(
                               [&] { ConvertItemFile(bank, ItemKind::kSequence, kSwitch, kWiiU, FileVersions{}); })
                               .empty();

    SEQLIB_CHECK(wii_u_bank == wii_u_spec.files[2].bytes);
    SEQLIB_CHECK(wii_u_sounds == older_sounds);
    SEQLIB_CHECK(std::equal(bank_back.begin(), bank_back.end(), bank.begin(), bank.end()));
    SEQLIB_CHECK(std::equal(sounds_back.begin(), sounds_back.end(), sounds.begin(), sounds.end()));
    SEQLIB_CHECK(version(wii_u_archive) == 0x00010000 && waves.size() == 2);
    for (std::size_t i = 0; i < waves.size() && i < wii_u_waves.size(); i++)
    {
        SEQLIB_CHECK(version(waves[i]) == 0x00010100 && same_wave(waves[i], wii_u_waves[i]));
    }
    SEQLIB_CHECK(version(wii_u_wave) == 0x00010100 && same_wave(wii_u_wave, wii_u_waves[1]));
    SEQLIB_CHECK(other_kind && other_console && sequence);
}

// The Wii's wave sounds, in archives of wave sounds that ntdlib's testing/rsar_files.h builds. The wave data of a wave
// sound file lists as a wave archive under the file's name: an RWAR from version 1.3 on, and before it the samples that
// the file's WAVE block describes, in DSP-ADPCM here. Only an RWAR goes out as a file. NW4R SND plays note 0 of a wave
// sound at the sound's pitch. The note's key, pan and pitch stay unread. A wave sound's WAV file holds the ramp that
// its note's wave holds, at the rate that the sound's pitch gives, with the wave's loop. A wave sound without a note,
// one whose note's wave the file lacks and a sound past the table throw with a message for the user.
void TestWiiWaveSoundFiles()
{
    const WiiWaveSoundArchives made = MakeWiiWaveSoundArchives();
    const std::vector<uint8_t>& rwar = made.rwar;
    const std::shared_ptr<const ntdlib::rsar::SoundArchive>& archive = made.archive;
    const std::shared_ptr<const ntdlib::rsar::SoundArchive>& old_archive = made.old_archive;

    const std::vector<WiiWaveArchive> wave_archives = WaveArchivesOf(*archive);
    const std::vector<WiiWaveArchive> old_wave_archives = WaveArchivesOf(*old_archive);

    SEQLIB_REQUIRE(wave_archives.size() == 1 && old_wave_archives.size() == 1);
    SEQLIB_CHECK(wave_archives[0].file_id == 0 && wave_archives[0].rwar && wave_archives[0].name == "WSD_FILE_0");
    SEQLIB_CHECK(old_wave_archives[0].file_id == 0 && !old_wave_archives[0].rwar);
    SEQLIB_CHECK_EQ(WaveCountOf(*archive, 0), 2);
    SEQLIB_CHECK_EQ(WaveCountOf(*old_archive, 0), 2);
    const std::span<const uint8_t> rwar_file = ItemFile(*archive, {ItemKind::kWaveArchive, 0, 0});
    SEQLIB_CHECK(std::ranges::equal(rwar_file, rwar));
    SEQLIB_CHECK(ThrowsRuntimeError([&] { ItemFile(*old_archive, {ItemKind::kWaveArchive, 0, 0}); }));
    SEQLIB_CHECK(hokuto::song::DecodeItemWave(*old_archive, 0, 1).Length() > 0);

    const WaveSoundNote note = WaveSoundNoteOf(*archive, 0);
    const WaveSoundNote plain = WaveSoundNoteOf(*archive, 1);

    SEQLIB_CHECK(note.wave_archive == 0 && note.wave == 0 && note.pitch == 1.5f);
    SEQLIB_CHECK(note.panned && !note.shaped && note.events == 0);
    SEQLIB_CHECK(plain.wave_archive == 0 && plain.wave == 1 && plain.pitch == 1.0f && !plain.panned);

    // Each WAV file holds its wave at the wave's rate times the sound's pitch.
    seqlib::Pcm once;
    once.channels = {WiiRamp(9600)};
    seqlib::Pcm round;
    round.loop = true;
    round.loop_end = 960;
    round.channels = {WiiRamp(960)};

    SEQLIB_CHECK(WaveSoundAsWav(*archive, 0) == seqlib::WavFile(once, 48000));
    SEQLIB_CHECK(WaveSoundAsWav(*archive, 1) == seqlib::WavFile(round, 32000));
    SEQLIB_CHECK(DecodeWaveSound(*archive, 0).sample_rate == 48000);
    SEQLIB_CHECK(WaveSoundWavFileName(*archive, 0) == "WSD_0.wav");
    SEQLIB_CHECK(
        DecodeWaveSound(*old_archive, 0).sample_rate ==
        static_cast<uint32_t>(std::lround(hokuto::song::DecodeItemWave(*old_archive, 0, 0).sample_rate * 1.5)));

    const auto error = [&archive](uint32_t sound)
    {
        return ErrorOf<std::runtime_error>([&] { WaveSoundNoteOf(*archive, sound); });
    };
    SEQLIB_CHECK(error(2) == "The wave sound file of WSD_2 is missing the wave that the sound uses.");
    SEQLIB_CHECK(error(3) == "WSD_3 has no notes.");
    SEQLIB_CHECK(error(4) == "The archive has no wave sound 4.");
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("items",
                                    []
                                    {
                                        TestPreviewItems();
                                        TestItemFiles();
                                        TestCutShortItemFiles();
                                        TestItemNames();
                                        TestFolderFailure();
                                        TestWaveSoundSets();
                                        TestWaveAsWav();
                                        TestWaveSoundAsWav();
                                        TestWaveSoundNoteZero();
                                        TestSharedSequenceFiles();
                                        TestOtherConsoleSequenceFile();
                                        TestOtherConsoleFiles();
                                        TestWiiWaveSoundFiles();
                                    });
}
