// SPDX-License-Identifier: MIT

// Tests Hokuto's path for Konami's GBA driver: the import and the round trip, the songs that the arrangement shows,
// playback, the exports, the audition and Another Console. Two small cartridges of konami_fixtures.h hold no game data.
// Each contains the short runs of the driver's code that ntdlib's detection reads its tables from, and the tables. The
// first contains the code of the driver's first revision. The second contains the code of a later revision, and the
// opcodes that the revision's command reader compares a command's byte with. That revision has notes with volumes up to
// 15, and calls.
//
// The expected ticks, keys, volumes and bytes come from the songs' bytes in konami_fixtures.h, worked out by hand. A
// wait of n lasts n ticks, and a note sounds until its track's next note or rest. A square wave's note n is MIDI key
// 36 + n.
//
// ntdlib's ArchivePlayer, run directly, gives the reference output. The model's output has to match it sample for
// sample, from the start and after a seek, in both mixing modes. So does the output with a muted track. The MIDI file
// and the SoundFont have to equal those of ntdlib's converter, run directly.

#include "ntdlib/konami/sequencer.h"

#include "konami_fixtures.h"

namespace
{

using hokuto::testing::ClockDrift;
using hokuto::testing::ClockRun;
using hokuto::testing::TakeRead;

std::vector<float> DirectEnhanced(const KonamiRom& rom, int song, std::size_t samples)
{
    konami::ArchivePlayer player(rom.rom, rom.info);
    player.Start(song, {.enhanced = true});
    return RunDirect<float>(player, samples);
}

// A ROM of the first revision comes in as engine 8 of the format, and comes back byte for byte from both forms of the
// file. A ROM without the driver's data doesn't count as such a ROM.
void TestImport()
{
    const std::vector<uint8_t> first = FirstCart();
    const std::vector<uint8_t> later = LaterCart();
    XSFLIB_CHECK(format::IsGbaRom(first));
    XSFLIB_CHECK(format::HoldsKonamiDriver(first));
    XSFLIB_CHECK(format::HoldsKonamiDriver(later));
    XSFLIB_CHECK(!format::HoldsKonamiDriver(EmptyCart(64)));

    for (const std::vector<uint8_t>* cart : {&first, &later})
    {
        const File file = format::ImportKonamiRom(*cart);
        XSFLIB_CHECK(file.engine == format::Engine::kKonami);
        SEQLIB_REQUIRE(file.native && file.native->driver);
        XSFLIB_CHECK(format::ExportArchive(file) == *cart);

        const File binary = format::Read(format::Write(file));
        XSFLIB_CHECK(binary.engine == format::Engine::kKonami);
        XSFLIB_CHECK(format::ExportArchive(binary) == *cart);
        XSFLIB_CHECK(format::Write(binary) == format::Write(file));

        const File text = format::ReadText(format::WriteText(file));
        XSFLIB_CHECK(text.engine == format::Engine::kKonami);
        XSFLIB_CHECK(format::ExportArchive(text) == *cart);
        XSFLIB_CHECK(format::Write(text) == format::Write(file));

        XSFLIB_CHECK(format::KonamiSongSequence(file, kRomBase + kSongTable, 0) == 0u);
        XSFLIB_CHECK(format::KonamiSongSequence(file, kRomBase + kSongTable, 1) == 1u);
        XSFLIB_CHECK(format::KonamiSongSequence(file, kRomBase + kSongTable, 2) == std::nullopt);
        XSFLIB_CHECK(format::KonamiTrackFields(file, kRomBase + kSongTable, 0).size() == 16);
    }

    XSFLIB_CHECK(static_cast<konami::Revision>(format::ImportKonamiRom(first).native->driver->revision) ==
                 konami::Revision::kUltimateMasters);
    XSFLIB_CHECK(static_cast<konami::Revision>(format::ImportKonamiRom(later).native->driver->revision) ==
                 konami::Revision::kWct2004);
}

// The songs and samples that a ROM shows, with their names, and the conversions between the views' values and the
// driver's.
void TestSongsAndSamples()
{
    const std::shared_ptr<const KonamiRom> rom = song::LoadKonamiRom(FirstCart());
    XSFLIB_CHECK(rom->info.song_table == kRomBase + kSongTable);
    XSFLIB_CHECK(rom->info.song_count == 2);
    XSFLIB_CHECK(rom->info.sample_table == kRomBase + kSampleTable);
    XSFLIB_CHECK(song::KonamiSounds(*rom) == (std::vector<uint32_t>{0, 1}));
    XSFLIB_CHECK(song::KonamiSoundName(*rom, 0) == "Song 00");
    XSFLIB_CHECK(song::KonamiSoundName(*rom, 1) == "Song 01");
    XSFLIB_CHECK(song::KonamiRate(*rom) == 65536);
    XSFLIB_CHECK(song::KonamiSamples(*rom) == (std::vector<int>{0, 1}));
    XSFLIB_CHECK(song::KonamiSampleName(1) == "Sample 0001");
    XSFLIB_CHECK(song::KonamiTrackPrograms(*rom, 0) == (std::vector<uint32_t>{0x40}));
    XSFLIB_CHECK(song::KonamiTrackPrograms(*rom, 1).empty());

    // The cartridge has no timer setting of the mixer, and ntdlib assumes 2^24 / 798 Hz. A step of 0x1000 plays a point
    // for each of the mixer's samples, and a step of 0xC2D plays 3,117 / 4,096 of one.
    constexpr double kMixRate = 16777216.0 / 798.0;
    XSFLIB_CHECK(std::abs(song::KonamiSampleRate(*rom, 0) - kMixRate) < 0.01);
    XSFLIB_CHECK(std::abs(song::KonamiSampleRate(*rom, 1) - kMixRate * 3117.0 / 4096.0) < 0.01);

    // The WAV file's samples hold the points at 16 bits. A point's byte is signed: 0x88 is -120.
    const seqlib::WavContents once = seqlib::ReadWav(song::KonamiSampleWav(*rom, 0));
    XSFLIB_CHECK(once.rate == 21024);
    SEQLIB_REQUIRE(once.pcm.channels.size() == 1 && once.pcm.channels[0].size() == 32);
    XSFLIB_CHECK(once.pcm.channels[0][1] == 8 * 256);
    XSFLIB_CHECK(once.pcm.channels[0][17] == -120 * 256);
    XSFLIB_CHECK(!once.pcm.loop);
    const seqlib::WavContents looped = seqlib::ReadWav(song::KonamiSampleWav(*rom, 1));
    SEQLIB_REQUIRE(looped.pcm.channels.size() == 1 && looped.pcm.channels[0].size() == 16);
    XSFLIB_CHECK(looped.pcm.loop);
    XSFLIB_CHECK(looped.pcm.loop_start == 4);
    XSFLIB_CHECK(looped.pcm.loop_end == 16);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { song::KonamiSampleWav(*rom, 7); }) != "");

    using konami::Revision;
    XSFLIB_CHECK(song::KonamiMostVolume(Revision::kUltimateMasters, 4) == 127);
    XSFLIB_CHECK(song::KonamiMostVolume(Revision::kUltimateMasters, 0) == 15);
    XSFLIB_CHECK(song::KonamiMostVolume(Revision::kWct2004, 4) == 15);
    XSFLIB_CHECK(song::KonamiVelocityOf(64, Revision::kUltimateMasters, true) == 64);
    XSFLIB_CHECK(song::KonamiVolumeOf(64, Revision::kUltimateMasters, true) == 64);
    XSFLIB_CHECK(song::KonamiVelocityOf(3, Revision::kUltimateMasters, false) == 25);
    XSFLIB_CHECK(song::KonamiVolumeOf(100, Revision::kUltimateMasters, false) == 12);
    XSFLIB_CHECK(song::KonamiVelocityOf(15, Revision::kWct2004, true) == 127);
    XSFLIB_CHECK(song::KonamiVelocityOf(8, Revision::kWct2004, true) == 68);
    XSFLIB_CHECK(song::KonamiVolumeOf(127, Revision::kWct2004, true) == 15);
    XSFLIB_CHECK(song::KonamiVolumeOf(1, Revision::kWct2004, true) == 1);
    XSFLIB_CHECK(song::KonamiVolumeOf(0, Revision::kWct2004, true) == 0);
}

