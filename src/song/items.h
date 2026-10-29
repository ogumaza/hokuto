// SPDX-License-Identifier: MIT

// Single items of a sound archive as separate files. The browser lists the archive's sounds, banks and wave archives,
// the waves in each wave archive, and the wave sound files with their wave sounds. A sequence sound goes out as its
// sequence file, and a wave sound as its file of wave sounds. A wave sound file, a bank, a wave archive and a wave go
// out as their files. Each file holds the bytes that the archive holds. Each file also goes out as a file of another
// console among the 3DS, the Wii U and the Switch. A stream goes out as a file of another of the three too, and a
// stream of the Wii as a file of any of them. A wave and a wave sound also go out as WAV files. A stream sound's stream
// lies outside the archive, in a file beside it (song/streams.h).

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include "ntdlib/rsar/decode.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/sar/decode.h"
#include "ntdlib/sar/formats.h"

#include "song/biquads.h"

namespace hokuto::song
{

// The kinds of item that go out as files.
enum class ItemKind
{
    kSequence,      // a sequence sound's sequence file
    kWaveSound,     // a wave sound's file: the file of its set of wave sounds
    kWaveSoundFile, // a wave sound file, given by its first wave sound
    kBank,
    kWaveArchive,
    kWave,   // a wave of a wave archive
    kStream, // a stream sound's stream: a file beside the archive
};

// An item of an archive. A sound, a bank or a wave archive is given by its index among the archive's items of its kind.
// A wave is given by its wave archive's index and by its index in the wave archive.
struct Item
{
    friend bool operator==(const Item&, const Item&) = default;

