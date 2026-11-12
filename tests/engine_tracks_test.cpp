// SPDX-License-Identifier: MIT

// Tests the track edits of the GBA, the DS and DSE (src/edit/engine_tracks.h, gba_tracks.cpp and dse_tracks.cpp):
// adding, copying and removing tracks. A small GBA cartridge contains songs whose headers list their tracks (GbaRom),
// and its ROM accepts the edits by MusicPlayer2000's rules. A small DS archive contains a sequence that allocates two
// tracks and a sequence of track 0 alone (DsArchive). The archive accepts the edits by NITRO-Composer's rules.

#include "cart_bytes.h"
#include "tracks_fixtures.h"

namespace
{

// The cartridge's songs. Song 0 plays two tracks on music player 0, of 10 tracks. Track 0 sets the tempo and plays
// keys 60 and 62. Track 1 calls a pattern of key 67, then plays key 64. Song 1 loops on one track of key 72. Song 2
// plays one track of key 48 on another voice group. Song 3 plays three tracks on music player 1, of 3 tracks. Song 4
// names song 1's header, and plays on music player 1. The bytes from 0x38000 on are 0xFF: the ROM's free space.
constexpr uint32_t kGbaSong = 0;
constexpr uint32_t kGbaLoop = 1;
constexpr uint32_t kGbaOther = 2;
constexpr uint32_t kGbaFull = 3;
constexpr uint32_t kGbaShared = 4;

std::vector<uint8_t> GbaRom()
{
    namespace mp2k_cart = ntdlib::testing::mp2k_cart;
    using enum ntdlib::mp2k::Command;

    mp2k_cart::Cart cart;
    const uint32_t wave = cart.Wave(mp2k_cart::Saw(), 13379, 0);
    const uint32_t voices = cart.VoiceGroup({{0, mp2k_cart::Cart::Voice(0, wave)}});
    const uint32_t other = cart.VoiceGroup({{0, mp2k_cart::Cart::Voice(0, wave)}});

    mp2k_cart::Track first;
    first.Cmd(kCmdTempo, 60).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Note(24, 60, 100).Wait(24);
    first.Note(24, 62, 100).Wait(24).Fine();
    mp2k_cart::Track second;
    second.Cmd(kCmdVoice, 0).Cmd(kCmdVol, 90);
    const std::size_t call = second.Here();
    second.Patt(call + 10).Note(24, 64, 100).Wait(24).Fine();
    second.Note(12, 67, 100).Wait(24).Raw({kCmdPend});
    cart.Song({first, second}, voices);

    mp2k_cart::Track looping;
    looping.Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100);
    const std::size_t loop = looping.Here();
    looping.Note(24, 72, 100).Wait(48).Goto(loop);
    const uint32_t looped = cart.Song({looping}, voices);

    mp2k_cart::Track low;
    low.Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Note(24, 48, 100).Wait(24).Fine();
    cart.Song({low}, other);

    mp2k_cart::Track one;
    one.Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Note(24, 50, 100).Wait(24).Fine();
    cart.Song({one, one, one}, voices, 1);

    cart.Song({one}, voices, 1);
    cart.Put32(mp2k_cart::kSongTable + 8 * kGbaShared, looped);

    for (uint32_t a = 0x38000; a < 0x40000; a++)
    {
        cart.Put8(mp2k_cart::kRomBase + a, 0xFF);
    }

    const ntdlib::agb::Rom rom = cart.ToRom();
    return hokuto::testing::RomBytes(rom);
}

// The ROM that `file`, a file of the cartridge, builds.
std::shared_ptr<const hokuto::song::GbaRom> GbaBuilt(const File& file)
{
    return hokuto::song::LoadGbaRom(hokuto::format::ExportArchive(file));
}

// Where each song plays in `file`. The program finds the places the same way.
std::vector<std::optional<SoundPlace>> GbaPlacesOf(const File& file)
{
    const std::shared_ptr<const hokuto::song::GbaRom> rom = GbaBuilt(file);
    std::vector<std::optional<uint32_t>> sequences;
    for (int song = 0; song < rom->info.song_count; song++)
    {
        sequences.push_back(hokuto::format::GbaSongSequence(file, rom->info.song_table, song));
    }

    return hokuto::edit::GbaSoundPlaces(sequences,
                                        hokuto::format::GbaTrackFields(file, rom->info.song_table, sequences.size()),
                                        hokuto::song::GbaPlayerTracks(*rom, sequences.size()));
}

// Song `song` as ntdlib's performer plays it from the ROM that `file` builds.
Sequence GbaPlayed(const File& file, uint32_t song)
{
    return hokuto::song::ReadSequence(*GbaBuilt(file), static_cast<int>(song));
}

