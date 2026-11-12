// SPDX-License-Identifier: MIT

// Tests the songs of the GBA (src/song/gba.cpp and gba_samples.cpp).

#include "cart_bytes.h"
#include "song_runs.h"

namespace
{

// A cartridge whose song 0 plays two tracks of notes on a looped saw wave: three notes on track 0, and one long note on
// track 1.
std::shared_ptr<const hokuto::song::GbaRom> TestGbaRom()
{
    namespace mp2k_cart = ntdlib::testing::mp2k_cart;
    using enum ntdlib::mp2k::Command;

    mp2k_cart::Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, mp2k_cart::Cart::Voice(0, cart.Wave(mp2k_cart::Saw(), 13379, 0))}});
    mp2k_cart::Track melody;
    melody.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Note(24, 60, 100).Wait(24).Note(24, 64, 100).Wait(24);
    melody.Note(48, 67, 100).Wait(48).Fine();
    mp2k_cart::Track bass;
    bass.Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Note(96, 48, 80).Wait(96).Fine();
    cart.Song({melody, bass}, voices);

    const ntdlib::agb::Rom rom = cart.ToRom();
    return hokuto::song::LoadGbaRom(hokuto::testing::RomBytes(rom));
}

// A GBA ROM with one song of two tracks. Voice 0 plays PSG channel 2's square wave, and voice 1 a sample. Track 0
// plays two square notes, at ticks 0 and 24: the first lasts `first_length` ticks. Track 1 plays a note of voice
// `voice` at tick 12, and a sample note at tick 24.
std::shared_ptr<const hokuto::song::GbaRom> GbaChannelsRom(int first_length, int voice)
{
    namespace mp2k_cart = ntdlib::testing::mp2k_cart;
    using enum ntdlib::mp2k::Command;

    mp2k_cart::Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, mp2k_cart::Cart::Voice(2, 2, 60, {0, 0, 15, 0})},
                                             {1, mp2k_cart::Cart::Voice(0, cart.Wave(mp2k_cart::Saw(), 13379, 0))}});
    mp2k_cart::Track first;
    first.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Note(first_length, 60, 100).Wait(24);
    first.Note(24, 62, 100).Wait(24).Fine();
    mp2k_cart::Track second;
    second.Cmd(kCmdVoice, voice).Cmd(kCmdVol, 100).Wait(12).Note(12, 64, 90).Wait(12).Cmd(kCmdVoice, 1);
    second.Note(12, 67, 90).Wait(12).Fine();
    cart.Song({first, second}, voices);
    const ntdlib::agb::Rom bytes = cart.ToRom();
    return hokuto::song::LoadGbaRom(hokuto::testing::RomBytes(bytes));
}

// A GBA song's notes keep their places on the driver's channels. Track 0's second square note takes the channel while
// the driver still holds the first, and cuts it short at tick 24. Track 1's square note at tick 12 meets the channel
// that an earlier track uses at the same priority, and never starts. Its sample note plays on a DirectSound channel.
void TestGbaChannels()
{
    using enum ntdlib::mp2k::Command;

    const std::shared_ptr<const hokuto::song::GbaRom> rom = GbaChannelsRom(48, 0);
    const Sequence sequence = ReadSequence(*rom, 0);

    SEQLIB_REQUIRE_EQ(sequence.tracks.size(), 2);
    const Track& square = sequence.tracks[0];
    SEQLIB_REQUIRE_EQ(square.notes.size(), 2);
    SEQLIB_CHECK(square.notes[0].psg == 2 && square.notes[0].cut && square.notes[0].length == 24);
    SEQLIB_CHECK(square.notes[1].psg == 2 && !square.notes[1].cut);
    SEQLIB_CHECK(square.unstarted.empty());
    const Track& other = sequence.tracks[1];
    SEQLIB_REQUIRE_EQ(other.notes.size(), 1);
    SEQLIB_CHECK(other.notes[0].key == 67 && other.notes[0].psg == 0 && !other.notes[0].cut);
    SEQLIB_REQUIRE_EQ(other.unstarted.size(), 1);
    const Note& unstarted = other.unstarted[0];
    SEQLIB_CHECK(unstarted.start == 12 && unstarted.length == 12 && unstarted.key == 64 && unstarted.velocity == 90);
    SEQLIB_CHECK(unstarted.psg == 2 && !unstarted.cut);

    // Like a note that plays, the unstarted note leads to its command: the note code, then the key and the velocity.
    // The offsets count from the song's first track.
    ntdlib::mp2k::SongHeader header;
    SEQLIB_REQUIRE(ntdlib::mp2k::ReadSong(rom->rom, rom->info, 0, header) && header.tracks.size() == 2);
    const uint32_t base = std::min(header.tracks[0], header.tracks[1]);
    SEQLIB_CHECK(rom->rom.U8(base + unstarted.command) >= kCmdTie);
    SEQLIB_CHECK(rom->rom.U8(base + unstarted.command + 1) == 64 && rom->rom.U8(base + unstarted.command + 2) == 90);
    SEQLIB_CHECK((hokuto::song::CollisionsOf(sequence) == hokuto::song::Collisions{1, 1}));
}

