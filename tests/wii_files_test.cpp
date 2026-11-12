// SPDX-License-Identifier: MIT

// Tests the replacement of the Wii's files (src/edit/wii_files.cpp).

#include "files_fixtures.h"

namespace
{

// The copies of file `id` in the groups of `archive`, an archive of the Wii, and of its wave data with `wave_data`.
std::vector<std::vector<uint8_t>> WiiCopies(std::span<const uint8_t> archive, uint32_t id, bool wave_data)
{
    const ntdlib::rsar::SoundArchive read = ntdlib::rsar::SoundArchive::Load(
        std::vector<uint8_t>(archive.begin(), archive.end()), {.playback_fields = true});
    std::vector<std::vector<uint8_t>> copies;
    for (const ntdlib::rsar::SoundArchive::Group& group : read.Groups())
    {
        for (const ntdlib::rsar::SoundArchive::Group::Item& item : group.items)
        {
            if (item.file_id == id)
            {
                const std::size_t start =
                    wave_data ? group.wave_data_offset + item.wave_data_offset : group.offset + item.offset;
                const std::size_t size = wave_data ? item.wave_data_size : item.size;
                copies.emplace_back(archive.begin() + start, archive.begin() + start + size);
            }
        }
    }

    return copies;
}

// A Wii archive holds a song, and a bank with its wave data in both of its groups. A new bank of 0x50 bytes takes the
// old bank's place in both, with zeros up to 0x60, and keeps the wave data. New wave data then takes the place of the
// bank's, and the bank keeps its bytes. The song stays as it was. A new file in the song's place is refused: the song's
// file lies among the archive's other bytes. Wave data for a file of a later console is refused too.
void TestWiiReplace()
{
    ntdlib::rsar::SequenceFile song;
    song.data = {0xC1, 0x64, 0x3C, 0x64, 0x30, 0x80, 0x30, 0xFF};
    song.labels = {{"SEQ_SONG", 0}};
    const std::vector<uint8_t> kBankBytes(0x40, 0x5A);
    const std::vector<uint8_t> kWaveBytes(0x20, 0xA5);
    const std::vector<uint8_t> original = ntdlib::testing::rsar_tools::MakeArchive(
        0x0104, {{song.Write()}, {kBankBytes, kWaveBytes}}, {{"SEQ_SONG", 0, 0}}, {{0, 1}, {1}});
    const File file = hokuto::format::ImportWiiArchive(original);

    const std::vector<uint8_t> kNewBankBytes(0x50, 0x3C);
    std::vector<uint8_t> padded = kNewBankBytes;
    padded.resize(0x60, 0);
    const std::vector<uint8_t> kNewWaveBytes(0x40, 0x7E);
    const File replaced = hokuto::edit::ReplaceFile(file, 1, kNewBankBytes);
    const File rewaved = hokuto::edit::ReplaceFile(replaced, 1, padded, kNewWaveBytes);
    const std::vector<uint8_t> after = hokuto::format::ExportArchive(replaced);
    const std::vector<uint8_t> after_waves = hokuto::format::ExportArchive(rewaved);

    const auto refusal = [](const File& from, uint32_t id, std::span<const uint8_t> bytes)
    {
        return ErrorOf<std::invalid_argument>([&] { hokuto::edit::ReplaceFile(from, id, bytes, bytes); });
    };

    XSFLIB_CHECK(hokuto::format::ExportArchive(file) == original);
    XSFLIB_CHECK((WiiCopies(after, 1, false) == std::vector<std::vector<uint8_t>>(2, padded)));
    XSFLIB_CHECK((WiiCopies(after, 1, true) == std::vector<std::vector<uint8_t>>(2, kWaveBytes)));
    XSFLIB_CHECK((WiiCopies(after_waves, 1, false) == std::vector<std::vector<uint8_t>>(2, padded)));
    XSFLIB_CHECK((WiiCopies(after_waves, 1, true) == std::vector<std::vector<uint8_t>>(2, kNewWaveBytes)));
    XSFLIB_CHECK(after.size() == original.size() + 2 * 0x20 && after_waves.size() == after.size() + 2 * 0x20);
    XSFLIB_CHECK(WiiCopies(after_waves, 0, false) == WiiCopies(original, 0, false));
    XSFLIB_CHECK(refusal(file, 0, kBankBytes).starts_with("The old file is mixed in with other data in the archive."));
    XSFLIB_CHECK(refusal(hokuto::format::ImportArchive(TestArchive(sar::Flavor::kNx), sar::Flavor::kNx), kBankFile,
                         kBankBytes) == "Only a Wii file has wave data beside it.");
}

// A stereo WAV file with a loop from sample 2 to 5 and one sample after it goes in as a wave of the Wii: 16-bit PCM,
// big-endian, with the loop and the rate, in an RWAV file of the given version. As DSP-ADPCM, the loop moves to the
// next frame of 14 samples, and the samples between repeat after the loop's end. The model keeps the samples before the
// encoding.
void TestWiiWave()
{
    seqlib::Pcm pcm;
    pcm.channels = {{100, -200, 300, -400, 500, -600}, {1, 2, 3, 4, 5, 6}};
    pcm.loop = true;
    pcm.loop_start = 2;
    pcm.loop_end = 5;
    const std::vector<uint8_t> wav = seqlib::WavFile(pcm, 22050);

    const IncomingWave pcm16 = hokuto::edit::ReadIncomingWiiWave(wav, 0x0102, sar::WaveEncoding::kPcm16);
    const ntdlib::rsar::WaveFile read = ntdlib::rsar::WaveFile::Read(pcm16.bytes);
    XSFLIB_CHECK(read.version == 0x0102 && read.encoding == ntdlib::rsar::WaveEncoding::kPcm16 && read.loop);
    XSFLIB_CHECK(read.sample_rate == 22050 && read.loop_start == 2 && read.loop_end == 5 && read.channels.size() == 2);
    XSFLIB_CHECK(read.channels[0].samples.size() >= 10 && read.channels[0].samples[0] == 0x00 &&
                 read.channels[0].samples[1] == 0x64 && !read.channels[0].adpcm);
    XSFLIB_CHECK(
        pcm16.version == 0x0102 && pcm16.frames == 5 && pcm16.after_loop == 1 && pcm16.model &&
        (pcm16.model->samples == std::vector<std::vector<int16_t>>{{100, -200, 300, -400, 500}, {1, 2, 3, 4, 5}}));

    const IncomingWave adpcm = hokuto::edit::ReadIncomingWiiWave(wav, 0x0102);
    const ntdlib::rsar::WaveFile encoded = ntdlib::rsar::WaveFile::Read(adpcm.bytes);
    XSFLIB_CHECK(encoded.encoding == ntdlib::rsar::WaveEncoding::kDspAdpcm && encoded.loop_start == 14 &&
                 adpcm.loop_moved == 12 && encoded.channels.size() == 2 && encoded.channels[1].adpcm);
    XSFLIB_CHECK(adpcm.model && adpcm.model->loop_start == 14 && adpcm.model->length == encoded.loop_end);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>(
                     [&] { hokuto::edit::ReadIncomingWiiWave(wav, 0x0102, sar::WaveEncoding::kPcm8); }) ==
                 "Hokuto can encode waves only as DSP-ADPCM or 16-bit PCM.");
}

