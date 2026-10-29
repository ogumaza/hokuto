// SPDX-License-Identifier: MIT

// A stream in the place of a stream sound's stream: Replace… of a stream sound in the browser. A game keeps its streams
// in files beside the archive (song/streams.h), and a Hokuto file keeps each one whole. The new stream takes the place
// of the file that the sound plays, and every sound that plays the file plays the new stream. The archive's tables stay
// as they are: the sound keeps its tracks, and each track the channels that it plays.
//
// The new stream comes from a stream file of the 3DS, the Switch or the Wii U, or from a WAV file. A stream file of the
// archive's console goes in as it is, unless the game may not read its version. One of another console goes in
// converted (ntdlib's ConvertStream). A WAV file's stream takes its rate, its channels and the first loop of its smpl
// chunk, in DSP-ADPCM or in 16-bit PCM. A stream plays up to the end of its loop. The samples after the loop's end stay
// out. ntdlib's MakeStream lays the stream out as NintendoWare's tools do. It moves a loop's start forward to the start
// of a block, and the samples between repeat after the loop's end. On the 3DS, the loop moves to the block after the
// one that holds it.
//
// Most of the Wii U's games read stream files only up to version 3.0.0. A few read 4.0.0 too. A stream for the Wii U
// therefore takes 3.0.0, unless the game's streams beside the archive show that the game reads 4.0.0
// (IncomingStreamVersion). A stream for the 3DS takes the version of the game's streams.
//
// Before version 2.3.0.0 of a 3DS archive, a stream sound plays the tracks that its stream file holds. The archive's
// tables give only the tracks that play and the channels. A new stream without tracks then takes the present stream's
// tracks (WithSoundTracks).
//
// A 3DS stream sound can play an AAC file in place of a stream file (song/aac.h). Its entry marks the stream as AAC and
// gives the loop. A new stream for such a sound is an AAC file too (ReplaceAacStream). An AAC file goes in as it is, at
// the present stream's rate and with its channels. It takes the present stream's loop where the loop ends within its
// samples: an AAC file holds no loop. A WAV file or a stream file of any console goes in encoded by the system's
// encoder, at the present stream's rate of bits, rate and channels. A mono stream goes into both channels of a stereo
// one, and a stereo stream into a mono one as their mean. The loop of the WAV file or the stream file follows the
// encoder's delay. The entries of the sounds that play the stream take the new loop. That change of the archive's
// tables requires the model form: the file goes into it. Where the file lacks the stream that the entry marks as AAC,
// the new stream keeps its channels, 1 or 2, and its rate where the encoder takes it, or else takes 48,000 Hz. It takes
// 128 kbit/s, the rate of Pokémon X's AAC files. An AAC file of 1 or 2 channels at 44,100 or 48,000 Hz goes in as it
// is, with the entry's loop where the loop ends within its samples: the game's file fits it.
//
// The Wii keeps its streams as RSTM files, and the stream file gives a stream sound's tracks too. The builds of NW4R
// SND from March 2008 on play the tracks that the sound's entry names, with the stream file's volumes and pans. The
// builds before play the stream's first two channels at full volume and in the centre
// (ntdlib::rvl::snd::StrmPlayer::TracksOf). A stream file of the Wii goes in as it is, with its tracks. One of the 3DS,
// the Wii U or the Switch goes in converted (ntdlib::rsar::WiiStreamOf). A WAV file's stream takes DSP-ADPCM or 16-bit
// PCM (ntdlib::rsar::MakeStream). ntdlib lays a stream of the Wii out as NW4R's tools do, and moves a loop's start to a
// block as they do. A stream without tracks, from a WAV file or from another console, takes the present stream's tracks
// where it holds the channels that they play. Otherwise it takes a track for each pair of its channels.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/stream_file.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/stream_file.h"

#include "edit/files.h"
#include "format/file.h"
#include "song/streams.h"