// A GBA song whose header lists more tracks than its music player plays, and that plays nothing that sounds. Its four
// tracks set volume 0 and wait 24 ticks. MusicPlayer2000 starts the player's first three, and the song reads with
// those three tracks and no notes.
void TestGbaSilentSongOfMoreTracks()
{
    namespace mp2k_cart = ntdlib::testing::mp2k_cart;
    using enum ntdlib::mp2k::Command;

    mp2k_cart::Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, mp2k_cart::Cart::Voice(0, cart.Wave(mp2k_cart::Saw(), 13379, 0))}});
    std::vector<mp2k_cart::Track> tracks(4);
    for (mp2k_cart::Track& track : tracks)
    {
        track.Cmd(kCmdVol, 0).Wait(24).Fine();
    }
    cart.Song(tracks, voices, 1);
    const ntdlib::agb::Rom bytes = cart.ToRom();
    const std::shared_ptr<const hokuto::song::GbaRom> rom = hokuto::song::LoadGbaRom(hokuto::testing::RomBytes(bytes));

    const Sequence sequence = ReadSequence(*rom, 0);

    SEQLIB_REQUIRE_EQ(sequence.tracks.size(), 3);
    for (int t = 0; t < 3; t++)
    {
        SEQLIB_CHECK(sequence.tracks[static_cast<std::size_t>(t)].number == t);
        SEQLIB_CHECK(sequence.tracks[static_cast<std::size_t>(t)].notes.empty());
    }
    SEQLIB_CHECK_EQ(sequence.length, 24);
}

// An export of a GBA ROM counts the collisions that the edits add to each song. Without the collisions, track 0's first
// note ends before the second starts, and track 1's first note plays a sample. A song that the list leaves out isn't
// read, and a song that loses collisions adds none.
void TestGbaAddedCollisions()
{
    using hokuto::song::AddedCollisions;
    using hokuto::song::Collisions;

    const std::shared_ptr<const hokuto::song::GbaRom> clear = GbaChannelsRom(24, 1);
    const std::shared_ptr<const hokuto::song::GbaRom> cut = GbaChannelsRom(48, 1);
    const std::shared_ptr<const hokuto::song::GbaRom> both = GbaChannelsRom(48, 0);
    SEQLIB_CHECK((CollisionsOf(ReadSequence(*clear, 0)) == Collisions{0, 0}));

    SEQLIB_CHECK((AddedGbaCollisions(*clear, *both, {0}) == std::vector<AddedCollisions>{{0, {1, 1}}}));
    SEQLIB_CHECK((AddedGbaCollisions(*clear, *cut, {0}) == std::vector<AddedCollisions>{{0, {1, 0}}}));
    SEQLIB_CHECK((AddedGbaCollisions(*cut, *both, {0}) == std::vector<AddedCollisions>{{0, {0, 1}}}));
    SEQLIB_CHECK(AddedGbaCollisions(*both, *clear, {0}).empty());
    SEQLIB_CHECK(AddedGbaCollisions(*clear, *both, {}).empty());
    SEQLIB_CHECK(AddedGbaCollisions(*clear, *both, {7}).empty());
}