// A file's bytes with zeros up to a multiple of 32 bytes. NW4R's tools lay a file out that way in an archive of the
// Wii.
std::vector<uint8_t> PaddedForWii(std::vector<uint8_t> bytes)
{
    bytes.resize((bytes.size() + 31) & ~std::size_t{31}, 0);

    return bytes;
}

// A Wii archive as NW4R's tools lay one out: a song, a bank of version 1.2 with an RWAR of two waves, and a wave sound
// file of version 1.3 with an RWAR of one wave. Two groups hold the bank.
std::vector<uint8_t> WiiItemsArchive()
{
    namespace rsar = ntdlib::rsar;
    namespace rsar_files = ntdlib::testing::rsar_files;
    rsar::SequenceFile song;
    song.data = {0xFF};
    song.labels = {{"SEQ_SONG", 0}};
    rsar_files::WaveSoundSpec spec;
    spec.notes.push_back({});
    rsar::ArchiveModel model;
    model.players = {{"PLAYER_TEST", 1, 0}};
    model.sounds.resize(2);
    model.sounds[0].name = "SEQ_SONG";
    model.sounds[1] = {.name = "WSD_ONE", .type = rsar::SoundType::kWaveSound, .file_id = 2};
    model.banks = {{"BANK_TEST", 1}};
    model.groups = {{"GROUP_A", {0, 1, 2}}, {"GROUP_B", {1}}};
    model.files = {{PaddedForWii(song.Write())},
                   {PaddedForWii(rsar_files::MakeNewBank()), PaddedForWii(rsar_files::MakeRwar())},
                   {PaddedForWii(rsar_files::MakeWaveSoundFile(0x0103, {spec})),
                    PaddedForWii(rsar_files::MakeRwarOf({rsar_files::MakeRwav(0x33)}))}};

    return rsar::BuildArchive(model);
}

// The archive that `from` exports, read as a game of the Wii reads it.
std::shared_ptr<const ntdlib::rsar::SoundArchive> ReadWii(const File& from)
{
    return hokuto::song::LoadWiiArchive(hokuto::format::ExportArchive(from), "test.brsar",
                                        ntdlib::rvl::snd::kRsarOptions);
}