// The arrangement shows the notes at the ticks of the commands, and the song's loop.
void TestRead()
{
    const std::shared_ptr<const KonamiRom> first = song::LoadKonamiRom(FirstCart());
    const Sequence song0 = song::ReadSequence(*first, 0);
    SEQLIB_REQUIRE(song0.loop);
    XSFLIB_CHECK(song0.loop->start == 12);
    XSFLIB_CHECK(song0.loop->end == 36);
    SEQLIB_REQUIRE(TrackOf(song0, 0) && TrackOf(song0, 4));
    const std::vector<song::Note>& square = TrackOf(song0, 0)->notes;
    SEQLIB_REQUIRE(square.size() == 1);
    XSFLIB_CHECK(square[0].start == 0);
    XSFLIB_CHECK(square[0].length == 12);
    XSFLIB_CHECK(square[0].key == edit::kKonamiLowestPsgKey + 24);
    XSFLIB_CHECK(square[0].velocity == 25);
    XSFLIB_CHECK(square[0].command == kFirstSquare + 3);
    SEQLIB_REQUIRE(TrackOf(song0, 4));
    const std::vector<song::Note>& voice = TrackOf(song0, 4)->notes;
    SEQLIB_REQUIRE(voice.size() == 2);
    XSFLIB_CHECK(voice[0].start == 0);
    XSFLIB_CHECK(voice[0].length == 10);
    XSFLIB_CHECK(voice[0].velocity == 64);
    XSFLIB_CHECK(voice[0].program == 0);
    XSFLIB_CHECK(voice[0].command == kFirstVoice + 1);
    XSFLIB_CHECK(voice[1].start == 12);
    XSFLIB_CHECK(voice[1].length == 24);
    XSFLIB_CHECK(voice[1].velocity == 48);
    XSFLIB_CHECK(voice[1].program == 1);
    XSFLIB_CHECK(voice[1].command == kFirstVoice + 11);

    const Sequence song1 = song::ReadSequence(*first, 1);
    XSFLIB_CHECK(!song1.loop);
    SEQLIB_REQUIRE(TrackOf(song1, 5));
    SEQLIB_REQUIRE(TrackOf(song1, 5)->notes.size() == 1);
    XSFLIB_CHECK(TrackOf(song1, 5)->notes[0].length == 8);
    XSFLIB_CHECK(TrackOf(song1, 5)->notes[0].velocity == 3);

    // The later revision's notes show its volumes on the Key Editor's scale. The call plays its commands' note again.
    const std::shared_ptr<const KonamiRom> later = song::LoadKonamiRom(LaterCart());
    const Sequence called = song::ReadSequence(*later, 0);
    SEQLIB_REQUIRE(called.loop);
    XSFLIB_CHECK(called.loop->start == 12);
    XSFLIB_CHECK(called.loop->end == 32);
    SEQLIB_REQUIRE(TrackOf(called, 4));
    const std::vector<song::Note>& notes = TrackOf(called, 4)->notes;
    SEQLIB_REQUIRE(notes.size() == 3);
    XSFLIB_CHECK(notes[0].start == 0);
    XSFLIB_CHECK(notes[0].velocity == 68);
    XSFLIB_CHECK(notes[0].command == 1);
    XSFLIB_CHECK(notes[1].start == 16);
    XSFLIB_CHECK(notes[1].length == 6);
    XSFLIB_CHECK(notes[1].velocity == 42);
    XSFLIB_CHECK(notes[1].command == 10);
    XSFLIB_CHECK(notes[2].start == 26);
    XSFLIB_CHECK(notes[2].length == 6);
    XSFLIB_CHECK(notes[2].command == 10);
    XSFLIB_CHECK(TrackOf(called, 0) && TrackOf(called, 0)->notes.size() == 1);
}