// A GBA song exports as the MIDI file and the SoundFont of ntdlib's converter of MusicPlayer2000, with the default
// settings of ntdlib::sequence_export::agb::ConvertSettings. The MIDI file contains the four notes of the song, and
// gets a title. A song that is missing from the table is refused with a message for the user.
void TestGbaExport()
{
    const std::shared_ptr<const hokuto::song::GbaRom> rom = TestGbaRom();
    ntdlib::sequence_export::agb::ConvertSettings settings;
    settings.program = "Hokuto";
    const ntdlib::sequence_export::mp2k::SongFiles theirs =
        ntdlib::sequence_export::mp2k::ConvertSongFiles(rom->rom, rom->info, 0, settings);

    const export_::Conversion conversion = ConvertToMidi(*rom, 0, "Hokuto");

    SEQLIB_REQUIRE(theirs.report.ok && !theirs.midi.empty() && !theirs.sf2.empty());
    SEQLIB_CHECK(conversion.midi == theirs.midi && conversion.sf2 == theirs.sf2);
    SEQLIB_CHECK(conversion.warnings == theirs.report.warnings && conversion.sfz.files.empty());
    std::vector<int> keys;
    for (const seqlib::midi::Note& note : seqlib::midi::Notes(seqlib::midi::Read(conversion.midi)))
    {
        keys.push_back(note.key);
    }
    std::sort(keys.begin(), keys.end());
    SEQLIB_CHECK((keys == std::vector<int>{48, 60, 64, 67}));
    const std::vector<uint8_t> titled = WithMidiTags(conversion.midi, {"Theme", ""});
    SEQLIB_CHECK(seqlib::midi::TrackNames(seqlib::midi::Read(titled))[0] == "Theme");
    SEQLIB_CHECK(seqlib::midi::Problems(titled).empty());

    const std::string refusal = ErrorOf<std::runtime_error>([&] { ConvertToMidi(*rom, 1, "Hokuto"); });
    SEQLIB_CHECK(refusal == "Can't convert Song 01. Its header can't be read.");
}

// A GBA ROM with four songs on two voice groups. Group A contains a saw sample at voice 0, PSG channel 2's square wave
// at voice 1, a drum kit at voice 2 whose only sound is at key 62, and nothing at voice 3. Group B contains the sample
// at voice 1 and at voice 4. Song 0 plays voices 0 and 1 of group A, song 1 voice 1 of group B, and song 2 voices 3 and
// 1 of group A. Song 3 plays voice 4 of group B from a track after its header. The decoder doesn't read such a song.
// Song 4 plays a note of 59 ticks on voice 1 of group A at full volume in the centre. An audition plays the note the
// same way.
struct GbaVoicesRom
{
    std::shared_ptr<const hokuto::song::GbaRom> rom;
    uint32_t group_a = 0;
    uint32_t group_b = 0;
};