// The archive of WiiItemsArchive. The bank's file goes in again, and the archive comes back. The bank keeps its waves
// in an RWAR. A bank of version 1.1 describes its waves in a WAVE block. Such a bank can't take the place of a bank
// with an RWAR. An RWAR of one wave takes the place of the bank's RWAR in both groups, and the bank names a wave past
// its end. A bank of the Switch is refused, and so is a bank read as a wave sound file. An RWAR that ntdlib can't read
// is refused with its reason once.
void TestWiiFiles()
{
    namespace rsar = ntdlib::rsar;
    namespace rsar_files = ntdlib::testing::rsar_files;
    const std::vector<uint8_t> bank = rsar_files::MakeNewBank();
    const std::vector<uint8_t> original = WiiItemsArchive();
    const File file = hokuto::format::ImportWiiArchive(original);

    const std::shared_ptr<const rsar::SoundArchive> before = ReadWii(file);

    // The items' files, and the sounds that play them.
    XSFLIB_CHECK(hokuto::edit::FileIdOf(*before, FileKind::kBank, 0) == 1);
    XSFLIB_CHECK(hokuto::edit::FileIdOf(*before, FileKind::kWaveArchive, 2) == 2);
    XSFLIB_CHECK(hokuto::edit::FileIdOf(*before, FileKind::kWaveSoundFile, 1) == 2);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::FileIdOf(*before, FileKind::kWaveArchive, 0); }) ==
                 "The archive has no such wave archive.");
    XSFLIB_CHECK((hokuto::edit::SoundsPlaying(*before, FileKind::kBank, 0) == std::vector<uint32_t>{0}));
    XSFLIB_CHECK((hokuto::edit::SoundsPlaying(*before, FileKind::kWaveArchive, 1) == std::vector<uint32_t>{0}));
    XSFLIB_CHECK((hokuto::edit::SoundsPlaying(*before, FileKind::kWaveSoundFile, 1) == std::vector<uint32_t>{1}));

    // The same bank, and a bank with a WAVE block.
    const IncomingFile same = hokuto::edit::ReadWiiIncomingFile(bank, FileKind::kBank);
    XSFLIB_CHECK(same.version == 0x0102 && same.bytes == bank);
    XSFLIB_CHECK(hokuto::format::ExportArchive(hokuto::edit::ReplaceWiiFile(file, *before, 0, same)) == original);
    const FileChanges unchanged = hokuto::edit::ChangesOf(file, *before, 0, same);
    XSFLIB_CHECK(unchanged.lacked.empty() && unchanged.programs.empty() && unchanged.sharing.empty() &&
                 unchanged.version == 0x0102 && unchanged.size == bank.size());
    XSFLIB_CHECK(unchanged.wave_block == false);
    const IncomingFile old = hokuto::edit::ReadWiiIncomingFile(rsar_files::MakeOldBank(), FileKind::kBank);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::ReplaceWiiFile(file, *before, 0, old); })
                     .starts_with("The current bank keeps its waves in an RWAR beside it."));

    // An RWAR of one wave beside the bank.
    const std::vector<uint8_t> one = rsar_files::MakeRwarOf({rsar_files::MakeRwav(0x34)});
    const IncomingFile waves = hokuto::edit::ReadWiiIncomingFile(one, FileKind::kWaveArchive);
    const FileChanges changes = hokuto::edit::ChangesOf(file, *before, 1, waves);
    XSFLIB_CHECK(changes.count == 1 && (changes.past_banks == std::vector<uint32_t>{0}) && changes.past_sounds.empty());
    XSFLIB_CHECK(!changes.wave_block);
    const File rewaved = hokuto::edit::ReplaceWiiFile(file, *before, 1, waves);
    const std::shared_ptr<const rsar::SoundArchive> after = ReadWii(rewaved);
    XSFLIB_CHECK(std::ranges::equal(after->WaveData(1), PaddedForWii(one)));
    XSFLIB_CHECK(std::ranges::equal(after->FileData(1), PaddedForWii(bank)));
    XSFLIB_CHECK(std::ranges::equal(after->WaveData(2), before->WaveData(2)));
    const std::vector<std::vector<uint8_t>> copies = WiiCopies(hokuto::format::ExportArchive(rewaved), 1, true);
    SEQLIB_REQUIRE(!copies.empty());
    for (const std::vector<uint8_t>& copy : copies)
    {
        XSFLIB_CHECK(copy == PaddedForWii(one));
    }
    const FileChanges past = hokuto::edit::ChangesOf(rewaved, *after, 0, same);
    SEQLIB_REQUIRE(past.lacked.size() == 1);
    XSFLIB_CHECK((past.lacked[0].waves == std::vector<uint32_t>{1}));

    // Files of another console, and a bank read as a wave sound file.
    const auto read_nx_bank = []
    {
        hokuto::edit::ReadWiiIncomingFile(Bank(sar::Flavor::kNx, {{kFirstWavesId, 0}}, {0}), FileKind::kBank);
    };
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>(read_nx_bank)
                     .starts_with("It's a bank file of the 3DS, the Wii U or the Switch."));
    XSFLIB_CHECK(
        ErrorOf<std::invalid_argument>([&] { hokuto::edit::ReadWiiIncomingFile(bank, FileKind::kWaveSoundFile); }) ==
        "It's a bank file of the Wii, not a wave sound file of the Wii.");

    // An RWAR without its TABL block. The refusal states ntdlib's reason once.
    std::vector<uint8_t> untabled = one;
    const std::size_t table = std::size_t{one.at(0x10)} << 24 | std::size_t{one.at(0x11)} << 16 |
                              std::size_t{one.at(0x12)} << 8 | one.at(0x13);
    untabled.at(table) = 'X';
    constexpr std::string_view kUnreadable = "Can't read the wave archive file. ";
    const std::string refusal =
        ErrorOf<std::invalid_argument>([&] { hokuto::edit::ReadWiiIncomingFile(untabled, FileKind::kWaveArchive); });
    XSFLIB_CHECK(refusal.starts_with(kUnreadable) && refusal.size() > kUnreadable.size() &&
                 refusal.find(kUnreadable, kUnreadable.size()) == std::string::npos);
}

// A wave of the Wii in 16-bit PCM at `rate`, of one channel with `samples`.
ntdlib::rsar::WaveFile WiiPcm16(const std::vector<int16_t>& samples, uint32_t rate)
{
    ntdlib::rsar::WaveFile made;
    made.encoding = ntdlib::rsar::WaveEncoding::kPcm16;
    made.sample_rate = rate;
    made.loop_end = static_cast<uint32_t>(samples.size());
    ntdlib::rsar::WaveFile::Channel& channel = made.channels.emplace_back();
    for (const int16_t sample : samples)
    {
        channel.samples.push_back(static_cast<uint8_t>(static_cast<uint16_t>(sample) >> 8));
        channel.samples.push_back(static_cast<uint8_t>(sample & 0xFF));
    }

    return made;
}

