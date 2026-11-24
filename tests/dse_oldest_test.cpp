// SPDX-License-Identifier: MIT

// Tests DSE's files of version 0x402 in Hokuto (song/dse.h). ntdlib reads them as files of version 0x415
// (ntdlib::dse::Upgraded), and its cut makes a song's tracks a sequence. Hokuto opens, plays, exports and edits them,
// and writes each file in its version.
//
// A small cartridge image holds a song of version 0x402 with its banks of version 0x415, and a wave bank of version
// 0x402 in another folder. The files are laid out as ntdlib/dse/oldest.h describes them, from the places that the
// driver of Luminous Arc reads. The song has to play and convert as ntdlib's dse::Player and converter give it. The
// file of the image has to export the image byte for byte, and come back through a save and a load. The browser's
// Export… has to give the song and the bank in version 0x402, before an edit and after one. An edit of a note, a new
// track, a removed track and Replace… of the song have to keep the song's version: its 16 bytes of settings give the
// new counts and ticks at their places. The edits of the bank's splits and waves, and Replace… of the bank and of a
// wave, have to write the values at the places of version 0x402 and keep every other byte. A rate that version 0x402
// can't hold has to be refused with a message that gives the limit.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "ntdlib/dse/driver.h"
#include "ntdlib/dse/formats.h"
#include "ntdlib/dse/layout.h"
#include "ntdlib/dse/oldest.h"
#include "ntdlib/sar/encodings.h"
#include "ntdlib/sequence_export/dse/convert.h"
#include "ntdlib/testing/dse_song_files.h"
#include "ntdlib/testing/nds_files.h"
#include "seqlib/wav.h"
#include "xsflib/binary.h"
#include "xsflib/testing/check.h"

#include "dse_playing.h"
#include "edit/dse_banks.h"
#include "edit/dse_files.h"
#include "edit/files.h"
#include "edit/notes.h"
#include "edit/replace.h"
#include "edit/tracks.h"
#include "format/archive.h"
#include "format/file.h"
#include "song/dse.h"
#include "song/midi_export.h"
#include "song/sequence.h"

