// SPDX-License-Identifier: MIT

// Tests the sample edits of Konami's driver (src/edit/konami_samples.cpp).

#include "konami_fixtures.h"

namespace
{

// The little-endian 32-bit value at `at` of `bytes`.
uint32_t Word32(std::span<const uint8_t> bytes, uint32_t at)
{
    return xsflib::LoadLe32(&bytes[at]);
}

// The Bank Editor's edits of a sample's rate and loop write the step and the loop start of its header, and nothing
// else. The cartridges' mixer runs at 2^24 / 798 Hz: a rate of half of it takes a step of 0x800. A sample that the
// table names twice changes in both places.
void TestSampleValues()
{
    const std::vector<uint8_t> cart = FirstCart();
    const File file = format::ImportKonamiRom(cart);
    const std::shared_ptr<const KonamiRom> rom = song::LoadKonamiRom(cart);
    const edit::KonamiRates rates = edit::KonamiSampleRates(*rom);
    XSFLIB_CHECK(rates.least == 329);
    XSFLIB_CHECK(rates.most == 336380);
    XSFLIB_CHECK(edit::KonamiSampleLoops(*rom, 0));
    XSFLIB_CHECK(edit::KonamiNoLoopReason(*rom, 0).empty());

    const File edited = edit::WithKonamiSampleValues(file, *rom, 1, {.rate = 10512, .loop_start = 8});
    std::vector<uint8_t> expected = cart;
    Put32(expected, kHeader1, 0x800);
    Put32(expected, kHeader1 + 8, 8);
    const std::vector<uint8_t> built = format::ExportArchive(edited);
    XSFLIB_CHECK(built == expected);
    const std::shared_ptr<const KonamiRom> edited_rom = song::LoadKonamiRom(built);
    XSFLIB_CHECK(std::abs(song::KonamiSampleRate(*edited_rom, 1) - 16777216.0 / 798.0 / 2.0) < 0.01);
    XSFLIB_CHECK(edited_rom->ShapeOf(1).loop_start == 8u);
    XSFLIB_CHECK(edited_rom->ShapeOf(1).length == 16u);

    // No loop: the loop start is -1.
    const File once = edit::WithKonamiSampleValues(file, *rom, 1, {.rate = 10512});
    Put32(expected, kHeader1 + 8, 0xFFFFFFFF);
    XSFLIB_CHECK(format::ExportArchive(once) == expected);
    XSFLIB_CHECK(!song::LoadKonamiRom(format::ExportArchive(once))->ShapeOf(1).loop_start);

    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::WithKonamiSampleValues(file, *rom, 1, {.rate = 328}); }) ==
                 "The rate must be from 329 to 336380 Hz.");
    XSFLIB_CHECK(
        ErrorOf<std::invalid_argument>([&] { edit::WithKonamiSampleValues(file, *rom, 1, {.rate = 336384}); }) != "");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>(
                     [&] { edit::WithKonamiSampleValues(file, *rom, 1, {.rate = 10512, .loop_start = 16}); })
                     .starts_with("The loop must start before the end of the sample"));
    XSFLIB_CHECK(
        ErrorOf<std::invalid_argument>([&] { edit::WithKonamiSampleValues(file, *rom, 7, {.rate = 10512}); }) ==
        "The ROM has no Sample 0007.");

    // No sample of the Dungeon Dice Monsters revision loops, and the Bank Editor gives the reason.
    KonamiRom dungeon = *rom;
    dungeon.info.revision = konami::Revision::kDungeonDiceMonsters;
    XSFLIB_CHECK(!edit::KonamiSampleLoops(dungeon, 0));
    XSFLIB_CHECK(edit::KonamiNoLoopReason(dungeon, 0) == "Samples of this driver can't loop.");

    // The second entry names the first header too. Both samples take the edit.
    std::vector<uint8_t> shared = cart;
    Put32(shared, kSampleTable + 4, kRomBase + kHeader0);
    const std::shared_ptr<const KonamiRom> shared_rom = song::LoadKonamiRom(shared);
    XSFLIB_CHECK(edit::KonamiSamplesSharing(*shared_rom, 0, std::vector<int>{0, 1}) == (std::vector<int>{1}));
    XSFLIB_CHECK(edit::KonamiSamplesSharing(*rom, 0, std::vector<int>{0, 1}).empty());
    const File shared_edit =
        edit::WithKonamiSampleValues(format::ImportKonamiRom(shared), *shared_rom, 1, {.rate = 10512});
    const std::shared_ptr<const KonamiRom> both = song::LoadKonamiRom(format::ExportArchive(shared_edit));
    XSFLIB_CHECK(std::abs(song::KonamiSampleRate(*both, 0) - song::KonamiSampleRate(*both, 1)) < 0.001);
    XSFLIB_CHECK(Word32(format::ExportArchive(shared_edit), kHeader0) == 0x800);
}