// A volume command starts a square channel again, at the pitch of the last note command that the track ran. The
// arrangement shows that start at the note command's key. Here that command plays at volume 0, and starts no note. The
// arrangement's note names the volume command and shows its volume. Track 0 of the first cart's song 0 takes these
// bytes:
//
//     +0: 00          wait 0
//     +1: D3 18       note 24 at volume 3, at tick 0
//     +3: 04          wait 4
//     +4: D0 1A       note 26 at volume 0, at tick 4: the channel falls silent
//     +6: 04          wait 4
//     +7: C2          volume 2, at tick 8: the channel starts again, at note 26
//     +8: 04          wait 4
//     +9: E0          rest, at tick 12
//    +10: 00          wait 0
//    +11: F3          the loop's start, at tick 12
//    +12: 00          wait 0
//    +13: E0          rest, at tick 12
//    +14: 18          wait 24
//    +15: FE          end until the song loops, at tick 36
void TestReadsVolumeStart()
{
    constexpr uint32_t kTrackAt = 0x200;
    constexpr uint32_t kTrack = kTrackAt - kSong0;
    const std::vector<uint8_t> track = {0x00, 0xD3, 0x18, 0x04, 0xD0, 0x1A, 0x04, 0xC2,
                                        0x04, 0xE0, 0x00, 0xF3, 0x00, 0xE0, 0x18, 0xFE};
    std::vector<uint8_t> cart = FirstCart();
    std::ranges::copy(track, cart.begin() + kTrackAt);
    Put16(cart, kSongTable + 4, kTrack);
    const std::shared_ptr<const KonamiRom> rom = song::LoadKonamiRom(cart);
    SEQLIB_REQUIRE(rom);

    // ntdlib's model of the driver: in the frame of the volume command, the track starts its channel at note 26.
    const std::unique_ptr<konami::Sequencer> sequencer = konami::Sequencer::Create(rom->rom, rom->info, 0);
    konami::TrackOutput at_volume;
    for (int frame = 0; frame <= 8; frame++)
    {
        at_volume = sequencer->Step()[0];
    }
    XSFLIB_CHECK(at_volume.trig == 1);
    XSFLIB_CHECK(at_volume.vol == 2);
    XSFLIB_CHECK(at_volume.pitch == 26 * 32);

    const Sequence read = song::ReadSequence(*rom, 0);
    SEQLIB_REQUIRE(TrackOf(read, 0));
    const std::vector<song::Note>& notes = TrackOf(read, 0)->notes;
    SEQLIB_REQUIRE(notes.size() == 2);
    XSFLIB_CHECK(notes[0].start == 0);
    XSFLIB_CHECK(notes[0].length == 4);
    XSFLIB_CHECK(notes[0].key == edit::kKonamiLowestPsgKey + 24);
    XSFLIB_CHECK(notes[0].velocity == 25);
    XSFLIB_CHECK(notes[0].command == kTrack + 1);
    XSFLIB_CHECK(notes[1].start == 8);
    XSFLIB_CHECK(notes[1].length == 4);
    XSFLIB_CHECK(notes[1].key == edit::kKonamiLowestPsgKey + 26);
    XSFLIB_CHECK(notes[1].velocity == 17);
    XSFLIB_CHECK(notes[1].command == kTrack + 7);
    XSFLIB_CHECK(notes[1].stretch == notes[0].stretch);
}

