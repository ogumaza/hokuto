// SPDX-License-Identifier: MIT

// Tests what else an edit of a track's mix or instrument reaches, and which other sounds reach a command
// (src/edit/reach.h). A small Switch archive holds a sequence file that three sounds play, and another file of a fourth
// sound. In the shared file, track 0 opens track 1, calls a subroutine that sets the volume, selects program 1 and
// plays a note. Track 1 calls the same subroutine, sets its pan and plays a note. The second sound starts further on.
// It opens track 2 where the first sound opens track 1, and its track 1 elsewhere. Its track 0 calls the subroutine and
// plays a note. The third sound starts where the first does.
//
// A small GBA ROM contains three songs. Songs 0 and 2 name one header, and song 1 another.
//
// A small DS archive contains a sequence file that sounds 0 and 1 play, and another file of sound 2. The shared file
// starts with an allocation of tracks 0 and 1. Its track 0 opens track 1, sets its volume and plays a note. Track 1
// sets its volume and plays a note.

#include "edit/reach.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/mp2k/song.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sdat/layout.h"
#include "ntdlib/testing/mp2k_cart.h"
#include "ntdlib/testing/sar_files.h"
#include "ntdlib/testing/sdat_files.h"
#include "xsflib/testing/check.h"

#include "cart_bytes.h"
#include "edit/replace.h"
#include "format/archive.h"
#include "format/file.h"
#include "song/archive.h"
#include "song/ds.h"
#include "song/gba.h"
#include "song/sequence.h"
#include "sound_places.h"

using hokuto::edit::Reach;
using hokuto::edit::ReachOfTracks;
using hokuto::edit::SoundPlace;
using hokuto::edit::SoundsReaching;
using hokuto::edit::TrackReach;
using hokuto::format::File;
using hokuto::song::Mix;
using hokuto::song::Sequence;
using hokuto::testing::PlacesOf;

