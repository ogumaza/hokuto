// SPDX-License-Identifier: MIT

// Tests the item views of the GBA (src/app/item_views_gba.cpp): voices, banks, patterns and groups. A voice group of a
// GBA ROM contains a sample voice, PSG voices, a drum kit and a key split. The Bank Editor shows it, and its edits
// change the voices' bytes and a wave's pattern in place (src/edit/gba_voices.h).

#include "item_views_fixtures.h"

#include "cart_bytes.h"
#include "ntdlib/testing/rd2_cart.h"
#include "song/rd2.h"

namespace
{

// The voice at `address` of the ROM that `file` builds.
ntdlib::mp2k::Voice GbaVoiceAt(const File& file, uint32_t address)
{
    const std::shared_ptr<const hokuto::song::GbaRom> rom =
        hokuto::song::LoadGbaRom(hokuto::format::ExportArchive(file));
    ntdlib::mp2k::Voice voice;
    XSFLIB_CHECK(ntdlib::mp2k::ReadVoice(rom->rom, address, voice));
    return voice;
}

// Each kind of GBA voice has its type: the type byte of the voice stores the PSG channel in its low bits, and marks a
// fixed sample, a key split and a drum kit. The type and song::GbaVoiceKind's name tell the same kinds apart.
void TestGbaVoiceTypes()
{
    using hokuto::app::GbaVoiceType;
    const std::vector<std::tuple<uint8_t, GbaVoiceType, std::string>> kKinds = {
        {0x00, GbaVoiceType::kSample, "Sample"},    {0x08, GbaVoiceType::kFixedSample, "Fixed sample"},
        {0x01, GbaVoiceType::kSquare1, "Square 1"}, {0x09, GbaVoiceType::kSquare1, "Square 1"},
        {0x02, GbaVoiceType::kSquare2, "Square 2"}, {0x03, GbaVoiceType::kWave, "Wave"},
        {0x04, GbaVoiceType::kNoise, "Noise"},      {0x05, GbaVoiceType::kUnknown, "Unknown"},
        {0x07, GbaVoiceType::kUnknown, "Unknown"},  {0x40, GbaVoiceType::kKeySplit, "Key split"},
        {0x80, GbaVoiceType::kDrumKit, "Drum kit"}};

    for (const auto& [type, expected, name] : kKinds)
    {
        ntdlib::mp2k::Voice voice;
        voice.type = type;
        XSFLIB_CHECK(hokuto::app::GbaVoiceTypeOf(voice) == expected);
        XSFLIB_CHECK(hokuto::song::GbaVoiceKind(voice) == name);
    }
}

// The Bank Editor shows each program that the song selects, the rows of the drum kit and the key split, the pattern
// with both places that play it, and the sample. The second drum kit's keys read other bytes as voices, and so does
// the third's key 0. They get no rows.
void TestGbaBankView()
{
    const GbaCart cart = MakeGbaCart();
    const std::shared_ptr<const hokuto::song::GbaRom> rom = hokuto::song::LoadGbaRom(cart.bytes);
    const hokuto::app::GbaBankView view = hokuto::app::MakeGbaBankView(*rom, cart.voices);
    SEQLIB_REQUIRE(view.programs.size() == 8);
    std::vector<std::string> kinds;
    for (const hokuto::app::GbaProgram& program : view.programs)
    {
        kinds.push_back(program.voice.kind);
        XSFLIB_CHECK(program.program == kinds.size() - 1 && program.voice.shared.empty());
    }
    XSFLIB_CHECK((kinds == std::vector<std::string>{"Sample", "Square 1", "Wave", "Drum kit", "Key split", "Wave",
                                                    "Drum kit", "Drum kit"}));

    // The drum kit's keys whose voices play something, and the key split's runs of keys.
    const auto rows = [](const hokuto::app::GbaProgram& program)
    {
        std::vector<std::string> shown;
        for (const hokuto::app::GbaVoiceRow& row : program.rows)
        {
            shown.push_back(std::to_string(row.first_key) + "-" + std::to_string(row.last_key) + " " + row.kind);
        }
        return shown;
    };
    XSFLIB_CHECK((rows(view.programs[3]) == std::vector<std::string>{"36-36 Noise", "38-38 Fixed sample"}));
    XSFLIB_CHECK((rows(view.programs[4]) == std::vector<std::string>{"0-59 Square 2", "60-127 Sample"}));
    ntdlib::mp2k::Voice stray;
    XSFLIB_CHECK(ntdlib::mp2k::ReadVoice(rom->rom, cart.key_table + 60, stray) && stray.PsgChannel() == 1);
    XSFLIB_CHECK(view.programs[6].rows.empty());
    XSFLIB_CHECK(ntdlib::mp2k::ReadVoice(rom->rom, cart.pattern, stray) && stray.PsgChannel() == 1);
    XSFLIB_CHECK(view.programs[7].rows.empty() || view.programs[7].rows[0].first_key > 0);

    // The pattern's steps, high nibble first, and the two programs that play it.
    SEQLIB_REQUIRE(view.patterns.contains(cart.pattern));
    const hokuto::edit::GbaPattern& steps = view.patterns.at(cart.pattern);
    XSFLIB_CHECK(steps[0] == 0 && steps[1] == 1 && steps[15] == 15 && steps[16] == 15 && steps[31] == 0);
    const std::string group = hokuto::song::GbaVoiceGroupName(cart.voices);
    XSFLIB_CHECK((view.pattern_places.at(cart.pattern) ==
                  std::vector<std::string>{group + ", program 2", group + ", program 5"}));

    // The one sample that three places play, and the song.
    SEQLIB_REQUIRE(view.samples.size() == 1);
    XSFLIB_CHECK(view.samples[0].address == cart.sample && view.samples[0].rate == 13379.0 && view.samples[0].loops &&
                 view.samples[0].size == 64);
    XSFLIB_CHECK(view.sample_names == std::vector<std::string>{hokuto::app::GbaSampleName(cart.sample)});
    XSFLIB_CHECK(view.songs.size() == 1 && view.song == 0 && view.name == group);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::app::MakeGbaBankView(*rom, cart.sample); }) ==
                 "No song in the ROM uses " + hokuto::song::GbaVoiceGroupName(cart.sample) + ".");
}