// The header of song `song` of the ROM that `file` builds.
ntdlib::mp2k::SongHeader GbaHeader(const File& file, uint32_t song)
{
    const std::shared_ptr<const hokuto::song::GbaRom> rom = GbaBuilt(file);
    ntdlib::mp2k::SongHeader header;
    XSFLIB_CHECK(ntdlib::mp2k::ReadSong(rom->rom, rom->info, static_cast<int>(song), header));

    return header;
}

// Whether `file`'s ROM imports again into a file that builds the same ROM.
bool GbaRoundTrips(const File& file)
{
    const std::vector<uint8_t> built = hokuto::format::ExportArchive(file);
    return hokuto::format::ExportArchive(hokuto::format::ImportGbaRom(built)) == built;
}

// A new track of a GBA song gets the next place in the header. It sets full volume and the instrument, then waits to
// the end of the song's notes. The header gains the track, and the song and its header move into the ROM's free space.
// A song that loops gets a track that loops with it. Song 4 shares song 1's header on a music player of 3 tracks: the
// header accepts two new tracks, and no third. Song 3 has the 3 tracks of its player already.
void TestGbaAdd()
{
    const File file = hokuto::format::ImportGbaRom(GbaRom());
    const std::vector<std::optional<SoundPlace>> places = GbaPlacesOf(file);
    const Sequence before = GbaPlayed(file, kGbaSong);
    const uint32_t sequence = places.at(kGbaSong)->sequence;

    const TrackEdit added = AddTrack(file, places, kGbaSong, before, Program{0, 0});
    const Sequence after = GbaPlayed(added.file, kGbaSong);
    XSFLIB_CHECK(added.track == 2 && Numbers(after) == (std::vector<int>{0, 1, 2}));
    XSFLIB_CHECK(Notes(after, 0) == Notes(before, 0) && Notes(after, 1) == Notes(before, 1));
    SEQLIB_REQUIRE(after.tracks.size() == 3);
    XSFLIB_CHECK(after.tracks[2].notes.empty() &&
                 after.tracks[2].volume == (std::vector<hokuto::song::Setting>{{0, 127}}));
    XSFLIB_CHECK(after.length == before.length);
    const ntdlib::mp2k::SongHeader header = GbaHeader(added.file, kGbaSong);
    XSFLIB_CHECK(header.tracks.size() == 3 && header.address != GbaHeader(file, kGbaSong).address);
    XSFLIB_CHECK(hokuto::format::MovedSongs(added.file) == std::vector<uint32_t>{sequence});
    XSFLIB_CHECK(GbaRoundTrips(added.file));

    const Sequence loop = GbaPlayed(file, kGbaLoop);
    const TrackEdit looped = AddTrack(file, places, kGbaLoop, loop, std::nullopt);
    const Sequence played = GbaPlayed(looped.file, kGbaLoop);
    XSFLIB_CHECK(looped.track == 1 && Numbers(played) == (std::vector<int>{0, 1}) && played.loop == loop.loop);
    XSFLIB_CHECK(Notes(played, 0) == Notes(loop, 0) && GbaRoundTrips(looped.file));
    XSFLIB_CHECK(GbaHeader(looped.file, kGbaShared).address == GbaHeader(looped.file, kGbaLoop).address);
    const TrackEdit again = AddTrack(looped.file, GbaPlacesOf(looped.file), kGbaLoop, played, std::nullopt);
    XSFLIB_CHECK(again.track == 2 && Numbers(GbaPlayed(again.file, kGbaShared)) == (std::vector<int>{0, 1, 2}));
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>(
                     [&] { AddTrack(again.file, GbaPlacesOf(again.file), kGbaLoop, played, std::nullopt); }) ==
                 "The song's music player plays only 3 tracks, and the song already has 3. A new track wouldn't play.");
    XSFLIB_CHECK(Refused([&] { AddTrack(file, places, kGbaFull, GbaPlayed(file, kGbaFull), std::nullopt); }));
    XSFLIB_CHECK(Refused([&] { DuplicateTrack(file, places, kGbaFull, 0); }));
}