namespace
{

namespace sar = ntdlib::sar;
namespace sar_files = ntdlib::testing::sar_files;

// The archive's sounds.
constexpr uint32_t kFirst = 0;
constexpr uint32_t kLater = 1;
constexpr uint32_t kSame = 2;
constexpr uint32_t kOther = 3;

// The shared file's bytecode. Each command's offset comes first in its comment. The Switch's sequences give arguments
// in little-endian order. Each note waits for its length.
const std::vector<uint8_t> kSharedBytecode = {
    0x88, 1,   20,   0, 0, // 0: open track 1 at 20
    0xB0, 48,              // 5: timebase 48
    0xE1, 120, 0x00,       // 7: tempo 120
    0x8A, 52,  0,    0,    // 10: call 52
    0x81, 1,               // 14: program 1
    60,   100, 24,         // 16: key 60 for 24 ticks
    0xFF,                  // 19: the end
    0x8A, 52,  0,    0,    // 20: track 1: call 52
    0xC0, 74,              // 24: pan 10 to the right
    64,   100, 24,         // 26: key 64 for 24 ticks
    0xFF,                  // 29: the end
    0x88, 2,   20,   0, 0, // 30: the second sound: open track 2 at 20
    0x88, 1,   48,   0, 0, // 35: open track 1 at 48
    0x8A, 52,  0,    0,    // 40: call 52
    67,   100, 24,         // 44: key 67 for 24 ticks
    0xFF,                  // 47: the end
    69,   100, 24,         // 48: the second sound's track 1: key 69 for 24 ticks
    0xFF,                  // 51: the end
    0xC1, 100,             // 52: the subroutine: volume 100
    0xFD,                  // 54: return
};

// The other file's bytecode.
const std::vector<uint8_t> kOtherBytecode = {
    0xC1, 100,     // 0: volume 100
    60,   100, 24, // 2: key 60 for 24 ticks
    0xFF,          // 5: the end
};

// The archive. The bank plays a looped wave for every key of its two programs.
std::vector<uint8_t> TestArchive()
{
    sar_files::WaveSpec wave;
    wave.encoding = 1;
    wave.loop = true;
    wave.sample_rate = 48000;
    wave.loop_end = 64;
    wave.channels = {std::vector<uint8_t>(128, 0x10)};

    sar_files::Region region;
    region.original_key = 60;
    sar_files::VelocityTable velocities;
    velocities.entries = {{0, region}};
    sar_files::KeyTable keys;
    keys.entries = {{0, velocities}};
    sar_files::BankSpec bank;
    bank.waves = {{0x05000000, 0}};
    bank.instruments = {keys, keys};

    sar_files::SoundSpec first;
    first.name = "SEQ_FIRST";
    first.volume = 127;
    first.banks = {0};
    first.allocate_track_flags = 3;
    sar_files::SoundSpec later = first;
    later.name = "SEQ_LATER";
    later.allocate_track_flags = 7;
    later.start_offset = 30;
    sar_files::SoundSpec same = first;
    same.name = "SEQ_SAME";
    same.start_offset = 0;
    sar_files::SoundSpec other = later;
    other.name = "SEQ_OTHER";
    other.allocate_track_flags = 1;
    other.file = 1;
    other.start_offset = 0;

    sar_files::ArchiveSpec spec;
    spec.sounds = {first, later, same, other};
    spec.banks = {{"BANK_TEST", 2}};
    spec.wave_archives = {{"WARC_TEST", 3}};
    spec.players = {{"PLAYER_TEST", 1}};
    spec.files = {{sar_files::Sequence(false, kSharedBytecode, {{"First", 0}, {"Later", 30}}), "", false},
                  {sar_files::Sequence(false, kOtherBytecode, {{"Other", 0}}), "", false},
                  {sar_files::Bank(false, bank), "", false},
                  {sar_files::WaveArchive(false, {sar_files::Wave(false, wave)}), "", false}};

    return sar_files::Archive(false, spec);
}

// What an edit of each track of sound `sound` of `file` reaches. The program finds it the same way.
std::vector<TrackReach> ReachOf(const File& file, uint32_t sound)
{
    const std::vector<uint8_t> built = hokuto::format::ExportArchive(file);
    const std::shared_ptr<const sar::SoundArchive> archive = hokuto::song::LoadArchive(built, "test.bfsar");
    const std::vector<std::optional<SoundPlace>> places =
        hokuto::edit::SoundPlaces(*archive, hokuto::format::SequencesOfFiles(file, built, *archive),
                                  hokuto::format::StartFieldsOfSounds(file, built));
    const Sequence read = hokuto::song::ReadSequence(*archive, sound);
    return ReachOfTracks(file, places, sound, read);
}

// Whether `reach` holds the tracks `tracks` and the sounds `sounds`, in that order.
bool Holds(const Reach& reach, const std::vector<int>& tracks, const std::vector<uint32_t>& sounds)
{
    return reach.tracks == tracks && reach.sounds == sounds;
}

// The subroutine's volume reaches the other track that calls it, and the other sounds of the file. Its pan and sends
// come from track 0's start: a new command there reaches the sound that starts there too. Its program change reaches
// that sound alone.
void TestFirstTrack()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());

    const std::vector<TrackReach> reach = ReachOf(file, kFirst);

    XSFLIB_CHECK(reach.size() == 2);
    XSFLIB_CHECK(Holds(reach.at(0).mix[static_cast<std::size_t>(Mix::kVolume)], {1}, {kLater, kSame}));
    XSFLIB_CHECK(Holds(reach.at(0).mix[static_cast<std::size_t>(Mix::kPan)], {}, {kSame}));
    XSFLIB_CHECK(Holds(reach.at(0).mix[static_cast<std::size_t>(Mix::kSendC)], {}, {kSame}));
    XSFLIB_CHECK(Holds(reach.at(0).instrument, {}, {kSame}));
}

// Track 1's pan command reaches the sounds that run its commands, through track 1 or track 2. Its instrument comes from
// the track's start. A new command there reaches only the sound whose track start opens track 1 there.
void TestOpenedTrack()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());

    const std::vector<TrackReach> reach = ReachOf(file, kFirst);

    XSFLIB_CHECK(Holds(reach.at(1).mix[static_cast<std::size_t>(Mix::kVolume)], {0}, {kLater, kSame}));
    XSFLIB_CHECK(Holds(reach.at(1).mix[static_cast<std::size_t>(Mix::kPan)], {}, {kLater, kSame}));
    XSFLIB_CHECK(Holds(reach.at(1).instrument, {}, {kSame}));
}