// The cart of the tests of voice edits, its file, the address of square 1 and its voice, and the cart loaded as a game.
struct GbaVoiceFixture
{
    GbaCart cart;
    File file;
    uint32_t square;
    ntdlib::mp2k::Voice before;
    std::shared_ptr<const hokuto::song::GbaRom> game;
};

// The fixture of the tests of voice edits.
GbaVoiceFixture MakeGbaVoiceFixture()
{
    namespace mp2k = ntdlib::mp2k;

    const GbaCart cart = MakeGbaCart();
    const File file = hokuto::format::ImportGbaRom(cart.bytes);
    const uint32_t square = cart.voices + 12;
    const mp2k::Voice before = GbaVoiceAt(file, square);
    const std::shared_ptr<const hokuto::song::GbaRom> game = hokuto::song::LoadGbaRom(cart.bytes);

    return {cart, file, square, before, game};
}

// The message of the std::invalid_argument that an edit of `file` to `changed_voice` throws.
std::string EditError(const File& file, const ntdlib::mp2k::Voice& changed_voice)
{
    return ErrorOf<std::invalid_argument>([&] { hokuto::edit::WithGbaVoice(file, changed_voice); });
}

// An edit of a voice changes its 12 bytes in place, and nothing else of the ROM. A value that the edit leaves as the
// ROM stores it isn't checked: square 1's length stays at 100.
void TestGbaVoiceEdit()
{
    namespace mp2k = ntdlib::mp2k;
    using hokuto::edit::WithGbaVoice;

    const GbaVoiceFixture fixture = MakeGbaVoiceFixture();
    const GbaCart& cart = fixture.cart;
    const File& file = fixture.file;
    const uint32_t square = fixture.square;
    const mp2k::Voice& before = fixture.before;
    const std::shared_ptr<const hokuto::song::GbaRom>& game = fixture.game;
    XSFLIB_CHECK(before.sustain == 9 && before.pan_sweep == 0x26 && before.length == 100);

    mp2k::Voice voice = before;
    voice.sustain = 12;
    voice.attack = 7;
    voice.pan_sweep = 0x1A;
    voice.wave = 3;
    const File edited = WithGbaVoice(file, voice);
    const mp2k::Voice after = GbaVoiceAt(edited, square);
    XSFLIB_CHECK(after.sustain == 12 && after.attack == 7 && after.pan_sweep == 0x1A && after.wave == 3 &&
                 after.length == 100 && after.decay == 2 && after.release == 3 && after.type == 1);
    const std::vector<uint8_t> bytes = hokuto::format::ExportArchive(edited);
    SEQLIB_REQUIRE(bytes.size() == cart.bytes.size());
    std::vector<std::size_t> changed;
    for (std::size_t i = 0; i < bytes.size(); ++i)
    {
        if (bytes[i] != cart.bytes[i])
        {
            changed.push_back(i);
        }
    }
    const std::size_t at = square - ntdlib::agb::kRomBase;
    XSFLIB_CHECK((changed == std::vector<std::size_t>{at + 3, at + 4, at + 8, at + 10}));
    XSFLIB_CHECK(hokuto::edit::GbaVoiceBytes(after) == hokuto::edit::GbaVoiceBytes(voice));

    // The song plays the edited voice. An export lists the collisions that its notes add.
    XSFLIB_CHECK(hokuto::song::GbaSongsWithOtherVoices(*game, *hokuto::song::LoadGbaRom(bytes)) == std::vector<int>{0});
    XSFLIB_CHECK(hokuto::song::GbaSongsWithOtherVoices(*game, *game).empty());

    // Key 60 of the key split names the square in place of the sample. Neither voice's bytes change. The cart without
    // the stray drum kits has no other voice that reads the key table.
    const GbaCart plain = MakeGbaCart(false);
    const std::array<uint8_t, 1> square_index{0};
    const File other_key =
        hokuto::edit::WithGbaBytes(hokuto::format::ImportGbaRom(plain.bytes), plain.key_table + 60, square_index);
    XSFLIB_CHECK(hokuto::song::GbaSongsWithOtherVoices(
                     *hokuto::song::LoadGbaRom(plain.bytes),
                     *hokuto::song::LoadGbaRom(hokuto::format::ExportArchive(other_key))) == std::vector<int>{0});
}