// A bend of a sample track in the later revision names its sample by the track's duty byte, and a wave command changes
// that byte. Song 1 of the cartridge below plays a note of sample 1 on track 4, sets wave 0, then bends. The driver's
// output stage then sets the voice's step from sample 0's header (song::KonamiBorrowedRates). The voice plays on round
// sample 1's loop. ntdlib's ArchivePlayer, run directly, gives the reference: a new step of sample 0 changes the song's
// output. With a byte that does nothing in place of the wave command, the bend names sample 1, and the new step of
// sample 0 changes nothing.
//
//     +0: 00          wait 0
//     +1: A3 01       note of sample 1 at volume 3, at tick 0
//     +3: 02          wait 2
//     +4: 84          wave 0: the track's duty byte holds 0, at tick 2
//     +5: 00          wait 0
//     +6: F2 41       bend of 1 up, at tick 2: the cartridge's pitch table holds 2 steps round its centre
//     +8: 06          wait 6
//     +9: E0          rest, at tick 8
//    +10: 00          wait 0
//    +11: FF          stop the song, at tick 8
//
// Every other track starts at +12: 00 FD. The song follows the cartridge's tables.
void TestBorrowedRates()
{
    const std::vector<uint8_t> song = {0x00, 0xA3, 0x01, 0x02, 0x84, 0x00, 0xF2,
                                       0x41, 0x06, 0xE0, 0x00, 0xFF, 0x00, 0xFD};
    const auto cart_of = [&song](uint8_t command, uint32_t step)
    {
        std::vector<uint8_t> bytes = LaterCart();
        bytes.resize(kCartSize + song.size());
        std::ranges::copy(song, bytes.begin() + kCartSize);
        bytes[kCartSize + 4] = command;
        PutEntry(bytes, 1, kCartSize, 12, 4, 0);
        Put32(bytes, kHeader0, step);
        return song::LoadKonamiRom(bytes);
    };
    const std::shared_ptr<const KonamiRom> waved = cart_of(0x84, 0x1000);
    const std::shared_ptr<const KonamiRom> waved_step = cart_of(0x84, 0x800);
    const std::shared_ptr<const KonamiRom> plain = cart_of(0xFC, 0x1000);
    const std::shared_ptr<const KonamiRom> plain_step = cart_of(0xFC, 0x800);

    const std::vector<int> borrowed = song::KonamiBorrowedRates(*waved, 1);
    const std::vector<int> none = song::KonamiBorrowedRates(*plain, 1);

    XSFLIB_CHECK(borrowed == (std::vector<int>{0}));
    XSFLIB_CHECK(none.empty());
    XSFLIB_CHECK(song::KonamiBorrowedRates(*waved, 0).empty());
    XSFLIB_CHECK(waved->SamplesOfSound(1) == (std::vector<int>{1}));
    XSFLIB_CHECK(DirectStrict(*waved, 1, 4096) != DirectStrict(*waved_step, 1, 4096));
    XSFLIB_CHECK(DirectStrict(*plain, 1, 4096) == DirectStrict(*plain_step, 1, 4096));
}

// The first cart and a copy with a WAV file in place of sample 0, as a file, as bytes and as a loaded ROM.
struct SampleFixture
{
    std::vector<uint8_t> cart;
    File file;
    std::shared_ptr<const KonamiRom> rom;
    seqlib::Pcm pcm;
    edit::IncomingKonamiSample incoming;
    File replaced;
    std::vector<uint8_t> built;
    std::shared_ptr<const KonamiRom> replaced_rom;
};