// A copy of track 1 runs its pattern too, and plays the track's notes. Track 0 of song 0 pastes into song 0 and song
// 1, without its tempo, and plays its notes there. Song 1 keeps its tempo. Song 2 plays another voice group, and
// accepts no track of song 0. A track of another console can't go in either.
void TestGbaCopies()
{
    const File file = hokuto::format::ImportGbaRom(GbaRom());
    const std::vector<std::optional<SoundPlace>> places = GbaPlacesOf(file);
    const Sequence before = GbaPlayed(file, kGbaSong);
    XSFLIB_CHECK(Notes(before, 1) == (std::vector<std::vector<uint32_t>>{{0, 12, 67, 100}, {24, 24, 64, 100}}));

    const TrackEdit copied = DuplicateTrack(file, places, kGbaSong, 1);
    const Sequence after = GbaPlayed(copied.file, kGbaSong);
    XSFLIB_CHECK(copied.track == 2 && Notes(after, 2) == Notes(before, 1) && Notes(after, 1) == Notes(before, 1));
    XSFLIB_CHECK(Notes(after, 0) == Notes(before, 0) && GbaRoundTrips(copied.file));
    XSFLIB_CHECK(Refused([&] { DuplicateTrack(file, places, kGbaSong, 2); }));

    const CopiedTrack track = CopyTrack(file, places, kGbaSong, before, 0);
    XSFLIB_CHECK(track.gba && track.entry == 0 && track.tick == 0 && track.banks.size() == 1);
    const auto tempo = [](const Command& command)
    {
        return command.kind == Kind::kTempo;
    };
    XSFLIB_CHECK(std::ranges::none_of(track.commands, tempo));
    const TrackEdit same = PasteTrack(file, places, kGbaSong, before, track);
    const Sequence twice = GbaPlayed(same.file, kGbaSong);
    XSFLIB_CHECK(same.track == 2 && Notes(twice, 2) == Notes(before, 0) && Notes(twice, 0) == Notes(before, 0));
    XSFLIB_CHECK(twice.tempo == before.tempo && GbaRoundTrips(same.file));

    // Track 1 of song 1 ends after a pass of the loop. The song's loop then comes after it.
    const Sequence loop = GbaPlayed(file, kGbaLoop);
    const TrackEdit pasted = PasteTrack(file, places, kGbaLoop, loop, track);
    const Sequence played = GbaPlayed(pasted.file, kGbaLoop);
    XSFLIB_CHECK(pasted.track == 1 && Notes(played, 1) == Notes(before, 0));
    XSFLIB_CHECK(Notes(played, 0).at(0) == Notes(loop, 0).at(0) && played.tempo == loop.tempo);
    XSFLIB_CHECK(GbaRoundTrips(pasted.file));

    XSFLIB_CHECK(
        ErrorOf<std::invalid_argument>([&] { PasteTrack(file, places, kGbaOther, GbaPlayed(file, kGbaOther), track); })
            .ends_with("A GBA song can use only one voice group."));
    CopiedTrack other = track;
    other.gba = false;
    XSFLIB_CHECK(Refused([&] { PasteTrack(file, places, kGbaLoop, loop, other); }));
    XSFLIB_CHECK(Refused(
        [&] { PasteTrack(ToolsFile(), PlacesOf(ToolsFile()), kSong, PlayedSequence(ToolsFile(), kSong), track); }));
}

// A removal deletes a track from the header of the song, and the tracks after it move up a number. The song keeps its
// commands, and stays in its place: only the header changes. A song keeps one track.
void TestGbaRemove()
{
    const File file = hokuto::format::ImportGbaRom(GbaRom());
    const std::vector<std::optional<SoundPlace>> places = GbaPlacesOf(file);
    const Sequence before = GbaPlayed(file, kGbaSong);

    const File removed = RemoveTrack(file, places, kGbaSong, 0);
    const Sequence after = GbaPlayed(removed, kGbaSong);
    XSFLIB_CHECK(Numbers(after) == (std::vector<int>{0}) && Notes(after, 0) == Notes(before, 1));
    XSFLIB_CHECK(removed.sequences[places[kGbaSong]->sequence] == file.sequences[places[kGbaSong]->sequence]);
    XSFLIB_CHECK(hokuto::format::MovedSongs(removed).empty());
    const std::vector<uint8_t> built = hokuto::format::ExportArchive(removed);
    const std::vector<uint8_t> original = hokuto::format::ExportArchive(file);
    const uint32_t header = GbaHeader(file, kGbaSong).address - ntdlib::agb::kRomBase;
    SEQLIB_REQUIRE(built.size() == original.size());
    bool kept = true;
    for (std::size_t i = 0; i < built.size(); i++)
    {
        kept = kept && (built[i] == original[i] || (i == header || (i >= header + 8 && i < header + 12)));
    }
    XSFLIB_CHECK(kept && built[header] == 1 && original[header] == 2);
    XSFLIB_CHECK(xsflib::LoadLe32(&built[header + 8]) == xsflib::LoadLe32(&original[header + 12]));
    XSFLIB_CHECK(GbaRoundTrips(removed));

    const File last = RemoveTrack(file, places, kGbaSong, 1);
    XSFLIB_CHECK(Numbers(GbaPlayed(last, kGbaSong)) == (std::vector<int>{0}) &&
                 Notes(GbaPlayed(last, kGbaSong), 0) == Notes(before, 0));
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { RemoveTrack(file, places, kGbaLoop, 0); }) ==
                 "A song must keep at least one track.");
    XSFLIB_CHECK(Refused([&] { RemoveTrack(file, places, kGbaSong, 2); }));
}