// A sound of a Konami cart, loaded for the model.
struct PlaysFixture
{
    std::shared_ptr<const KonamiRom> rom;
    std::shared_ptr<const Sequence> sequence;
};

// The fixture of the tests of sound `sound` of the cart `cart`.
PlaysFixture MakePlaysFixture(const std::vector<uint8_t>& cart, uint32_t sound)
{
    const std::shared_ptr<const KonamiRom> rom = song::LoadKonamiRom(cart);
    const auto sequence = std::make_shared<const Sequence>(song::ReadSequence(*rom, sound));

    return {rom, sequence};
}

// The model plays the reference's samples, from the start, after a seek and with a track muted.
void TestPlaysStrict(const std::vector<uint8_t>& cart, uint32_t sound)
{
    const PlaysFixture fixture = MakePlaysFixture(cart, sound);
    const std::shared_ptr<const KonamiRom>& rom = fixture.rom;
    const std::shared_ptr<const Sequence>& sequence = fixture.sequence;

    SequenceModel strict(rom, sequence, hokuto::song::ModelOptions{.mixing = Mixing::kStrict, .snapshot_interval = 20});
    XSFLIB_CHECK(strict.SampleRate() == 65536);
    XSFLIB_CHECK(strict.FrameSamples() == 1024);
    const std::size_t frame = 2 * strict.FrameSamples();
    const Played played = Play(strict, 120);
    SEQLIB_REQUIRE(played.samples.size() >= frame);
    XSFLIB_CHECK(HasSound(std::span<const int16_t>(played.samples)));
    const std::vector<int16_t> expected = DirectStrict(*rom, static_cast<int>(sound), 2048 * 130);
    SEQLIB_REQUIRE(played.samples.size() <= expected.size());
    XSFLIB_CHECK(std::equal(played.samples.begin(), played.samples.end(), expected.begin()));
    if (played.samples.size() > 31 * frame)
    {
        XSFLIB_CHECK(strict.SeekFrame(30));
        XSFLIB_CHECK(strict.Output().size() == frame);
        XSFLIB_CHECK(std::equal(strict.Output().begin(), strict.Output().end(), expected.begin() + 30 * frame));
    }
}