// A PSG voice accepts only the values that fit in the PSG registers and the MP2K envelope.
void TestGbaPsgLimits()
{
    namespace mp2k = ntdlib::mp2k;
    using hokuto::edit::WithGbaVoice;

    const GbaVoiceFixture fixture = MakeGbaVoiceFixture();
    const File& file = fixture.file;
    const uint32_t square = fixture.square;
    const mp2k::Voice& before = fixture.before;

    // The PSG's limits, for each value that changes.
    mp2k::Voice wrong = before;
    wrong.sustain = 16;
    XSFLIB_CHECK(EditError(file, wrong) == "The sustain of a PSG voice must be from 0 to 15.");
    wrong = before;
    wrong.release = 8;
    XSFLIB_CHECK(EditError(file, wrong) == "The release of a PSG voice must be from 0 to 7.");
    wrong = before;
    wrong.length = 101;
    XSFLIB_CHECK(EditError(file, wrong) == "The length of a PSG voice must be from 0 to 63.");
    wrong = before;
    wrong.wave = 4;
    XSFLIB_CHECK(EditError(file, wrong) == "The duty cycle of a PSG voice must be from 0 to 3.");
    wrong = before;
    wrong.type = 2;
    XSFLIB_CHECK(GbaVoiceAt(WithGbaVoice(file, wrong), square).type == 2);
}

// A voice can take another kind, and a drum kit and a key split keep their tables.
void TestGbaDrumKitVoice()
{
    namespace mp2k = ntdlib::mp2k;
    using hokuto::edit::WithGbaVoice;

    const GbaVoiceFixture fixture = MakeGbaVoiceFixture();
    const GbaCart& cart = fixture.cart;
    const File& file = fixture.file;
    const std::shared_ptr<const hokuto::song::GbaRom>& game = fixture.game;

    // The noise of the drum kit gets a mode and a pan. Its table stays, and so does the table of the key split.
    const mp2k::Voice kit = GbaVoiceAt(file, cart.voices + 3 * 12);
    const uint32_t noise_place = kit.wave + 36 * 12;
    mp2k::Voice noise = GbaVoiceAt(file, noise_place);
    XSFLIB_CHECK(noise.PsgChannel() == 4 && noise.pan_sweep == 0xA0);
    noise.wave = 1;
    noise.pan_sweep = 0xE0;
    noise.key = 40;
    const File drummed = WithGbaVoice(file, noise);
    const mp2k::Voice panned = GbaVoiceAt(drummed, noise_place);
    XSFLIB_CHECK(panned.wave == 1 && panned.pan_sweep == 0xE0 && panned.key == 40);
    XSFLIB_CHECK(hokuto::song::GbaSongsWithOtherVoices(
                     *game, *hokuto::song::LoadGbaRom(hokuto::format::ExportArchive(drummed))) == std::vector<int>{0});
    noise.wave = 2;
    XSFLIB_CHECK(EditError(file, noise) == "The noise mode of a PSG voice must be from 0 to 1.");
    mp2k::Voice moved_kit = kit;
    moved_kit.wave = cart.voices;
    XSFLIB_CHECK(EditError(file, moved_kit) == "Can't change the tables of a key split or a drum kit.");
    mp2k::Voice split = GbaVoiceAt(file, cart.voices + 4 * 12);
    split.key_table += 1;
    XSFLIB_CHECK(EditError(file, split) == "Can't change the tables of a key split or a drum kit.");
    mp2k::Voice unsplit = kit;
    unsplit.type = 0;
    XSFLIB_CHECK(EditError(file, unsplit) == "Can't change the tables of a key split or a drum kit.");
}