// The DS archive's sounds. Sounds 0 and 1 play sequence file 0. The file allocates tracks 0 and 1, and track 0 opens
// track 1:
//
//    0: FE 03 00        allocate tracks 0 and 1
//    3: 93 01 10 00 00  open track 1 at 16
//    8: C1 64           volume 100
//   10: C0 20           pan 0x20
//   12: 3C 7F 30        note 60, velocity 127, 48 ticks
//   15: FF              end
//   16: C1 40           track 1: volume 64
//   18: 80 18           wait 24 ticks
//   20: 43 50 18        note 67, velocity 80, 24 ticks
//   23: FF              end
//
// Sounds 2 and 3 play sequence file 1. The file allocates nothing, and NITRO-Composer plays track 0 alone:
//
//    0: C1 64     volume 100
//    2: 3C 7F 30  note 60, velocity 127, 48 ticks
//    5: FF        end
//
// Sound 3 plays bank 1, and the others bank 0. The two banks hold the same instrument.
constexpr uint32_t kDsTwo = 0;
constexpr uint32_t kDsShared = 1;
constexpr uint32_t kDsOne = 2;
constexpr uint32_t kDsOtherBank = 3;

std::vector<uint8_t> DsArchive()
{
    namespace sdat_files = ntdlib::testing::sdat_files;
    const sdat_files::Bytes two_tracks = {0xFE, 0x03, 0x00, 0x93, 0x01, 0x10, 0x00, 0x00, 0xC1, 0x64, 0xC0, 0x20,
                                          0x3C, 0x7F, 0x30, 0xFF, 0xC1, 0x40, 0x80, 0x18, 0x43, 0x50, 0x18, 0xFF};
    const sdat_files::Bytes one_track = {0xC1, 0x64, 0x3C, 0x7F, 0x30, 0xFF};
    const sdat_files::Wave square{
        .loop = 1,
        .loop_length = 4,
        .data = {0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0}};
    constexpr uint16_t kNone = 0xFFFF;

    ntdlib::sdat::ArchiveModel model;
    model.sequences = {ntdlib::sdat::SequenceInfo{0, 0, 127, 64, 64, 0, 0, ""},
                       ntdlib::sdat::SequenceInfo{0, 0, 127, 64, 64, 0, 0, ""},
                       ntdlib::sdat::SequenceInfo{1, 0, 127, 64, 64, 0, 0, ""},
                       ntdlib::sdat::SequenceInfo{1, 1, 127, 64, 64, 0, 0, ""}};
    model.banks = {ntdlib::sdat::BankInfo{2, {0, kNone, kNone, kNone}, ""},
                   ntdlib::sdat::BankInfo{3, {0, kNone, kNone, kNone}, ""}};
    model.wave_archives = {ntdlib::sdat::WaveArchiveInfo{4, ""}};
    model.players = {ntdlib::sdat::PlayerInfo{1, 0, 0, 0, ""}};
    model.files = {sdat_files::Sequence(two_tracks), sdat_files::Sequence(one_track),
                   sdat_files::Bank({{.type = 1, .region = {.wave = 0}}}),
                   sdat_files::Bank({{.type = 1, .region = {.wave = 0}}}), sdat_files::WaveArchive({square})};

    return ntdlib::sdat::BuildArchive(model);
}

// Where each sound of the DS file `file` plays.
std::vector<std::optional<SoundPlace>> DsPlacesOf(const File& file)
{
    return hokuto::edit::DsSoundPlaces(hokuto::format::DsSoundSequences(file, hokuto::format::ExportArchive(file)));
}

// Sound `sound` of the DS file `file` as ntdlib's performer plays it from the archive that the file builds.
Sequence DsPlayed(const File& file, uint32_t sound)
{
    return hokuto::song::ReadSequence(*hokuto::song::LoadDsArchive(hokuto::format::ExportArchive(file)), sound);
}

// The tracks that sequence `sequence` of the DS file `file` allocates with its first command, a bit each, or nothing
// without such a command.
std::optional<uint32_t> DsAllocation(const File& file, uint32_t sequence)
{
    const ntdlib::commands::Sequence& commands = *file.sequences.at(sequence);
    if (commands.items.empty() || commands.items.front().kind != Kind::kAllocateTracks)
    {
        return std::nullopt;
    }

    return static_cast<uint32_t>(commands.items.front().value.value / kOne);
}