// In the enhanced mixing, the model plays the reference's samples from the start and after a seek.
void TestPlaysEnhanced(const std::vector<uint8_t>& cart, uint32_t sound)
{
    const PlaysFixture fixture = MakePlaysFixture(cart, sound);
    const std::shared_ptr<const KonamiRom>& rom = fixture.rom;
    const std::shared_ptr<const Sequence>& sequence = fixture.sequence;

    SequenceModel enhanced(rom, sequence,
                           hokuto::song::ModelOptions{.mixing = Mixing::kEnhanced, .snapshot_interval = 20});
    const std::size_t enhanced_frame = 2 * enhanced.FrameSamples();
    const Played enhanced_played = Play(enhanced, 120);
    SEQLIB_REQUIRE(enhanced_played.floats.size() >= enhanced_frame);
    XSFLIB_CHECK(HasSound(std::span<const float>(enhanced_played.floats)));
    const std::vector<float> enhanced_expected = DirectEnhanced(*rom, static_cast<int>(sound), 804 * 130);
    SEQLIB_REQUIRE(enhanced_played.floats.size() <= enhanced_expected.size());
    XSFLIB_CHECK(std::equal(enhanced_played.floats.begin(), enhanced_played.floats.end(), enhanced_expected.begin()));
    if (enhanced_played.floats.size() > 31 * enhanced_frame)
    {
        XSFLIB_CHECK(enhanced.SeekFrame(30));
        const std::span<const float> sought = enhanced.FloatOutput();
        XSFLIB_CHECK(sought.size() == enhanced_frame);
        XSFLIB_CHECK(std::equal(sought.begin(), sought.end(), enhanced_expected.begin() + 30 * enhanced_frame));
    }
}

// A muted track and the meters, in the first sound of the cart `cart`. A meter follows the track that plays alone.
void TestPlaysMuted(const std::vector<uint8_t>& cart)
{
    const std::shared_ptr<const KonamiRom> rom = song::LoadKonamiRom(cart);

    // A muted track leaves the reference's output with that track muted. Its meter stays at 0, and the track that
    // plays moves its meter.
    const auto sequence = std::make_shared<const Sequence>(song::ReadSequence(*rom, 0));
    SequenceModel muted(rom, sequence);
    muted.SetMutes(1u << 4);
    const Played muted_played = Play(muted, 60);
    const std::vector<int16_t> muted_expected = DirectStrict(*rom, 0, 2048 * 70, 4);
    SEQLIB_REQUIRE(!muted_played.samples.empty() && muted_played.samples.size() <= muted_expected.size());
    XSFLIB_CHECK(std::equal(muted_played.samples.begin(), muted_played.samples.end(), muted_expected.begin()));
    const std::vector<int16_t> open_expected = DirectStrict(*rom, 0, 2048 * 70);
    XSFLIB_CHECK(muted_expected != open_expected);

    SequenceModel metered(rom, sequence);
    bool square = false;
    bool voice = false;
    bool quiet = true;
    int played_frames = 0;
    for (bool more = metered.SeekFrame(0); more && played_frames < 8; more = metered.RunFrame(), played_frames++)
    {
        const std::array<int32_t, 2> meter = metered.TrackPeaks(0);
        const std::array<int32_t, 2> other = metered.TrackPeaks(4);
        square = square || meter[0] > 0 || meter[1] > 0;
        voice = voice || other[0] > 0 || other[1] > 0;
        quiet = quiet && metered.TrackPeaks(6) == std::array<int32_t, 2>{};
    }
    XSFLIB_CHECK(square);
    XSFLIB_CHECK(voice);
    XSFLIB_CHECK(quiet);

    SequenceModel muted_metered(rom, sequence);
    muted_metered.SetMutes(1u << 4);
    bool muted_quiet = true;
    int muted_frames = 0;
    for (bool more = muted_metered.SeekFrame(0); more && muted_frames < 8;
         more = muted_metered.RunFrame(), muted_frames++)
    {
        muted_quiet = muted_quiet && muted_metered.TrackPeaks(4) == std::array<int32_t, 2>{};
    }
    XSFLIB_CHECK(muted_frames == played_frames && muted_quiet);
}

// The reader's thread gives the sound and the ROM, and an error for a number past the sounds.
void TestReader()
{
    const std::shared_ptr<const KonamiRom> rom = song::LoadKonamiRom(FirstCart());
    song::SequenceReader reader;
    reader.Request(rom, 0);
    std::optional<song::SequenceReader::Result> read;
    read = TakeRead(reader);
    SEQLIB_REQUIRE(read && read->sequence);
    XSFLIB_CHECK(*read->sequence == song::ReadSequence(*rom, 0));
    XSFLIB_CHECK(read->driver == rom);
    XSFLIB_CHECK(!read->archive && !read->ds && !read->dse);

    reader.Request(rom, 7);
    read.reset();
    read = TakeRead(reader);
    SEQLIB_REQUIRE(read);
    XSFLIB_CHECK(!read->sequence && !read->error.empty());
}