// A sample's voice changes to a fixed pitch.
void TestGbaSampleVoice()
{
    namespace mp2k = ntdlib::mp2k;
    using hokuto::edit::WithGbaVoice;

    const GbaVoiceFixture fixture = MakeGbaVoiceFixture();
    const GbaCart& cart = fixture.cart;
    const File& file = fixture.file;

    // A sample voice gets another sample, a fixed pitch and any envelope.
    mp2k::Voice sample = GbaVoiceAt(file, cart.voices);
    sample.type = mp2k::kVoiceFixed;
    sample.attack = 12;
    sample.sustain = 0;
    const mp2k::Voice fixed = GbaVoiceAt(WithGbaVoice(file, sample), cart.voices);
    XSFLIB_CHECK(fixed.type == mp2k::kVoiceFixed && fixed.attack == 12 && fixed.sustain == 0 &&
                 fixed.wave == cart.sample && fixed.decay == 250 && fixed.release == 165);
}

// Bytes that fall within a song, or that hold an address of the songs, take no edit.
void TestGbaSongBytes()
{
    const GbaVoiceFixture fixture = MakeGbaVoiceFixture();
    const GbaCart& cart = fixture.cart;
    const File& file = fixture.file;

    // The header of the song contains the address of its track, and the track is within the song.
    std::vector<uint8_t> header(12);
    std::copy_n(cart.bytes.begin() + (cart.header - ntdlib::agb::kRomBase), 12, header.begin());
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::WithGbaBytes(file, cart.header, header); }) ==
                 "These bytes contain the address of a song or a track. Can't change them here.");
    const uint32_t track = xsflib::LoadLe32(header.data() + 8);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::WithGbaBytes(file, track, header); }) ==
                 "These bytes are part of a song, or overlap one. Can't change them here.");
}

// A pattern gets new steps in its 16 bytes, high nibble first, and both voices play them. A step ranges from 0 to 15.
void TestGbaPatternEdit()
{
    const GbaCart cart = MakeGbaCart();
    const File file = hokuto::format::ImportGbaRom(cart.bytes);
    hokuto::edit::GbaPattern steps{};
    for (std::size_t i = 0; i < steps.size(); ++i)
    {
        steps[i] = static_cast<uint8_t>(i < 16 ? 15 - i : i - 16);
    }
    const File edited = hokuto::edit::WithGbaPattern(file, cart.pattern, steps);
    const std::shared_ptr<const hokuto::song::GbaRom> rom =
        hokuto::song::LoadGbaRom(hokuto::format::ExportArchive(edited));
    XSFLIB_CHECK(hokuto::edit::GbaPatternOf(rom->rom, cart.pattern) == steps);
    XSFLIB_CHECK(rom->rom.U8(cart.pattern) == 0xFE && rom->rom.U8(cart.pattern + 15) == 0xEF);
    XSFLIB_CHECK(hokuto::app::MakeGbaBankView(*rom, cart.voices).patterns.at(cart.pattern) == steps);
    XSFLIB_CHECK(hokuto::song::GbaSongsWithOtherVoices(*hokuto::song::LoadGbaRom(cart.bytes), *rom) ==
                 std::vector<int>{0});

    steps[7] = 16;
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::WithGbaPattern(file, cart.pattern, steps); }) ==
                 "Wave pattern steps must be from 0 to 15.");
    XSFLIB_CHECK(
        ErrorOf<std::invalid_argument>([&] { hokuto::edit::GbaPatternOf(rom->rom, ntdlib::agb::kRomBase - 16); }) ==
        "The wave pattern is outside the ROM.");
}

// The voice groups at `addresses`, as the songs of a ROM play them.
std::vector<hokuto::song::GbaVoiceGroup> GbaGroupsAt(const std::vector<uint32_t>& addresses)
{
    std::vector<hokuto::song::GbaVoiceGroup> groups;
    for (const uint32_t address : addresses)
    {
        groups.push_back({.address = address});
    }

    return groups;
}