// An archive of the Wii: a song, a bank with two waves, and a wave sound file with one. Two groups hold the bank. With
// `old`, both describe their waves in WAVE blocks: a bank of version 1.1 and a wave sound file of version 1.2.
// Otherwise they keep their waves in RWARs: a bank of 1.2 and a wave sound file of 1.3. ntdlib lays both out as the
// tools do. `bank` and `wave_sounds` give the two files.
std::vector<uint8_t> ModelWiiItemsArchive(bool old, ntdlib::rsar::BankFile::Files& bank,
                                          ntdlib::rsar::WsdFile::Files& wave_sounds)
{
    namespace rsar = ntdlib::rsar;
    rsar::BankFile::Instrument instrument;
    instrument.key_regions = {{0, 127, {{0, 127, rsar::Region{}}}}};
    const std::vector<std::optional<rsar::BankFile::Instrument>> instruments = {instrument};
    bank = rsar::BankFile::FromInstruments(old ? 0x0101 : 0x0102, instruments,
                                           {WiiPcm16({1, 2, 3}, 32000), WiiPcm16({4, 5}, 32000)})
               .Write();
    rsar::WsdFile sounds;
    sounds.version = old ? 0x0102 : 0x0103;
    rsar::WsdFile::WaveSound& sound = sounds.sounds.emplace_back();
    sound.tracks = {{{{0.0f, 0.0f, 0}}}};
    sound.notes = {{.wave_index = 0}};
    sounds.waves = {WiiPcm16({7, 8}, 32000)};
    wave_sounds = sounds.Write();
    rsar::SequenceFile song;
    song.data = {0xFF};
    song.labels = {{"SEQ_SONG", 0}};
    rsar::ArchiveModel model;
    model.sounds = {{.name = "SEQ_SONG", .file_id = 0},
                    {.name = "WSD_ONE", .type = rsar::SoundType::kWaveSound, .file_id = 2}};
    model.banks = {{.name = "BANK_TEST", .file_id = 1}};
    model.players = {{.name = "PLAYER_TEST"}};
    model.groups = {{.name = "GROUP_A", .file_ids = {0, 1, 2}}, {.name = "GROUP_B", .file_ids = {1}}};
    model.files = {{.bytes = song.Write()},
                   {.bytes = bank.file, .wave_data = bank.wave_data},
                   {.bytes = wave_sounds.file, .wave_data = wave_sounds.wave_data}};

    return rsar::BuildArchive(model);
}

// A wave of the wave data of a file of the Wii takes a wave file of the Wii in its place, and a WAV file's wave goes
// after the waves. The archive's bank keeps two waves in an RWAR, and its program 0 plays the first. Its wave sound
// file keeps the one wave that its wave sound plays. The other waves keep their values, and the bank's file and the
// wave sound file's wave data keep their bytes. The new wave takes the present wave's version. A wave file of another
// console goes in converted. A new wave plays through a preview with its samples.
void TestWiiWaves()
{
    namespace rsar = ntdlib::rsar;
    rsar::BankFile::Files bank;
    rsar::WsdFile::Files wave_sounds;
    const File file = hokuto::format::ImportWiiArchive(ModelWiiItemsArchive(false, bank, wave_sounds));

    const std::shared_ptr<const rsar::SoundArchive> before = ReadWii(file);
    const std::vector<rsar::WaveFile> present = hokuto::edit::WavesOf(*before, 1);

    // The present waves, and what plays them.
    const hokuto::edit::WaveChanges changes = hokuto::edit::ChangesOfWiiWave(*before, 1, 0);
    XSFLIB_CHECK(changes.channels == 1 && changes.rate == 32000 && !changes.loop && changes.loop_end == 3 &&
                 changes.encoding == sar::WaveEncoding::kPcm16);
    XSFLIB_CHECK((changes.banks == std::vector<uint32_t>{0}) && changes.sounds.empty());
    XSFLIB_CHECK(hokuto::edit::ChangesOfWiiWave(*before, 1, 1).banks.empty());
    const hokuto::edit::WaveChanges played = hokuto::edit::ChangesOfWiiWave(*before, 2, 0);
    XSFLIB_CHECK(played.banks.empty() && (played.sounds == std::vector<uint32_t>{1}));

    // A wave file of the Wii in the place of the bank's second wave.
    rsar::WaveFile other = WiiPcm16({9, 10, 11, 12}, 22050);
    other.version = 0x0100;
    const std::vector<uint8_t> rwav = other.Write();
    const hokuto::edit::IncomingWave incoming = hokuto::edit::ReadIncomingWiiWaveFile(rwav);
    XSFLIB_CHECK(incoming.bytes == rwav && incoming.version == 0x0100 && incoming.channels == 1 &&
                 incoming.rate == 22050 && incoming.frames == 4 && !incoming.loop);
    const std::shared_ptr<const rsar::SoundArchive> after =
        ReadWii(hokuto::edit::WithIncomingWiiWave(file, *before, 1, 1, incoming));
    const std::vector<rsar::WaveFile> waves = hokuto::edit::WavesOf(*after, 1);
    SEQLIB_REQUIRE(present.size() > 1);
    other.version = present[1].version;
    XSFLIB_CHECK(waves.size() == 2 && waves[0] == present[0] && waves[1] == other);
    XSFLIB_CHECK(std::ranges::equal(after->FileData(1), before->FileData(1)));
    XSFLIB_CHECK(std::ranges::equal(after->WaveData(2), before->WaveData(2)));

    // A WAV file's wave in 16-bit PCM after the waves.
    seqlib::Pcm pcm;
    pcm.channels = {{100, -200, 300}};
    const hokuto::edit::IncomingWave pcm16 =
        hokuto::edit::ReadIncomingWiiWave(seqlib::WavFile(pcm, 16000), 0x0102, sar::WaveEncoding::kPcm16);
    const std::vector<rsar::WaveFile> three =
        hokuto::edit::WavesOf(*ReadWii(hokuto::edit::WithIncomingWiiWave(file, *before, 1, std::nullopt, pcm16)), 1);
    XSFLIB_CHECK(three.size() == 3 && three[0] == present[0] && three[1] == present[1]);
    XSFLIB_CHECK(three[2].sample_rate == 16000 &&
                 (hokuto::edit::DecodedSamples(three[2], 3) == std::vector<std::vector<int16_t>>{{100, -200, 300}}));

    // The Replace and Import windows' Play plays a new wave of the Wii through a preview: the wave's samples at its
    // rate, up to its length. A preview without the length played nothing.
    const ntdlib::wave::Pcm previewed = hokuto::edit::WavePcm(rsar::WaveFile::Read(pcm16.bytes));
    hokuto::song::WavePreview preview(previewed);
    std::vector<float> out(8 * 2, 1.0f);
    const std::size_t rendered = preview.Render(out.data(), 8);
    XSFLIB_CHECK(preview.SampleRate() == 16000 && previewed.Length() == 3 && !previewed.loop && rendered == 3);
    XSFLIB_CHECK(out[0] == 100.0f / 32768.0f && out[1] == out[0] && out[2] == -200.0f / 32768.0f &&
                 out[4] == 300.0f / 32768.0f && out[5] == out[4]);
    const ntdlib::wave::Pcm replacing = hokuto::edit::WavePcm(rsar::WaveFile::Read(rwav));
    XSFLIB_CHECK(replacing.sample_rate == 22050 &&
                 (replacing.channels == std::vector<std::vector<int16_t>>{{9, 10, 11, 12}}));
    const rsar::WaveFile looped = rsar::WaveFile::Read(ntdlib::testing::rsar_files::MakeRwav(0x31));
    const ntdlib::wave::Pcm round = hokuto::edit::WavePcm(looped);
    const uint32_t length = std::min(looped.loop_end, hokuto::edit::HeldSamples(looped));
    XSFLIB_CHECK(length > 0 && round.Length() == length && round.loop == (looped.loop_start < length) &&
                 round.loop_start == looped.loop_start &&
                 round.channels == hokuto::edit::DecodedSamples(looped, length));

    // Refusals.
    XSFLIB_CHECK(
        ErrorOf<std::invalid_argument>([&] { hokuto::edit::WithIncomingWiiWave(file, *before, 1, 5, incoming); }) ==
        "The wave data has no wave 5.");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::ReadIncomingWiiWaveFile(bank.file); }) ==
                 "It's a bank file of the Wii, not a wave file.");

    // A wave file of the Switch goes in converted, with its samples.
    const std::vector<uint8_t> later = Wave(sar::Flavor::kNx, 1);
    const hokuto::edit::IncomingWave converted = hokuto::edit::ReadIncomingWiiWaveFile(later);
    XSFLIB_CHECK(converted.converted && converted.converted->flavor == sar::Flavor::kNx &&
                 converted.converted->size == later.size());
    const rsar::WaveFile wii = rsar::WaveFile::Read(converted.bytes);
    XSFLIB_CHECK(converted.version == wii.version && converted.channels == 1 && converted.frames == 4);
    XSFLIB_CHECK(hokuto::edit::DecodedSamples(wii, 4) == hokuto::edit::DecodedSamples(sar::WaveFile::Read(later), 4));
}