namespace
{

namespace files = ntdlib::testing::dse_song_files;
namespace dse_export = ntdlib::sequence_export::dse;
using files::Builder;
using files::Bytes;
using hokuto::format::File;
using hokuto::song::DseSet;
using xsflib::testing::Check;
using xsflib::testing::ErrorOf;

constexpr int kTick = ntdlib::dse::Driver::kTickMicroseconds;

// A header of version 0x402: the magic word, a size that the caller fills, the version, the bank's id, 8 zeros, a date
// and a name with the tools' bytes after its zero.
void OldHeaderStart(Builder& b, const char* magic, uint16_t id)
{
    b.Text(magic).U32(0).U32(0).U16(0x0402).U16(id).Fill(8, 0);
    b.Append({0xD6, 0x07, 0x09, 0x1A, 0x12, 0x11, 0x1A, 0x05});
    b.Text("B_OLD").Fill(1, 0).Fill(10, 0xAA);
}

// A bank's chunk header of version 0x402: the label, 8 bytes that give the version and an alignment of 16, the size.
void OldBankChunk(Builder& b, const char* label, uint32_t size)
{
    b.Text(label).Append({0x00, 0x00, 0x02, 0x04, 0x10, 0x00, 0x00, 0x00}).U32(size);
}

// A song of version 0x402 with `tracks`, each padded to 4 bytes. Its 16 bytes of settings give 48 ticks to a quarter
// note, the number of tracks, 16 channels, byte 6 of 1, the global volume's index 2 in the high bits of byte 7, and a
// priority of 8.
Bytes OldSongFile(const std::vector<Bytes>& tracks)
{
    Builder b;
    OldHeaderStart(b, "smdl", files::kBankId);
    b.U32(1).U32(1).U32(0xFFFFFFFF).U32(0xFFFFFFFF);
    b.Text("song").Append({0x00, 0x00, 0x00, 0x01, 0x10, 0xFF, 0x00, 0x00, 0xB0, 0xFF, 0xFF, 0xFF});
    b.Append({0x01, 0x00, 0x30, 0x00, static_cast<uint8_t>(tracks.size()), 0x10, 0x01, 0x20, 0x08, 0x00, 0x7F, 0x40,
              0x00, 0x00, 0x00, 0x0F});
    for (const Bytes& track : tracks)
    {
        files::TrackChunk(b, "trk ", static_cast<uint32_t>(track.size())).Append(track).Align(4, 0x98);
    }
    files::TrackChunk(b, "eoc ", 0);
    b.Put32(0x08, static_cast<uint32_t>(b.Size()));
    return b.Take();
}

// A song of a conductor at a tempo of 120 and a track that plays note 48 for 48 ticks, then note 55 for 48.
std::vector<Bytes> TestTracks()
{
    const Bytes conductor = {0, 0, 0, 0, 0xA4, 0x78, 0x98};
    const Bytes track = files::Join({{1, 1, 0, 0},
                                     {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x7F},
                                     files::Note(0x7F, 0, 0, 0x30, 1),
                                     {0x92, 0x30},
                                     files::Note(0x60, 7, 0, 0x30, 1),
                                     {0x92, 0x30, 0x98}});
    return {conductor, track};
}

// A wave bank of version 0x402 with one wave of 16 bytes of IMA-ADPCM at 32,728 Hz, and program 1 with one split over
// every key that plays it. The program's header gives its number and its count of splits, 16 entries of 8 bytes that
// the driver doesn't read and 4 LFOs follow, then the split.
Bytes OldBankFile()
{
    Builder b;
    OldHeaderStart(b, "swdl", 0x0102);
    b.Append({0x00, 0xFF, 0xFF, 0xFF}).Fill(8, 0).U32(0x10);
    b.U32(16).U16(0).Append({1, 2, 1}).Fill(7, 0xFF);

    OldBankChunk(b, "wavi", 16 + 0x40);
    b.U16(16).Fill(14, 0);
    b.Append({0x01, 0x00, 0x00, 0x00, 0x3C, 0x00, 0x7F, 0x40, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    b.Append({0x02, 0x00}).U16(32728).U32(0).U32(0).U32(4).Fill(16, 0);
    b.Append({0x01, 0x01, 0x01, 0x03, 0x03, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x7F, 0x00, 0x7F, 0x28, 0xFF});

    OldBankChunk(b, "prgi", 16 + 0x10 + 0x80 + 0x40 + 0x30);
    b.U16(0).U16(16).Fill(12, 0);
    b.Append({0x01, 0x01, 0x00, 0x00, 0x7F, 0x40, 0x00, 0x0F, 0x00, 0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00});
    for (int i = 0; i < 16; i++)
    {
        b.Append({0x00, 0x00, 0xFF, 0x08, 0x00, 0x0F, 0x00, 0x00});
    }
    b.Fill(0x40, 0);
    b.Append({0x00, 0x00, 0x02, 0x01, 0x00, 0x7F, 0x00, 0x7F, 0x00, 0x7F, 0x00, 0x7F, 0x00, 0x00, 0x00, 0x00});
    b.Append({0x00, 0x00}).U16(0);
    b.Append({60, 0x00, 0x7F, 0x40, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    b.Append({0x01, 0x01, 0x01, 0x03, 0x03, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x7F, 0x45, 0x5F, 0x37, 0xFF});

    OldBankChunk(b, "kgrp", 8);
    b.Append({0x00, 0x00, 0xFF, 0x08, 0x00, 0xFF, 0x00, 0x00});
    b.Fill(8, 0);
    OldBankChunk(b, "pcmd", 16);
    b.Append({0x00, 0x00, 0x00, 0x00, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77});
    OldBankChunk(b, "eod ", 0);
    b.Put32(0x08, static_cast<uint32_t>(b.Size()));
    return b.Take();
}

// The image: the main bank, the song of version 0x402 and its bank in SOUND/BGM/, and the bank of version 0x402 in
// SOUND/SE/.
std::vector<uint8_t> TestImage()
{
    ntdlib::testing::nds_files::Image image;
    image.files = {
        {"SOUND/BGM/bgm.swd", files::MainBankFile()},
        {"SOUND/BGM/bgm0001.smd", OldSongFile(TestTracks())},
        {"SOUND/BGM/bgm0001.swd", files::BankFile()},
        {"SOUND/SE/se.swd", OldBankFile()},
    };

    return ntdlib::testing::nds_files::BuildImage(image).first;
}

// The index of the bank of version 0x402 among the set's banks, or a number past them.
uint32_t OldBank(const DseSet& set)
{
    const auto oldest = [](const hokuto::song::DseBank& bank)
    {
        return bank.oldest != nullptr;
    };
    return static_cast<uint32_t>(std::ranges::find_if(set.banks, oldest) - set.banks.begin());
}

// Whether `bank` holds program 1.
bool HasProgram(const ntdlib::dse::WaveBank& bank)
{
    return bank.instruments && bank.instruments->programs.size() > 1 && bank.instruments->programs[1].has_value();
}

std::shared_ptr<const DseSet> SetOf(const File& file)
{
    return hokuto::song::LoadDseSet(hokuto::format::ExportArchive(file), ntdlib::dse::Build::kLater, {});
}

// The bytes of the file at `path` in the set that `file` builds.
Bytes FileAt(const File& file, const std::string& path)
{
    const std::vector<uint8_t> set = hokuto::format::ExportArchive(file);
    for (const ntdlib::dse::NamedFile& each : ntdlib::dse::ReadSet(set))
    {
        if (each.path == path)
        {
            return {each.bytes.begin(), each.bytes.end()};
        }
    }

    return {};
}

// The places of the values in OldBankFile: the wave's rate, and the split's tuning and volume.
constexpr std::size_t kWave = 0x70;
constexpr std::size_t kSplit = 0xD0 + 0x10 + 0x80 + 0x40;
constexpr std::size_t kRate = kWave + 0x12;
constexpr std::size_t kTuning = kSplit + 0x12;
constexpr std::size_t kVolume = kSplit + 0x16;

// The places at which `a` and `b` differ, or the size of the longer one where their sizes differ.
std::vector<std::size_t> Differences(const Bytes& a, const Bytes& b)
{
    if (a.size() != b.size())
    {
        return {std::max(a.size(), b.size())};
    }

    std::vector<std::size_t> places;
    for (std::size_t i = 0; i < a.size(); i++)
    {
        if (a[i] != b[i])
        {
            places.push_back(i);
        }
    }

    return places;
}

// The image opens without a note on the version. The bank of version 0x402 keeps its file's bytes, and the others
// don't. The song has a sequence.
void TestOpen()
{
    const std::vector<uint8_t> image = TestImage();
    const std::shared_ptr<const DseSet> set = hokuto::song::LoadDseSet(image);

    Check(ntdlib::dse::IsOldestVersion(OldSongFile(TestTracks())) && ntdlib::dse::IsOldestVersion(OldBankFile()),
          "the files are of version 0x402");
    SEQLIB_REQUIRE(set->sounds.size() == 1 && set->banks.size() == 3);
    const uint32_t bank = OldBank(*set);
    const auto oldest = [](const hokuto::song::DseBank& each)
    {
        return each.oldest != nullptr;
    };
    Check(std::ranges::count_if(set->banks, oldest) == 1 && bank < set->banks.size() &&
              *set->banks[bank].oldest == OldBankFile() && HasProgram(*set->banks[bank].bank),
          "one bank is of version 0x402, with its bytes and its program");
    const std::vector<std::string> notes = hokuto::song::DseOpenNotes(image, *set);
    Check(std::ranges::none_of(notes, [](const std::string& note) { return note.find("0x402") != std::string::npos; }),
          "no note on the version");

    const File file = hokuto::format::ImportDseImage(image);
    const std::vector<std::optional<uint32_t>> sequences = hokuto::format::DseSoundSequences(
        file, hokuto::format::ExportArchive(file), hokuto::song::DseSoundRefs(*SetOf(file)));
    Check(sequences.size() == 1 && sequences[0].has_value(), "the song has a sequence");
}

// The song plays and converts as ntdlib gives it, from the image and from the file of the image.
void TestPlays()
{
    const std::vector<uint8_t> image = TestImage();
    const std::shared_ptr<const DseSet> set = hokuto::song::LoadDseSet(image);
    const std::shared_ptr<const DseSet> of_file = hokuto::edit::DseSetOfFile(hokuto::format::ImportDseImage(image));
    SEQLIB_REQUIRE(set->sounds.size() == 1 && of_file->sounds.size() == 1);

    const std::vector<int16_t> direct = hokuto::testing::DirectDse(*set, 0, kTick);
    Check(hokuto::testing::StartsDse(hokuto::testing::ModelledDse(set, 0), direct),
          "the model plays what the player plays");
    Check(hokuto::testing::StartsDse(hokuto::testing::ModelledDse(of_file, 0), direct), "from the file too");

    const dse_export::PerformOptions options{.build = set->build, .tick_microseconds = kTick};
    const std::vector<uint8_t> midi =
        dse_export::ConvertSequence(set->sounds[0].Files(), hokuto::song::DseSoundName(*set, 0), options, "Hokuto")
            .midi;
    Check(hokuto::song::ConvertToMidi(*set, 0, "Hokuto").midi == midi, "the MIDI file is the converter's");
}

// The file of the image exports the image byte for byte, and comes back through a save and a load. The browser's
// Export… gives the song and the bank of version 0x402 as their files, from the image and from the file of the image.
void TestExport()
{
    const std::vector<uint8_t> image = TestImage();
    const File file = hokuto::format::ImportDseImage(image);
    for (const std::shared_ptr<const DseSet>& set : {hokuto::song::LoadDseSet(image), SetOf(file)})
    {
        Check(hokuto::song::DseItemBytes(*set, {.kind = hokuto::song::ItemKind::kSequence, .index = 0}) ==
                  OldSongFile(TestTracks()),
              "the song exports as its file");
        Check(hokuto::song::DseItemBytes(*set, {.kind = hokuto::song::ItemKind::kBank, .index = OldBank(*set)}) ==
                  OldBankFile(),
              "the bank exports as its file");
    }

    Check(hokuto::format::ExportDsImage(file).image == image, "the image comes back byte for byte");
    Check(hokuto::format::Read(hokuto::format::Write(file)) == file, "the file comes back through a save and a load");
}

// The song's notes as ntdlib's performer plays them from the set that `file` builds: on each track, the start, the
// length, the key and the velocity of each note.
std::vector<std::vector<std::vector<uint32_t>>> NotesOf(const File& file)
{
    const hokuto::song::Sequence played = hokuto::song::ReadSequence(*SetOf(file), 0);
    std::vector<std::vector<std::vector<uint32_t>>> notes;
    for (const hokuto::song::Track& track : played.tracks)
    {
        std::vector<std::vector<uint32_t>> held;
        for (const hokuto::song::Note& note : track.notes)
        {
            held.push_back({static_cast<uint32_t>(note.start), static_cast<uint32_t>(note.length),
                            static_cast<uint32_t>(note.key), static_cast<uint32_t>(note.velocity)});
        }
        notes.push_back(held);
    }

    return notes;
}

// A softer note, a new track and its removal keep the song's version, and the 16 bytes of settings give the count of
// tracks.
void TestSongEdits()
{
    const std::string path = "SOUND/BGM/bgm0001.smd";
    const Bytes song = OldSongFile(TestTracks());
    const File file = hokuto::format::ImportDseImage(TestImage());
    const std::shared_ptr<const DseSet> set = SetOf(file);
    const std::vector<std::optional<hokuto::edit::SoundPlace>> places = hokuto::edit::DsSoundPlaces(
        hokuto::format::DseSoundSequences(file, hokuto::format::ExportArchive(file), hokuto::song::DseSoundRefs(*set)));
    SEQLIB_REQUIRE(places.size() == 1 && places[0]);
    const hokuto::song::Sequence played = hokuto::song::ReadSequence(*set, 0);
    SEQLIB_REQUIRE(played.tracks.size() == 2 && played.tracks[1].notes.size() == 2);
    Check((NotesOf(file)[1] == std::vector<std::vector<uint32_t>>{{0, 48, 48, 127}, {48, 48, 55, 96}}),
          "the song's notes");

    // A softer first note keeps the file's size, and changes only the note's byte.
    const hokuto::song::Track& track = played.tracks[1];
    hokuto::edit::NoteEdit softer{.command = track.notes[0].command,
                                  .stretch = track.stretches.at(track.notes[0].stretch)};
    softer.velocity = 90;
    const hokuto::edit::Edited edited =
        hokuto::edit::EditNotes(file, places[0]->sequence, std::vector<hokuto::edit::NoteEdit>{softer});
    const Bytes softened = FileAt(edited.file, path);
    Check(ntdlib::dse::IsOldestVersion(softened) && Differences(softened, song).size() == 1,
          "a softer note changes one byte of the song");
    Check((NotesOf(edited.file)[1] == std::vector<std::vector<uint32_t>>{{0, 48, 48, 90}, {48, 48, 55, 96}}),
          "the note plays softer");
    Check(hokuto::song::DseItemBytes(*SetOf(edited.file), {.kind = hokuto::song::ItemKind::kSequence, .index = 0}) ==
              softened,
          "the edited song exports in version 0x402");

    // A new track counts in the settings' byte 4, and its removal takes the count back.
    const hokuto::edit::TrackEdit added = hokuto::edit::AddTrack(file, places, 0, played, std::nullopt);
    const Bytes with_track = FileAt(added.file, path);
    const ntdlib::dse::SettingsPlaces settings = ntdlib::dse::SongSettingsPlaces(with_track);
    Check(ntdlib::dse::IsOldestVersion(with_track) && with_track[settings.tracks] == 3 &&
              with_track.size() > song.size() && xsflib::LoadLe32(&with_track[0x08]) == with_track.size(),
          "a new track counts in the settings, and the size follows");
    Check(std::equal(song.begin() + 0x0C, song.begin() + 0x54, with_track.begin() + 0x0C) &&
              std::equal(song.begin() + 0x55, song.begin() + 0x60, with_track.begin() + 0x55),
          "the other bytes of the header and the settings stay");
    Check(NotesOf(added.file).size() == 3, "the song plays 3 tracks");

    const File removed = hokuto::edit::RemoveTrack(
        added.file,
        hokuto::edit::DsSoundPlaces(hokuto::format::DseSoundSequences(
            added.file, hokuto::format::ExportArchive(added.file), hokuto::song::DseSoundRefs(*SetOf(added.file)))),
        0, 2);
    Check(FileAt(removed, path) == song, "the removal gives the song back");

    // Replace… with the same tracks in a song of version 0x415 at 96 ticks to a quarter note writes the ticks at their
    // place, and keeps the version.
    hokuto::edit::DseIncomingSong incoming = hokuto::edit::ReadIncomingDseSong(files::SongFile(TestTracks()));
    incoming.ticks_per_quarter = 96;
    const Bytes replaced = FileAt(hokuto::edit::ReplaceDseSound(file, *set, 0, incoming), path);
    Check(ntdlib::dse::IsOldestVersion(replaced) && xsflib::LoadLe16(&replaced[settings.ticks]) == 96,
          "Replace… writes the ticks at their place");
    Check((Differences(replaced, song) == std::vector<std::size_t>{settings.ticks}),
          "Replace… with the same tracks changes only the ticks");
}

// The edits of the bank of version 0x402 write each value at its place, and keep every other byte.
void TestBankEdits()
{
    const std::string path = "SOUND/SE/se.swd";
    const Bytes bank_file = OldBankFile();
    const std::vector<uint8_t> image = TestImage();
    const File file = hokuto::format::ImportDseImage(image);
    const std::shared_ptr<const DseSet> set = SetOf(file);
    const uint32_t bank = OldBank(*set);
    SEQLIB_REQUIRE(bank < set->banks.size() && HasProgram(*set->banks[bank].bank));

    // A split's volume.
    hokuto::edit::DseSplit split = set->banks[bank].bank->instruments->programs[1]->splits[0];
    split[hokuto::edit::kDseVolume] = 0x40;
    const File louder_file = hokuto::edit::WithDseSplit(file, *set, {.bank = bank, .program = 1, .split = 0}, split);
    const Bytes louder = FileAt(louder_file, path);
    Check((Differences(louder, bank_file) == std::vector<std::size_t>{kVolume}) && louder[kVolume] == 0x40,
          "a split's volume goes to its place");
    Check(hokuto::song::DseItemBytes(*SetOf(louder_file), {.kind = hokuto::song::ItemKind::kBank, .index = bank}) ==
              louder,
          "the edited bank exports in version 0x402");

    // A wave's rate. The tuning of the split that plays it moves too.
    const File slower = hokuto::edit::WithDseWave(file, *set, bank, 0, {.rate = 16364});
    const Bytes slower_file = FileAt(slower, path);
    Check(xsflib::LoadLe16(&slower_file[kRate]) == 16364 &&
              static_cast<int16_t>(xsflib::LoadLe16(&slower_file[kTuning])) == -3072,
          "the rate and the tuning go to their places");
    // Half the rate moves the tuning from 0 to -3072: its low byte stays 0.
    Check((Differences(slower_file, bank_file) == std::vector<std::size_t>{kRate, kRate + 1, kTuning + 1}),
          "nothing else changes");
    Check(SetOf(slower)->banks[bank].bank->waves[0]->SampleRate() == 16364, "the bank reads the new rate");
    Check(ErrorOf<std::invalid_argument>([&] { hokuto::edit::WithDseWave(file, *set, bank, 0, {.rate = 70000}); })
                  .find("65,535 Hz") != std::string::npos,
          "a rate past 65,535 Hz is refused");

    // Replace… of the bank by itself gives the image back, and of a wave puts the new samples after the bank's.
    Check(hokuto::format::ExportDsImage(hokuto::edit::ReplaceDseBank(file, *set, bank, *set->banks[bank].bank)).image ==
              image,
          "Replace… of the bank by itself gives the image back");
    seqlib::Pcm pcm;
    pcm.channels.resize(1);
    for (int i = 0; i < 64; i++)
    {
        pcm.channels[0].push_back(static_cast<int16_t>(i * 100));
    }
    const hokuto::edit::IncomingWave read =
        hokuto::edit::ReadIncomingDsWave(seqlib::WavFile(pcm, 32728), ntdlib::sar::WaveEncoding::kPcm16);
    const File new_wave = hokuto::edit::ReplaceDseWave(file, *set, bank, 0, {read.bytes, read.rate});
    const Bytes new_file = FileAt(new_wave, path);
    const ntdlib::dse::WaveBank after = ntdlib::dse::WaveBank::Parse(new_file);
    Check(ntdlib::dse::IsOldestVersion(new_file) && new_file.size() == bank_file.size() + 128 &&
              xsflib::LoadLe32(&new_file[0x08]) == new_file.size(),
          "the bank grows by the new samples, and its size follows");
    Check(after.waves[0]->Format() == 0x100 && after.waves[0]->SamplePosition() == 16 && !after.waves[0]->Loops(),
          "the wave takes the new samples");
    Check(std::equal(bank_file.begin() + 0x0C, bank_file.begin() + kWave, new_file.begin() + 0x0C),
          "the header and the table stay");
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("dse_oldest",
                                    []
                                    {
                                        TestOpen();
                                        TestPlays();
                                        TestExport();
                                        TestSongEdits();
                                        TestBankEdits();
                                    });
}