// The fixture of the tests of a replaced sample.
SampleFixture MakeSampleFixture()
{
    const std::vector<uint8_t> cart = FirstCart();
    const File file = format::ImportKonamiRom(cart);
    const std::shared_ptr<const KonamiRom> rom = song::LoadKonamiRom(cart);

    seqlib::Pcm pcm;
    pcm.channels.emplace_back(40);
    for (std::size_t i = 0; i < 40; i++)
    {
        pcm.channels[0][i] = static_cast<int16_t>((static_cast<int>(i) - 20) * 5 * 256);
    }
    pcm.loop = true;
    pcm.loop_start = 10;
    pcm.loop_end = 40;
    const edit::IncomingKonamiSample incoming = edit::ReadIncomingKonamiSample(seqlib::WavFile(pcm, 16000));

    const File replaced = edit::WithKonamiSample(file, *rom, 0, incoming);
    const std::vector<uint8_t> built = format::ExportArchive(replaced);

    const std::shared_ptr<const KonamiRom> replaced_rom = song::LoadKonamiRom(built);

    return {cart, file, rom, pcm, incoming, replaced, built, replaced_rom};
}

// A WAV file goes into a sample's place: its header and PCM go into the free space at the ROM's end, with 16 zeros
// after them, and the sample's entry names them. The cartridge has no run of filler at its end, and its free space
// starts at its end. The samples of the WAV file are multiples of 256, and come back as they are. 16,000 Hz takes the
// step of sample 1, 0xC2D. That step plays 15,999 Hz.
void TestSampleReplaced()
{
    const SampleFixture fixture = MakeSampleFixture();
    const std::vector<uint8_t>& cart = fixture.cart;
    const seqlib::Pcm& pcm = fixture.pcm;
    const edit::IncomingKonamiSample& incoming = fixture.incoming;
    const File& replaced = fixture.replaced;
    const std::vector<uint8_t>& built = fixture.built;
    const std::shared_ptr<const KonamiRom>& replaced_rom = fixture.replaced_rom;

    XSFLIB_CHECK(incoming.rate == 16000);
    XSFLIB_CHECK(incoming.loop_start == 10u);
    SEQLIB_REQUIRE(incoming.pcm.size() == 40);
    XSFLIB_CHECK(incoming.pcm[0] == -100);
    XSFLIB_CHECK(incoming.pcm[39] == 95);

    SEQLIB_REQUIRE(built.size() == kCartSize + 12 + 40 + 16);
    XSFLIB_CHECK(Word32(built, kSampleTable) == kRomBase + kCartSize);
    XSFLIB_CHECK(Word32(built, kCartSize) == 0xC2D);
    XSFLIB_CHECK(Word32(built, kCartSize + 4) == 40);
    XSFLIB_CHECK(Word32(built, kCartSize + 8) == 10);
    XSFLIB_CHECK(std::equal(built.begin(), built.begin() + kSampleTable, cart.begin()));
    XSFLIB_CHECK(
        std::equal(built.begin() + kSampleTable + 4, built.begin() + kCartSize, cart.begin() + kSampleTable + 4));
    XSFLIB_CHECK(replaced.native->driver->free_start == kCartSize + 0x100);

    const seqlib::WavContents wav = seqlib::ReadWav(song::KonamiSampleWav(*replaced_rom, 0));
    XSFLIB_CHECK(wav.pcm.channels == pcm.channels);
    XSFLIB_CHECK(wav.pcm.loop && wav.pcm.loop_start == 10 && wav.pcm.loop_end == 40);
    XSFLIB_CHECK(wav.rate == 15999);

    // The file keeps the new bytes and its free space in both forms.
    XSFLIB_CHECK(format::ExportArchive(format::Read(format::Write(replaced))) == built);
    XSFLIB_CHECK(format::ExportArchive(format::ReadText(format::WriteText(replaced))) == built);
}