namespace hokuto::edit
{

// A stream to put in: the stream file, and what the file held. IncomingWave gives the same for a wave. `bytes` holds
// the stream file. A loop in a WAV file's stream starts `loop_moved` samples later, at the start of a block.
using IncomingStream = IncomingWave;

// Whether `bytes` start as a stream file of the 3DS, the Wii U or the Switch does: with CSTM or FSTM.
bool IsStreamFile(std::span<const uint8_t> bytes);

// Whether the stream sounds of `archive` play the tracks of their stream files: on the 3DS, before version 2.3.0.0 of
// the archive.
bool TracksInFiles(const ntdlib::sar::SoundArchive& archive);

// The version of a stream that goes into the archive that `file` builds, an archive of `flavor`. On the Wii U, a stream
// takes 4.0.0 when one of the game's streams that the file keeps holds 4.0.0 or a later version, and 3.0.0 otherwise.
// On the Switch, a stream takes 4.0.0. On the 3DS, a stream takes the latest version among the game's streams that the
// file keeps, and 2.0.0.0 when the file keeps none.
uint32_t IncomingStreamVersion(const format::File& file, ntdlib::sar::Flavor flavor);

// Reads `bytes` as a stream file for an archive of `flavor`. The archive takes streams of `version`
// (IncomingStreamVersion). The file's header gives its size. Bytes past it stay out. A stream file of another console
// goes in converted, in `version`. A stream file of the Wii U or the 3DS with a later version than `version` goes in
// written again in `version`, with the same samples. For such a file and for a file of another console, `converted`
// holds the file that was read. Throws std::invalid_argument with a message for the user for bytes that aren't a stream
// file, and a stream that ntdlib can't read, write or convert.
IncomingStream ReadIncomingStreamFile(std::span<const uint8_t> bytes, ntdlib::sar::Flavor flavor, uint32_t version);

// Reads `bytes` as a WAV file, and makes a stream of `flavor` and `version` of it in `encoding`: DSP-ADPCM or 16-bit
// PCM. Throws std::invalid_argument with a message for the user for bytes that aren't a WAV file that Hokuto reads, a
// file of no samples, of too many or of more channels than a stream holds, and another encoding.
IncomingStream ReadIncomingStreamWav(std::span<const uint8_t> bytes, ntdlib::sar::Flavor flavor, uint32_t version,
                                     ntdlib::sar::WaveEncoding encoding = ntdlib::sar::WaveEncoding::kDspAdpcm);

// The stream that a stream sound plays, and what plays it.
struct StreamChanges
{
    uint32_t file = 0;    // the stream file's ID in the archive's file table
    bool present = false; // whether the file holds the stream. Without it, the next fields keep their defaults.
    ntdlib::sar::WaveEncoding encoding = ntdlib::sar::WaveEncoding::kDspAdpcm;
    std::size_t channels = 0;
    uint32_t rate = 0;
    bool loop = false;
    uint32_t loop_start = 0;
    uint32_t length = 0;   // the samples in each channel
    std::size_t size = 0;  // the stream file's size
    bool from_game = true; // whether the game holds the same stream

    std::vector<uint32_t> sounds;             // the other stream sounds that play the file
    std::size_t table_channels = 0;           // the channels that the sound's entry gives
    std::vector<std::vector<uint8_t>> tracks; // the channels of each track that the sound plays