// A new track gets a bit of the allocation at the sequence's start, and the sound that shares the sequence plays it
// too. The other tracks play as before. A sequence without an allocation gets one before its first command, with the
// bit of track 0 too.
void TestDsAdd()
{
    const File file = hokuto::format::ImportDsArchive(DsArchive());
    const std::vector<std::optional<SoundPlace>> places = DsPlacesOf(file);
    const Sequence before = DsPlayed(file, kDsTwo);
    SEQLIB_REQUIRE(places.size() == 4 && places[kDsTwo] && places[kDsOne]);
    XSFLIB_CHECK(Numbers(before) == (std::vector<int>{0, 1}));

    const TrackEdit added = AddTrack(file, places, kDsTwo, before, Program{0, 0});
    const Sequence after = DsPlayed(added.file, kDsTwo);
    XSFLIB_CHECK(added.track == 2 && Numbers(after) == (std::vector<int>{0, 1, 2}));
    XSFLIB_CHECK(Notes(after, 0) == Notes(before, 0) && Notes(after, 1) == Notes(before, 1));
    XSFLIB_CHECK(after.length == before.length);
    XSFLIB_CHECK(DsAllocation(added.file, places[kDsTwo]->sequence) == 7u);
    XSFLIB_CHECK(Numbers(DsPlayed(added.file, kDsShared)) == (std::vector<int>{0, 1, 2}));

    const Sequence one = DsPlayed(file, kDsOne);
    const uint32_t sequence = places[kDsOne]->sequence;
    XSFLIB_CHECK(Numbers(one) == (std::vector<int>{0}) && !DsAllocation(file, sequence));
    const TrackEdit opened = AddTrack(file, places, kDsOne, one, std::nullopt);
    const Sequence played = DsPlayed(opened.file, kDsOne);
    XSFLIB_CHECK(opened.track == 1 && Numbers(played) == (std::vector<int>{0, 1}));
    XSFLIB_CHECK(Notes(played, 0) == Notes(one, 0) && played.length == one.length);
    XSFLIB_CHECK(DsAllocation(opened.file, sequence) == 3u);
}

// A copy of track 1 plays its note. Sound 2 plays the same bank. Track 1 pastes into it, and plays its note at the same
// tick there. Sound 3 plays another bank, and accepts no track of sound 0. A track of a GBA song can't go into a DS
// sequence, and a DS track can't go into a GBA song.
void TestDsCopies()
{
    const File file = hokuto::format::ImportDsArchive(DsArchive());
    const std::vector<std::optional<SoundPlace>> places = DsPlacesOf(file);
    const Sequence before = DsPlayed(file, kDsTwo);
    XSFLIB_CHECK(Notes(before, 1) == (std::vector<std::vector<uint32_t>>{{24, 24, 67, 80}}));

    const TrackEdit copied = DuplicateTrack(file, places, kDsTwo, 1);
    const Sequence after = DsPlayed(copied.file, kDsTwo);
    XSFLIB_CHECK(copied.track == 2 && Notes(after, 2) == Notes(before, 1) && Notes(after, 1) == Notes(before, 1));
    XSFLIB_CHECK(Notes(after, 0) == Notes(before, 0));
    XSFLIB_CHECK(DsAllocation(copied.file, places[kDsTwo]->sequence) == 7u);
    XSFLIB_CHECK(Refused([&] { DuplicateTrack(file, places, kDsOne, 1); }));

    const CopiedTrack track = CopyTrack(file, places, kDsTwo, before, 1);
    XSFLIB_CHECK(track.ds && !track.gba && track.banks == std::vector<uint32_t>{0} && track.tick == 0);
    const Sequence one = DsPlayed(file, kDsOne);
    const TrackEdit pasted = PasteTrack(file, places, kDsOne, one, track);
    const Sequence played = DsPlayed(pasted.file, kDsOne);
    XSFLIB_CHECK(pasted.track == 1 && Notes(played, 1) == Notes(before, 1) && Notes(played, 0) == Notes(one, 0));
    XSFLIB_CHECK(DsAllocation(pasted.file, places[kDsOne]->sequence) == 3u);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>(
                     [&] { PasteTrack(file, places, kDsOtherBank, DsPlayed(file, kDsOtherBank), track); }) ==
                 "The track uses BANK_0, but the sound uses BANK_1. A DS sequence can use only one bank.");

    const File gba = hokuto::format::ImportGbaRom(GbaRom());
    const std::vector<std::optional<SoundPlace>> gba_places = GbaPlacesOf(gba);
    const Sequence song = GbaPlayed(gba, kGbaSong);
    const CopiedTrack from_gba = CopyTrack(gba, gba_places, kGbaSong, song, 0);
    XSFLIB_CHECK(Refused([&] { PasteTrack(file, places, kDsOne, one, from_gba); }));
    XSFLIB_CHECK(Refused([&] { PasteTrack(gba, gba_places, kGbaSong, song, track); }));
}