// The Switch's bank of the tests of converted files. It names the second wave of wave archive kFirstWavesId and the
// first of kSecondWavesId, and has instruments at programs 0 and 2.
std::vector<uint8_t> LaterBank()
{
    return Bank(sar::Flavor::kNx, {{kFirstWavesId, 1}, {kSecondWavesId, 0}}, {0, 2});
}

// Files of the 3DS, the Wii U and the Switch go into an archive of the Wii converted. Their magic and byte order mark
// tell them from the Wii's files.
void TestWiiLaterConsoles()
{
    namespace rsar_files = ntdlib::testing::rsar_files;

    // The consoles of the files.
    const std::vector<uint8_t> later_bank = LaterBank();
    XSFLIB_CHECK(hokuto::edit::LaterConsoleOf(later_bank, FileKind::kBank) == sar::Flavor::kNx);
    XSFLIB_CHECK(hokuto::edit::LaterConsoleOf(Bank(sar::Flavor::kCafe, {{kFirstWavesId, 0}}, {0}), FileKind::kBank) ==
                 sar::Flavor::kCafe);
    XSFLIB_CHECK(hokuto::edit::LaterConsoleOf(Bank(sar::Flavor::kCtr, {{kFirstWavesId, 0}}, {0}), FileKind::kBank) ==
                 sar::Flavor::kCtr);
    XSFLIB_CHECK(!hokuto::edit::LaterConsoleOf(later_bank, FileKind::kWaveArchive));
    XSFLIB_CHECK(!hokuto::edit::LaterConsoleOf(rsar_files::MakeNewBank(), FileKind::kBank));
}

// A wave archive of the Switch goes in as an RWAR of its waves.
void TestWiiLaterWaveArchive()
{
    namespace rsar = ntdlib::rsar;

    // A wave archive of the Switch, as an RWAR of its waves.
    const std::vector<uint8_t> later_waves =
        sar_files::WaveArchive(sar::Flavor::kNx, {Wave(sar::Flavor::kNx, 1), Wave(sar::Flavor::kNx, 2)});
    const IncomingFile rwar = hokuto::edit::ReadWiiIncomingFile(later_waves, FileKind::kWaveArchive);
    XSFLIB_CHECK(rwar.kind == FileKind::kWaveArchive && rwar.converted && rwar.converted->flavor == sar::Flavor::kNx &&
                 rwar.converted->size == later_waves.size() && rwar.wave_data.empty());
    const rsar::WaveArchiveFile rwar_file = rsar::WaveArchiveFile::Read(rwar.bytes);
    XSFLIB_CHECK(rwar_file.waves.size() == 2);
    for (std::size_t i = 0; i < rwar_file.waves.size(); i++)
    {
        const std::vector<uint8_t> wave = Wave(sar::Flavor::kNx, static_cast<uint8_t>(i + 1));
        XSFLIB_CHECK(hokuto::edit::DecodedSamples(rsar::WaveFile::Read(rwar_file.waves[i]), 4) ==
                     hokuto::edit::DecodedSamples(sar::WaveFile::Read(wave), 4));
    }
}