    ItemKind kind = ItemKind::kSequence;
    uint32_t index = 0;
    uint32_t wave = 0;
};

// The options by which the console's sound library reads the files of `archive`. ntdlib's engines read them by the same
// rules.
ntdlib::sar::Options ItemOptions(const ntdlib::sar::SoundArchive& archive);

// The programs of bank `bank` that hold an instrument, and the number of waves in wave archive `wave_archive`. Each
// takes the item's index among the archive's items of its kind. Both give nothing when the archive lacks the item's
// file. Both throw std::runtime_error when the file can't be read.
std::vector<uint32_t> ProgramsOf(const ntdlib::sar::SoundArchive& archive, std::size_t bank);
std::size_t WaveCountOf(const ntdlib::sar::SoundArchive& archive, std::size_t wave_archive);

// The programs of bank `bank` of the Wii's `archive` that play a region at some key and velocity
// (ntdlib::rsar::Bank::Plays). Nothing when the archive lacks the bank's file. Throws xsflib::FormatError when the file
// can't be read.
std::vector<uint32_t> ProgramsOf(const ntdlib::rsar::SoundArchive& archive, std::size_t bank);

// The bytes of `item`'s file in `archive`. A wave's file is the wave file that its wave archive holds. Throws
// std::runtime_error with a message for the user when the archive lacks the item or its file, and when a wave archive
// can't be read. A file in a group file that the archive lacks takes MissingGroupNote's message. The archive holds no
// stream: the call throws for one.
std::span<const uint8_t> ItemFile(const ntdlib::sar::SoundArchive& archive, const Item& item);

// The files of `archive` whose bytes it lacks because only group files beside the archive hold them, and the archive
// was read without those group files. Each comes with the names of the groups that hold it: the file table lists a
// file's groups. The 3DS's file table lists none, and its files come without names. A file whose list names no group
// beside the archive isn't among them, and none is in an archive without such groups. Neither is a file outside the
// archive, such as a stream, or a file past the end of an archive that is cut short.
std::map<uint32_t, std::vector<std::string>> FilesInMissingGroups(const ntdlib::sar::SoundArchive& archive);

// What the program says of the file of item `name` when only the group files of `groups` hold it, and the archive lacks
// them (FilesInMissingGroups). Without names, it speaks of a group file.
std::string MissingGroupNote(const std::string& name, std::span<const std::string> groups);

// What the program says of sound `sound` of `archive` when the files of banks in its slots lie in group files that the
// archive lacks (FilesInMissingGroups). The notes that play such a bank have no instrument, and stay silent. Nothing
// when every bank's file is there.
std::string MissingBanksNote(const ntdlib::sar::SoundArchive& archive, uint32_t sound);

// What the program says when it can't write the file named `name`.
std::string WriteFailure(const std::string& name);

// What the program says when it can't make the folder named `name`, with the system's reason from `error`. The message
// ends with one full stop: the Mac's reasons have none, and those of Windows have one.
std::string FolderFailure(const std::string& name, const std::error_code& error);

// The name of the format of a file of `kind` on the console of `flavor`, such as BFSEQ for a sequence file of the
// Switch or the Wii U, and BCSEQ for one of the 3DS. A stream's format is BFSTM or BCSTM.
std::string ItemFormat(ItemKind kind, ntdlib::sar::Flavor flavor);

// A wave sound file and its wave sounds. NintendoWare's tools call the file a set of wave sounds.
struct WaveSoundSet
{
    uint32_t file_id = 0;
    std::string name;
    std::vector<uint32_t> sounds; // by their indexes in the sound table, in order
};

// The wave sound files of `archive`, in the order of their first wave sounds. Each takes the name of the sound group
// that holds its wave sounds (ntdlib's SoundArchive::SoundGroups), such as WSDSET_SE. A group without a name gives
// WSDSET_ and the group's index. A file outside every group takes the name of its first wave sound, or WSD_FILE_ and
// its file ID when that sound has no name.
std::vector<WaveSoundSet> WaveSoundSets(const ntdlib::sar::SoundArchive& archive);

// The name of `item`'s file: the item's name and its format's extension, such as SEQ_TITLE.bfseq. A wave takes its wave
// archive's name and its index. `waves` is the wave archive's number of waves. The index takes zeros in front up to the
// digits of the last index, `waves` - 1: WARC_SE_007.bfwav for wave 7 of 300, and WARC_SE_7.bfwav for wave 7 of 10. A
// stream takes the name of its file beside the archive (StreamFileName). The name suits a file on every system.
std::string ItemFileName(const ntdlib::sar::SoundArchive& archive, const Item& item, std::size_t waves = 0);

// The name of the WAV file of wave `wave` of wave archive `wave_archive`: the name of its wave file (ItemFileName),
// with .wav in place of its format's extension.
std::string WavFileName(const ntdlib::sar::SoundArchive& archive, uint32_t wave_archive, uint32_t wave,
                        std::size_t waves);

// The name that the user sees for `item`: a sound's, a bank's or a wave archive's name, a wave sound file's name from
// WaveSoundSets, or "WARC_SE, wave 7".
std::string ItemName(const ntdlib::sar::SoundArchive& archive, const Item& item);

// Wave `wave` of wave archive `wave_archive` as the console decodes it: its first two channels, up to the end of its
// loop or its last sample. Throws std::runtime_error with a message for the user when the archive lacks the wave, or
// when the wave can't be decoded.
ntdlib::sar::Pcm DecodeItemWave(const ntdlib::sar::SoundArchive& archive, uint32_t wave_archive, uint32_t wave);

// Wave `wave` of wave archive `wave_archive` as a WAV file: 16-bit PCM with the wave's sample rate and its first two
// channels. The console plays only those two. A loop goes in a smpl chunk. Throws std::runtime_error with a message for
// the user when the archive lacks the wave, or when the wave can't be decoded.
std::vector<uint8_t> WaveAsWav(const ntdlib::sar::SoundArchive& archive, uint32_t wave_archive, uint32_t wave);

// The number of channels in the wave file of wave `wave` of wave archive `wave_archive`. Throws std::runtime_error with
// a message for the user when the archive lacks the wave, or when the wave file can't be read.
std::size_t WaveChannels(const ntdlib::sar::SoundArchive& archive, uint32_t wave_archive, uint32_t wave);

// The note that a wave sound plays, and what the sound does to it. A wave sound's tracks hold note events, and each
// event names one of the sound's notes. The consoles read none of the events, and play note 0. A note names a wave of a
// wave archive, with a bank region's settings.
struct WaveSoundNote
{
    uint32_t wave_archive = 0; // by its index among the archive's wave archives
    uint32_t wave = 0;         // by its index in the wave archive
    float pitch = 1.0f;        // the sound's pitch times the note's
    bool panned = false;       // whether the sound or the note sets a pan away from the centre
    bool shaped = false;       // whether an envelope fades the sound in, or down before the release
    std::size_t events = 0;    // the note events of all the sound's tracks
};

// Note 0 of wave sound `sound`: the note that the consoles play (ntdlib's WaveSoundPlayer). Throws std::runtime_error
// with a message for the user when the archive lacks the sound or its file, when the file can't be read, when the sound
// has no note, when the file lacks the note's wave, and when the archive lacks the wave's wave archive.
WaveSoundNote WaveSoundNoteOf(const ntdlib::sar::SoundArchive& archive, uint32_t sound);

// The wave that wave sound `sound` plays, decoded as DecodeItemWave decodes it, at the rate at which the sound plays
// it: the wave's rate times the note's pitch (WaveSoundNoteOf), to the nearest whole number. Throws std::runtime_error
// with a message for the user when WaveSoundNoteOf or DecodeItemWave does, and when that rate is below 1 Hz or above
// 2^31 - 1 Hz.
ntdlib::sar::Pcm DecodeWaveSound(const ntdlib::sar::SoundArchive& archive, uint32_t sound);

// Wave sound `sound` as a WAV file: DecodeWaveSound's samples at its rate, in 16-bit PCM, with the wave's loop in a
// smpl chunk. Throws std::runtime_error with a message for the user when DecodeWaveSound does.
std::vector<uint8_t> WaveSoundAsWav(const ntdlib::sar::SoundArchive& archive, uint32_t sound);

// The name of the WAV file of wave sound `sound`: the sound's name and .wav. The name suits a file on every system.
std::string WaveSoundWavFileName(const ntdlib::sar::SoundArchive& archive, uint32_t sound);

// The other sounds whose file is the file of sound `sound`, a sequence sound or a wave sound, by their indexes in the
// sound table, in order. Each is a sound of the same kind. A sequence file can hold the sequences of several sounds,
// and a wave sound file the wave sounds of several.
std::vector<uint32_t> SoundsSharingFile(const ntdlib::sar::SoundArchive& archive, uint32_t sound);

// The name of the console of `flavor`: "3DS", "Wii U" or "Switch".
std::string ConsoleName(ntdlib::sar::Flavor flavor);

// The version of a file of the console of `flavor` in words: the four bytes on the 3DS, such as 1.0.1.0, and the three
// bytes below the top one on the Wii U and the Switch, such as 1.2.0. Each goes from the highest.
std::string VersionText(ntdlib::sar::Flavor flavor, uint32_t version);

// Whether versions of the files of the consoles of `a` and `b` compare. The 3DS numbers its versions otherwise.
bool VersionsCompare(ntdlib::sar::Flavor a, ntdlib::sar::Flavor b);

// Whether a wave of `version` on the console of `flavor` holds the original start of its loop: from 1.2.0 on the Wii U
// and the Switch, and from 2.1.1.0 on the 3DS.
bool HoldsOriginalStart(ntdlib::sar::Flavor flavor, uint32_t version);

// The version in the header of `file`, a file of the 3DS, the Wii U or the Switch, or nothing when the file is too
// short for a header or holds no byte order mark. The mark gives the version's byte order.
std::optional<uint32_t> HeaderVersion(std::span<const uint8_t> file);

// The console whose files are nearest those of `flavor`'s console: the Wii U for the Switch and the 3DS, and the Switch
// for the Wii U. The Wii U's and the Switch's files hold the same structures in other byte orders.
ntdlib::sar::Flavor OtherConsole(ntdlib::sar::Flavor flavor);

// The consoles among the 3DS, the Wii U and the Switch other than `flavor`'s: OtherConsole first, then the third.
std::vector<ntdlib::sar::Flavor> OtherConsoles(ntdlib::sar::Flavor flavor);

// A sequence file for another console, and what its conversion changed in the biquad filters.
struct ConvertedSequence
{
    std::vector<uint8_t> file;
    BiquadChanges biquads;
};

// `file`, a sequence file of the console of `from`, as a file of the console of `to`. The three consoles' commands take
// the same lengths, except the settings of LFOs 1 to 3: the 3DS reads them without their bytes. The 3DS's player skips
// those settings, LFO 0's curve, phase and period, and the third send (Runs). The arguments of the 3DS's and the Wii
// U's commands are big-endian, and the Switch's little-endian. The Wii U's header and labels are big-endian, and the
// others' little-endian. The commands keep their order, and the labels follow them. Each command keeps its offset
// unless a setting of LFOs 1 to 3 comes before it. `starts` gives the offsets where sounds start in the bytecode. The
// decoder reads commands from there too. The file takes the oldest version that the target's games hold
// (ExportVersions). Where bytes that no command reaches read as commands, they go out as the target's encoder writes
// them. Other bytes stay as they are. The biquad filters take the types and values that come nearest on `to` to the
// filters of `from` (MapBiquads). Throws std::runtime_error with a message for the user when the file can't be read,
// when its commands overlap or jump into one another, and when the target's console can't hold them in their places.
ConvertedSequence ConvertSequenceFile(std::span<const uint8_t> file, ntdlib::sar::Flavor from, ntdlib::sar::Flavor to,
                                      std::span<const uint32_t> starts);

// The sequence file of sequence sound `sound` as a file of the console of `to` (ConvertSequenceFile), with the starts
// of the sounds that play the file. Throws std::runtime_error with a message for the user when the archive lacks the
// sound or its file, and when the file can't be converted.
ConvertedSequence OtherConsoleSequenceFile(const ntdlib::sar::SoundArchive& archive, uint32_t sound,
                                           ntdlib::sar::Flavor to);

// The versions of a converted file: the file's, and for a wave archive, its waves'.
struct FileVersions
{
    uint32_t file = 0;
    uint32_t waves = 0;
};

// The versions of the files of `kind` that go out for the console of `flavor`: the oldest that the console's games
// hold. A game's sound library may refuse a file newer than those of the game. A sequence file takes 1.0.0.0 on the
// 3DS, 1.0.0 on the Wii U and 2.0.0 on the Switch. A bank file takes 1.0.1.0 on the 3DS, and 1.0.0 on the Wii U and the
// Switch. A wave archive file takes 1.0.0.0 on the 3DS, and 1.0.0 on the others. A wave sound file takes 1.0.0.0 on the
// 3DS, 1.0.0 on the Wii U and 1.1.0 on the Switch. A wave takes 2.1.0.0 on the 3DS, 1.1.0 on the Wii U and 1.2.0 on the
// Switch. Each wave sound of a file of the Switch's 1.1.0 holds a filter, and those of 1.0.0 hold none. A file for the
// Wii U keeps its filters. A stream takes 2.0.0.0 on the 3DS, 3.0.0 on the Wii U and 4.0.0 on the Switch. Most of the
// Wii U's games read stream files only up to 3.0.0.
FileVersions ExportVersions(ItemKind kind, ntdlib::sar::Flavor flavor);

// `file`, the file of an item of `kind` on the console of `from`, as a file of the console of `to` with `versions`: a
// bank file, a wave sound file, a wave archive file, a wave file or a stream file (ConvertStreamFile). The three
// consoles' files hold the same structures. The Wii U's are big-endian, and the 3DS's and the Switch's little-endian
// (ntdlib's byte_order.h). A wave's 16-bit PCM samples take the target's byte order too. The bytes after a wave's last
// sample stay out, and so do the samples past a stream's end that the 3DS's tools encode. Throws std::invalid_argument
// for a sequence file: ConvertSequenceFile converts those. Throws std::runtime_error with a message for the user when
// the file isn't a file of the kind on that console, and when ntdlib can't convert it.
std::vector<uint8_t> ConvertItemFile(std::span<const uint8_t> file, ItemKind kind, ntdlib::sar::Flavor from,
                                     ntdlib::sar::Flavor to, const FileVersions& versions);

// The file of `item` as a file of the console of `to`: the sequence file of a sequence sound
// (OtherConsoleSequenceFile), or ConvertItemFile's file with ExportVersions for another item. Throws std::runtime_error
// with a message for the user when the archive lacks the item or its file, and when the file can't be converted. The
// archive holds no stream: the call throws for one.
std::vector<uint8_t> OtherConsoleFile(const ntdlib::sar::SoundArchive& archive, const Item& item,
                                      ntdlib::sar::Flavor to);

// The file of `item`, a wave or a wave archive, as a file of the Wii: an RWAV file of the wave, or an RWAR of the wave
// archive's waves in their order (ntdlib::rsar::WiiWaveOf and WiiWaveArchiveOf). A wave keeps its encoding, its rate,
// its loop and its first two channels. NW4R SND plays only those. Throws std::runtime_error with a message for the user
// when the archive lacks the item or its file, and when the file can't be converted, such as a wave in IMA-ADPCM.
std::vector<uint8_t> WiiWaveFileOf(const ntdlib::sar::SoundArchive& archive, const Item& item);

// The Wii's items. A sound archive of the Wii (RSAR) keeps each file in its groups. A group holds its files, then the
// wave data of its banks and wave sound files. Before RSAR 1.4, a file's wave data holds samples that the file's WAVE
// block describes. From 1.4 on, it can be an RWAR of RWAV files. A wave sound file of version 1.3 holds an RWAR. The
// Wii's archives hold no wave archives of their own.
//
// Hokuto takes the wave data of each bank and of each wave sound file for a wave archive. An Item of kWaveArchive or
// kWave gives it by its file's ID, in place of an index among the archive's wave archives.

// A wave archive of the Wii: the wave data of file `file_id`, a bank's or a wave sound file's, under the name of its
// bank or its wave sound file. `rwar` tells whether the wave data is an RWAR, a file by itself.
struct WiiWaveArchive
{
    uint32_t file_id = 0;
    std::string name;
    bool rwar = false;
};

// The wave archives of the Wii's `archive` whose waves Hokuto reads: the wave data of each bank, in the order of the
// banks, then that of each wave sound file, in the order of the files' first wave sounds (WaveSoundSets). The wave data
// of a file that the archive lacks stays out, and so does that of a file of two banks after the first.
std::vector<WiiWaveArchive> WaveArchivesOf(const ntdlib::rsar::SoundArchive& archive);

// The wave sound files of the Wii's `archive`, in the order of their first wave sounds. The Wii's archives name no sets
// of wave sounds. Each file takes WSD_FILE_ and its file ID.
std::vector<WaveSoundSet> WaveSoundSets(const ntdlib::rsar::SoundArchive& archive);

// The number of waves in the wave data of file `file_id` of the Wii's `archive`, one of WaveArchivesOf. Throws
// std::runtime_error with a message for the user when the archive lacks the wave archive, and when its waves can't be
// read.
std::size_t WaveCountOf(const ntdlib::rsar::SoundArchive& archive, uint32_t file_id);

// The bytes of `item`'s file in the Wii's `archive`: a sequence file (RSEQ), a wave sound file (RWSD), a bank (RBNK), a
// wave archive (RWAR) or a wave (RWAV) of a wave archive. Throws std::runtime_error with a message for the user when
// the archive lacks the item or its file, when a wave archive isn't an RWAR, and when the RWAR can't be read. The
// archive holds no stream: the call throws for one.
std::span<const uint8_t> ItemFile(const ntdlib::rsar::SoundArchive& archive, const Item& item);

// The bytes that Export… writes for `item` of the Wii's `archive`: its file (ItemFile). Wave data that a WAVE block
// describes goes out as an RWAR of the same waves, and a wave there as an RWAV file. The tools lay the samples that a
// WAVE block describes out from such waves (ntdlib::rsar::RawWaveData). Throws std::runtime_error with a message for
// the user as ItemFile does, and when the file of the WAVE block can't be read.
std::vector<uint8_t> ItemBytes(const ntdlib::rsar::SoundArchive& archive, const Item& item);

// The file of `item` of the Wii's `archive`, a wave or a wave archive, as a file of the console of `to` with
// ExportVersions: a wave file, or a wave archive file of the wave data's waves in their order
// (ntdlib::rsar::WaveFileOf). A wave keeps its encoding, its rate, its loop and its first two channels. Throws
// std::runtime_error with a message for the user when the archive lacks the item, and when a wave can't be read or
// converted.
std::vector<uint8_t> LaterWaveFileOf(const ntdlib::rsar::SoundArchive& archive, const Item& item,
                                     ntdlib::sar::Flavor to);

// The name of the format of a file of `kind` on the Wii, such as BRSEQ for a sequence file and BRSTM for a stream.
std::string WiiItemFormat(ItemKind kind);

// The name of `item`'s file in the Wii's `archive`, with the Wii's extensions: SEQ_TITLE.brseq, or BANK_SE_007.brwav
// for a wave of a bank's 300. ItemFileName follows the same scheme for the other consoles.
std::string ItemFileName(const ntdlib::rsar::SoundArchive& archive, const Item& item, std::size_t waves = 0);

// The name of the WAV file of wave `wave` of the wave archive of file `file_id`: the name of its wave file
// (ItemFileName) with .wav in place of .brwav.
std::string WavFileName(const ntdlib::rsar::SoundArchive& archive, uint32_t file_id, uint32_t wave, std::size_t waves);

// The name that the user sees for `item` of the Wii's `archive`: a sound's, a bank's or a wave archive's name, a wave
// sound file's name from WaveSoundSets, or "BANK_SE, wave 7".
std::string ItemName(const ntdlib::rsar::SoundArchive& archive, const Item& item);

// Wave `wave` of the wave archive of file `file_id` as NW4R SND decodes it: its first two channels, up to the end of
// its loop or its last sample. Throws std::runtime_error with a message for the user when the archive lacks the wave,
// or when the wave can't be decoded.
ntdlib::rsar::Pcm DecodeItemWave(const ntdlib::rsar::SoundArchive& archive, uint32_t file_id, uint32_t wave);

// The same wave as a WAV file: 16-bit PCM with the wave's sample rate and its first two channels, with a loop in a smpl
// chunk. Throws std::runtime_error with a message for the user when DecodeItemWave does, and when a WAV file can't hold
// the wave.
std::vector<uint8_t> WaveAsWav(const ntdlib::rsar::SoundArchive& archive, uint32_t file_id, uint32_t wave);

// The number of channels of the same wave. Throws std::runtime_error with a message for the user when the archive lacks
// the wave, or when the wave can't be read.
std::size_t WaveChannels(const ntdlib::rsar::SoundArchive& archive, uint32_t file_id, uint32_t wave);

// The note that the Wii's wave sound `sound` plays: note 0 of the sound. NW4R SND plays that note (ntdlib's WsdPlayer).
// The note names a wave of its file. The wave archive is the file's wave data, by the file's ID (WaveArchivesOf). Of
// the note, NW4R SND reads only the wave and the envelope. The sound's pitch alone sets the pitch, and the sound's pan
// and surround pan set the pan. The Wii's reader leaves the tracks out, and `events` stays 0. The archive has to be
// read by ntdlib::rvl::snd::kRsarOptions. Throws std::runtime_error with a message for the user when the archive lacks
// the sound or its file, when the file can't be read, when the sound plays no note, and when the file lacks the note's
// wave.
WaveSoundNote WaveSoundNoteOf(const ntdlib::rsar::SoundArchive& archive, uint32_t sound);

// The wave that the Wii's wave sound `sound` plays, decoded as DecodeItemWave decodes it, at the rate at which the
// sound plays it: the wave's rate times the sound's pitch, to the nearest whole number. Throws std::runtime_error with
// a message for the user when WaveSoundNoteOf or DecodeItemWave does, and when that rate is below 1 Hz or above
// 2^31 - 1 Hz.
ntdlib::rsar::Pcm DecodeWaveSound(const ntdlib::rsar::SoundArchive& archive, uint32_t sound);

// The Wii's wave sound `sound` as a WAV file: DecodeWaveSound's samples at its rate, in 16-bit PCM, with the wave's
// loop in a smpl chunk. Throws std::runtime_error with a message for the user when DecodeWaveSound does.
std::vector<uint8_t> WaveSoundAsWav(const ntdlib::rsar::SoundArchive& archive, uint32_t sound);

// The name of the WAV file of the Wii's wave sound `sound`: the sound's name and .wav. The name suits a file on every
// system.
std::string WaveSoundWavFileName(const ntdlib::rsar::SoundArchive& archive, uint32_t sound);

// The other sounds whose file is the file of sound `sound` of the Wii's `archive`, a sequence sound or a wave sound, by
// their indexes in the sound table, in order. Each is a sound of the same kind.
std::vector<uint32_t> SoundsSharingFile(const ntdlib::rsar::SoundArchive& archive, uint32_t sound);

} // namespace hokuto::song