GbaVoicesRom MakeGbaVoicesRom()
{
    namespace mp2k_cart = ntdlib::testing::mp2k_cart;
    using enum ntdlib::mp2k::Command;

    mp2k_cart::Cart cart;
    const uint32_t saw = cart.Wave(mp2k_cart::Saw(), 13379, 0);
    const uint32_t drums = cart.VoiceGroup({{62, mp2k_cart::Cart::Voice(0, saw, 62)}});
    GbaVoicesRom made;
    made.group_a = cart.VoiceGroup({{0, mp2k_cart::Cart::Voice(0, saw)},
                                    {1, mp2k_cart::Cart::Voice(2, 2, 60, {0, 0, 15, 0})},
                                    {2, mp2k_cart::Cart::Voice(ntdlib::mp2k::kVoiceDrumKit, drums)}});
    made.group_b = cart.VoiceGroup({{1, mp2k_cart::Cart::Voice(0, saw)}, {4, mp2k_cart::Cart::Voice(0, saw)}});
    mp2k_cart::Track first;
    first.Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Note(24, 60, 100).Wait(24).Cmd(kCmdVoice, 1).Note(24, 62, 100);
    first.Wait(24).Fine();
    cart.Song({first}, made.group_a);
    mp2k_cart::Track second;
    second.Cmd(kCmdVoice, 1).Cmd(kCmdVol, 100).Note(24, 60, 100).Wait(24).Fine();
    cart.Song({second}, made.group_b);
    mp2k_cart::Track third;
    third.Cmd(kCmdVoice, 3).Cmd(kCmdVol, 100).Note(24, 60, 100).Wait(24).Cmd(kCmdVoice, 1).Note(24, 64, 100);
    third.Wait(24).Fine();
    cart.Song({third}, made.group_a);
    const ntdlib::agb::Rom placed = cart.ToRom();
    std::vector<uint8_t> bytes = hokuto::testing::RomBytes(placed);

    // Song 3's header comes before its track.
    bytes.resize((bytes.size() + 3) & ~std::size_t{3});
    const uint32_t header = mp2k_cart::kRomBase + static_cast<uint32_t>(bytes.size());
    mp2k_cart::Track after;
    after.Cmd(kCmdVoice, 4).Cmd(kCmdVol, 100).Note(24, 60, 100).Wait(24).Fine();
    bytes.insert(bytes.end(), {1, 0, 0, 0});
    mp2k_cart::AppendLe32(bytes, made.group_b);
    mp2k_cart::AppendLe32(bytes, header + 12);
    const std::vector<uint8_t> track = after.Bytes(header + 12);
    bytes.insert(bytes.end(), track.begin(), track.end());
    xsflib::StoreLe32(&bytes[mp2k_cart::kSongTable - mp2k_cart::kRomBase + 3 * 8], header);

    // Song 4 holds its note for 59 ticks: a tick in each of the console's frames at the driver's starting tempo, for
    // the 59 whole frames in a second.
    bytes.resize((bytes.size() + 3) & ~std::size_t{3});
    const uint32_t reference = mp2k_cart::kRomBase + static_cast<uint32_t>(bytes.size());
    mp2k_cart::Track note;
    note.Cmd(kCmdVoice, 1).Cmd(kCmdVol, 127).Cmd(kCmdPan, 0x40).Tie(60, 100).Wait(59).Eot(60).Fine();
    bytes.insert(bytes.end(), {1, 0, 0, 0});
    mp2k_cart::AppendLe32(bytes, made.group_a);
    mp2k_cart::AppendLe32(bytes, reference + 12);
    const std::vector<uint8_t> note_bytes = note.Bytes(reference + 12);
    bytes.insert(bytes.end(), note_bytes.begin(), note_bytes.end());
    xsflib::StoreLe32(&bytes[mp2k_cart::kSongTable - mp2k_cart::kRomBase + 4 * 8], reference);
    made.rom = hokuto::song::LoadGbaRom(std::move(bytes));

    return made;
}

// The browser lists each voice group that the songs play, with the programs that their program changes select, in the
// order of the groups' addresses. ntdlib's performer reads the programs of a song that the decoder can't read. The
// browser names a group by its address.
void TestGbaVoiceGroups()
{
    using hokuto::song::GbaVoiceGroup;

    const GbaVoicesRom made = MakeGbaVoicesRom();
    SEQLIB_REQUIRE(made.rom->info.song_count >= 5);
    SEQLIB_REQUIRE((hokuto::song::GbaSongs(*made.rom) == std::vector<int>{0, 1, 2, 3, 4}));
    ntdlib::mp2k::SongHeader header;
    SEQLIB_REQUIRE(ntdlib::mp2k::ReadSong(made.rom->rom, made.rom->info, 3, header));
    const bool refused =
        !ErrorOf<std::invalid_argument>([&] { ntdlib::mp2k::DecodeSong(made.rom->rom, header); }).empty();

    SEQLIB_REQUIRE(refused && made.group_a < made.group_b);
    SEQLIB_CHECK((hokuto::song::GbaVoiceGroups(*made.rom) ==
                  std::vector<GbaVoiceGroup>{{made.group_a, {0, 1, 3}, {0, 2, 4}}, {made.group_b, {1, 4}, {1, 3}}}));
    SEQLIB_CHECK(hokuto::song::GbaVoiceGroupName(0x081A2B3C) == "Voice group 0x081A2B3C");
    SEQLIB_CHECK(ReadSequence(*made.rom, 0).tracks.at(0).bank == hokuto::song::GbaVoiceGroupName(made.group_a));
}