// A bank of the Switch names the waves of its wave archives. WiiFileOf gives it the waves of the Wii archive's wave
// data in their place: in an RWAR for a bank of version 1.2, and in a WAVE block for one of 1.1. A wave sound file of
// the Switch goes in the same way. The converted bank takes the place of the archive's bank with its wave data, in both
// groups. A bank of the Wii takes the wave data of a file of the archive.
void TestWiiLaterBank()
{
    namespace rsar = ntdlib::rsar;

    const std::vector<uint8_t> later_bank = LaterBank();
    // The versions of the archive's files.
    rsar::BankFile::Files bank;
    rsar::WsdFile::Files wave_sounds;
    const File file = hokuto::format::ImportWiiArchive(ModelWiiItemsArchive(false, bank, wave_sounds));
    const std::shared_ptr<const rsar::SoundArchive> before = ReadWii(file);
    XSFLIB_CHECK(std::ranges::equal(before->FileData(1), PaddedForWii(bank.file)) &&
                 std::ranges::equal(before->WaveData(2), PaddedForWii(wave_sounds.wave_data)));
    XSFLIB_CHECK(hokuto::edit::NewWiiFileVersion(*before, FileKind::kBank) == 0x0102 &&
                 hokuto::edit::NewWiiFileVersion(*before, FileKind::kWaveSoundFile) == 0x0103 &&
                 hokuto::edit::NewWiiFileVersion(*before, FileKind::kWaveArchive) == 0x0100);
    XSFLIB_CHECK(hokuto::edit::PresentWiiVersion(*before, FileKind::kBank, 0) == 0x0102 &&
                 hokuto::edit::PresentWiiVersion(*before, FileKind::kWaveSoundFile, 1) == 0x0103);

    // The Switch's bank with the second wave of the bank's wave data, and the wave of the wave sound file's.
    const IncomingFile incoming = hokuto::edit::ReadIncomingFile(later_bank, FileKind::kBank, sar::Flavor::kNx);
    const std::map<uint32_t, uint32_t> kMap = {{kFirstWavesId, 1}, {kSecondWavesId, 2}};
    SEQLIB_REQUIRE(hokuto::edit::WavesOf(*before, 1).size() > 1 && !hokuto::edit::WavesOf(*before, 2).empty());
    const std::vector<rsar::WaveFile> expected = {hokuto::edit::WavesOf(*before, 1)[1],
                                                  hokuto::edit::WavesOf(*before, 2)[0]};
    const IncomingFile converted = hokuto::edit::WiiFileOf(incoming, sar::Flavor::kNx, *before, kMap, 0x0102);
    XSFLIB_CHECK(converted.kind == FileKind::kBank && converted.version == 0x0102 && converted.converted &&
                 converted.converted->flavor == sar::Flavor::kNx && converted.converted->size == later_bank.size());
    const rsar::BankFile wii_bank = rsar::BankFile::Read(converted.bytes, converted.wave_data);
    XSFLIB_CHECK(wii_bank.version == 0x0102 && wii_bank.waves == expected && wii_bank.instruments.size() == 3);
    const IncomingFile old = hokuto::edit::WiiFileOf(incoming, sar::Flavor::kNx, *before, kMap, 0x0101);
    XSFLIB_CHECK(old.version == 0x0101 && rsar::BankFile::Read(old.bytes, old.wave_data).waves == expected &&
                 old.wave_data == rsar::RawWaveData(expected));
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>(
                     [&]
                     { hokuto::edit::WiiFileOf(incoming, sar::Flavor::kNx, *before, {{kFirstWavesId, 1}}, 0x0102); }) ==
                 "Choose wave data of the archive for wave archive 1.");

    // A wave sound file of the Switch with the same wave of the bank's wave data.
    const IncomingFile later_sounds = hokuto::edit::ReadIncomingFile(
        WaveSounds(sar::Flavor::kNx, {{kFirstWavesId, 1}}, {0}), FileKind::kWaveSoundFile, sar::Flavor::kNx);
    const IncomingFile wsd =
        hokuto::edit::WiiFileOf(later_sounds, sar::Flavor::kNx, *before, {{kFirstWavesId, 1}}, 0x0103);
    const rsar::WsdFile wii_sounds = rsar::WsdFile::Read(wsd.bytes, wsd.wave_data);
    XSFLIB_CHECK(wsd.kind == FileKind::kWaveSoundFile && wii_sounds.version == 0x0103 &&
                 wii_sounds.sounds.size() == 1 && (wii_sounds.waves == std::vector<rsar::WaveFile>{expected[0]}));

    // The converted bank in the place of the archive's bank, with its wave data.
    const File replaced = hokuto::edit::ReplaceWiiFile(file, *before, 0, converted);
    const std::shared_ptr<const rsar::SoundArchive> after = ReadWii(replaced);
    XSFLIB_CHECK(std::ranges::equal(after->FileData(1), PaddedForWii(converted.bytes)) &&
                 std::ranges::equal(after->WaveData(1), PaddedForWii(converted.wave_data)));
    XSFLIB_CHECK(hokuto::edit::WavesOf(*after, 1) == expected);
    XSFLIB_CHECK(std::ranges::equal(after->FileData(2), before->FileData(2)) &&
                 std::ranges::equal(after->WaveData(2), before->WaveData(2)));
    XSFLIB_CHECK(WiiCopies(hokuto::format::ExportArchive(replaced), 1, false) ==
                 std::vector<std::vector<uint8_t>>(2, PaddedForWii(converted.bytes)));
    XSFLIB_CHECK(WiiCopies(hokuto::format::ExportArchive(replaced), 1, true) ==
                 std::vector<std::vector<uint8_t>>(2, PaddedForWii(converted.wave_data)));

    // The converted bank as a bank of the Wii, with the bank's wave data and the wave sound file's. The bank names two
    // waves, and the wave sound file's wave data holds one.
    const IncomingFile wii = hokuto::edit::ReadWiiIncomingFile(converted.bytes, FileKind::kBank);
    const IncomingFile again = hokuto::edit::WiiFileWithWaves(wii, hokuto::edit::WiiWaveDataOf(*after, 1), 0x0102);
    XSFLIB_CHECK(again.bytes == converted.bytes && again.wave_data == PaddedForWii(converted.wave_data) &&
                 !again.converted);
    XSFLIB_CHECK(hokuto::format::ExportArchive(hokuto::edit::ReplaceWiiFile(replaced, *after, 0, again)) ==
                 hokuto::format::ExportArchive(replaced));
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>(
                     [&] { hokuto::edit::WiiFileWithWaves(wii, hokuto::edit::WiiWaveDataOf(*after, 2), 0x0102); }) ==
                 "The bank refers to wave 1, but its wave data has only 1 wave.");
}

