// SPDX-License-Identifier: MIT

// Tests the replacements of DSE's sounds, banks and waves from files (edit/dse_files.h). ntdlib's builders make a small
// cartridge image: a main bank that contains the samples, two songs whose banks contain the same bytes, and a bank of
// sound effects. The bank of a song is ntdlib's test bank, of id 0x7F00. Its waves 0 to 2 lie in the main bank.
//
// The expected values come from DSE's formats and the files' bytes, worked out by hand. A song file's settings give
// the ticks of a quarter note at 0x52 and the counts of tracks and channels at 0x56 and 0x57. The tools write a split's
// tuning as 3072 log2(rate / 32728). A wave at half its rate therefore has a tuning 3072 lower. ntdlib's performer
// plays each replaced sound, and the arrangement has to show the new notes on the new programs.

#include "edit/dse_files.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "ntdlib/dse/formats.h"
#include "ntdlib/dse/layout.h"
#include "ntdlib/testing/dse_song_files.h"
#include "ntdlib/testing/nds_files.h"
#include "seqlib/midi/builder.h"
#include "seqlib/wav.h"
#include "xsflib/testing/check.h"

#include "edit/files.h"
#include "format/archive.h"
#include "song/dse.h"
#include "song/sequence.h"

namespace
{

namespace files = ntdlib::testing::dse_song_files;
namespace nds_files = ntdlib::testing::nds_files;
using files::Bytes;
using files::Join;
using files::Note;
using hokuto::format::File;

constexpr uint32_t kSong = 0;
constexpr uint32_t kOtherSong = 1;
constexpr uint32_t kEffect = 2;

// The image: the main bank, two songs whose banks are the same bytes, and a bank of one sound effect.
std::vector<uint8_t> Image()
{
    const Bytes effect = Join({{0, 0, 0, 0, 0xA9, 0x7F, 0xAA, 0x00}, Note(0x60, 0, 0, 0x18, 1), {0x92, 0x18, 0x98}});
    nds_files::Image image;
    image.files = {{"SOUND/BGM/bgm.swd", files::MainBankFile()},
                   {"SOUND/BGM/bgm0001.smd", files::SongFile()},
                   {"SOUND/BGM/bgm0001.swd", files::BankFile()},
                   {"SOUND/BGM/bgm0002.smd", files::SongFile()},
                   {"SOUND/BGM/bgm0002.swd", files::BankFile()},
                   {"SOUND/SE/se.sed", files::EffectsFile({files::Effect{.tracks = {effect}}}, 0x0101)}};

    return nds_files::BuildImage(image).first;
}

std::shared_ptr<const hokuto::song::DseSet> SetOf(const File& file)
{
    return hokuto::song::LoadDseSet(hokuto::format::ExportArchive(file), ntdlib::dse::Build::kLater);
}

// The file of `path` in the set that `file` builds.
std::vector<uint8_t> FileAt(const File& file, const std::string& path)
{
    const std::vector<uint8_t> set = hokuto::format::ExportArchive(file);
    for (const ntdlib::dse::NamedFile& named : ntdlib::dse::ReadSet(set))
    {
        if (named.path == path)
        {
            return {named.bytes.begin(), named.bytes.end()};
        }
    }

    return {};
}

// The place in the set's banks of the first bank of id `id`.
uint32_t BankNamed(const hokuto::song::DseSet& set, uint16_t id)
{
    for (uint32_t bank = 0; bank < set.banks.size(); bank++)
    {
        if (hokuto::song::DseBankId(*set.banks[bank].bank) == id)
        {
            return bank;
        }
    }

    return 0;
}

std::string RefusalOf(const std::function<void()>& edit)
{
    return xsflib::testing::ErrorOf<std::invalid_argument>([&] { edit(); });
}

// A song file of 96 ticks a quarter note: a conductor, and a track on channel 3 that selects bank 0x1234, program 1
// and plays keys 60 and 64.
std::vector<uint8_t> OtherSong()
{
    std::vector<uint8_t> song = files::SongFile({{0, 0, 0, 0, 0xA4, 0x78, 0x98},
                                                 Join({{1, 3, 0, 0},
                                                       {0xA8, 0x12, 0x34, 0xAC, 0x01, 0xE0, 0x64},
                                                       Note(0x50, 0, 1, 0x30, 1),
                                                       {0x92, 0x30},
                                                       Note(0x60, 4, 0, 0x30, 1),
                                                       {0x92, 0x30, 0x98}})});
    song[0x52] = 96;

    return song;
}

// A song file's tracks take a song's place. The bank select gets the bank of the song, and the settings get the new
// ticks of a quarter note and the new counts. Tracks that select the song's bank keep their other selects. The other
// song keeps its tracks. A program map sets another program for the notes. The set comes back from a new import.
void TestSong()
{
    const File file = hokuto::format::ImportDseImage(Image());
    const std::shared_ptr<const hokuto::song::DseSet> set = SetOf(file);
    const hokuto::edit::DseIncomingSong incoming = hokuto::edit::ReadIncomingDseSong(OtherSong());
    XSFLIB_CHECK(incoming.ticks_per_quarter == 96);
    XSFLIB_CHECK((hokuto::edit::DseSongPrograms(incoming) == std::vector<uint32_t>{1}));
    XSFLIB_CHECK(hokuto::edit::DseSongTracks(incoming) == 2);
    const std::vector<std::string> warnings = hokuto::edit::DseSoundWarnings(*set, kSong, incoming);
    XSFLIB_CHECK(warnings.size() == 1 && warnings[0].find("banks that this sound doesn't play") != std::string::npos);

    const File replaced = hokuto::edit::ReplaceDseSound(file, *set, kSong, incoming);

    const std::vector<uint8_t> song = FileAt(replaced, "SOUND/BGM/bgm0001.smd");
    SEQLIB_REQUIRE(song.size() > 0x80);
    XSFLIB_CHECK(song[0x52] == 96 && song[0x53] == 0 && song[0x56] == 2 && song[0x57] == 16);
    const std::shared_ptr<const hokuto::song::DseSet> after = SetOf(replaced);
    const hokuto::song::Sequence played = hokuto::song::ReadSequence(*after, kSong);
    XSFLIB_CHECK(played.timebase == 96);
    SEQLIB_REQUIRE(played.tracks.size() == 2);
    const hokuto::song::Track& track = played.tracks[1];
    SEQLIB_REQUIRE(track.notes.size() == 2);
    XSFLIB_CHECK(track.notes[0].key == 60 && track.notes[0].start == 0 && track.notes[0].length == 0x30);
    XSFLIB_CHECK(track.notes[1].key == 64 && track.notes[1].start == 0x30);
    XSFLIB_CHECK(track.program == 1 && track.unstarted.empty());
    XSFLIB_CHECK(hokuto::song::ReadSequence(*after, kOtherSong).tracks.size() == 5);
    XSFLIB_CHECK(!replaced.sequences.at(0)->labels.empty() &&
                 replaced.sequences.at(0)->labels.front().name == file.sequences.at(0)->labels.front().name);
    const std::vector<uint8_t> exported = hokuto::format::ExportArchive(replaced);
    XSFLIB_CHECK(hokuto::format::ExportArchive(hokuto::format::ImportDseSet(exported)) == exported);

    // A song file that declares more tracks than it contains keeps its count.
    std::vector<uint8_t> counted = OtherSong();
    counted[0x56] = 3;
    const std::vector<uint8_t> counted_song =
        FileAt(hokuto::edit::ReplaceDseSound(file, *set, kSong, hokuto::edit::ReadIncomingDseSong(counted)),
               "SOUND/BGM/bgm0001.smd");
    XSFLIB_CHECK(counted_song.size() > 0x57 && counted_song[0x56] == 3);

    // Tracks that select the sound's bank keep a select of another bank. A game's tracks can hold one.
    std::vector<uint8_t> both = files::SongFile({{0, 0, 0, 0, 0xA4, 0x78, 0x98},
                                                 Join({{1, 3, 0, 0},
                                                       {0xA8, 0x7F, 0x00, 0xAC, 0x01, 0xE0, 0x64},
                                                       Note(0x50, 0, 1, 0x30, 1),
                                                       {0x92, 0x30, 0xA8, 0x12, 0x34},
                                                       Note(0x60, 4, 0, 0x30, 1),
                                                       {0x92, 0x30, 0x98}})});
    const hokuto::edit::DseIncomingSong kept = hokuto::edit::ReadIncomingDseSong(both);
    const std::vector<std::string> kept_warnings = hokuto::edit::DseSoundWarnings(*set, kSong, kept);
    XSFLIB_CHECK(kept_warnings.size() == 1 &&
                 kept_warnings[0].find("select banks that this sound doesn't load") != std::string::npos);
    const std::vector<uint8_t> kept_song =
        FileAt(hokuto::edit::ReplaceDseSound(file, *set, kSong, kept), "SOUND/BGM/bgm0001.smd");
    const std::vector<uint8_t> select = {0xA8, 0x12, 0x34};
    XSFLIB_CHECK(std::ranges::search(kept_song, select).begin() != kept_song.end());

    const File mapped = hokuto::edit::ReplaceDseSound(file, *set, kSong, incoming, {{hokuto::edit::Program{0, 1}, 3}});
    const hokuto::song::Sequence remapped = hokuto::song::ReadSequence(*SetOf(mapped), kSong);
    SEQLIB_REQUIRE(remapped.tracks.size() == 2);
    XSFLIB_CHECK(remapped.tracks[1].program == 3);
}

// A sound effect has at most 4 tracks.
void TestEffectTracks()
{
    const File file = hokuto::format::ImportDseImage(Image());
    const std::shared_ptr<const hokuto::song::DseSet> set = SetOf(file);
    std::vector<Bytes> tracks;
    for (uint8_t track = 0; track < 5; track++)
    {
        tracks.push_back({track, track, 0, 0, 0x92, 0x10, 0x98});
    }
    const hokuto::edit::DseIncomingSong incoming = hokuto::edit::ReadIncomingDseSong(files::SongFile(tracks));

    const std::string refusal = RefusalOf([&] { hokuto::edit::ReplaceDseSound(file, *set, kEffect, incoming); });

    XSFLIB_CHECK(refusal == "se_000 is a sound effect, and plays at most 4 tracks. The file has 5.");
    XSFLIB_CHECK(RefusalOf([] { hokuto::edit::ReadIncomingDseSong(files::MainBankFile()); })
                     .starts_with("Can't read the file as a DSE song (smdl)"));
}

// A MIDI file's channels become tracks. Each selects the sound's bank, and its notes play at their ticks with their
// program. A loop marker sets the loop point.
void TestMidi()
{
    seqlib::midi::FileBuilder midi(48);
    seqlib::midi::TrackBuilder& track = midi.AddTrack();
    track.Tempo(0, 500000);
    track.Meta(0, 0x06, "loopStart");
    track.Program(0, 0, 1);
    track.Control(0, 0, 7, 100);
    track.NoteOn(0, 0, 60, 100);
    track.NoteOff(48, 0, 60);
    track.NoteOn(48, 0, 64, 90);
    track.NoteOff(96, 0, 64);
    track.Meta(192, 0x06, "loopEnd");
    const File file = hokuto::format::ImportDseImage(Image());
    const std::shared_ptr<const hokuto::song::DseSet> set = SetOf(file);

    const hokuto::edit::DseIncomingSong incoming = hokuto::edit::DseSongOfMidi(midi.Encode(), "song.mid");
    const File replaced = hokuto::edit::ReplaceDseSound(file, *set, kSong, incoming);

    const hokuto::song::Sequence played = hokuto::song::ReadSequence(*SetOf(replaced), kSong);
    XSFLIB_CHECK(played.timebase == 48);
    XSFLIB_CHECK(played.loop && played.loop->start == 0 && played.loop->end == 192);
    std::vector<const hokuto::song::Note*> notes;
    for (const hokuto::song::Track& each : played.tracks)
    {
        for (const hokuto::song::Note& note : each.notes)
        {
            notes.push_back(&note);
        }
        if (!each.notes.empty())
        {
            XSFLIB_CHECK(each.program == 1 && each.unstarted.empty());
        }
    }
    SEQLIB_REQUIRE(notes.size() == 2);
    XSFLIB_CHECK(notes[0]->key == 60 && notes[0]->start == 0 && notes[0]->length == 48 && notes[0]->velocity == 100);
    XSFLIB_CHECK(notes[1]->key == 64 && notes[1]->start == 48 && notes[1]->length == 48 && notes[1]->velocity == 90);
}

// The tempo command of DSE stores a whole number of beats a minute. A MIDI tempo of 130.0001 BPM therefore causes the
// warning about whole tempos, and the song plays at about 130 BPM. The warning names DSE. A MIDI tempo of exactly 100
// BPM causes none.
void TestMidiTempos()
{
    const auto midi_at = [](uint32_t micros)
    {
        seqlib::midi::FileBuilder midi(48);
        seqlib::midi::TrackBuilder& track = midi.AddTrack();
        track.Tempo(0, micros);
        track.NoteOn(0, 0, 60, 100);
        track.NoteOff(192, 0, 60);
        return midi.Encode();
    };
    const File file = hokuto::format::ImportDseImage(Image());
    const std::shared_ptr<const hokuto::song::DseSet> set = SetOf(file);
    const auto whole_tempos = [](const std::string& warning)
    {
        return warning.find("tempos must be whole BPM values") != std::string::npos;
    };

    const hokuto::edit::DseIncomingSong uneven = hokuto::edit::DseSongOfMidi(midi_at(461'538), "song.mid");
    const hokuto::edit::DseIncomingSong whole = hokuto::edit::DseSongOfMidi(midi_at(600'000), "song.mid");
    const File replaced = hokuto::edit::ReplaceDseSound(file, *set, kSong, uneven);

    const auto warning = std::ranges::find_if(uneven.warnings, whole_tempos);
    XSFLIB_CHECK(warning != uneven.warnings.end() && warning->starts_with("DSE tempos must be whole BPM values."));
    XSFLIB_CHECK(std::ranges::none_of(whole.warnings, whole_tempos));
    const hokuto::song::Sequence played = hokuto::song::ReadSequence(*SetOf(replaced), kSong);
    SEQLIB_REQUIRE(!played.tempo.empty());
    XSFLIB_CHECK(std::abs(played.tempo.front().bpm - 130.0) < 1.0);
}

// A bank file replaces the bank in each file that contains it. The new bank keeps the id and the name, and its waves
// stay in the main bank. The warnings name a program that the songs use and that is missing from the new bank.
void TestBank()
{
    const File file = hokuto::format::ImportDseImage(Image());
    const std::shared_ptr<const hokuto::song::DseSet> set = SetOf(file);
    const uint32_t bank = BankNamed(*set, files::kBankId);
    ntdlib::dse::WaveBank incoming = ntdlib::dse::WaveBank::Parse(files::BankFile());
    incoming.header[0x0E] = 0x34;
    incoming.header[0x0F] = 0x12;
    incoming.header[0x20] = 'N';
    incoming.instruments->programs[2].reset();

    const std::vector<std::string> warnings = hokuto::edit::DseBankWarnings(*set, bank, incoming);
    const File replaced = hokuto::edit::ReplaceDseBank(file, *set, bank, incoming);

    XSFLIB_CHECK(!warnings.empty() && warnings[0] == "Programs missing from the new bank: 2. Their notes are silent.");
    XSFLIB_CHECK(std::ranges::find(warnings,
                                   "The new bank holds no samples. Its splits play the waves of the main bank "
                                   "B_TEST with the same numbers.") != warnings.end());
    for (const char* path : {"SOUND/BGM/bgm0001.swd", "SOUND/BGM/bgm0002.swd"})
    {
        const ntdlib::dse::WaveBank written = ntdlib::dse::WaveBank::Parse(FileAt(replaced, path));
        XSFLIB_CHECK(written.header[0x0E] == 0x00 && written.header[0x0F] == 0x7F && written.header[0x20] == 'B');
        XSFLIB_CHECK(written.instruments && !written.instruments->programs[2] && written.instruments->programs[1]);
    }
    const hokuto::song::Sequence played = hokuto::song::ReadSequence(*SetOf(replaced), kOtherSong);
    std::size_t notes = 0;
    for (const hokuto::song::Track& track : played.tracks)
    {
        for (const hokuto::song::Note& note : track.notes)
        {
            XSFLIB_CHECK(note.program != 2);
            ++notes;
        }
    }
    XSFLIB_CHECK(notes > 0);
    XSFLIB_CHECK(RefusalOf([] { hokuto::edit::ReadIncomingDseBank(files::SongFile()); })
                     .starts_with("Can't read the file as a DSE wave bank (swdl)"));
}

// A WAV file replaces a wave in the main bank. A wave that fits stays at its place, and a larger one moves after the
// samples. The bank of the song gets the new values, and the split that plays the wave moves its tuning by the rate.
// The current wave's values come from its parameters.
void TestWave()
{
    const File file = hokuto::format::ImportDseImage(Image());
    const std::shared_ptr<const hokuto::song::DseSet> set = SetOf(file);
    const uint32_t main = BankNamed(*set, 0);
    const auto wav_of = [](std::size_t samples, uint32_t rate)
    {
        seqlib::Pcm pcm;
        pcm.channels.resize(1);
        for (std::size_t i = 0; i < samples; i++)
        {
            pcm.channels[0].push_back(static_cast<int16_t>(i * 100));
        }

        return seqlib::WavFile(pcm, rate);
    };
    const auto incoming_of = [&](std::size_t samples)
    {
        const hokuto::edit::IncomingWave read =
            hokuto::edit::ReadIncomingDsWave(wav_of(samples, 16000), ntdlib::sar::WaveEncoding::kPcm16);
        return hokuto::edit::DseIncomingWave{read.bytes, read.rate};
    };

    const File small = hokuto::edit::ReplaceDseWave(file, *set, main, 0, incoming_of(64));
    const File large = hokuto::edit::ReplaceDseWave(file, *set, main, 0, incoming_of(300));

    for (const File* edited : {&small, &large})
    {
        const std::shared_ptr<const hokuto::song::DseSet> after = SetOf(*edited);
        const ntdlib::wave::Pcm pcm = hokuto::song::DseWavePcm(*after, main, 0);
        const std::size_t samples = edited == &small ? 64 : 300;
        SEQLIB_REQUIRE(pcm.channels.size() == 1 && pcm.channels[0].size() >= samples);
        XSFLIB_CHECK(pcm.sample_rate == 16000 && !pcm.loop);
        XSFLIB_CHECK(pcm.channels[0][10] == 1000 &&
                     pcm.channels[0][samples - 1] == static_cast<int16_t>((samples - 1) * 100));
        const ntdlib::dse::WaveBank& main_bank = *after->banks[main].bank;
        XSFLIB_CHECK(main_bank.waves[0]->SamplePosition() == (edited == &small ? 0u : 0x400u));
        XSFLIB_CHECK(main_bank.waves[1]->SamplePosition() == 0x100);

        const ntdlib::dse::WaveBank& song_bank = *after->banks[BankNamed(*after, files::kBankId)].bank;
        XSFLIB_CHECK(song_bank.waves[0]->SampleRate() == 16000 && !song_bank.waves[0]->Loops());
        const auto& split = song_bank.instruments->programs[0]->splits[0];
        XSFLIB_CHECK(static_cast<int16_t>(split[0x14] | split[0x15] << 8) == -1742 - 3072);
    }
    XSFLIB_CHECK(hokuto::edit::DseWaveUses(*set, main, 0) == std::vector<std::string>{"B_TEST: 1 split"});

    // The main bank's wave 0 is 16-bit PCM at 32,000 Hz, with a loop from word 4 for 60 words: two samples a word.
    const hokuto::edit::WaveChanges changes = hokuto::edit::ChangesOfDseWave(*set, main, 0);
    XSFLIB_CHECK(changes.encoding == ntdlib::sar::WaveEncoding::kPcm16 && changes.rate == 32000 && changes.loop);
    XSFLIB_CHECK(changes.loop_start == 8 && changes.loop_end == 128 && changes.size == 256);
    XSFLIB_CHECK(changes.banks == std::vector<uint32_t>{BankNamed(*set, files::kBankId)});

    // The size of the current wave counts its samples alone, and so does the size of a new wave: 300 samples of 16-bit
    // PCM fill 150 words. The DS's wave bytes hold 12 bytes of parameters before them.
    const hokuto::edit::DseIncomingWave new_wave = incoming_of(300);
    XSFLIB_CHECK(hokuto::edit::DseWaveSize(new_wave.wave) == 600 && new_wave.wave.size() == 612);

    XSFLIB_CHECK(RefusalOf([&] { hokuto::edit::ReplaceDseWave(file, *set, main, 3, incoming_of(8)); }) ==
                 "The image has no B_TEST, wave 3.");
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("dse_files",
                                    []
                                    {
                                        TestSong();
                                        TestEffectTracks();
                                        TestMidi();
                                        TestMidiTempos();
                                        TestBank();
                                        TestWave();
                                    });
}