// The Bank Editor follows a voice group whose table moves from A to B and then from B to C, back through two Undos and
// forward through two Redos. B keeps both of its moves. The table of another group that a later move put at A, the old
// place of the first group, returns the editor to the place of that group after an Undo of that move. An address that
// no move names stays.
void TestGbaGroupMoves()
{
    constexpr uint32_t kA = 0x08100000;
    constexpr uint32_t kB = 0x08200000;
    constexpr uint32_t kC = 0x08300000;
    constexpr uint32_t kX = 0x08400000;
    hokuto::app::GbaGroupMoves moves;
    hokuto::app::RecordGbaGroupMove(moves, kA, kB);
    hokuto::app::RecordGbaGroupMove(moves, kB, kC);
    hokuto::app::GbaGroupMoves into_place;
    hokuto::app::RecordGbaGroupMove(into_place, kA, kB);
    hokuto::app::RecordGbaGroupMove(into_place, kX, kA);

    XSFLIB_CHECK(hokuto::app::FollowedGbaGroup(moves, kC, GbaGroupsAt({kC})) == kC);
    XSFLIB_CHECK(hokuto::app::FollowedGbaGroup(moves, kC, GbaGroupsAt({kB})) == kB);
    XSFLIB_CHECK(hokuto::app::FollowedGbaGroup(moves, kB, GbaGroupsAt({kA})) == kA);
    XSFLIB_CHECK(hokuto::app::FollowedGbaGroup(moves, kA, GbaGroupsAt({kB})) == kB);
    XSFLIB_CHECK(hokuto::app::FollowedGbaGroup(moves, kB, GbaGroupsAt({kC})) == kC);
    XSFLIB_CHECK(hokuto::app::FollowedGbaGroup(into_place, kA, GbaGroupsAt({kB, kX})) == kX);
    XSFLIB_CHECK(hokuto::app::FollowedGbaGroup(moves, kX, GbaGroupsAt({kC})) == kX);
}

// The Bank Editor's view of a ROM with another of the GBA's drivers shows its samples before a thread has read their
// songs. Until then each sample's songs are being read, not missing. The read then fills in the songs of each sample. A
// read that failed fills in none.
void TestDriverSampleSongs()
{
    namespace cart = ntdlib::testing::rd2_cart;
    cart::Cart image;
    image.Sequences({{cart::TrackData().Bytes({0xC2, cart::kLooped}).Note(60, 24, 100).Wait(24).End()}});
    const std::shared_ptr<const hokuto::song::Rd2Rom> rom =
        hokuto::song::LoadRd2Rom(hokuto::testing::RomBytes(image.ToRom()));

    hokuto::app::DriverBankView view = hokuto::app::MakeDriverBankView(*rom, {});
    SEQLIB_REQUIRE(!view.samples.empty() && view.samples[0].sample == 0);
    for (const hokuto::app::DriverSampleView& sample : view.samples)
    {
        XSFLIB_CHECK(sample.songs_state == hokuto::app::DriverSongsState::kReading && sample.songs.empty());
    }

    const hokuto::app::DriverSampleSongs songs = hokuto::app::SongsOfDriverSamples(*rom);
    XSFLIB_CHECK(songs.state == hokuto::app::DriverSongsState::kRead);
    hokuto::app::ShowDriverSampleSongs(view, songs);
    for (const hokuto::app::DriverSampleView& sample : view.samples)
    {
        XSFLIB_CHECK(sample.songs_state == hokuto::app::DriverSongsState::kRead);
        XSFLIB_CHECK(sample.songs ==
                     (sample.sample == 0 ? std::vector<std::string>{rom->SoundName(0)} : std::vector<std::string>()));
    }

    hokuto::app::ShowDriverSampleSongs(view, {.state = hokuto::app::DriverSongsState::kUnreadable});
    XSFLIB_CHECK(view.samples[0].songs_state == hokuto::app::DriverSongsState::kUnreadable);
    XSFLIB_CHECK(view.samples[0].songs.empty());
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("gba_item_views",
                                    []
                                    {
                                        TestGbaVoiceTypes();
                                        TestGbaBankView();
                                        TestGbaVoiceEdit();
                                        TestGbaPsgLimits();
                                        TestGbaDrumKitVoice();
                                        TestGbaSampleVoice();
                                        TestGbaSongBytes();
                                        TestGbaPatternEdit();
                                        TestGbaGroupMoves();
                                        TestDriverSampleSongs();
                                    });
}