// The free space after the sample starts on the next multiple of 256 bytes. A song that grows then goes there.
void TestSampleAfterFreeSpace()
{
    const SampleFixture fixture = MakeSampleFixture();
    const edit::IncomingKonamiSample& incoming = fixture.incoming;
    const File& replaced = fixture.replaced;
    const std::vector<uint8_t>& built = fixture.built;
    const std::shared_ptr<const KonamiRom>& replaced_rom = fixture.replaced_rom;

    // The next sample goes after the free space's new start. A song that grows goes after it too, and the samples keep
    // their bytes.
    const File again = edit::WithKonamiSample(replaced, *replaced_rom, 1, incoming);
    const std::vector<uint8_t> twice = format::ExportArchive(again);
    XSFLIB_CHECK(Word32(twice, kSampleTable + 4) == kRomBase + kCartSize + 0x100);
    XSFLIB_CHECK(std::equal(built.begin() + kCartSize, built.end(), twice.begin() + kCartSize));
    const Sequence song0 = ReadOf(again, 0);
    const song::Track& square = *TrackOf(song0, 0);
    const edit::NewNote added =
        edit::NewKonamiNote(again, 0, square, square.stretches.at(square.notes[0].stretch), 30, 62, 3, 3);
    const std::vector<uint8_t> grown = format::ExportArchive(edit::EditNotes(again, 0, {}, std::span(&added, 1)).file);
    XSFLIB_CHECK(std::equal(twice.begin() + kCartSize, twice.end(), grown.begin() + kCartSize));
    XSFLIB_CHECK(Word32(grown, kSongTable) >= kRomBase + kCartSize + 0x200);
    XSFLIB_CHECK(StartsAt(song::ReadSequence(*song::LoadKonamiRom(grown), 0), 0, {0, 30}));
}

// The bytes of a replaced sample, written by hand, and the refusals of a sample.
void TestSampleByHand()
{
    const SampleFixture fixture = MakeSampleFixture();
    const std::vector<uint8_t>& cart = fixture.cart;
    const File& file = fixture.file;
    const std::shared_ptr<const KonamiRom>& rom = fixture.rom;
    const edit::IncomingKonamiSample& incoming = fixture.incoming;
    const std::vector<uint8_t>& built = fixture.built;
    const std::shared_ptr<const KonamiRom>& replaced_rom = fixture.replaced_rom;

    // The ROM plays the new sample as the model reads it from bytes written by hand.
    std::vector<uint8_t> by_hand = cart;
    by_hand.resize(kCartSize + 12 + 40 + 16);
    Put32(by_hand, kSampleTable, kRomBase + kCartSize);
    Put32(by_hand, kCartSize, 0xC2D);
    Put32(by_hand, kCartSize + 4, 40);
    Put32(by_hand, kCartSize + 8, 10);
    for (std::size_t i = 0; i < 40; i++)
    {
        by_hand[kCartSize + 12 + i] = static_cast<uint8_t>(incoming.pcm[i]);
    }
    XSFLIB_CHECK(by_hand == built);
    XSFLIB_CHECK(DirectStrict(*replaced_rom, 0, 4096) != DirectStrict(*rom, 0, 4096));

    edit::IncomingKonamiSample slow = incoming;
    slow.rate = 100;
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::WithKonamiSample(file, *rom, 0, slow); }) ==
                 "The rate must be from 329 to 336380 Hz.");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::WithKonamiSample(file, *rom, 5, incoming); }) ==
                 "The ROM has no Sample 0005.");
    XSFLIB_CHECK(
        ErrorOf<std::invalid_argument>([&] { edit::ReadIncomingKonamiSample(std::vector<uint8_t>{1, 2, 3}); }) != "");
}

// A ROM without room for a sample.
void TestSampleNoRoom()
{
    const SampleFixture fixture = MakeSampleFixture();
    const std::vector<uint8_t>& cart = fixture.cart;
    const edit::IncomingKonamiSample& incoming = fixture.incoming;

    // A ROM that fills the GBA's 32 MiB has no room.
    std::vector<uint8_t> full = cart;
    full.resize(konami::kLargestRom, 0x22);
    XSFLIB_CHECK(
        ErrorOf<std::invalid_argument>(
            [&] { edit::WithKonamiSample(format::ImportKonamiRom(full), *song::LoadKonamiRom(full), 0, incoming); })
            .starts_with("No room for 68 more bytes."));
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("konami_samples",
                                    []
                                    {
                                        TestSampleValues();
                                        TestBorrowedRates();
                                        TestSampleReplaced();
                                        TestSampleAfterFreeSpace();
                                        TestSampleByHand();
                                        TestSampleNoRoom();
                                    });
}