// A removal clears the bit of the track in the allocation, and neither sound of the sequence plays the track any more.
// Track 0 stays. A sequence without an allocation plays no other track to remove.
void TestDsRemove()
{
    const File file = hokuto::format::ImportDsArchive(DsArchive());
    const std::vector<std::optional<SoundPlace>> places = DsPlacesOf(file);
    const Sequence before = DsPlayed(file, kDsTwo);

    const File removed = RemoveTrack(file, places, kDsTwo, 1);
    const Sequence after = DsPlayed(removed, kDsTwo);
    XSFLIB_CHECK(Numbers(after) == (std::vector<int>{0}) && Notes(after, 0) == Notes(before, 0));
    XSFLIB_CHECK(DsAllocation(removed, places[kDsTwo]->sequence) == 1u);
    XSFLIB_CHECK(Numbers(DsPlayed(removed, kDsShared)) == (std::vector<int>{0}));
    XSFLIB_CHECK(Refused([&] { RemoveTrack(file, places, kDsTwo, 0); }));
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { RemoveTrack(file, places, kDsOne, 1); }) ==
                 "The sound doesn't allocate that track.");
}

// A DSE song's tracks and its bank, and a second song of the same bank. Its first track is the conductor. A sound
// effect has one track.
constexpr uint32_t kDseSong = 0;
constexpr uint32_t kDseOther = 1;
constexpr uint32_t kDseLooped = 2;
constexpr uint32_t kDseEffect = 3;

std::vector<uint8_t> DseImage()
{
    namespace files = ntdlib::testing::dse_song_files;
    using files::Join;
    const files::Bytes conductor = {0, 0, 0, 0, 0xA4, 0x78, 0x98};
    const files::Bytes first =
        Join({{1, 1, 0, 0}, {0xAC, 0x00, 0xE0, 0x64}, files::Note(0x7F, 0, 0, 0x30, 1), {0x92, 0x30, 0x98}});
    const files::Bytes second =
        Join({{2, 2, 0, 0}, {0xAC, 0x00, 0x92, 0x18}, files::Note(0x50, 7, 0, 0x18, 1), {0x92, 0x18, 0x98}});
    const files::Bytes looped =
        Join({{1, 1, 0, 0}, {0xAC, 0x00, 0x92, 0x18, 0x99}, files::Note(0x7F, 0, 0, 0x30, 1), {0x92, 0x30, 0x98}});
    const files::Bytes effect = Join({{0, 0, 0, 0, 0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x7F},
                                      files::Note(0x60, 0, 0, 0x18, 1),
                                      {0x92, 0x18, 0x98}});
    ntdlib::testing::nds_files::Image image;
    image.files = {
        {"SOUND/BGM/bgm.swd", files::MainBankFile()},
        {"SOUND/BGM/bgm0001.smd", files::SongFile({conductor, first, second})},
        {"SOUND/BGM/bgm0001.swd", files::BankFile()},
        {"SOUND/BGM/bgm0002.smd", files::SongFile({conductor, second})},
        {"SOUND/BGM/bgm0002.swd", files::BankFile()},
        {"SOUND/BGM/bgm0003.smd", files::SongFile({conductor, looped})},
        {"SOUND/BGM/bgm0003.swd", files::BankFile()},
        {"SOUND/SE/se.sed", files::EffectsFile({files::Effect{.tracks = {effect}}}, 0x0101)},
    };

    return ntdlib::testing::nds_files::BuildImage(image).first;
}

std::vector<std::optional<SoundPlace>> DsePlacesOf(const File& file)
{
    const std::vector<uint8_t> set = hokuto::format::ExportArchive(file);
    const std::shared_ptr<const hokuto::song::DseSet> sounds =
        hokuto::song::LoadDseSet(set, ntdlib::dse::Build::kLater);
    return hokuto::edit::DsSoundPlaces(
        hokuto::format::DseSoundSequences(file, set, hokuto::song::DseSoundRefs(*sounds)));
}

// Sound `sound` of the DSE file `file` as ntdlib's performer plays it from the set that the file builds.
Sequence DsePlayed(const File& file, uint32_t sound)
{
    return hokuto::song::ReadSequence(
        *hokuto::song::LoadDseSet(hokuto::format::ExportArchive(file), ntdlib::dse::Build::kLater), sound);
}

// The channel of each track of `sequence`.
std::vector<int> ChannelsOf(const Sequence& sequence)
{
    std::vector<int> channels;
    for (const Track& track : sequence.tracks)
    {
        channels.push_back(track.channel);
    }

    return channels;
}