// An export reads again the songs whose voice groups play other voices. The performer reports the programs of song 3
// from the notes that sound. Voice 4 of group B then gets an attack of 0, and its envelope never rises: song 3 selects
// program 4 only before the edit. The program still counts, and both songs of group B play other voices.
void TestGbaSongsWithSilencedVoice()
{
    namespace mp2k_cart = ntdlib::testing::mp2k_cart;

    const GbaVoicesRom made = MakeGbaVoicesRom();
    std::vector<uint8_t> bytes(made.rom->rom.Ptr(mp2k_cart::kRomBase),
                               made.rom->rom.Ptr(mp2k_cart::kRomBase) + made.rom->rom.Size());
    constexpr uint32_t kAttack = 8;
    bytes[made.group_b + 4 * 12 + kAttack - mp2k_cart::kRomBase] = 0;
    const std::shared_ptr<const hokuto::song::GbaRom> after = hokuto::song::LoadGbaRom(std::move(bytes));

    const std::vector<hokuto::song::GbaVoiceGroup> groups = hokuto::song::GbaVoiceGroups(*after);
    SEQLIB_REQUIRE(groups.size() == 2 && groups[1].address == made.group_b);
    SEQLIB_REQUIRE((groups[1].programs == std::vector<int>{1} && groups[1].songs == std::vector<int>{1, 3}));
    SEQLIB_CHECK((hokuto::song::GbaSongsWithOtherVoices(*made.rom, *after) == std::vector<int>{1, 3}));
}

// A voice group exports as ntdlib's converter writes it, with the default settings of
// ntdlib::sequence_export::agb::ConvertSettings: one SoundFont with each voice at its program, under the ROM's title
// and the group's name. A voice that plays nothing gets a warning, and a group whose voices play nothing is refused.
void TestGbaVoiceGroupExport()
{
    const GbaVoicesRom made = MakeGbaVoicesRom();
    ntdlib::sequence_export::agb::ConvertSettings settings;
    settings.program = "Hokuto";
    const std::string title = made.rom->rom.Title();
    const ntdlib::sequence_export::mp2k::VoiceGroupFiles theirs = ntdlib::sequence_export::mp2k::ConvertVoiceGroup(
        made.rom->rom, made.rom->info, made.group_a, {0, 1, 3}, settings,
        (title.empty() ? "" : title + " ") + hokuto::song::GbaVoiceGroupName(made.group_a));

    const export_::BankConversion ours = ConvertVoiceGroup(*made.rom, made.group_a, {0, 1, 3}, "Hokuto");

    SEQLIB_REQUIRE(theirs.ok && ours.sound_fonts.size() == 1);
    SEQLIB_CHECK(ours.sound_fonts[0].sf2 == theirs.sf2 && ours.sfz.files.empty());
    SEQLIB_CHECK(ours.sound_fonts[0].first == 0 && ours.sound_fonts[0].last == 3);
    SEQLIB_CHECK((ours.warnings == std::vector<std::string>{"voice 3 has no samples to play"}));
    const seqlib::sf2::Records records = seqlib::sf2::Read(ours.sound_fonts[0].sf2);
    std::vector<int> presets;
    for (std::size_t i = 0; i + 1 < records.Count("phdr", 38); i++)
    {
        presets.push_back(xsflib::LoadLe16(records.Record("phdr", 38, i) + 20));
    }
    std::sort(presets.begin(), presets.end());
    SEQLIB_CHECK((presets == std::vector<int>{0, 1}));
    std::string refusal;
    try
    {
        ConvertVoiceGroup(*made.rom, made.group_a, {3}, "Hokuto");
    }
    catch (const hokuto::song::BankWithoutInstruments& error)
    {
        refusal = error.what();
    }
    SEQLIB_CHECK(refusal == hokuto::song::GbaVoiceGroupName(made.group_a) + " has no voices that make a sound.");
}