// A ROM that fills the GBA's 32 MiB has no room at its end. A song that grows then asks for the user's choice, and goes
// into a filler run of 0xFF bytes when the file allows it. The ROM keeps its size.
void TestFullRom()
{
    std::vector<uint8_t> cart = FirstCart();
    cart.resize(konami::kLargestRom, 0x22);
    constexpr uint32_t kFiller = 0x10000;
    std::fill_n(cart.begin() + kFiller, 0x8000, uint8_t{0xFF});

    File file = format::ImportKonamiRom(cart);
    XSFLIB_CHECK(format::ExportArchive(file) == cart);
    XSFLIB_CHECK(!format::LacksRoom(file));
    const Sequence song0 = ReadOf(file, 0);
    const song::Track& square = *TrackOf(song0, 0);
    const edit::NewNote added =
        edit::NewKonamiNote(file, 0, square, square.stretches.at(square.notes[0].stretch), 30, 62, 3, 3);
    File grown = edit::EditNotes(file, 0, {}, std::span(&added, 1)).file;
    XSFLIB_CHECK(format::LacksRoom(grown));
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { format::ExportArchive(grown); }) != "");

    grown.native.Edit([](format::NativeArchive& native) { native.gba_filler = true; });
    const std::vector<uint8_t> built = format::ExportArchive(grown);
    XSFLIB_CHECK(built.size() == konami::kLargestRom);
    const std::shared_ptr<const KonamiRom> rom = song::LoadKonamiRom(built);
    XSFLIB_CHECK(StartsAt(song::ReadSequence(*rom, 0), 0, {0, 30}));
    XSFLIB_CHECK(StartsOf(song::ReadSequence(*rom, 1), 5) == (std::vector<uint32_t>{0}));
    uint32_t song_address = 0;
    for (uint32_t i = 0; i < 4; i++)
    {
        song_address |= uint32_t{built[kSongTable + i]} << (8 * i);
    }
    XSFLIB_CHECK(song_address >= kRomBase + kFiller);
    XSFLIB_CHECK(song_address < kRomBase + kFiller + 0x8000);
}

// A sound's MIDI file and SoundFont are those of ntdlib's converter. The MIDI file contains the notes of the song. The
// samples go out as one SoundFont.
void TestExports()
{
    for (const std::vector<uint8_t>& cart : {FirstCart(), LaterCart()})
    {
        const std::shared_ptr<const KonamiRom> rom = song::LoadKonamiRom(cart);
        for (const uint32_t sound : {0u, 1u})
        {
            ntdlib::sequence_export::agb::ConvertSettings settings;
            settings.program = "Hokuto";
            const ntdlib::sequence_export::konami::SongFiles expected =
                ntdlib::sequence_export::konami::ConvertSongFiles(rom->rom, rom->info, static_cast<int>(sound),
                                                                  settings);
            const ntdlib::sequence_export::Conversion conversion = rom->ConvertToMidi(sound, "Hokuto");
            XSFLIB_CHECK(expected.report.ok);
            XSFLIB_CHECK(conversion.midi == expected.midi);
            XSFLIB_CHECK(conversion.sf2 == expected.sf2);
            XSFLIB_CHECK(!conversion.midi.empty() && !conversion.sf2.empty());

            // The SFZ files are the converter's too: one for each MIDI track that plays, with its samples.
            SEQLIB_REQUIRE(conversion.sfz.files.size() == expected.sfz.files.size());
            XSFLIB_CHECK(!conversion.sfz.files.empty() && !conversion.sfz.samples.empty());
            for (std::size_t i = 0; i < conversion.sfz.files.size(); i++)
            {
                XSFLIB_CHECK(conversion.sfz.files[i].name == expected.sfz.files[i].name);
                XSFLIB_CHECK(conversion.sfz.files[i].text == expected.sfz.files[i].text);
            }
            SEQLIB_REQUIRE(conversion.sfz.samples.size() == expected.sfz.samples.size());
            for (std::size_t i = 0; i < conversion.sfz.samples.size(); i++)
            {
                XSFLIB_CHECK(conversion.sfz.samples[i].name == expected.sfz.samples[i].name);
                XSFLIB_CHECK(conversion.sfz.samples[i].wav == expected.sfz.samples[i].wav);
            }
        }
        XSFLIB_CHECK(rom->WritesSfz());

        const seqlib::midi::File midi = seqlib::midi::Read(rom->ConvertToMidi(1, "Hokuto").midi);
        const std::vector<seqlib::midi::Note> notes = seqlib::midi::Notes(midi);
        SEQLIB_REQUIRE(notes.size() == 1);
        XSFLIB_CHECK(notes[0].on == 0);

        const std::vector<int> samples = song::KonamiSamples(*rom);
        const ntdlib::sequence_export::BankConversion bank = rom->ConvertSamples(samples, "Samples");
        XSFLIB_CHECK(!bank.sound_fonts.empty() && !bank.sound_fonts.front().sf2.empty());

        // Each sample's SFZ file plays it on every key. Sample 1 loops from point 4 to its end, 12 points. The
        // SoundFont repeats a loop until it lasts 32 points or more: 36 here. SFZ's loop end is the loop's last point.
        SEQLIB_REQUIRE(samples == std::vector<int>({0, 1}) && bank.sfz.files.size() == 2);
        XSFLIB_CHECK(bank.sfz.files[0].name == "Samples_0.sfz" && bank.sfz.files[1].name == "Samples_1.sfz");
        XSFLIB_CHECK(bank.sfz.files[0].text.find("lokey=0 hikey=127") != std::string::npos);
        XSFLIB_CHECK(bank.sfz.files[0].text.find("loop_mode=no_loop") != std::string::npos);
        XSFLIB_CHECK(bank.sfz.files[1].text.find("loop_mode=loop_continuous loop_start=4 loop_end=39") !=
                     std::string::npos);
        XSFLIB_CHECK(bank.sfz.samples.size() == 2);
    }
}