// A new track of a song is added after its last track, with the next number, on the lowest channel that no track uses.
// It plays no notes, and the other tracks play as before. With a loop, the sound still loops where it did, and the end
// of the new track jumps back to its loop point. The new track of a sound effect selects the bank of the effect first,
// and a note in it plays. A song without a free channel accepts no new track, and a sound effect has 4 tracks at most.
void TestDseAdd()
{
    namespace files = ntdlib::testing::dse_song_files;
    const File file = hokuto::format::ImportDseImage(DseImage());
    const std::vector<std::optional<SoundPlace>> places = DsePlacesOf(file);
    const Sequence before = DsePlayed(file, kDseSong);
    SEQLIB_REQUIRE(places.size() == 4 && places[kDseSong]);
    XSFLIB_CHECK(Numbers(before) == (std::vector<int>{0, 1, 2}));
    XSFLIB_CHECK(ChannelsOf(before) == (std::vector<int>{0, 1, 2}));

    const TrackEdit added = AddTrack(file, places, kDseSong, before, Program{0, 1});
    const Sequence after = DsePlayed(added.file, kDseSong);
    XSFLIB_CHECK(added.track == 3 && Numbers(after) == (std::vector<int>{0, 1, 2, 3}));
    XSFLIB_CHECK(ChannelsOf(after) == (std::vector<int>{0, 1, 2, 3}));
    XSFLIB_CHECK(Notes(after, 1) == Notes(before, 1) && Notes(after, 2) == Notes(before, 2));
    XSFLIB_CHECK(after.tracks[3].notes.empty() && after.length == before.length);
    XSFLIB_CHECK(Numbers(DsePlayed(added.file, kDseOther)) == (std::vector<int>{0, 1}));

    // The new track selects the sound's bank. A note on it then sounds, on the track's program.
    const hokuto::edit::NewNote note{.stretch = after.tracks[3].stretches.at(0), .tick = 0, .key = 60, .length = 12};
    const Sequence with_note = DsePlayed(
        hokuto::edit::EditNotes(added.file, places[kDseSong]->sequence, {}, std::vector{note}).file, kDseSong);
    SEQLIB_REQUIRE(with_note.tracks.size() == 4 && with_note.tracks[3].notes.size() == 1);
    XSFLIB_CHECK(with_note.tracks[3].program == 1);

    const Sequence looped = DsePlayed(file, kDseLooped);
    SEQLIB_REQUIRE(looped.loop.has_value());
    const TrackEdit in_loop = AddTrack(file, places, kDseLooped, looped, std::nullopt);
    const Sequence loops = DsePlayed(in_loop.file, kDseLooped);
    SEQLIB_REQUIRE(loops.loop.has_value());
    XSFLIB_CHECK(in_loop.track == 2 && Notes(loops, 1) == Notes(looped, 1));
    XSFLIB_CHECK(loops.loop->start == looped.loop->start && loops.loop->end == looped.loop->end);

    const Sequence effect = DsePlayed(file, kDseEffect);
    const TrackEdit effect_added = AddTrack(file, places, kDseEffect, effect, Program{0, 0});
    const Sequence effect_after = DsePlayed(effect_added.file, kDseEffect);
    XSFLIB_CHECK(effect_added.track == 1 && Numbers(effect_after) == (std::vector<int>{0, 1}));
    XSFLIB_CHECK(Notes(effect_after, 0) == Notes(effect, 0));
    const hokuto::edit::NewNote effect_note{
        .stretch = effect_after.tracks.at(1).stretches.at(0), .tick = 0, .key = 60, .length = 12};
    const Sequence effect_with_note = DsePlayed(
        hokuto::edit::EditNotes(effect_added.file, places[kDseEffect]->sequence, {}, std::vector{effect_note}).file,
        kDseEffect);
    SEQLIB_REQUIRE(effect_with_note.tracks.size() == 2);
    XSFLIB_CHECK(effect_with_note.tracks[1].notes.size() == 1 && effect_with_note.tracks[1].unstarted.empty());

    TrackEdit full{effect_added.file, 1};
    for (int more = 0; more < 2; more++)
    {
        full = AddTrack(full.file, places, kDseEffect, effect, std::nullopt);
    }
    XSFLIB_CHECK(Numbers(DsePlayed(full.file, kDseEffect)).size() == 4);
    XSFLIB_CHECK(
        ErrorOf<std::invalid_argument>([&] { AddTrack(full.file, places, kDseEffect, effect, std::nullopt); }) ==
        "DSE plays at most 4 tracks in a sound effect.");

    std::vector<files::Bytes> sixteen = {{0, 0, 0, 0, 0xA4, 0x78, 0x98}};
    for (uint8_t track = 1; track < 16; track++)
    {
        sixteen.push_back({track, track, 0, 0, 0x98});
    }
    ntdlib::testing::nds_files::Image image;
    image.files = {{"SOUND/BGM/bgm.swd", files::MainBankFile()},
                   {"SOUND/BGM/bgm0001.smd", files::SongFile(sixteen)},
                   {"SOUND/BGM/bgm0001.swd", files::BankFile()}};
    const File taken = hokuto::format::ImportDseImage(ntdlib::testing::nds_files::BuildImage(image).first);
    const std::vector<std::optional<SoundPlace>> taken_places = DsePlacesOf(taken);
    XSFLIB_CHECK(
        ErrorOf<std::invalid_argument>([&] { AddTrack(taken, taken_places, 0, DsePlayed(taken, 0), std::nullopt); }) ==
        "All 16 channels of the sound already have a track. Tracks on the same channel share its volume, pan "
        "and program.");
}