// A voice of a GBA ROM plays a note as song 4 plays it: key 60 at velocity 100 on the voice of the song's voice group,
// at full volume in the centre, held for the 59 whole frames in a second and then released. The output equals the
// output of ntdlib's ArchivePlayer for song 4, in both mixing modes, and the preview ends with the release. A note of
// the Key Editor holds until its flag lets it go. A drum kit plays its nearest key with a sound, and a voice without
// one plays nothing.
void TestGbaNotePreview()
{
    using hokuto::song::GbaNotePreview;

    const GbaVoicesRom made = MakeGbaVoicesRom();
    const auto direct = [&made](bool enhanced)
    {
        ntdlib::mp2k::ArchivePlayer player(made.rom->rom, made.rom->info);
        player.Start(4, ntdlib::mp2k::ArchivePlayerOptions{.enhanced = enhanced});
        std::vector<float> out;
        std::vector<int16_t> samples;
        while (player.Busy())
        {
            if (enhanced)
            {
                player.RunFrame(out);
            }
            else
            {
                player.RunFrame(samples);
            }
        }
        for (const int16_t sample : samples)
        {
            out.push_back(static_cast<float>(sample) / 32768.0f);
        }

        return out;
    };
    GbaNotePreview strict(made.rom, 0, 1);
    GbaNotePreview enhanced(made.rom, 0, 1, Mixing::kEnhanced);
    const auto release = std::make_shared<std::atomic<bool>>(false);
    GbaNotePreview held(made.rom, 0, 1, 64, 90, release);
    GbaNotePreview silent(made.rom, 0, 3);

    const std::vector<float> strict_played = RunPreview(strict, GbaNotePreview::LongestFrames(), 2048);
    const std::vector<float> enhanced_played = RunPreview(enhanced, GbaNotePreview::LongestFrames(), 2048);
    const std::size_t hold = 2 * GbaNotePreview::HoldFrames();
    const std::vector<float> before = RunPreview(held, hold, 1024);
    release->store(true);
    const std::vector<float> after = RunPreview(held, GbaNotePreview::LongestFrames(), 2048);

    const std::vector<float> strict_expected = direct(false);
    const std::vector<float> enhanced_expected = direct(true);
    SEQLIB_CHECK_EQ(GbaNotePreview::HoldFrames(), 59);
    SEQLIB_CHECK_EQ(strict.SampleRate(), hokuto::song::GbaRate(*made.rom));
    SEQLIB_CHECK_EQ(enhanced.SampleRate(), ntdlib::agb::kEnhancedRate);
    SEQLIB_CHECK(HasSound(std::span<const float>(strict_played)) && strict_played == strict_expected);
    SEQLIB_CHECK(HasSound(std::span<const float>(enhanced_played)) && enhanced_played == enhanced_expected);
    SEQLIB_CHECK(before.size() == hold * 1024 * 2 && HasSound(std::span<const float>(before).last(2048)));
    SEQLIB_CHECK(!after.empty() && after.size() < GbaNotePreview::LongestFrames() * 2048);
    SEQLIB_CHECK_EQ(GbaNotePreview::KeyOf(*made.rom, made.group_a, 1), 60);
    SEQLIB_CHECK_EQ(GbaNotePreview::KeyOf(*made.rom, made.group_a, 2), 62);
    SEQLIB_CHECK_EQ(GbaNotePreview::KeyOf(*made.rom, made.group_a, 3), -1);
    SEQLIB_CHECK(RunPreview(silent, 10, 2048).empty());
}