// The archive of ModelWiiItemsArchive whose bank and wave sound file describe their waves in WAVE blocks, the two
// files, and the file that imports the archive and its read.
struct WaveBlockFixture
{
    ntdlib::rsar::BankFile::Files bank;
    ntdlib::rsar::WsdFile::Files sounds;
    std::vector<uint8_t> original;
    File file;
    std::shared_ptr<const ntdlib::rsar::SoundArchive> before;
};

// The fixture of the tests of the Wii's files with WAVE blocks.
WaveBlockFixture MakeWaveBlockFixture()
{
    namespace rsar = ntdlib::rsar;

    // An archive whose bank and wave sound file describe their waves in WAVE blocks.
    rsar::BankFile::Files old_bank;
    rsar::WsdFile::Files old_sounds;
    const std::vector<uint8_t> old_original = ModelWiiItemsArchive(true, old_bank, old_sounds);
    const File old_file = hokuto::format::ImportWiiArchive(old_original);
    const std::shared_ptr<const rsar::SoundArchive> old_before = ReadWii(old_file);

    return {old_bank, old_sounds, old_original, old_file, old_before};
}

// New files of an archive whose files describe their waves in WAVE blocks take those versions: 1.1 for a bank and 1.2
// for a wave sound file. The RWAR that Export… writes for wave data that a WAVE block describes gives the file and its
// wave data back.
void TestWiiWaveBlockRoundTrip()
{
    namespace rsar = ntdlib::rsar;

    const WaveBlockFixture fixture = MakeWaveBlockFixture();
    const rsar::BankFile::Files& old_bank = fixture.bank;
    const rsar::WsdFile::Files& old_sounds = fixture.sounds;
    const std::vector<uint8_t>& old_original = fixture.original;
    const File& old_file = fixture.file;
    const std::shared_ptr<const rsar::SoundArchive>& old_before = fixture.before;

    XSFLIB_CHECK(hokuto::edit::NewWiiFileVersion(*old_before, FileKind::kBank) == 0x0101 &&
                 hokuto::edit::NewWiiFileVersion(*old_before, FileKind::kWaveSoundFile) == 0x0102);

    // The RWARs that Export… writes for the wave data give the files and their wave data back.
    const std::vector<rsar::WaveFile> old_waves = hokuto::edit::WavesOf(*old_before, 1);
    const hokuto::edit::WiiWaveData exported = hokuto::edit::ReadWiiWaveData(
        hokuto::song::ItemBytes(*old_before, {.kind = hokuto::song::ItemKind::kWaveArchive, .index = 1}));
    XSFLIB_CHECK(exported.rwar && old_waves.size() == 2 && exported.waves == old_waves);
    const IncomingFile bank_back = hokuto::edit::WiiFileWithWaves(
        hokuto::edit::ReadWiiIncomingFile(old_bank.file, FileKind::kBank), exported, 0x0101);
    XSFLIB_CHECK(bank_back.bytes == old_bank.file && bank_back.wave_data == old_bank.wave_data && !bank_back.converted);
    XSFLIB_CHECK(hokuto::format::ExportArchive(hokuto::edit::ReplaceWiiFile(old_file, *old_before, 0, bank_back)) ==
                 old_original);
    XSFLIB_CHECK(hokuto::edit::ChangesOf(old_file, *old_before, 0, bank_back).wave_block == true);
    const hokuto::edit::WiiWaveData sounds_exported = hokuto::edit::ReadWiiWaveData(
        hokuto::song::ItemBytes(*old_before, {.kind = hokuto::song::ItemKind::kWaveArchive, .index = 2}));
    const IncomingFile sounds_back = hokuto::edit::WiiFileWithWaves(
        hokuto::edit::ReadWiiIncomingFile(old_sounds.file, FileKind::kWaveSoundFile), sounds_exported, 0x0102);
    XSFLIB_CHECK(sounds_back.bytes == old_sounds.file && sounds_back.wave_data == old_sounds.wave_data);
}

// Into an archive whose bank holds version 1.1, a bank of 1.2 goes in written again.
void TestWiiNewerBankRewritten()
{
    namespace rsar = ntdlib::rsar;

    const WaveBlockFixture fixture = MakeWaveBlockFixture();
    const rsar::BankFile::Files& old_bank = fixture.bank;
    const std::shared_ptr<const rsar::SoundArchive>& old_before = fixture.before;

    // A bank of version 1.2 goes in written again in 1.1, the present bank's version.
    rsar::BankFile model = rsar::BankFile::Read(old_bank.file, old_bank.wave_data);
    model.version = 0x0102;
    const rsar::BankFile::Files newer = model.Write();
    const IncomingFile rewritten = hokuto::edit::WiiFileWithWaves(
        hokuto::edit::ReadWiiIncomingFile(newer.file, FileKind::kBank), hokuto::edit::WiiWaveDataOf(*old_before, 1),
        hokuto::edit::PresentWiiVersion(*old_before, FileKind::kBank, 0));
    XSFLIB_CHECK(rewritten.version == 0x0101 && rewritten.converted && rewritten.converted->wii &&
                 rewritten.converted->version == 0x0102 && rewritten.converted->size == newer.file.size());
    XSFLIB_CHECK(rewritten.bytes == old_bank.file && rewritten.wave_data == old_bank.wave_data);
}