    // Whether the sound plays the tracks of its stream file: on the 3DS, before version 2.3.0.0 of the archive, and on
    // the Wii. The tracks above then come from the present stream, and `file_tracks` holds them whole. On the Wii,
    // `wii_tracks` holds them instead.
    bool tracks_in_file = false;
    std::vector<ntdlib::sar::StreamFile::Track> file_tracks;
    std::vector<ntdlib::rsar::StreamFile::Track> wii_tracks;
};

// The stream that stream sound `sound` of `archive` plays, from `file`, the file that builds the archive. Throws
// std::invalid_argument with a message for the user for a sound that isn't a stream sound, a sound whose stream doesn't
// lie beside the archive, in its folder, and a stream in the file that ntdlib can't read.
StreamChanges ChangesOfStream(const format::File& file, const ntdlib::sar::SoundArchive& archive, uint32_t sound);

// When the sound of `changes` plays the tracks of its stream file (StreamChanges::tracks_in_file) and a new stream
// holds none, the tracks that the new stream takes: the present stream's tracks. A present track that plays a channel
// that the new stream lacks gives way to a track for each pair of the new stream's channels, the last alone. So does a
// sound whose present stream the file doesn't hold. Nothing elsewhere, and for a new stream with tracks. Throws
// std::invalid_argument with a message for the user for a stream that ntdlib can't read.
std::optional<std::vector<ntdlib::sar::StreamFile::Track>> SoundTracksFor(const IncomingStream& incoming,
                                                                          const StreamChanges& changes);

// `incoming` with the tracks of SoundTracksFor, when it gives any. Throws std::invalid_argument with a message for the
// user for a stream that ntdlib can't read or write.
IncomingStream WithSoundTracks(IncomingStream incoming, const StreamChanges& changes);

// `file` after its native archive's stream file `file_id` holds `bytes`. A stream with the bytes that the file held
// keeps its mark: the game's or Hokuto's. Any other stream is Hokuto's, and an export writes it. Throws
// std::invalid_argument with a message for the user for a file without a native archive.
format::File ReplaceStream(const format::File& file, uint32_t file_id, std::vector<uint8_t> bytes);

// The AAC stream that a 3DS stream sound plays, and what plays it. Where the file lacks the stream, `present` is false,
// and the channels, the rate, the length and the size are 0.
struct AacChanges
{
    uint32_t file = 0;                 // the stream file's ID in the archive's file table
    bool present = true;               // whether the file holds the stream
    std::size_t channels = 0;          // the present stream's, or else the sound's entry's
    uint32_t rate = 0;                 // the present stream's
    uint32_t length = 0;               // the decoder's samples up to the present stream's end
    std::optional<song::AacLoop> loop; // the present stream's, or the entry's
    std::size_t size = 0;              // the present AAC file's size
    uint32_t bit_rate = 0;             // the rate of song::kAacBitRates nearest the present stream's
    std::vector<uint32_t> sounds;      // the other stream sounds that play the file
};

// The AAC stream that stream sound `sound` of `archive` plays, from `file`, the file that builds the archive. Nothing
// for a sound whose entry marks no AAC stream with a file beside the archive, and for a file that holds another kind of
// stream there. Throws std::runtime_error with a message for the user when the system can't decode the stream.
std::optional<AacChanges> ChangesOfAacStream(const format::File& file, const ntdlib::sar::SoundArchive& archive,
                                             uint32_t sound);

// An AAC stream to put in the place of an AAC stream (ReadIncomingAac).
struct IncomingAac
{
    std::vector<uint8_t> bytes;        // the AAC file in ADTS
    std::size_t channels = 0;          // of the file that came in
    uint32_t rate = 0;                 // of the file that came in
    std::size_t stream_channels = 0;   // of the new stream
    uint32_t stream_rate = 0;          // of the new stream
    std::optional<song::AacLoop> loop; // in the new stream's decoded samples
    uint32_t length = 0;               // the new stream's decoded samples up to its end
    bool encoded = false;              // whether Hokuto encoded it: a WAV file or a stream file came in
    uint32_t delay = 0;                // the encoder's delay, where Hokuto encoded it
    bool present_loop = false;         // whether an AAC file took the present stream's loop
    std::size_t after_loop = 0;        // the samples that stay out after a WAV file's or a stream file's loop
};

// Reads `bytes`, an AAC file in ADTS, a WAV file or a stream file of any console, as a new stream for the sound of
// `changes`. The new stream takes the present stream's channels, or else the 1 or 2 channels that the sound's entry
// gives. A mono file then fills both channels of a stereo stream. Throws std::invalid_argument with a message for the
// user for other bytes, for more channels than the stream holds where it holds more than one, and when the system can't
// decode or encode the stream.
IncomingAac ReadIncomingAac(std::span<const uint8_t> bytes, const AacChanges& changes);

// `file` in the model form, with `incoming`'s AAC file in the place of the stream of `changes`. The entries of the
// sounds that play the stream mark it as AAC, with the new loop or none. Throws std::invalid_argument with a message
// for the user for a file whose archive the model form doesn't take.
format::File ReplaceAacStream(const format::File& file, const AacChanges& changes, const IncomingAac& incoming);

// The values of a stream that its properties edit: its encoding, its rate and its loop. A stream plays up to its end,
// and its loop runs to its end too.
struct StreamValues
{
    friend bool operator==(const StreamValues&, const StreamValues&) = default;