// A later version of Camelot's mixer plays a fixed-pitch sample of a negative size in 4-bit steps. Such a voice sounds,
// its table lists it, and the browser plays it. The preview ends once the echo dies away.
void TestGbaStepVoice()
{
    using hokuto::song::GbaNotePreview;
    namespace mp2k_cart = ntdlib::testing::mp2k_cart;
    using enum ntdlib::mp2k::Command;

    mp2k_cart::Cart cart(mp2k_cart::kLaterCamelotMode, {10, 3}, true, false);
    cart.WriteCamelot();
    cart.WriteLaterCamelot(88, 32, -6);
    std::vector<uint8_t> steps = {0x44};
    steps.resize(2000, 0x00);
    const uint32_t wave = cart.StepWave(steps, 4000);
    const uint32_t voices = cart.VoiceGroup(
        {{5, mp2k_cart::Cart::Voice(ntdlib::mp2k::kVoiceFixed, wave)}, {7, mp2k_cart::Cart::Voice(0, wave)}});
    mp2k_cart::Track track;
    track.Cmd(kCmdVoice, 5).Cmd(kCmdVol, 100).Note(24, 60, 100).Wait(24).Fine();
    cart.Song({track}, voices);
    const ntdlib::agb::Rom placed = cart.ToRom();
    const std::shared_ptr<const hokuto::song::GbaRom> rom = hokuto::song::LoadGbaRom(hokuto::testing::RomBytes(placed));

    ntdlib::mp2k::Voice fixed;
    ntdlib::mp2k::Voice following;
    SEQLIB_REQUIRE(ntdlib::mp2k::ReadVoice(rom->rom, voices + 12 * 5, fixed));
    SEQLIB_REQUIRE(ntdlib::mp2k::ReadVoice(rom->rom, voices + 12 * 7, following));
    SEQLIB_CHECK(hokuto::song::GbaVoicePlaysSteps(*rom, fixed) && hokuto::song::GbaVoiceSounds(*rom, fixed));

    // The driver doesn't play a voice that follows the key in steps.
    SEQLIB_CHECK(!hokuto::song::GbaVoicePlaysSteps(*rom, following) && !hokuto::song::GbaVoiceSounds(*rom, following));
    SEQLIB_CHECK_EQ(GbaNotePreview::KeyOf(*rom, voices, 5), 60);
    SEQLIB_CHECK_EQ(GbaNotePreview::KeyOf(*rom, voices, 7), -1);

    GbaNotePreview preview(rom, 0, 5);
    const std::vector<float> played = RunPreview(preview, GbaNotePreview::LongestFrames(), 2048);
    SEQLIB_CHECK(HasSound(std::span<const float>(played)));
    SEQLIB_CHECK(!played.empty() && played.size() < GbaNotePreview::LongestFrames() * 2048);
}

// The output of ntdlib's ArchivePlayer run directly on song 0 of `rom`, for `frames` of the console's frames, with
// `muted` tracks muted.
template <typename Sample>
std::vector<Sample> DirectGba(const hokuto::song::GbaRom& rom, bool enhanced, int frames, uint32_t muted = 0)
{
    ntdlib::mp2k::ArchivePlayer player(rom.rom, rom.info);
    player.Start(0, ntdlib::mp2k::ArchivePlayerOptions{.enhanced = enhanced});
    for (int track = 0; track < ntdlib::mp2k::kMaxTracks; track++)
    {
        player.MuteTrack(track, (muted >> track & 1) != 0);
    }
    std::vector<Sample> out;
    for (int frame = 0; frame < frames; frame++)
    {
        player.RunFrame(out);
    }

    return out;
}

// The model's output from its start, frame after frame, for `frames` of its frames at most.
std::vector<int16_t> PlayedFrames(SequenceModel& model, int frames)
{
    std::vector<int16_t> played;
    int frame = 0;
    for (bool more = model.SeekFrame(0); more && frame < frames; more = model.RunFrame(), frame++)
    {
        played.insert(played.end(), model.Output().begin(), model.Output().end());
    }

    return played;
}