// An RWAR's waves take the place of the waves of a bank with a WAVE block.
void TestWiiWaveBlockRewaved()
{
    namespace rsar = ntdlib::rsar;
    namespace rsar_files = ntdlib::testing::rsar_files;

    const WaveBlockFixture fixture = MakeWaveBlockFixture();
    const File& old_file = fixture.file;
    const std::shared_ptr<const rsar::SoundArchive>& old_before = fixture.before;

    // An RWAR of one wave gives the bank with the WAVE block its waves, in both groups.
    const std::vector<uint8_t> one = rsar_files::MakeRwarOf({rsar_files::MakeRwav(0x34)});
    const File rewaved = hokuto::edit::ReplaceWiiFile(old_file, *old_before, 1,
                                                      hokuto::edit::ReadWiiIncomingFile(one, FileKind::kWaveArchive));
    const std::shared_ptr<const rsar::SoundArchive> old_after = ReadWii(rewaved);
    XSFLIB_CHECK((hokuto::edit::WavesOf(*old_after, 1) ==
                  std::vector<rsar::WaveFile>{rsar::WaveFile::Read(rsar_files::MakeRwav(0x34))}));
    XSFLIB_CHECK(hokuto::edit::PresentWiiVersion(*old_after, FileKind::kBank, 0) == 0x0101);
    const std::span<const uint8_t> new_file = old_after->FileData(1);
    const std::span<const uint8_t> new_data = old_after->WaveData(1);
    XSFLIB_CHECK(WiiCopies(hokuto::format::ExportArchive(rewaved), 1, false) ==
                 std::vector<std::vector<uint8_t>>(2, std::vector<uint8_t>(new_file.begin(), new_file.end())));
    XSFLIB_CHECK(WiiCopies(hokuto::format::ExportArchive(rewaved), 1, true) ==
                 std::vector<std::vector<uint8_t>>(2, std::vector<uint8_t>(new_data.begin(), new_data.end())));
    XSFLIB_CHECK(std::ranges::equal(old_after->WaveData(2), old_before->WaveData(2)));
}

// A bank of version 1.1 whose WAVE block describes other waves than the present wave data holds takes an RWAR of its
// waves.
void TestWiiBankGivenWaves()
{
    namespace rsar = ntdlib::rsar;
    namespace rsar_files = ntdlib::testing::rsar_files;

    const WaveBlockFixture fixture = MakeWaveBlockFixture();
    const rsar::BankFile::Files& old_bank = fixture.bank;
    const File& old_file = fixture.file;
    const std::shared_ptr<const rsar::SoundArchive>& old_before = fixture.before;

    // A bank of version 1.1 alone in the place of the bank keeps the present wave data. Its WAVE block describes longer
    // waves, and Hokuto can't read it with that wave data. An RWAR of its waves then gives it its waves.
    rsar::BankFile::Instrument instrument;
    instrument.key_regions = {{0, 127, {{0, 127, rsar::Region{}}}}};
    std::vector<int16_t> first(64);
    std::vector<int16_t> second(48);
    for (std::size_t i = 0; i < first.size(); i++)
    {
        first[i] = static_cast<int16_t>(i * 100);
        second[i % second.size()] = static_cast<int16_t>(-static_cast<int>(i));
    }
    const std::vector<rsar::WaveFile> longer = {WiiPcm16(first, 32000), WiiPcm16(second, 32000)};
    const std::vector<std::optional<rsar::BankFile::Instrument>> one_instrument = {instrument};
    const rsar::BankFile::Files other = rsar::BankFile::FromInstruments(0x0101, one_instrument, longer).Write();
    const File alone = hokuto::edit::ReplaceWiiFile(old_file, *old_before, 0,
                                                    hokuto::edit::ReadWiiIncomingFile(other.file, FileKind::kBank));
    const std::shared_ptr<const rsar::SoundArchive> alone_after = ReadWii(alone);
    XSFLIB_CHECK(alone_after->FileData(1).size() == other.file.size() &&
                 std::ranges::equal(alone_after->WaveData(1), old_bank.wave_data));
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::WavesOf(*alone_after, 1); })
                     .starts_with("Can't read the bank."));
    std::vector<std::vector<uint8_t>> rwavs;
    for (const rsar::WaveFile& wave : longer)
    {
        rwavs.push_back(wave.Write());
    }
    const File given = hokuto::edit::ReplaceWiiFile(
        alone, *alone_after, 1,
        hokuto::edit::ReadWiiIncomingFile(rsar_files::MakeRwarOf(rwavs), FileKind::kWaveArchive));
    const std::shared_ptr<const rsar::SoundArchive> given_after = ReadWii(given);
    XSFLIB_CHECK(hokuto::edit::WavesOf(*given_after, 1) == longer);
    XSFLIB_CHECK(std::ranges::equal(given_after->FileData(1), other.file) &&
                 std::ranges::equal(given_after->WaveData(1), other.wave_data));
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("wii_files",
                                    []
                                    {
                                        TestWiiReplace();
                                        TestWiiWave();
                                        TestWiiFiles();
                                        TestWiiWaves();
                                        TestWiiLaterConsoles();
                                        TestWiiLaterWaveArchive();
                                        TestWiiLaterBank();
                                        TestWiiWaveBlockRoundTrip();
                                        TestWiiNewerBankRewritten();
                                        TestWiiWaveBlockRewaved();
                                        TestWiiBankGivenWaves();
                                    });
}