// The second sound's volume reaches its track 2 and the other sounds that call the subroutine. Track 2 calls it too,
// and runs the first sound's track 1. No other sound starts where the second sound starts, or opens track 1 where the
// second sound opens it. A new command at either opening reaches no other sound. A sound of another file is never
// reached.
void TestLaterStart()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());

    const std::vector<TrackReach> later = ReachOf(file, kLater);
    const std::vector<TrackReach> other = ReachOf(file, kOther);

    XSFLIB_CHECK(later.size() == 3);
    XSFLIB_CHECK(Holds(later.at(0).mix[static_cast<std::size_t>(Mix::kVolume)], {2}, {kFirst, kSame}));
    XSFLIB_CHECK(Holds(later.at(0).mix[static_cast<std::size_t>(Mix::kPan)], {}, {}));
    XSFLIB_CHECK(Holds(later.at(0).instrument, {}, {}));
    XSFLIB_CHECK(Holds(later.at(1).mix[static_cast<std::size_t>(Mix::kVolume)], {}, {}));
    XSFLIB_CHECK(Holds(later.at(2).mix[static_cast<std::size_t>(Mix::kPan)], {}, {kFirst, kSame}));
    XSFLIB_CHECK(other.size() == 1 && Holds(other.at(0).mix[static_cast<std::size_t>(Mix::kVolume)], {}, {}));
}

// The first sound's tempo reaches the third sound. Both sounds start at the same command. The subroutine's volume and
// track 1's pan reach the second sound too, through its call and its track 2. The second sound's track 1 reaches the
// second sound alone. An offset where no command starts reaches nothing, and no other sound plays the fourth sound's
// file. A sound past the sound table has no reach.
void TestSoundsReaching()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const std::vector<std::optional<SoundPlace>> places = PlacesOf(file);

    const std::vector<std::vector<uint32_t>> first =
        SoundsReaching(file, places, kFirst, std::vector<uint32_t>{7, 52, 24, 48, 8});
    const std::vector<std::vector<uint32_t>> other = SoundsReaching(file, places, kOther, std::vector<uint32_t>{0});
    const bool refused = !xsflib::testing::ErrorOf<std::invalid_argument>(
                              [&] { SoundsReaching(file, places, 4, std::vector<uint32_t>{7}); })
                              .empty();

    XSFLIB_CHECK(
        (first == std::vector<std::vector<uint32_t>>{{kSame}, {kLater, kSame}, {kLater, kSame}, {kLater}, {}}));
    XSFLIB_CHECK((other == std::vector<std::vector<uint32_t>>{{}}));
    XSFLIB_CHECK(refused);
}

// A GBA ROM of three songs. Songs 0 and 2 name one header with two tracks: track 0 sets its volume and plays a note,
// and track 1 plays a note. Song 1 has a track of its own.
std::vector<uint8_t> GbaRom()
{
    namespace mp2k_cart = ntdlib::testing::mp2k_cart;
    using enum ntdlib::mp2k::Command;

    mp2k_cart::Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, mp2k_cart::Cart::Voice(0, cart.Wave(mp2k_cart::Saw(), 13379, 0))}});
    mp2k_cart::Track first;
    first.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Note(24, 60, 100).Wait(24).Fine();
    mp2k_cart::Track second;
    second.Cmd(kCmdVoice, 0).Note(24, 64, 100).Wait(24).Fine();
    mp2k_cart::Track other;
    other.Cmd(kCmdVoice, 0).Cmd(kCmdVol, 80).Note(24, 67, 100).Wait(24).Fine();
    const uint32_t shared = cart.Song({first, second}, voices);
    cart.Song({other}, voices);
    cart.Put32(mp2k_cart::kSongTable + 16, shared);

    const ntdlib::agb::Rom rom = cart.ToRom();
    return hokuto::testing::RomBytes(rom);
}

// The fields of a song's header start its tracks, and no command opens them. Each of two songs that name one header
// reaches the other's commands, and the openings of its tracks. Song 1 reaches neither.
void TestGbaSongs()
{
    const std::vector<uint8_t> bytes = GbaRom();
    const File file = hokuto::format::ImportGbaRom(bytes);
    const std::shared_ptr<const hokuto::song::GbaRom> rom = hokuto::song::LoadGbaRom(bytes);
    std::vector<std::optional<uint32_t>> sequences;
    for (int song = 0; song < 3; song++)
    {
        sequences.push_back(hokuto::format::GbaSongSequence(file, rom->info.song_table, song));
    }
    const std::vector<std::optional<SoundPlace>> places = hokuto::edit::GbaSoundPlaces(
        sequences, hokuto::format::GbaTrackFields(file, rom->info.song_table, sequences.size()));

    const std::vector<TrackReach> shared = ReachOfTracks(file, places, 0, hokuto::song::ReadSequence(*rom, 0));
    const std::vector<TrackReach> other = ReachOfTracks(file, places, 1, hokuto::song::ReadSequence(*rom, 1));

    XSFLIB_CHECK(sequences[0] && sequences[0] == sequences[2] && sequences[1] != sequences[0]);
    XSFLIB_CHECK(places[0]->track_fields.size() == 2 && places[0]->track_fields == places[2]->track_fields);
    XSFLIB_CHECK(shared.size() == 2 && other.size() == 1);
    XSFLIB_CHECK(Holds(shared.at(0).mix[static_cast<std::size_t>(Mix::kVolume)], {}, {2}));
    XSFLIB_CHECK(Holds(shared.at(1).mix[static_cast<std::size_t>(Mix::kVolume)], {}, {2}));
    XSFLIB_CHECK(Holds(shared.at(1).mix[static_cast<std::size_t>(Mix::kPan)], {}, {2}));
    XSFLIB_CHECK(Holds(other.at(0).mix[static_cast<std::size_t>(Mix::kVolume)], {}, {}));
}