// A GBA song reads with MP2K's performer, and plays as ntdlib's ArchivePlayer plays it run directly. In strict mixing,
// a model frame contains a 64th of a second of the console's output. The driver of the cartridge sets the output to
// 65,536 Hz, and the console frames contain 1,097.25 samples. The output runs on from frame to frame, from the start
// and after a seek. The song lasts 1.6 s. In enhanced mixing, a model frame is a console frame: 804 samples at about
// 48,022 Hz. A muted track's notes leave the output. The reader returns the same sequence, with the ROM.
void TestGbaPlays()
{
    const std::shared_ptr<const hokuto::song::GbaRom> rom = TestGbaRom();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*rom, 0));

    const auto model = std::make_shared<SequenceModel>(
        rom, sequence, hokuto::song::ModelOptions{.mixing = Mixing::kStrict, .snapshot_interval = 50});
    const std::vector<int16_t> played = PlayedFrames(*model, 400);
    const std::size_t frame = 2 * model->FrameSamples();
    const std::vector<int16_t> expected = DirectGba<int16_t>(*rom, false, 400);
    const bool sought = model->SeekFrame(70);
    const std::vector<int16_t> after_seek(model->Output().begin(), model->Output().end());

    const auto muted = std::make_shared<SequenceModel>(
        rom, sequence, hokuto::song::ModelOptions{.mixing = Mixing::kStrict, .snapshot_interval = 50});
    muted->SetMutes(1);
    const std::vector<int16_t> muted_played = PlayedFrames(*muted, 400);
    const std::vector<int16_t> muted_expected = DirectGba<int16_t>(*rom, false, 400, 1);

    const auto enhanced = std::make_shared<SequenceModel>(
        rom, sequence, hokuto::song::ModelOptions{.mixing = Mixing::kEnhanced, .snapshot_interval = 50});
    std::vector<float> enhanced_played;
    for (bool more = enhanced->SeekFrame(0); more && enhanced_played.size() < 2 * 804 * 300;
         more = enhanced->RunFrame())
    {
        const std::span<const float> output = enhanced->FloatOutput();
        enhanced_played.insert(enhanced_played.end(), output.begin(), output.end());
    }
    const std::vector<float> enhanced_expected = DirectGba<float>(*rom, true, 300);

    SequenceReader reader;
    reader.Request(rom, 0);
    const std::optional<SequenceReader::Result> read = TakeRead(reader);

    SEQLIB_CHECK_EQ(sequence->timebase, 24);
    SEQLIB_REQUIRE(sequence->tracks.size() == 2 && sequence->tracks[0].notes.size() == 3);
    SEQLIB_CHECK(sequence->tracks[0].notes[2].start == 48 && sequence->tracks[0].notes[2].key == 67);
    SEQLIB_CHECK(sequence->tracks[1].notes.size() == 1 && sequence->tracks[1].notes[0].length == 96);
    SEQLIB_CHECK(sequence->name == "Song 00");
    SEQLIB_CHECK_EQ(model->SampleRate(), hokuto::song::GbaRate(*rom));
    SEQLIB_CHECK_EQ(model->FrameSamples(), static_cast<std::size_t>(model->SampleRate() / 64));
    SEQLIB_REQUIRE(played.size() > 90 * frame && played.size() < expected.size());
    SEQLIB_CHECK(HasSound(std::span<const int16_t>(played)));
    SEQLIB_CHECK(std::equal(played.begin(), played.end(), expected.begin()));
    SEQLIB_CHECK(!HasSound(std::span<const int16_t>(expected).subspan(played.size())));
    SEQLIB_CHECK(sought && after_seek.size() == frame &&
                 std::equal(after_seek.begin(), after_seek.end(), expected.begin() + 70 * frame));
    SEQLIB_REQUIRE(muted_played.size() > 90 * frame && muted_played.size() <= muted_expected.size());
    SEQLIB_CHECK(std::equal(muted_played.begin(), muted_played.end(), muted_expected.begin()));
    SEQLIB_CHECK(muted_played != std::vector<int16_t>(played.begin(), played.begin() + muted_played.size()));
    SEQLIB_CHECK(HasSound(std::span<const int16_t>(muted_played)));
    SEQLIB_CHECK_EQ(enhanced->FrameSamples(), static_cast<std::size_t>(ntdlib::agb::kEnhancedFrameSamples));
    SEQLIB_CHECK_EQ(enhanced->SampleRate(), ntdlib::agb::kEnhancedRate);
    SEQLIB_REQUIRE(enhanced_played.size() > 90 * 2 * 804 && enhanced_played.size() <= enhanced_expected.size());
    SEQLIB_CHECK(std::equal(enhanced_played.begin(), enhanced_played.end(), enhanced_expected.begin()));
    SEQLIB_CHECK(read && read->sequence && *read->sequence == *sequence && read->gba == rom && !read->archive);
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("gba_song",
                                    []
                                    {
                                        TestGbaPlays();
                                        TestGbaChannels();
                                        TestGbaSilentSongOfMoreTracks();
                                        TestGbaAddedCollisions();
                                        TestGbaExport();
                                        TestGbaVoiceGroups();
                                        TestGbaSongsWithSilencedVoice();
                                        TestGbaVoiceGroupExport();
                                        TestGbaNotePreview();
                                        TestGbaStepVoice();
                                    });
}