// A copy plays the track's notes on a free channel, and leaves out the tempo. A track pastes into a song of the same
// bank, with its notes at the same ticks. A sound of another driver can't accept it, and a DSE sound accepts no track
// of another driver.
void TestDseCopies()
{
    const File file = hokuto::format::ImportDseImage(DseImage());
    const std::vector<std::optional<SoundPlace>> places = DsePlacesOf(file);
    const Sequence before = DsePlayed(file, kDseSong);

    const TrackEdit copied = DuplicateTrack(file, places, kDseSong, 1);
    const Sequence after = DsePlayed(copied.file, kDseSong);
    XSFLIB_CHECK(copied.track == 3 && Notes(after, 3) == Notes(before, 1) && Notes(after, 1) == Notes(before, 1));
    XSFLIB_CHECK(ChannelsOf(after) == (std::vector<int>{0, 1, 2, 3}));
    XSFLIB_CHECK(Refused([&] { DuplicateTrack(file, places, kDseSong, 3); }));

    const TrackEdit conductor = DuplicateTrack(file, places, kDseSong, 0);
    const Sequence with_copy = DsePlayed(conductor.file, kDseSong);
    XSFLIB_CHECK(conductor.track == 3 && with_copy.tempo == before.tempo);
    const auto tempos = [&places](const File& in)
    {
        return std::ranges::count(in.sequences.at(places[kDseSong]->sequence)->items, Kind::kTempo, &Command::kind);
    };
    XSFLIB_CHECK(tempos(file) == 1 && tempos(conductor.file) == 1);

    const CopiedTrack track = CopyTrack(file, places, kDseSong, before, 1);
    XSFLIB_CHECK(track.dse && !track.ds && !track.gba && track.tick == 0);
    const Sequence other = DsePlayed(file, kDseOther);
    const TrackEdit pasted = PasteTrack(file, places, kDseOther, other, track);
    const Sequence played = DsePlayed(pasted.file, kDseOther);
    XSFLIB_CHECK(pasted.track == 2 && Notes(played, 2) == Notes(before, 1) && Notes(played, 1) == Notes(other, 1));

    const File ds = hokuto::format::ImportDsArchive(DsArchive());
    const std::vector<std::optional<SoundPlace>> ds_places = DsPlacesOf(ds);
    const Sequence ds_one = DsPlayed(ds, kDsOne);
    const CopiedTrack from_ds = CopyTrack(ds, ds_places, kDsTwo, DsPlayed(ds, kDsTwo), 1);
    XSFLIB_CHECK(Refused([&] { PasteTrack(file, places, kDseOther, other, from_ds); }));
    XSFLIB_CHECK(Refused([&] { PasteTrack(ds, ds_places, kDsOne, ds_one, track); }));
}

// A removal deletes the track from the file, and the tracks after it move up a number and keep their channels. The
// track that sets the tempo stays, and so does a sound's last track. A sound effect has no conductor.
void TestDseRemove()
{
    const File file = hokuto::format::ImportDseImage(DseImage());
    const std::vector<std::optional<SoundPlace>> places = DsePlacesOf(file);
    const Sequence before = DsePlayed(file, kDseSong);

    const File removed = RemoveTrack(file, places, kDseSong, 1);
    const Sequence after = DsePlayed(removed, kDseSong);
    XSFLIB_CHECK(Numbers(after) == (std::vector<int>{0, 1}) && Notes(after, 1) == Notes(before, 2));
    XSFLIB_CHECK(ChannelsOf(after) == (std::vector<int>{0, 2}));
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { RemoveTrack(file, places, kDseSong, 0); }) ==
                 "Track 0 sets the tempo. Can't remove it.");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { RemoveTrack(file, places, kDseSong, 3); }) ==
                 "The sound has no such track.");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { RemoveTrack(file, places, kDseEffect, 0); }) ==
                 "A sound must keep at least one track.");
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("engine_tracks",
                                    []
                                    {
                                        TestGbaAdd();
                                        TestGbaCopies();
                                        TestGbaRemove();
                                        TestDsAdd();
                                        TestDsCopies();
                                        TestDsRemove();
                                        TestDseAdd();
                                        TestDseCopies();
                                        TestDseRemove();
                                    });
}