// A note of a sample sounds on the driver's model, from a copy of the ROM whose only song plays it.
void TestAudition()
{
    for (const std::vector<uint8_t>& cart : {FirstCart(), LaterCart()})
    {
        const std::shared_ptr<const KonamiRom> rom = song::LoadKonamiRom(cart);
        const std::shared_ptr<const KonamiRom> audition =
            song::KonamiAuditionRom(*rom, song::KonamiSampleAudition(*rom, 1, 60, 127));
        const std::vector<int16_t> direct = DirectStrict(*audition, 0, 2048 * 8);
        XSFLIB_CHECK(HasSound(std::span<const int16_t>(direct)));

        // A key of the Key Editor plays on the square wave's duty: two semitones above the song's note, and a sample's
        // note on the track that plays it.
        const File file = format::ImportKonamiRom(cart);
        const Sequence song0 = song::ReadSequence(*rom, 0);
        SEQLIB_REQUIRE(TrackOf(song0, 0) && TrackOf(song0, 0)->notes.size() > 0);
        const std::optional<song::DriverAudition> square = song::KonamiNoteAudition(
            *rom, *file.sequences.at(0), song0, 0, static_cast<int>(TrackOf(song0, 0)->notes[0].program), 62, 100);
        SEQLIB_REQUIRE(square);
        XSFLIB_CHECK(square->track == 0);
        XSFLIB_CHECK(square->note.key == 26);
        XSFLIB_CHECK(
            HasSound(std::span<const int16_t>(DirectStrict(*song::KonamiAuditionRom(*rom, *square), 0, 2048 * 8))));
        SEQLIB_REQUIRE(TrackOf(song0, 4) && TrackOf(song0, 4)->notes.size() > 0);
        const std::optional<song::DriverAudition> voice = song::KonamiNoteAudition(
            *rom, *file.sequences.at(0), song0, 4, static_cast<int>(TrackOf(song0, 4)->notes[0].program),
            TrackOf(song0, 4)->notes[0].key, 100);
        SEQLIB_REQUIRE(voice);
        XSFLIB_CHECK(voice->track == 4);
        XSFLIB_CHECK(voice->note.sample == 0);
    }
}

// Whether `sequence` has notes and `sent` starts each of them within 10 ms of its time, at the note's key in the song's
// MIDI file. `sent` also has to start no other note up to 10 ms before the end of `sequence`. The performer can play a
// conversion on into a second pass of its loop. The notes of that pass start at the end of the first pass, within the
// same 10 ms.
bool PlaysNotesOf(const Sequence& sequence, const Sequence& sent)
{
    const double end = song::MsOfTick(sequence, sequence.length) - 10.0;
    const auto count_before_end = [end](const Sequence& song)
    {
        std::size_t notes = 0;
        for (const song::Track& track : song.tracks)
        {
            notes += static_cast<std::size_t>(std::ranges::count_if(
                track.notes, [&](const song::Note& note) { return song::MsOfTick(song, note.start) < end; }));
        }

        return notes;
    };
    if (count_before_end(sent) != count_before_end(sequence))
    {
        return false;
    }

    std::size_t count = 0;
    for (const song::Track& track : sequence.tracks)
    {
        for (const song::Note& note : track.notes)
        {
            ++count;
            const double ms = song::MsOfTick(sequence, note.start);
            bool found = false;
            for (const song::Track& sent_track : sent.tracks)
            {
                found =
                    found || std::ranges::any_of(sent_track.notes,
                                                 [&](const song::Note& played)
                                                 {
                                                     return played.key == note.midi_key &&
                                                            std::abs(song::MsOfTick(sent, played.start) - ms) <= 10.0;
                                                 });
            }
            if (!found)
            {
                return false;
            }
        }
    }

    return count > 0;
}