// The DS archive.
std::vector<uint8_t> DsArchive()
{
    namespace sdat_files = ntdlib::testing::sdat_files;
    const sdat_files::Bytes shared = {0xFE, 0x03, 0x00, 0x93, 0x01, 0x0E, 0x00, 0x00, 0xC1, 0x64, 0x3C,
                                      0x7F, 0x30, 0xFF, 0xC1, 0x40, 0x80, 0x18, 0x43, 0x50, 0x18, 0xFF};
    const sdat_files::Bytes other = {0xC1, 0x64, 0x3C, 0x7F, 0x30, 0xFF};
    const sdat_files::Wave square{
        .loop = 1,
        .loop_length = 4,
        .data = {0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0}};
    constexpr uint16_t kNone = 0xFFFF;

    ntdlib::sdat::ArchiveModel model;
    model.sequences = {ntdlib::sdat::SequenceInfo{0, 0, 127, 64, 64, 0, 0, ""},
                       ntdlib::sdat::SequenceInfo{0, 0, 127, 64, 64, 0, 0, ""},
                       ntdlib::sdat::SequenceInfo{1, 0, 127, 64, 64, 0, 0, ""}};
    model.banks = {ntdlib::sdat::BankInfo{2, {0, kNone, kNone, kNone}, ""}};
    model.wave_archives = {ntdlib::sdat::WaveArchiveInfo{3, ""}};
    model.players = {ntdlib::sdat::PlayerInfo{1, 0, 0, 0, ""}};
    model.files = {sdat_files::Sequence(shared), sdat_files::Sequence(other),
                   sdat_files::Bank({{.type = 1, .region = {.wave = 0}}}), sdat_files::WaveArchive({square})};

    return ntdlib::sdat::BuildArchive(model);
}

// On the DS, the allocation of tracks runs before track 0, and track 0 opens at the command after it. The other sound
// of the file starts at the allocation too. A new command at track 0's opening therefore reaches that sound. So does
// the volume of either track. Sound 2 plays another file, and nothing reaches it.
void TestDsAllocation()
{
    const File file = hokuto::format::ImportDsArchive(DsArchive());
    const std::vector<uint8_t> built = hokuto::format::ExportArchive(file);
    const std::shared_ptr<const hokuto::song::DsArchive> archive = hokuto::song::LoadDsArchive(built);
    const std::vector<std::optional<SoundPlace>> places =
        hokuto::edit::DsSoundPlaces(hokuto::format::DsSoundSequences(file, built));

    const std::vector<TrackReach> reach = ReachOfTracks(file, places, 0, hokuto::song::ReadSequence(*archive, 0));

    XSFLIB_CHECK(places.size() == 3 && places[0] && places[1] && places[0]->sequence == places[1]->sequence);
    XSFLIB_CHECK(reach.size() == 2);
    XSFLIB_CHECK(Holds(reach.at(0).mix[static_cast<std::size_t>(Mix::kPan)], {}, {1}));
    XSFLIB_CHECK(Holds(reach.at(0).mix[static_cast<std::size_t>(Mix::kVolume)], {}, {1}));
    XSFLIB_CHECK(Holds(reach.at(1).mix[static_cast<std::size_t>(Mix::kPan)], {}, {1}));
    XSFLIB_CHECK(Holds(reach.at(1).mix[static_cast<std::size_t>(Mix::kVolume)], {}, {1}));
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("reach",
                                    []
                                    {
                                        TestFirstTrack();
                                        TestOpenedTrack();
                                        TestLaterStart();
                                        TestSoundsReaching();
                                        TestGbaSongs();
                                        TestDsAllocation();
                                    });
}