    ntdlib::sar::WaveEncoding encoding = ntdlib::sar::WaveEncoding::kDspAdpcm;
    uint32_t rate = 0;
    bool loop = false;
    uint32_t loop_start = 0;
    uint32_t length = 0; // the samples in each channel
};

// The values of the stream file `bytes`, of the Wii (RSTM) or of a later console. Throws std::invalid_argument with a
// message for the user for bytes that ntdlib can't read as a stream file.
StreamValues StreamValuesOf(std::span<const uint8_t> bytes);

// `bytes`, a stream file, with `values`. A rate, and a loop that starts at a block of the stream with the stream's
// length, keep the encoded samples. A loop in DSP-ADPCM then starts again with the decoder's state at its start. On the
// 3DS, a loop from the first block doesn't keep them: the 3DS's tools never start a loop there. A 3DS stream that keeps
// its samples keeps the samples past its end that the tools encode. Those samples follow the old loop. Any other change
// makes the stream again from its decoded samples up to `length`, with ntdlib's MakeStream. A loop's start then moves
// forward to a block, and the samples between repeat after the loop's end. The file keeps its tracks.
// A stream of the Wii (RSTM) takes the values the same way. Its decoder's state at a loop's start comes from the Wii's
// sums, in 64 bits. A stream in DSP-ADPCM without a loop takes the state that the tools write for one: the first
// frame's predictor and scale, and no history. A stream made again keeps its tracks and their kind.
// Throws std::invalid_argument with a message for the user for a stream that ntdlib can't read or write, a rate of 0, a
// length of 0 or past the stream's samples, a loop that starts at its end or after it, and an encoding other than
// DSP-ADPCM and 16-bit PCM where the stream is made again.
std::vector<uint8_t> StreamWithValues(std::span<const uint8_t> bytes, const StreamValues& values);

// Whether `bytes` start as a stream file of the Wii does: with RSTM and the big-endian byte order mark.
bool IsWiiStreamFile(std::span<const uint8_t> bytes);

// Reads `bytes` as a stream file for an archive of the Wii. The file's header gives its size. Bytes past it stay out. A
// stream file of the Wii goes in as it is. One of the 3DS, the Wii U or the Switch goes in converted, and `converted`
// holds the file that was read. A converted stream without tracks takes `tracks`, such as the present stream's, where
// it holds the channels that each one plays. Otherwise it takes a track for each pair of its channels. Throws
// std::invalid_argument with a message for the user for bytes that aren't a stream file, and a stream that ntdlib can't
// read, convert or write.
IncomingStream ReadIncomingWiiStreamFile(std::span<const uint8_t> bytes,
                                         std::span<const ntdlib::rsar::StreamFile::Track> tracks = {});

// Reads `bytes` as a WAV file, and makes a stream of the Wii of it in `encoding`: DSP-ADPCM or 16-bit PCM. The stream
// takes `tracks` where it holds the channels that each one plays, and otherwise a track for each pair of its channels.
// Throws std::invalid_argument with a message for the user for bytes that aren't a WAV file that Hokuto reads, a file
// of no samples, of too many or of more channels than a stream holds, and another encoding.
IncomingStream ReadIncomingWiiStreamWav(std::span<const uint8_t> bytes,
                                        ntdlib::sar::WaveEncoding encoding = ntdlib::sar::WaveEncoding::kDspAdpcm,
                                        std::span<const ntdlib::rsar::StreamFile::Track> tracks = {});

// The stream that stream sound `sound` of the Wii's `archive` plays, from `file`, the file that builds the archive.
// ChangesOfStream gives the same on the later consoles. `table_channels` holds the channels that the sound's entry
// names: a count from version 1.4 of the archive on, and a bit for each channel before. The present stream's tracks go
// in `wii_tracks`. Throws std::invalid_argument with a message for the user for a sound that isn't a stream sound, a
// sound whose stream doesn't lie beside the archive, in its folder, and a stream in the file that ntdlib can't read.
StreamChanges ChangesOfWiiStream(const format::File& file, const ntdlib::rsar::SoundArchive& archive, uint32_t sound);

} // namespace hokuto::edit