// A song goes to the Wii U through its MIDI file and SoundFont, and on to the other consoles. It plays on the Wii U's
// engine through the same conversion. No sound of another console plays on the driver's engine.
void TestOtherConsoles()
{
    const File file = format::ImportKonamiRom(FirstCart());
    const Sequence song0 = ReadOf(file, 0);
    for (const ntdlib::sar::Flavor flavor :
         {ntdlib::sar::Flavor::kCafe, ntdlib::sar::Flavor::kNx, ntdlib::sar::Flavor::kCtr})
    {
        const edit::OtherConsoleSong sent = edit::ForOtherConsole(file, 0, flavor);
        const auto archive =
            song::LoadArchive(format::ExportArchive(sent.file), "test", format::ExportGroupFiles(sent.file));
        const Sequence read = song::ReadSequence(*archive, 0);
        XSFLIB_CHECK(PlaysNotesOf(song0, read));
        XSFLIB_CHECK(read.loop.has_value());
    }

    XSFLIB_CHECK(edit::ForWii(file, 0).file.engine == format::Engine::kWii);
    XSFLIB_CHECK(edit::ForDs(file, 0).file.engine == format::Engine::kDs);

    // The reader gives the song's conversion for the Wii U's engine, with the map of the arrangement's ticks. Each note
    // of the song falls on a note of the conversion with its key in the song's MIDI file, at the tick that the map
    // gives.
    const std::shared_ptr<const KonamiRom> rom = song::LoadKonamiRom(FirstCart());
    song::SequenceReader reader;
    reader.Request(rom, 0,
                   [&file]
                   {
                       const edit::OtherConsoleSong converted = edit::ForEngine(file, 0, format::Engine::kWiiU);
                       return song::SequenceReader::EngineSound{
                           .played = song::LoadArchive(format::ExportArchive(converted.file), "test")};
                   });
    std::optional<song::SequenceReader::Result> read;
    read = TakeRead(reader);
    SEQLIB_REQUIRE(read && read->sequence && read->engine_sequence && read->engine_sequence->arrangement_ticks);
    const Sequence& played = *read->engine_sequence;
    std::size_t checked = 0;
    for (const song::Track& track : read->sequence->tracks)
    {
        for (const song::Note& note : track.notes)
        {
            ++checked;
            const double tick = song::PlayedTickOf(*read->sequence, played, note.start);
            const auto lies_on = [&](const song::Track& other)
            {
                return std::ranges::any_of(other.notes, [&](const song::Note& sent)
                                           { return sent.key == note.midi_key && std::abs(sent.start - tick) <= 1.0; });
            };
            XSFLIB_CHECK(std::ranges::any_of(played.tracks, lies_on));
        }
    }
    XSFLIB_CHECK(checked > 0);

    XSFLIB_CHECK(!edit::EngineRefused(file, format::Engine::kWiiU));
    XSFLIB_CHECK(!edit::EngineRefused(file, format::Engine::kKonami));
    XSFLIB_CHECK(edit::EngineRefused(File{}, format::Engine::kKonami).has_value());
}

// The model's clock follows the arrangement: after 10 s of play, or at the song's end, the arrangement's time at the
// clock is within 2 frames of the time played.
void TestClockFollowsArrangement()
{
    int long_runs = 0;
    for (const std::vector<uint8_t>& cart : {FirstCart(), LaterCart()})
    {
        const std::shared_ptr<const KonamiRom> rom = song::LoadKonamiRom(cart);
        for (const uint32_t sound : rom->Sounds())
        {
            const ClockRun run = ClockDrift(rom, sound, 600);
            XSFLIB_CHECK(std::abs(run.drift) <= 2.0);
            long_runs += run.played >= 500 ? 1 : 0;
        }
    }

    // One sound at least plays on: a clock at 60 frames a second in place of 59.73 drifts 2.7 frames by 600.
    XSFLIB_CHECK(long_runs > 0);
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("konami",
                                    []
                                    {
                                        TestImport();
                                        TestSongsAndSamples();
                                        TestRead();
                                        TestReadsVolumeStart();
                                        for (const std::vector<uint8_t>& cart : {FirstCart(), LaterCart()})
                                        {
                                            for (const uint32_t sound : {0u, 1u})
                                            {
                                                TestPlaysStrict(cart, sound);
                                                TestPlaysEnhanced(cart, sound);
                                            }
                                            TestPlaysMuted(cart);
                                        }
                                        TestReader();
                                        TestClockFollowsArrangement();
                                        TestFullRom();
                                        TestExports();
                                        TestAudition();
                                        TestOtherConsoles();
                                    });
}
